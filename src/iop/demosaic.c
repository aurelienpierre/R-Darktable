/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/interpolation.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"
#include "develop/tiling.h"

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "control/conf.h"
#include "common/colorspaces_inline_conversions.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <memory.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <complex.h>
#include <glib.h>

#ifdef __GNUC__
  #define INLINE __inline
#else
  #define INLINE inline
#endif

#define DEMOSAIC_XTRANS 1024 // masks for non-Bayer demosaic ops
#define DEMOSAIC_DUAL 2048   // masks for dual demosaicing methods
#define REDUCESIZE 64

#define XTRANS_SNAPPER 3
#define BAYER_SNAPPER 2

DT_MODULE_INTROSPECTION(4, dt_iop_demosaic_params_t)

typedef enum dt_iop_demosaic_method_t
{
  // methods for Bayer images
  DT_IOP_DEMOSAIC_PPG = 0,   // $DESCRIPTION: "PPG"
  DT_IOP_DEMOSAIC_AMAZE = 1, // $DESCRIPTION: "AMaZE"
  DT_IOP_DEMOSAIC_VNG4 = 2,  // $DESCRIPTION: "VNG4"
  DT_IOP_DEMOSAIC_RCD = 5,   // $DESCRIPTION: "RCD"
  DT_IOP_DEMOSAIC_LMMSE = 6, // $DESCRIPTION: "LMMSE"
  DT_IOP_DEMOSAIC_RCD_VNG = DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_RCD, // $DESCRIPTION: "RCD + VNG4"
  DT_IOP_DEMOSAIC_AMAZE_VNG = DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_AMAZE, // $DESCRIPTION: "AMaZE + VNG4"
  DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME = 3, // $DESCRIPTION: "passthrough (monochrome)"
  DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR = 4, // $DESCRIPTION: "photosite color (debug)"
  DT_IOP_DEMOSAIC_BILINEAR = 7, // $DESCRIPTION: "bilinear"
  // methods for x-trans images
  DT_IOP_DEMOSAIC_VNG = DEMOSAIC_XTRANS | 0,           // $DESCRIPTION: "VNG"
  DT_IOP_DEMOSAIC_MARKESTEIJN = DEMOSAIC_XTRANS | 1,   // $DESCRIPTION: "Markesteijn 1-pass"
  DT_IOP_DEMOSAIC_MARKESTEIJN_3 = DEMOSAIC_XTRANS | 2, // $DESCRIPTION: "Markesteijn 3-pass"
  DT_IOP_DEMOSAIC_FDC = DEMOSAIC_XTRANS | 4,           // $DESCRIPTION: "frequency domain chroma"
  DT_IOP_DEMOSAIC_MARKEST3_VNG = DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_MARKESTEIJN_3, // $DESCRIPTION: "Markesteijn 3-pass + VNG"
  DT_IOP_DEMOSAIC_PASSTHR_MONOX = DEMOSAIC_XTRANS | 3, // $DESCRIPTION: "passthrough (monochrome)"
  DT_IOP_DEMOSAIC_PASSTHR_COLORX = DEMOSAIC_XTRANS | 5, // $DESCRIPTION: "photosite color (debug)"
} dt_iop_demosaic_method_t;

typedef enum dt_iop_demosaic_greeneq_t
{
  DT_IOP_GREEN_EQ_NO = 0,    // $DESCRIPTION: "disabled"
  DT_IOP_GREEN_EQ_LOCAL = 1, // $DESCRIPTION: "local average"
  DT_IOP_GREEN_EQ_FULL = 2,  // $DESCRIPTION: "full average"
  DT_IOP_GREEN_EQ_BOTH = 3   // $DESCRIPTION: "full and local average"
} dt_iop_demosaic_greeneq_t;

typedef enum dt_iop_demosaic_qual_flags_t
{
  // either perform full scale demosaicing or choose simple half scale
  // or third scale interpolation instead
  DEMOSAIC_FULL_SCALE              = 1 << 0,
  DEMOSAIC_ONLY_VNG_LINEAR         = 1 << 1,
  DEMOSAIC_XTRANS_FULL             = 1 << 2,
  DEMOSAIC_MEDIUM_QUAL             = 1 << 3
} dt_iop_demosaic_qual_flags_t;

typedef enum dt_iop_demosaic_smooth_t
{
  DEMOSAIC_SMOOTH_OFF = 0, // $DESCRIPTION: "disabled"
  DEMOSAIC_SMOOTH_1 = 1,   // $DESCRIPTION: "once"
  DEMOSAIC_SMOOTH_2 = 2,   // $DESCRIPTION: "twice"
  DEMOSAIC_SMOOTH_3 = 3,   // $DESCRIPTION: "three times"
  DEMOSAIC_SMOOTH_4 = 4,   // $DESCRIPTION: "four times"
  DEMOSAIC_SMOOTH_5 = 5,   // $DESCRIPTION: "five times"
} dt_iop_demosaic_smooth_t;

typedef enum dt_iop_demosaic_lmmse_t
{
  LMMSE_REFINE_0 = 0,   // $DESCRIPTION: "basic"
  LMMSE_REFINE_1 = 1,   // $DESCRIPTION: "median"
  LMMSE_REFINE_2 = 2,   // $DESCRIPTION: "3x median"
  LMMSE_REFINE_3 = 3,   // $DESCRIPTION: "refine & medians"
  LMMSE_REFINE_4 = 4,   // $DESCRIPTION: "2x refine + medians"
} dt_iop_demosaic_lmmse_t;

typedef struct dt_iop_demosaic_global_data_t
{
  // demosaic pattern
  int kernel_green_eq_lavg;
  int kernel_green_eq_favg_reduce_first;
  int kernel_green_eq_favg_reduce_second;
  int kernel_green_eq_favg_apply;
  int kernel_pre_median;
  int kernel_passthrough_monochrome;
  int kernel_passthrough_color;
  int kernel_ppg_green;
  int kernel_ppg_redblue;
  int kernel_zoom_half_size;
  int kernel_downsample;
  int kernel_border_interpolate;
  int kernel_color_smoothing;
  int kernel_zoom_passthrough_monochrome;
  int kernel_vng_border_interpolate;
  int kernel_vng_lin_interpolate;
  int kernel_zoom_third_size;
  int kernel_vng_green_equilibrate;
  int kernel_vng_interpolate;
  int kernel_markesteijn_initial_copy;
  int kernel_markesteijn_green_minmax;
  int kernel_markesteijn_interpolate_green;
  int kernel_markesteijn_solitary_green;
  int kernel_markesteijn_recalculate_green;
  int kernel_markesteijn_red_and_blue;
  int kernel_markesteijn_interpolate_twoxtwo;
  int kernel_markesteijn_convert_yuv;
  int kernel_markesteijn_differentiate;
  int kernel_markesteijn_homo_threshold;
  int kernel_markesteijn_homo_set;
  int kernel_markesteijn_homo_sum;
  int kernel_markesteijn_homo_max;
  int kernel_markesteijn_homo_max_corr;
  int kernel_markesteijn_homo_quench;
  int kernel_markesteijn_zero;
  int kernel_markesteijn_accu;
  int kernel_markesteijn_final;
  int kernel_rcd_populate;
  int kernel_rcd_write_output;
  int kernel_rcd_step_1_1;
  int kernel_rcd_step_1_2;
  int kernel_rcd_step_2_1;
  int kernel_rcd_step_3_1;
  int kernel_rcd_step_4_1;
  int kernel_rcd_step_4_2;
  int kernel_rcd_step_5_1;
  int kernel_rcd_step_5_2;
  int kernel_rcd_border_redblue;
  int kernel_rcd_border_green;
  int kernel_write_blended_dual;
  float *lmmse_gamma_in;
  float *lmmse_gamma_out;
} dt_iop_demosaic_global_data_t;


typedef struct dt_iop_demosaic_data_t
{
  uint32_t green_eq;
  uint32_t color_smoothing;
  uint32_t demosaicing_method;
  uint32_t lmmse_refine;
  float median_thrs;
  double CAM_to_RGB[3][4];
  float dual_thrs;
} dt_iop_demosaic_data_t;


static inline float intp(float a, float b, float c)
{   // taken from rt code
    // calculate a * b + (1 - a) * c
    // following is valid:
    // intp(a, b+x, c+x) = intp(a, b, c) + x
    // intp(a, b*x, c*x) = intp(a, b, c) * x
    return a * (b - c) + c;
}

typedef enum dt_iop_demosaic_quality_t
{
  DT_DEMOSAIC_FAST = 0,
  DT_DEMOSAIC_FAIR = 1,
  DT_DEMOSAIC_BEST = 2
} dt_iop_demosaic_quality_t;


typedef struct dt_iop_demosaic_params_t
{
  dt_iop_demosaic_greeneq_t green_eq; // $DEFAULT: DT_IOP_GREEN_EQ_NO $DESCRIPTION: "match greens"
  float median_thrs; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "edge threshold"
  dt_iop_demosaic_smooth_t color_smoothing; // $DEFAULT: DEMOSAIC_SMOOTH_OFF $DESCRIPTION: "color smoothing"
  dt_iop_demosaic_method_t demosaicing_method; // $DEFAULT: DT_IOP_DEMOSAIC_RCD $DESCRIPTION: "demosaicing method"
  dt_iop_demosaic_lmmse_t lmmse_refine; // $DEFAULT: LMMSE_REFINE_1 $DESCRIPTION: "LMMSE refine"
  float dual_thrs; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.20 $DESCRIPTION: "dual threshold"
} dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
  GtkWidget *median_thrs;
  GtkWidget *greeneq;
  GtkWidget *color_smoothing;
  GtkWidget *demosaic_method_bayer;
  GtkWidget *demosaic_method_xtrans;
  GtkWidget *dual_thrs;
  GtkWidget *lmmse_refine;
  gboolean visual_mask;
} dt_iop_demosaic_gui_data_t;

// set flags for demosaic quality based on factors besides demosaic
// method (e.g. config, scale, pixelpipe type)
static int demosaic_qual_flags(const dt_dev_pixelpipe_iop_t *const piece,
                               const dt_image_t *const img,
                               const dt_iop_roi_t *const roi_out)
{
  return DEMOSAIC_FULL_SCALE | DEMOSAIC_XTRANS_FULL;
}

// Implemented on amaze_demosaic_RT.cc
void amaze_demosaic_RT(
    dt_dev_pixelpipe_iop_t *piece,
    const float *const in,
    float *out,
    const dt_iop_roi_t *const roi_in,
    const dt_iop_roi_t *const roi_out,
    const uint32_t filters);


// Mind the order of includes, there are internal dependencies
// FIXME: handle all the branching uniformingly
#include "demosaic/basic.c"
#include "demosaic/passthrough.c"
#include "demosaic/rcd.c"
#include "demosaic/lmmse.c"
#include "demosaic/ppg.c"
#include "demosaic/vng.c"
#include "demosaic/markesteijn.c"
#include "demosaic/dual.c"


const char *name()
{
  return _("demosaic");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("reconstruct full RGB pixels from a sensor color filter array reading"),
                                      _("mandatory"),
                                      _("linear, raw, scene-referred"),
                                      _("linear, raw"),
                                      _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_FENCE;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  typedef struct dt_iop_demosaic_params_t dt_iop_demosaic_params_v4_t;
  typedef struct dt_iop_demosaic_params_v3_t
  {
    dt_iop_demosaic_greeneq_t green_eq;
    float median_thrs;
    uint32_t color_smoothing;
    dt_iop_demosaic_method_t demosaicing_method;
    dt_iop_demosaic_lmmse_t lmmse_refine;
  } dt_iop_demosaic_params_v3_t;

  if(old_version == 3 && new_version == 4)
  {
    dt_iop_demosaic_params_v3_t *o = (dt_iop_demosaic_params_v3_t *)old_params;
    dt_iop_demosaic_params_v4_t *n = (dt_iop_demosaic_params_v4_t *)new_params;
    memcpy(n, o, sizeof *o);
    n->dual_thrs = 0.20f;
    return 0;
  }

  if(old_version == 2 && new_version == 3)
  {
    dt_iop_demosaic_params_t *o = (dt_iop_demosaic_params_t *)old_params;
    dt_iop_demosaic_params_t *n = (dt_iop_demosaic_params_t *)new_params;
    n->green_eq = o->green_eq;
    n->median_thrs = o->median_thrs;
    n->color_smoothing = 0;
    n->demosaicing_method = DT_IOP_DEMOSAIC_PPG;
    n->lmmse_refine = LMMSE_REFINE_1;
    return 0;
  }
  return 1;
}

int input_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                     dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

int output_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static const char* method2string(dt_iop_demosaic_method_t method)
{
  const char *string;

  switch(method)
  {
    case DT_IOP_DEMOSAIC_PPG:
      string = "PPG";
      break;
    case DT_IOP_DEMOSAIC_AMAZE:
      string = "AMaZE";
      break;
    case DT_IOP_DEMOSAIC_VNG4:
      string = "VNG4";
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME:
      string = "passthrough monochrome";
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR:
      string = "photosites";
      break;
    case DT_IOP_DEMOSAIC_RCD:
      string = "RCD";
      break;
    case DT_IOP_DEMOSAIC_LMMSE:
      string = "LMMSE";
      break;
    case DT_IOP_DEMOSAIC_RCD_VNG:
      string = "RCD + VNG4";
      break;
    case DT_IOP_DEMOSAIC_AMAZE_VNG:
      string = "AMaZE + VNG4";
      break;
    case DT_IOP_DEMOSAIC_VNG:
      string = "VNG (xtrans)";
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN:
      string = "Markesteijn-1 (XTrans)";
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN_3:
      string = "Markesteijn-3 (XTrans)";
      break;
    case DT_IOP_DEMOSAIC_MARKEST3_VNG:
      string = "Markesteijn 3-pass + VNG";
      break;
    case DT_IOP_DEMOSAIC_FDC:
      string = "Frequency Domain Chroma (XTrans)";
      break;
    case DT_IOP_DEMOSAIC_PASSTHR_MONOX:
      string = "passthrough monochrome (XTrans)";
      break;
    case DT_IOP_DEMOSAIC_PASSTHR_COLORX:
      string = "photosites (XTrans)";
      break;
    case DT_IOP_DEMOSAIC_BILINEAR:
      string = "bilinear";
      break;
    default:
      string = "(unknown method)";
  }
  return string;
}


void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  dt_interpolation_resample_roi_1c(itor, out, roi_out, in, roi_in);
}

void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;

  // snap to start of mosaic block:
  roi_out->x = 0; // MAX(0, roi_out->x & ~1);
  roi_out->y = 0; // MAX(0, roi_out->y & ~1);
}

// which roi input is needed to process to this output?
// roi_out is unchanged, full buffer in is full buffer out.
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  // this op is disabled for preview pipe/filters == 0

  *roi_in = *roi_out;
  // need 1:1, demosaic and then sub-sample. or directly sample half-size
  roi_in->x /= roi_out->scale;
  roi_in->y /= roi_out->scale;
  roi_in->width /= roi_out->scale;
  roi_in->height /= roi_out->scale;
  roi_in->scale = 1.0f;

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  const int method = data->demosaicing_method;
  const gboolean passthrough = (method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
                               (method == DT_IOP_DEMOSAIC_PASSTHR_MONOX);

  // set position to closest sensor pattern snap
  if(!passthrough)
  {
    const int aligner = (piece->pipe->dsc.filters != 9u) ? BAYER_SNAPPER : XTRANS_SNAPPER;
    const int dx = roi_in->x % aligner;
    const int dy = roi_in->y % aligner;
    const int shift_x = (dx > aligner / 2) ? aligner - dx : -dx;
    const int shift_y = (dy > aligner / 2) ? aligner - dy : -dy;

    roi_in->x = MAX(0, roi_in->x + shift_x);
    roi_in->y = MAX(0, roi_in->y + shift_y);
  }

  // clamp numeric inaccuracies to full buffer, to avoid scaling/copying in pixelpipe:
  if(abs(piece->pipe->image.width - roi_in->width) < MAX(ceilf(1.0f / roi_out->scale), 10))
    roi_in->width = piece->pipe->image.width;

  if(abs(piece->pipe->image.height - roi_in->height) < MAX(ceilf(1.0f / roi_out->scale), 10))
    roi_in->height = piece->pipe->image.height;
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_image_t *img = &self->dev->image_storage;
  const float threshold = 0.0001f * img->exif_iso;
  dt_times_t start_time = { 0 }, end_time = { 0 };

  dt_dev_clear_rawdetail_mask(piece->pipe);

  dt_iop_roi_t roi = *roi_in;
  dt_iop_roi_t roo = *roi_out;
  roo.x = roo.y = 0;
  // roi_out->scale = global scale: (iscale == 1.0, always when demosaic is on)
  const gboolean info = ((darktable.unmuted & (DT_DEBUG_DEMOSAIC | DT_DEBUG_PERF)) && (piece->pipe->type == DT_DEV_PIXELPIPE_FULL));

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int qual_flags = demosaic_qual_flags(piece, img, roi_out);
  int demosaicing_method = data->demosaicing_method;

  gboolean showmask = FALSE;
  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL)
  {
    dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
    if(g) showmask = (g->visual_mask);
    // take care of passthru modes
    if(piece->pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU)
      demosaicing_method = (piece->pipe->dsc.filters != 9u) ? DT_IOP_DEMOSAIC_RCD : DT_IOP_DEMOSAIC_MARKESTEIJN;
    else if(piece->pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU_MONO)
      demosaicing_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  }

  if((qual_flags & DEMOSAIC_MEDIUM_QUAL)
  // only overwrite setting if quality << requested and in dr mode and not a special method
  && (demosaicing_method != DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
  && (demosaicing_method != DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
  // dual demosaicing with the show mask option on is also a special method for ui
  && !((demosaicing_method & DEMOSAIC_DUAL) && showmask))
    demosaicing_method = (piece->pipe->dsc.filters != 9u) ? DT_IOP_DEMOSAIC_RCD : DT_IOP_DEMOSAIC_MARKESTEIJN;

  const float *const pixels = (float *)i;

  if(qual_flags & DEMOSAIC_FULL_SCALE)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);
    float *tmp = (float *) o;
    if(scaled)
    {
      // demosaic and then clip and zoom
      // we demosaic at 1:1 the size of input roi, so make sure
      // we fit these bounds exactly, to avoid crashes..
      roo.width = roi_in->width;
      roo.height = roi_in->height;
      roo.scale = 1.0f;
      tmp = (float *)dt_alloc_align_float((size_t)4 * roo.width * roo.height);
    }
    if(info) dt_get_times(&start_time);

    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    {
      passthrough_monochrome(tmp, pixels, &roo, &roi);
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
    {
      passthrough_color(tmp, pixels, &roo, &roi, piece->pipe->dsc.filters, xtrans);
    }
    else if(piece->pipe->dsc.filters == 9u)
    {
      const int passes = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN) ? 1 : 3;
      if(demosaicing_method == DT_IOP_DEMOSAIC_MARKEST3_VNG)
        xtrans_markesteijn_interpolate(tmp, pixels, &roo, &roi, xtrans, passes);
      else if(demosaicing_method == DT_IOP_DEMOSAIC_FDC && (qual_flags & DEMOSAIC_XTRANS_FULL))
        xtrans_fdc_interpolate(self, tmp, pixels, &roo, &roi, xtrans);
      else if(demosaicing_method >= DT_IOP_DEMOSAIC_MARKESTEIJN && (qual_flags & DEMOSAIC_XTRANS_FULL))
        xtrans_markesteijn_interpolate(tmp, pixels, &roo, &roi, xtrans, passes);
      else
        vng_interpolate(tmp, pixels, &roo, &roi, piece->pipe->dsc.filters, xtrans, qual_flags & DEMOSAIC_ONLY_VNG_LINEAR);
    }
    else
    {
      float *in = (float *)pixels;
      float *aux;

      if(!(img->flags & DT_IMAGE_4BAYER) && data->green_eq != DT_IOP_GREEN_EQ_NO)
      {
        in = (float *)dt_alloc_align_float((size_t)roi_in->height * roi_in->width);
        switch(data->green_eq)
        {
          case DT_IOP_GREEN_EQ_FULL:
            green_equilibration_favg(in, pixels, roi_in->width, roi_in->height, piece->pipe->dsc.filters,
                                     roi_in->x, roi_in->y);
            break;
          case DT_IOP_GREEN_EQ_LOCAL:
            green_equilibration_lavg(in, pixels, roi_in->width, roi_in->height, piece->pipe->dsc.filters,
                                     roi_in->x, roi_in->y, threshold);
            break;
          case DT_IOP_GREEN_EQ_BOTH:
            aux = dt_alloc_align_float((size_t)roi_in->height * roi_in->width);
            green_equilibration_favg(aux, pixels, roi_in->width, roi_in->height, piece->pipe->dsc.filters,
                                     roi_in->x, roi_in->y);
            green_equilibration_lavg(in, aux, roi_in->width, roi_in->height, piece->pipe->dsc.filters, roi_in->x,
                                     roi_in->y, threshold);
            dt_free_align(aux);
            break;
        }
      }

      if(demosaicing_method == DT_IOP_DEMOSAIC_VNG4 || (img->flags & DT_IMAGE_4BAYER))
      {
        vng_interpolate(tmp, in, &roo, &roi, piece->pipe->dsc.filters, xtrans, qual_flags & DEMOSAIC_ONLY_VNG_LINEAR);
        if(img->flags & DT_IMAGE_4BAYER)
        {
          dt_colorspaces_cygm_to_rgb(tmp, roo.width*roo.height, data->CAM_to_RGB);
          dt_colorspaces_cygm_to_rgb(piece->pipe->dsc.processed_maximum, 1, data->CAM_to_RGB);
        }
      }
      else if((demosaicing_method & ~DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_RCD)
      {
        rcd_demosaic(piece, tmp, in, &roo, &roi, piece->pipe->dsc.filters);
      }
      else if(demosaicing_method == DT_IOP_DEMOSAIC_BILINEAR)
      {
        // separate out G1 and G2 in RGGB Bayer patterns
        uint32_t filters4 = 0;
        if(piece->pipe->dsc.filters == 9 || FILTERS_ARE_4BAYER(piece->pipe->dsc.filters)) // x-trans or CYGM/RGBE
          filters4 = piece->pipe->dsc.filters;
        else if((piece->pipe->dsc.filters & 3) == 1)
          filters4 = piece->pipe->dsc.filters | 0x03030303u;
        else
          filters4 = piece->pipe->dsc.filters | 0x0c0c0c0cu;

        lin_interpolate(tmp, in, &roo, &roi, filters4, xtrans);
      }
      else if(demosaicing_method == DT_IOP_DEMOSAIC_LMMSE)
      {
        if(gd->lmmse_gamma_in == NULL)
        {
          gd->lmmse_gamma_in = dt_alloc_align_float(65536);
          gd->lmmse_gamma_out = dt_alloc_align_float(65536);
#ifdef _OPENMP
    #pragma omp for
#endif
          for(int j = 0; j < 65536; j++)
          {
            const double x = (double)j / 65535.0;
            gd->lmmse_gamma_in[j]  = (x <= 0.001867) ? x * 17.0 : 1.044445 * exp(log(x) / 2.4) - 0.044445;
            gd->lmmse_gamma_out[j] = (x <= 0.031746) ? x / 17.0 : exp(log((x + 0.044445) / 1.044445) * 2.4);
          }
        }
        lmmse_demosaic(piece, tmp, in, &roo, &roi, piece->pipe->dsc.filters, data->lmmse_refine, gd->lmmse_gamma_in, gd->lmmse_gamma_out);
      }
      else if((demosaicing_method & ~DEMOSAIC_DUAL) != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg(tmp, in, &roo, &roi, piece->pipe->dsc.filters,
                     data->median_thrs); // wanted ppg or zoomed out a lot and quality is limited to 1
      else
        amaze_demosaic_RT(piece, in, tmp, &roi, &roo, piece->pipe->dsc.filters);

      if(!(img->flags & DT_IMAGE_4BAYER) && data->green_eq != DT_IOP_GREEN_EQ_NO) dt_free_align(in);
    }

    if(info)
    {
      const float mpixels = (roo.width * roo.height) / 1.0e6;
      dt_get_times(&end_time);
      const float tclock = end_time.clock - start_time.clock;
      const float uclock = end_time.user - start_time.user;
      fprintf(stderr," [demosaic] process CPU `%s' did %.2fmpix, %.4f secs (%.4f CPU), %.2f pix/us\n",
        method2string(demosaicing_method & ~DEMOSAIC_DUAL), mpixels, tclock, uclock, mpixels / tclock);
    }

    dt_dev_write_rawdetail_mask(piece, tmp, roi_in, DT_DEV_DETAIL_MASK_DEMOSAIC);

    if((demosaicing_method & DEMOSAIC_DUAL))
    {
      dual_demosaic(piece, tmp, pixels, &roo, &roi, piece->pipe->dsc.filters, xtrans, showmask, data->dual_thrs);
    }

    if(scaled)
    {
      roi = *roi_out;
      dt_iop_clip_and_zoom_roi((float *)o, tmp, &roi, &roo, roi.width, roo.width);
      dt_free_align(tmp);
    }
  }
  else
  {
    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
      dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f((float *)o, pixels, &roo, &roi, roo.width, roi.width);
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
       dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f((float *)o, pixels, &roo, &roi, roo.width, roi.width);
    else if(piece->pipe->dsc.filters == 9u)
      dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f((float *)o, pixels, &roo, &roi, roo.width, roi.width, xtrans);
    else
      dt_iop_clip_and_zoom_demosaic_half_size_f((float *)o, pixels, &roo, &roi, roo.width, roi.width,
                                                piece->pipe->dsc.filters);

    // this is used for preview pipes, currently there is now writing mask implemented
    // we just clear the mask data as we might have changed the preview downsampling
    dt_dev_clear_rawdetail_mask(piece->pipe);
  }
  if(data->color_smoothing)
    color_smoothing(o, roi_out, data->color_smoothing);
}

#ifdef HAVE_OPENCL
static int process_default_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                              cl_mem dev_out, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out, const int demosaicing_method)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;
  const dt_image_t *img = &self->dev->image_storage;

  const int devid = piece->pipe->devid;
  const int qual_flags = demosaic_qual_flags(piece, img, roi_out);

  cl_mem dev_aux = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_med = NULL;
  cl_mem dev_green_eq = NULL;
  cl_int err = -999;

  if(qual_flags & DEMOSAIC_FULL_SCALE)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    int width = roi_out->width;
    int height = roi_out->height;

    // green equilibration
    if(data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if(dev_green_eq == NULL) goto error;

      if(!green_equilibration_cl(self, piece, dev_in, dev_green_eq, roi_in))
        goto error;

      dev_in = dev_green_eq;
    }

    // need to reserve scaled auxiliary buffer or use dev_out
    if(scaled)
    {
      dev_aux = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(dev_aux == NULL) goto error;
      width = roi_in->width;
      height = roi_in->height;
    }
    else
      dev_aux = dev_out;

    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    {
      size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 1, sizeof(cl_mem), &dev_aux);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 3, sizeof(int), &height);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_passthrough_monochrome, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
    {
      size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 1, sizeof(cl_mem), &dev_aux);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 4, sizeof(int), (void *)&roi_in->x);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 5, sizeof(int), (void *)&roi_in->y);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 6, sizeof(uint32_t), (void *)&piece->pipe->dsc.filters);

      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_passthrough_color, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PPG)
    {
      dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(dev_tmp == NULL) goto error;

      {
        const int myborder = 3;
        // manage borders
        size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
        dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 0, sizeof(cl_mem), &dev_in);
        dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 1, sizeof(cl_mem), &dev_tmp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 2, sizeof(int), (void *)&width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 3, sizeof(int), (void *)&height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 4, sizeof(uint32_t), (void *)&piece->pipe->dsc.filters);
        dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 5, sizeof(int), (void *)&myborder);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_border_interpolate, sizes);
        if(err != CL_SUCCESS) goto error;
      }

      if(data->median_thrs > 0.0f)
      {
        dev_med = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
        if(dev_med == NULL) goto error;

        dt_opencl_local_buffer_t locopt
          = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                        .cellsize = 1 * sizeof(float), .overhead = 0,
                                        .sizex = 1 << 8, .sizey = 1 << 8 };

        if(!dt_opencl_local_buffer_opt(devid, gd->kernel_pre_median, &locopt))
        goto error;

        size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
        size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_med);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 2, sizeof(int), &width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 3, sizeof(int), &height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 4, sizeof(uint32_t),
                                 (void *)&piece->pipe->dsc.filters);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 5, sizeof(float), (void *)&data->median_thrs);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 6,
                             sizeof(float) * (locopt.sizex + 4) * (locopt.sizey + 4), NULL);
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_pre_median, sizes, local);
        if(err != CL_SUCCESS) goto error;
        dev_in = dev_aux;
      }
      else dev_med = dev_in;

      {
        dt_opencl_local_buffer_t locopt
          = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                        .cellsize = sizeof(float) * 1, .overhead = 0,
                                        .sizex = 1 << 8, .sizey = 1 << 8 };

        if(!dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_green, &locopt))
        goto error;

        size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
        size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_med);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_tmp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 2, sizeof(int), &width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 3, sizeof(int), &height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 4, sizeof(uint32_t),
                                 (void *)&piece->pipe->dsc.filters);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 5,
                             sizeof(float) * (locopt.sizex + 2*3) * (locopt.sizey + 2*3), NULL);

        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_ppg_green, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }

      {
        dt_opencl_local_buffer_t locopt
          = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                        .cellsize = 4 * sizeof(float), .overhead = 0,
                                        .sizex = 1 << 8, .sizey = 1 << 8 };

        if(!dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_redblue, &locopt))
        goto error;

        size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
        size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_tmp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_aux);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 2, sizeof(int), &width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 3, sizeof(int), &height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 4, sizeof(uint32_t),
                                 (void *)&piece->pipe->dsc.filters);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 5,
                             sizeof(float) * 4 * (locopt.sizex + 2) * (locopt.sizey + 2), NULL);

        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_ppg_redblue, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }
    }

    dt_dev_write_rawdetail_mask_cl(piece, dev_aux, roi_in, DT_DEV_DETAIL_MASK_DEMOSAIC);

    if(scaled)
    {
      // scale aux buffer to output buffer
      err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_aux, roi_out, roi_in);
      if(err != CL_SUCCESS) goto error;
    }
  }
  else
  {
    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    {
      // sample image:
      const int zero = 0;
      cl_mem dev_pix = dev_in;
      const int width = roi_out->width;
      const int height = roi_out->height;

      size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 0, sizeof(cl_mem), &dev_pix);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 4, sizeof(int), (void *)&zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 5, sizeof(int), (void *)&zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 6, sizeof(int),
                               (void *)&roi_in->width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 7, sizeof(int),
                               (void *)&roi_in->height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 8, sizeof(float),
                               (void *)&roi_out->scale);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 9, sizeof(uint32_t),
                               (void *)&piece->pipe->dsc.filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_passthrough_monochrome, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      // sample half-size image:
      const int zero = 0;
      cl_mem dev_pix = dev_in;
      const int width = roi_out->width;
      const int height = roi_out->height;

      size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 0, sizeof(cl_mem), &dev_pix);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 4, sizeof(int), (void *)&zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 5, sizeof(int), (void *)&zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 6, sizeof(int), (void *)&roi_in->width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 7, sizeof(int), (void *)&roi_in->height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 8, sizeof(float), (void *)&roi_out->scale);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 9, sizeof(uint32_t),
                               (void *)&piece->pipe->dsc.filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_half_size, sizes);
      if(err != CL_SUCCESS) goto error;
    }
  }

  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  if(dev_med != dev_in) dt_opencl_release_mem_object(dev_med);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_tmp);
  dev_aux = dev_green_eq = dev_tmp = dev_med = NULL;

  // color smoothing
  if(data->color_smoothing)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out, data->color_smoothing))
      goto error;
  }

  return TRUE;

error:
  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  if(dev_med != dev_in) dt_opencl_release_mem_object(dev_med);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_times_t start_time = { 0 }, end_time = { 0 };
  const gboolean info = ((darktable.unmuted & (DT_DEBUG_DEMOSAIC | DT_DEBUG_PERF)) && (piece->pipe->type == DT_DEV_PIXELPIPE_FULL));

  dt_dev_clear_rawdetail_mask(piece->pipe);

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  int demosaicing_method = data->demosaicing_method;

  gboolean showmask = FALSE;
  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL)
  {
    dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
    if(g) showmask = (g->visual_mask);
    // take care of passthru modes
    if(piece->pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU)
      demosaicing_method = (piece->pipe->dsc.filters != 9u) ? DT_IOP_DEMOSAIC_RCD : DT_IOP_DEMOSAIC_MARKESTEIJN;
    else if(piece->pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU_MONO)
      demosaicing_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  }

  const int qual_flags = demosaic_qual_flags(piece, &self->dev->image_storage, roi_out);
  cl_mem high_image = NULL;
  cl_mem low_image = NULL;
  cl_mem blend = NULL;
  cl_mem details = NULL;
  cl_mem dev_aux = NULL;
  const gboolean dual = ((demosaicing_method & DEMOSAIC_DUAL) && (qual_flags & DEMOSAIC_FULL_SCALE) && (data->dual_thrs > 0.0f));
  const int devid = piece->pipe->devid;
  gboolean retval = FALSE;

  if(info) dt_get_times(&start_time);

  if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME ||
     demosaicing_method == DT_IOP_DEMOSAIC_PPG ||
     demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR )
  {
    if(!process_default_cl(self, piece, dev_in, dev_out, roi_in, roi_out, demosaicing_method)) return FALSE;
  }
  else if((demosaicing_method & ~DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_RCD)
  {
    if(dual)
    {
      high_image = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(high_image == NULL) return FALSE;
      if(!process_rcd_cl(self, piece, dev_in, high_image, roi_in, roi_in, FALSE)) goto finish;
    }
    else
    {
     if(!process_rcd_cl(self, piece, dev_in, dev_out, roi_in, roi_out, TRUE)) return FALSE;
    }
  }
  else if(demosaicing_method ==  DT_IOP_DEMOSAIC_VNG4 || demosaicing_method == DT_IOP_DEMOSAIC_VNG)
  {
    if(!process_vng_cl(self, piece, dev_in, dev_out, roi_in, roi_out, TRUE, FALSE)) return FALSE;
  }
  else if((demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN || demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) &&
    !(qual_flags & DEMOSAIC_XTRANS_FULL))
  {
    if(!process_vng_cl(self, piece, dev_in, dev_out, roi_in, roi_out, TRUE, qual_flags & DEMOSAIC_ONLY_VNG_LINEAR)) return FALSE;
  }
  else if(((demosaicing_method & ~DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_MARKESTEIJN ) ||
          ((demosaicing_method & ~DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_MARKESTEIJN_3))
  {
    if(dual)
    {
      high_image = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(high_image == NULL) return FALSE;
      if(!process_markesteijn_cl(self, piece, dev_in, high_image, roi_in, roi_in, FALSE)) return FALSE;
    }
    else
    {
      if(!process_markesteijn_cl(self, piece, dev_in, dev_out, roi_in, roi_out, TRUE)) return FALSE;
    }
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] demosaicing method '%s' not yet supported by opencl code\n", method2string(demosaicing_method));
    return FALSE;
  }

  if(info)
  {
    const float mpixels = (roi_in->width * roi_in->height) / 1.0e6;
    dt_get_times(&end_time);
    const float tclock = end_time.clock - start_time.clock;
    const float uclock = end_time.user - start_time.user;
    fprintf(stderr," [demosaic] process GPU `%s' did %.2fmpix, %.4f secs (%.4f CPU), %.2f pix/us\n",
      method2string(demosaicing_method & ~DEMOSAIC_DUAL), mpixels, tclock, uclock, mpixels / tclock);
  }
  if(!dual)
  {
    retval = TRUE;
    goto finish;
  }

  // This is dual demosaicing only stuff
  const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

  int width = roi_out->width;
  int height = roi_out->height;
  // need to reserve scaled auxiliary buffer or use dev_out
  if(scaled)
  {
    dev_aux = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
    if(dev_aux == NULL) goto finish;
    width = roi_in->width;
    height = roi_in->height;
  }
  else
    dev_aux = dev_out;

  // here we have work to be done only for dual demosaicers
  blend = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  details = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  low_image = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if((blend == NULL) || (low_image == NULL) || (details == NULL)) goto finish;

  if(info) dt_get_times(&start_time);
  if(process_vng_cl(self, piece, dev_in, low_image, roi_in, roi_in, FALSE, FALSE))
  {
    if(!color_smoothing_cl(self, piece, low_image, low_image, roi_in, 2))
    {
      retval = FALSE;
      goto finish;
    }
    retval = dual_demosaic_cl(self, piece, details, blend, high_image, low_image, dev_aux, width, height, showmask);
  }

  if(info)
  {
    dt_get_times(&end_time);
    fprintf(stderr," [demosaic] GPU dual blending %.4f secs (%.4f CPU)\n", end_time.clock - start_time.clock, end_time.user - start_time.user);
  }

  if(scaled)
  {
    // scale aux buffer to output buffer
    const int err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_aux, roi_out, roi_in);
    if(err != CL_SUCCESS)
      retval = FALSE;
  }

  finish:
  dt_opencl_release_mem_object(high_image);
  dt_opencl_release_mem_object(low_image);
  dt_opencl_release_mem_object(details);
  dt_opencl_release_mem_object(blend);
  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  if(!retval) dt_control_log(_("[dual demosaic_cl] internal problem"));
  return retval;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  const float ioratio = (float)roi_out->width * roi_out->height / ((float)roi_in->width * roi_in->height);
  const float smooth = data->color_smoothing ? ioratio : 0.0f;
  const float greeneq
      = ((piece->pipe->dsc.filters != 9u) && (data->green_eq != DT_IOP_GREEN_EQ_NO)) ? 0.25f : 0.0f;
  const dt_iop_demosaic_method_t demosaicing_method = data->demosaicing_method & ~DEMOSAIC_DUAL;

  const int qual_flags = demosaic_qual_flags(piece, &self->dev->image_storage, roi_out);
  const int full_scale_demosaicing = qual_flags & DEMOSAIC_FULL_SCALE;

  // check if output buffer has same dimension as input buffer (thus avoiding one
  // additional temporary buffer)
  const int unscaled = (roi_out->width == roi_in->width && roi_out->height == roi_in->height);

  if((demosaicing_method == DT_IOP_DEMOSAIC_PPG) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_AMAZE))
  {
    // Bayer pattern with PPG, Passthrough or Amaze
    tiling->factor = 1.0f + ioratio;         // in + out

    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    else if(full_scale_demosaicing)
      tiling->factor += fmax(2.0f + greeneq, smooth);  // + tmp + aux + greeneq | + smooth
    else
      tiling->factor += smooth;                        // + smooth

    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = 5; // take care of border handling
  }
  else if(((demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN) ||
           (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ||
           (demosaicing_method == DT_IOP_DEMOSAIC_FDC)) &&
          (qual_flags & DEMOSAIC_XTRANS_FULL))
  {
    // X-Trans pattern full Markesteijn processing
    const int ndir = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 8 : 4;
    const int overlap = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 18 : 12;

    tiling->factor = 1.0f + ioratio;
    tiling->factor += ndir * 1.0f      // rgb
                      + ndir * 0.25f   // drv
                      + ndir * 0.125f  // homo + homosum
                      + 1.0f;          // aux

    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(1.0f + greeneq, smooth);
    else if(full_scale_demosaicing)
      tiling->factor += fmax(2.0f + greeneq, smooth);
    else
      tiling->factor += smooth;

    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = XTRANS_SNAPPER;
    tiling->yalign = XTRANS_SNAPPER;
    tiling->overlap = overlap;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_RCD)
  {
    tiling->factor = 1.0f + ioratio;
    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    else if(full_scale_demosaicing)
      tiling->factor += fmax(2.0f + greeneq, smooth);  // + tmp + aux + greeneq | + smooth
    else
      tiling->factor += smooth;                        // + smooth
    tiling->maxbuf = 1.0f;
    tiling->overhead = sizeof(float) * RCD_TILESIZE * RCD_TILESIZE * 8 * MAX(1, darktable.num_openmp_threads);
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = 10;
    tiling->factor_cl = tiling->factor + 3.0f;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_LMMSE)
  {
    tiling->factor = 1.0f + ioratio;
    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    else if(full_scale_demosaicing)
      tiling->factor += fmax(2.0f + greeneq, smooth);  // + tmp + aux + greeneq | + smooth
    else
      tiling->factor += smooth;                        // + smooth
    tiling->maxbuf = 1.0f;
    tiling->overhead = sizeof(float) * LMMSE_GRP * LMMSE_GRP * 6 * MAX(1, darktable.num_openmp_threads);
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = 10;
  }
  else
  {
    // VNG
    tiling->factor = 1.0f + ioratio;

    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(1.0f + greeneq, smooth);
    else if(full_scale_demosaicing)
      tiling->factor += fmax(2.0f + greeneq, smooth);
    else
      tiling->factor += smooth;

    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 6; // covering Bayer pattern for VNG4 as well as xtrans for VNG
    tiling->yalign = 6; // covering Bayer pattern for VNG4 as well as xtrans for VNG
    tiling->overlap = 6;
  }
  if(data->demosaicing_method & DEMOSAIC_DUAL)
  {
    // make sure VNG4 is also possible
    tiling->factor += 1.0f;
    tiling->xalign = MAX(6, tiling->xalign);
    tiling->yalign = MAX(6, tiling->yalign);
    tiling->overlap = MAX(6, tiling->overlap);
  }
  return;
}
#undef LMMSE_GRP

void init_global(dt_iop_module_so_t *module)
{
  const int program = 0; // from programs.conf
  dt_iop_demosaic_global_data_t *gd
      = (dt_iop_demosaic_global_data_t *)malloc(sizeof(dt_iop_demosaic_global_data_t));
  module->data = gd;
  gd->kernel_zoom_half_size = dt_opencl_create_kernel(program, "clip_and_zoom_demosaic_half_size");
  gd->kernel_ppg_green = dt_opencl_create_kernel(program, "ppg_demosaic_green");
  gd->kernel_green_eq_lavg = dt_opencl_create_kernel(program, "green_equilibration_lavg");
  gd->kernel_green_eq_favg_reduce_first = dt_opencl_create_kernel(program, "green_equilibration_favg_reduce_first");
  gd->kernel_green_eq_favg_reduce_second = dt_opencl_create_kernel(program, "green_equilibration_favg_reduce_second");
  gd->kernel_green_eq_favg_apply = dt_opencl_create_kernel(program, "green_equilibration_favg_apply");
  gd->kernel_pre_median = dt_opencl_create_kernel(program, "pre_median");
  gd->kernel_ppg_redblue = dt_opencl_create_kernel(program, "ppg_demosaic_redblue");
  gd->kernel_downsample = dt_opencl_create_kernel(program, "clip_and_zoom");
  gd->kernel_border_interpolate = dt_opencl_create_kernel(program, "border_interpolate");
  gd->kernel_color_smoothing = dt_opencl_create_kernel(program, "color_smoothing");

  const int other = 14; // from programs.conf
  gd->kernel_passthrough_monochrome = dt_opencl_create_kernel(other, "passthrough_monochrome");
  gd->kernel_passthrough_color = dt_opencl_create_kernel(other, "passthrough_color");
  gd->kernel_zoom_passthrough_monochrome = dt_opencl_create_kernel(other, "clip_and_zoom_demosaic_passthrough_monochrome");

  const int vng = 15; // from programs.conf
  gd->kernel_vng_border_interpolate = dt_opencl_create_kernel(vng, "vng_border_interpolate");
  gd->kernel_vng_lin_interpolate = dt_opencl_create_kernel(vng, "vng_lin_interpolate");
  gd->kernel_zoom_third_size = dt_opencl_create_kernel(vng, "clip_and_zoom_demosaic_third_size_xtrans");
  gd->kernel_vng_green_equilibrate = dt_opencl_create_kernel(vng, "vng_green_equilibrate");
  gd->kernel_vng_interpolate = dt_opencl_create_kernel(vng, "vng_interpolate");

  const int markesteijn = 16; // from programs.conf
  gd->kernel_markesteijn_initial_copy = dt_opencl_create_kernel(markesteijn, "markesteijn_initial_copy");
  gd->kernel_markesteijn_green_minmax = dt_opencl_create_kernel(markesteijn, "markesteijn_green_minmax");
  gd->kernel_markesteijn_interpolate_green = dt_opencl_create_kernel(markesteijn, "markesteijn_interpolate_green");
  gd->kernel_markesteijn_solitary_green = dt_opencl_create_kernel(markesteijn, "markesteijn_solitary_green");
  gd->kernel_markesteijn_recalculate_green = dt_opencl_create_kernel(markesteijn, "markesteijn_recalculate_green");
  gd->kernel_markesteijn_red_and_blue = dt_opencl_create_kernel(markesteijn, "markesteijn_red_and_blue");
  gd->kernel_markesteijn_interpolate_twoxtwo = dt_opencl_create_kernel(markesteijn, "markesteijn_interpolate_twoxtwo");
  gd->kernel_markesteijn_convert_yuv = dt_opencl_create_kernel(markesteijn, "markesteijn_convert_yuv");
  gd->kernel_markesteijn_differentiate = dt_opencl_create_kernel(markesteijn, "markesteijn_differentiate");
  gd->kernel_markesteijn_homo_threshold = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_threshold");
  gd->kernel_markesteijn_homo_set = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_set");
  gd->kernel_markesteijn_homo_sum = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_sum");
  gd->kernel_markesteijn_homo_max = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_max");
  gd->kernel_markesteijn_homo_max_corr = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_max_corr");
  gd->kernel_markesteijn_homo_quench = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_quench");
  gd->kernel_markesteijn_zero = dt_opencl_create_kernel(markesteijn, "markesteijn_zero");
  gd->kernel_markesteijn_accu = dt_opencl_create_kernel(markesteijn, "markesteijn_accu");
  gd->kernel_markesteijn_final = dt_opencl_create_kernel(markesteijn, "markesteijn_final");

  const int rcd = 31; // from programs.conf
  gd->kernel_rcd_populate = dt_opencl_create_kernel(rcd, "rcd_populate");
  gd->kernel_rcd_write_output = dt_opencl_create_kernel(rcd, "rcd_write_output");
  gd->kernel_rcd_step_1_1 = dt_opencl_create_kernel(rcd, "rcd_step_1_1");
  gd->kernel_rcd_step_1_2 = dt_opencl_create_kernel(rcd, "rcd_step_1_2");
  gd->kernel_rcd_step_2_1 = dt_opencl_create_kernel(rcd, "rcd_step_2_1");
  gd->kernel_rcd_step_3_1 = dt_opencl_create_kernel(rcd, "rcd_step_3_1");
  gd->kernel_rcd_step_4_1 = dt_opencl_create_kernel(rcd, "rcd_step_4_1");
  gd->kernel_rcd_step_4_2 = dt_opencl_create_kernel(rcd, "rcd_step_4_2");
  gd->kernel_rcd_step_5_1 = dt_opencl_create_kernel(rcd, "rcd_step_5_1");
  gd->kernel_rcd_step_5_2 = dt_opencl_create_kernel(rcd, "rcd_step_5_2");
  gd->kernel_rcd_border_redblue = dt_opencl_create_kernel(rcd, "rcd_border_redblue");
  gd->kernel_rcd_border_green = dt_opencl_create_kernel(rcd, "rcd_border_green");
  gd->kernel_write_blended_dual  = dt_opencl_create_kernel(rcd, "write_blended_dual");
  gd->lmmse_gamma_in = NULL;
  gd->lmmse_gamma_out = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_zoom_half_size);
  dt_opencl_free_kernel(gd->kernel_ppg_green);
  dt_opencl_free_kernel(gd->kernel_pre_median);
  dt_opencl_free_kernel(gd->kernel_green_eq_lavg);
  dt_opencl_free_kernel(gd->kernel_green_eq_favg_reduce_first);
  dt_opencl_free_kernel(gd->kernel_green_eq_favg_reduce_second);
  dt_opencl_free_kernel(gd->kernel_green_eq_favg_apply);
  dt_opencl_free_kernel(gd->kernel_ppg_redblue);
  dt_opencl_free_kernel(gd->kernel_downsample);
  dt_opencl_free_kernel(gd->kernel_border_interpolate);
  dt_opencl_free_kernel(gd->kernel_color_smoothing);
  dt_opencl_free_kernel(gd->kernel_passthrough_monochrome);
  dt_opencl_free_kernel(gd->kernel_passthrough_color);
  dt_opencl_free_kernel(gd->kernel_zoom_passthrough_monochrome);
  dt_opencl_free_kernel(gd->kernel_vng_border_interpolate);
  dt_opencl_free_kernel(gd->kernel_vng_lin_interpolate);
  dt_opencl_free_kernel(gd->kernel_zoom_third_size);
  dt_opencl_free_kernel(gd->kernel_vng_green_equilibrate);
  dt_opencl_free_kernel(gd->kernel_vng_interpolate);
  dt_opencl_free_kernel(gd->kernel_markesteijn_initial_copy);
  dt_opencl_free_kernel(gd->kernel_markesteijn_green_minmax);
  dt_opencl_free_kernel(gd->kernel_markesteijn_interpolate_green);
  dt_opencl_free_kernel(gd->kernel_markesteijn_solitary_green);
  dt_opencl_free_kernel(gd->kernel_markesteijn_recalculate_green);
  dt_opencl_free_kernel(gd->kernel_markesteijn_red_and_blue);
  dt_opencl_free_kernel(gd->kernel_markesteijn_interpolate_twoxtwo);
  dt_opencl_free_kernel(gd->kernel_markesteijn_convert_yuv);
  dt_opencl_free_kernel(gd->kernel_markesteijn_differentiate);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_threshold);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_set);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_sum);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_max);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_max_corr);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_quench);
  dt_opencl_free_kernel(gd->kernel_markesteijn_zero);
  dt_opencl_free_kernel(gd->kernel_markesteijn_accu);
  dt_opencl_free_kernel(gd->kernel_markesteijn_final);
  dt_opencl_free_kernel(gd->kernel_rcd_populate);
  dt_opencl_free_kernel(gd->kernel_rcd_write_output);
  dt_opencl_free_kernel(gd->kernel_rcd_step_1_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_1_2);
  dt_opencl_free_kernel(gd->kernel_rcd_step_2_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_3_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_4_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_4_2);
  dt_opencl_free_kernel(gd->kernel_rcd_step_5_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_5_2);
  dt_opencl_free_kernel(gd->kernel_rcd_border_redblue);
  dt_opencl_free_kernel(gd->kernel_rcd_border_green);
  dt_opencl_free_kernel(gd->kernel_write_blended_dual);
  dt_free_align(gd->lmmse_gamma_in);
  dt_free_align(gd->lmmse_gamma_out);
  free(module->data);
  module->data = NULL;
}


gboolean force_enable(struct dt_iop_module_t *self, const gboolean current_state)
{
  // This needs to be enabled for raw images, disabled for other images.
  // There is no messing around.

  const int is_raw = dt_image_is_raw(&self->dev->image_storage);
  gboolean enable = current_state;

  if(is_raw && !current_state)
  {
    enable = TRUE;
  }
  else if(!is_raw && current_state)
  {
    enable = FALSE;
  }
  return enable;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;

  d->green_eq = p->green_eq;
  d->color_smoothing = p->color_smoothing;
  d->median_thrs = p->median_thrs;
  d->dual_thrs = p->dual_thrs;
  d->lmmse_refine = p->lmmse_refine;
  dt_iop_demosaic_method_t use_method = p->demosaicing_method;
  const gboolean xmethod = use_method & DEMOSAIC_XTRANS;
  const gboolean bayer   = (self->dev->image_storage.buf_dsc.filters != 9u);

  if(bayer && xmethod)   use_method = DT_IOP_DEMOSAIC_RCD;
  if(!bayer && !xmethod) use_method = DT_IOP_DEMOSAIC_MARKESTEIJN;

  if(use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME || use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  if(use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR || use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  const gboolean passing = (use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME || use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR);

  if(!(use_method == DT_IOP_DEMOSAIC_PPG))
    d->median_thrs = 0.0f;

  if(passing)
  {
    d->green_eq = DT_IOP_GREEN_EQ_NO;
    d->color_smoothing = 0;
  }

  if(use_method & DEMOSAIC_DUAL)
    d->color_smoothing = 0;

  d->demosaicing_method = use_method;

  // OpenCL only supported by some of the demosaicing methods
  switch(d->demosaicing_method)
  {
    case DT_IOP_DEMOSAIC_PPG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_AMAZE:
      piece->process_cl_ready = 0;
      break;
    case DT_IOP_DEMOSAIC_VNG4:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_RCD:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_LMMSE:
      piece->process_cl_ready = 0;
      break;
    case DT_IOP_DEMOSAIC_RCD_VNG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_AMAZE_VNG:
      piece->process_cl_ready = 0;
      break;
    case DT_IOP_DEMOSAIC_MARKEST3_VNG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_VNG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN_3:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_FDC:
      piece->process_cl_ready = 0;
      break;
    default:
      piece->process_cl_ready = 0;
  }

  // green-equilibrate over full image excludes tiling
  // The details mask is written inside process, this does not allow tiling.
  if((d->green_eq == DT_IOP_GREEN_EQ_FULL
      || d->green_eq == DT_IOP_GREEN_EQ_BOTH)
     || ((use_method & DEMOSAIC_DUAL) && (d->dual_thrs > 0.0f))
     || (piece->pipe->want_detail_mask == (DT_DEV_DETAIL_MASK_REQUIRED | DT_DEV_DETAIL_MASK_DEMOSAIC)))
  {
    piece->process_tiling_ready = 0;
  }

  if(self->dev->image_storage.flags & DT_IMAGE_4BAYER)
  {
    // 4Bayer images not implemented in OpenCL yet
    piece->process_cl_ready = 0;

    // Get and store the matrix to go from camera to RGB for 4Bayer images
    if(!dt_colorspaces_conversion_matrices_rgb(self->dev->image_storage.adobe_XYZ_to_CAM,
                                               NULL, d->CAM_to_RGB,
                                               self->dev->image_storage.d65_color_matrix, NULL))
    {
      const char *camera = self->dev->image_storage.camera_makermodel;
      fprintf(stderr, "[colorspaces] `%s' color matrix not found for 4bayer image!\n", camera);
      dt_control_log(_("`%s' color matrix not found for 4bayer image!"), camera);
    }
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_demosaic_data_t));
  piece->data_size = sizeof(dt_iop_demosaic_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_demosaic_params_t *d = (dt_iop_demosaic_params_t *)module->default_params;

  if(dt_image_is_monochrome(&module->dev->image_storage))
    d->demosaicing_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  else if(module->dev->image_storage.buf_dsc.filters == 9u)
    d->demosaicing_method = DT_IOP_DEMOSAIC_MARKESTEIJN;
  else
    d->demosaicing_method = DT_IOP_DEMOSAIC_RCD;

  module->hide_enable_button = 1;

  module->default_enabled = dt_image_is_raw(&module->dev->image_storage);
  if(module->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(module->widget), module->default_enabled ? "raw" : "non_raw");
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;

  const gboolean bayer = (self->dev->image_storage.buf_dsc.filters != 9u);
  dt_iop_demosaic_method_t use_method = p->demosaicing_method;
  const gboolean xmethod = use_method & DEMOSAIC_XTRANS;

  if(bayer && xmethod)   use_method = DT_IOP_DEMOSAIC_RCD;
  if(!bayer && !xmethod) use_method = DT_IOP_DEMOSAIC_MARKESTEIJN;

  const gboolean isppg = (use_method == DT_IOP_DEMOSAIC_PPG);
  const gboolean isdual = (use_method & DEMOSAIC_DUAL);
  const gboolean islmmse = (use_method == DT_IOP_DEMOSAIC_LMMSE);
  const gboolean passing = ((use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
                            (use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR) ||
                            (use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX) ||
                            (use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX));

  gtk_widget_set_visible(g->demosaic_method_bayer, bayer);
  gtk_widget_set_visible(g->demosaic_method_xtrans, !bayer);
  if(bayer)
    dt_bauhaus_combobox_set_from_value(g->demosaic_method_bayer, p->demosaicing_method);
  else
    dt_bauhaus_combobox_set_from_value(g->demosaic_method_xtrans, p->demosaicing_method);

  gtk_widget_set_visible(g->median_thrs, bayer && isppg);
  gtk_widget_set_visible(g->greeneq, !passing);
  gtk_widget_set_visible(g->color_smoothing, !passing && !isdual);
  gtk_widget_set_visible(g->dual_thrs, isdual);
  gtk_widget_set_visible(g->lmmse_refine, islmmse);

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'w');
  int changed = img->flags & DT_IMAGE_MONOCHROME_BAYER;
  if((p->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
     (p->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX))
    img->flags |= DT_IMAGE_MONOCHROME_BAYER;
  else
    img->flags &= ~DT_IMAGE_MONOCHROME_BAYER;
  const int mask_bw = dt_image_monochrome_flags(img);
  changed ^= img->flags & DT_IMAGE_MONOCHROME_BAYER;
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
  if(changed)
  {
    dt_imageio_update_monochrome_workflow_tag(self->dev->image_storage.id, mask_bw);
    // only done if one of the passthrough monochrome demosaicers has changed
    // we reload the image so all modules respect this setting
    dt_dev_reload_image(self->dev, self->dev->image_storage.id);
  }
}
void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);

  g->visual_mask = FALSE;
  gui_changed(self, NULL, NULL);

  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "raw" : "non_raw");
}

static void _visualize_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;

  g->visual_mask = dt_bauhaus_widget_get_quad_active(quad);
  dt_dev_invalidate(self->dev);
  dt_dev_refresh_ui_images(self->dev);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  if(!in)
  {
    const gboolean was_dualmask = g->visual_mask;
    dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
    g->visual_mask = FALSE;
    if(was_dualmask) dt_dev_invalidate(self->dev);
    dt_dev_refresh_ui_images(self->dev);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = IOP_GUI_ALLOC(demosaic);

  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->demosaic_method_bayer = dt_bauhaus_combobox_from_params(self, "demosaicing_method");
  for(int i=0;i<7;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayer, 10);
  gtk_widget_set_tooltip_text(g->demosaic_method_bayer, _("Bayer sensor demosaicing method, PPG and RCD are fast, AMaZE and LMMSE are slow.\nLMMSE is suited best for high ISO images.\ndual demosaicers double processing time."));

  g->demosaic_method_xtrans = dt_bauhaus_combobox_from_params(self, "demosaicing_method");
  for(int i=0;i<10;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_xtrans, 0);
  gtk_widget_set_tooltip_text(g->demosaic_method_xtrans, _("X-Trans sensor demosaicing method, Markesteijn 3-pass and frequency domain chroma are slow.\ndual demosaicers double processing time."));

  g->median_thrs = dt_bauhaus_slider_from_params(self, "median_thrs");
  dt_bauhaus_slider_set_digits(g->median_thrs, 3);
  gtk_widget_set_tooltip_text(g->median_thrs, _("threshold for edge-aware median.\nset to 0.0 to switch off\n"
                                                "set to 1.0 to ignore edges"));

  g->dual_thrs = dt_bauhaus_slider_from_params(self, "dual_thrs");
  dt_bauhaus_slider_set_digits(g->dual_thrs, 2);
  gtk_widget_set_tooltip_text(g->dual_thrs, _("contrast threshold for dual demosaic.\nset to 0.0 for high frequency content\n"
                                                "set to 1.0 for flat content\ntoggle to visualize the mask"));
  dt_bauhaus_widget_set_quad_paint(g->dual_thrs, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->dual_thrs, TRUE);
  dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
  g_signal_connect(G_OBJECT(g->dual_thrs), "quad-pressed", G_CALLBACK(_visualize_callback), self);

  g->lmmse_refine = dt_bauhaus_combobox_from_params(self, "lmmse_refine");
  gtk_widget_set_tooltip_text(g->lmmse_refine, _("LMMSE refinement steps. the median steps average the output,\nrefine adds some recalculation of red & blue channels"));

  g->color_smoothing = dt_bauhaus_combobox_from_params(self, "color_smoothing");
  gtk_widget_set_tooltip_text(g->color_smoothing, _("how many color smoothing median steps after demosaicing"));

  g->greeneq = dt_bauhaus_combobox_from_params(self, "green_eq");
  gtk_widget_set_tooltip_text(g->greeneq, _("green channels matching method"));

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);

  GtkWidget *label_non_raw = dt_ui_label_new(_("not applicable"));
  gtk_widget_set_tooltip_text(label_non_raw, _("demosaicing is only used for color raw images"));

  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
