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

#include "control/jobs/control_jobs.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/gpx.h"
#include "common/history.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_dng.h"
#include "common/imageio_module.h"
#include "common/mipmap_cache.h"
#include "common/tags.h"
#include "common/undo.h"
#include "common/grouping.h"
#include "common/utility.h"
#include "common/datetime.h"
#include "control/conf.h"
#include "develop/imageop_math.h"

#include "common/selection.h"

#include "gui/gtk.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#ifndef _WIN32
#include <glob.h>
#endif
#ifdef __APPLE__
#include "osx/osx.h"
#endif
#ifdef _WIN32
#include "win/dtwin.h"
#endif

// Control of the collection updates during an import.  Start with a short interval to feel responsive,
// but use fairly infrequent updates for large imports to minimize overall time.
#define INIT_UPDATE_INTERVAL	2 //seconds
#define MAX_UPDATE_INTERVAL     3.0 //seconds
// How long (in seconds) between updates of the "importing N/M" progress indicator?  Should be relatively
// short to avoid the impression that the import has gotten stuck.  Setting this too low will impact the
// overall time for a large import.
#define PROGRESS_UPDATE_INTERVAL 1

typedef enum dt_job_type_t
{
  IMPORT = 0,
  EXPORT = 1,
}dt_job_type_t;

const char *job_type_string[2] = { "importing", "exporting" };


typedef struct dt_control_datetime_t
{
  GTimeSpan offset;
  char datetime[DT_DATETIME_LENGTH];
} dt_control_datetime_t;

typedef struct dt_control_gpx_apply_t
{
  gchar *filename;
  gchar *tz;
} dt_control_gpx_apply_t;

typedef struct dt_control_image_enumerator_t
{
  GList *index;
  int flag;
  gpointer data;
} dt_control_image_enumerator_t;

/* enumerator of images from filmroll */
static void dt_control_image_enumerator_job_film_init(dt_control_image_enumerator_t *t, int32_t filmid)
{
  sqlite3_stmt *stmt;
  /* get a list of images in filmroll */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM main.images WHERE film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int imgid = sqlite3_column_int(stmt, 0);
    t->index = g_list_append(t->index, GINT_TO_POINTER(imgid));
  }
  sqlite3_finalize(stmt);
}

static int32_t _generic_dt_control_fileop_images_job_run(dt_job_t *job,
                                                         int32_t (*fileop_callback)(const int32_t,
                                                                                    const int32_t),
                                                         const char *desc, const char *desc_pl)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  GList *t = params->index;
  const guint total = g_list_length(t);
  char message[512] = { 0 };
  double fraction = 0;
  gchar *newdir = (gchar *)params->data;

  g_snprintf(message, sizeof(message), ngettext(desc, desc_pl, total), total);
  dt_control_job_set_progress_message(job, message);

  // create new film roll for the destination directory
  dt_film_t new_film;
  const int32_t film_id = dt_film_new(&new_film, newdir);
  g_free(newdir);

  if(film_id <= 0)
  {
    dt_control_log(_("failed to create film roll for destination directory, aborting move.."));
    return -1;
  }

  gboolean completeSuccess = TRUE;
  while(t && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
  {
    completeSuccess &= (fileop_callback(GPOINTER_TO_INT(t->data), film_id) != -1);
    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }

  if(completeSuccess)
  {
    char collect[1024];
    snprintf(collect, sizeof(collect), "1:0:0:%s$", new_film.dirname);
    dt_collection_deserialize(collect);
  }
  dt_film_remove_empty();
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_copy(params->index));
  dt_control_queue_redraw_center();
  return 0;
}

static void *dt_control_image_enumerator_alloc()
{
  dt_control_image_enumerator_t *params = calloc(1, sizeof(dt_control_image_enumerator_t));
  if(!params) return NULL;
  return params;
}

static void dt_control_image_enumerator_cleanup(void *p)
{
  dt_control_image_enumerator_t *params = p;

  // TODO: fix the fucking mess in jobs because each one cleans up its own way
  g_list_free(params->index);
  params->index = NULL;

  //FIXME: we need to free params->data to avoid a memory leak, but doing so here causes memory corruption....
//  g_free(params->data);

  free(params);
}

typedef enum {PROGRESS_NONE, PROGRESS_SIMPLE, PROGRESS_CANCELLABLE} progress_type_t;

void _refresh_progress_counter(dt_job_t *job, const int elements, const int index, const dt_job_type_t job_type)
{
  gchar message[64] = { 0 };
  double fraction = 0.0f;
  fraction += 1.0f / (double) elements;

  gchar *type = g_strdup(job_type_string[job_type]);
  dt_capitalize_label(type);
  
  snprintf(message, sizeof(message), ngettext("%s %i/%i image", "%s %i/%i images", index), type, index, elements);
  dt_control_job_set_progress_message(job, message);
  dt_control_job_set_progress(job, fraction);
  g_free(type);
  g_usleep(100);
}

static dt_job_t *dt_control_generic_images_job_create(dt_job_execute_callback execute, const char *message,
                                                      int flag, gpointer data, progress_type_t progress_type,
                                                      gboolean only_visible)
{
  dt_job_t *job = dt_control_job_create(execute, "%s", message);
  if(!job) return NULL;
  dt_control_image_enumerator_t *params = dt_control_image_enumerator_alloc();
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  if(progress_type != PROGRESS_NONE)
    dt_control_job_add_progress(job, _(message), progress_type == PROGRESS_CANCELLABLE);
  params->index = dt_act_on_get_images(only_visible, TRUE, FALSE);

  dt_control_job_set_params(job, params, dt_control_image_enumerator_cleanup);

  params->flag = flag;
  params->data = data;
  return job;
}

static dt_job_t *dt_control_generic_image_job_create(dt_job_execute_callback execute, const char *message,
                                                     int flag, gpointer data, progress_type_t progress_type,
                                                     int imgid)
{
  dt_job_t *job = dt_control_job_create(execute, "%s", message);
  if(!job) return NULL;
  dt_control_image_enumerator_t *params = dt_control_image_enumerator_alloc();
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  if(progress_type != PROGRESS_NONE)
    dt_control_job_add_progress(job, _(message), progress_type == PROGRESS_CANCELLABLE);

  params->index = g_list_append(NULL, GINT_TO_POINTER(imgid));

  dt_control_job_set_params(job, params, dt_control_image_enumerator_cleanup);

  params->flag = flag;
  params->data = data;
  return job;
}

static int32_t dt_control_write_sidecar_files_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET write_timestamp = STRFTIME('%s', 'now') WHERE id = ?1", -1,
                              &stmt, NULL);
  while(t)
  {
    gboolean from_cache = FALSE;
    const int imgid = GPOINTER_TO_INT(t->data);
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, (int32_t)imgid, 'r');
    char dtfilename[PATH_MAX] = { 0 };
    dt_image_full_path(img->id,  dtfilename,  sizeof(dtfilename),  &from_cache, __FUNCTION__);
    dt_image_path_append_version(img->id, dtfilename, sizeof(dtfilename));
    g_strlcat(dtfilename, ".xmp", sizeof(dtfilename));
    if(!dt_exif_xmp_write(imgid, dtfilename))
    {
      // put the timestamp into db. this can't be done in exif.cc since that code gets called
      // for the copy exporter, too
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
    }
    dt_image_cache_read_release(darktable.image_cache, img);
    t = g_list_next(t);
  }
  sqlite3_finalize(stmt);
  return 0;
}

typedef struct dt_control_merge_hdr_t
{
  uint32_t first_imgid;
  uint32_t first_filter;
  uint8_t first_xtrans[6][6];

  float *pixels, *weight;

  int wd;
  int ht;
  dt_image_orientation_t orientation;

  float whitelevel;
  float epsw;
  dt_aligned_pixel_t wb_coeffs;
  float adobe_XYZ_to_CAM[4][3];
  char camera_makermodel[128];

  // 0 - ok; 1 - errors, abort
  gboolean abort;
} dt_control_merge_hdr_t;

typedef struct dt_control_merge_hdr_format_t
{
  dt_imageio_module_data_t parent;
  dt_control_merge_hdr_t *d;
} dt_control_merge_hdr_format_t;

static int dt_control_merge_hdr_bpp(dt_imageio_module_data_t *data)
{
  return 32;
}

static int dt_control_merge_hdr_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
}

static const char *dt_control_merge_hdr_mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

static float envelope(const float xx)
{
  const float x = CLAMPS(xx, 0.0f, 1.0f);
  // const float alpha = 2.0f;
  const float beta = 0.5f;
  if(x < beta)
  {
    // return 1.0f-fabsf(x/beta-1.0f)^2
    const float tmp = fabsf(x / beta - 1.0f);
    return 1.0f - tmp * tmp;
  }
  else
  {
    const float tmp1 = (1.0f - x) / (1.0f - beta);
    const float tmp2 = tmp1 * tmp1;
    const float tmp3 = tmp2 * tmp1;
    return 3.0f * tmp2 - 2.0f * tmp3;
  }
}

static int dt_control_merge_hdr_process(dt_imageio_module_data_t *datai, const char *filename,
                                        const void *const ivoid,
                                        dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                                        void *exif, int exif_len, int imgid, int num, int total,
                                        dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  dt_control_merge_hdr_format_t *data = (dt_control_merge_hdr_format_t *)datai;
  dt_control_merge_hdr_t *d = data->d;

  // just take a copy. also do it after blocking read, so filters will make sense.
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  const dt_image_t image = *img;
  dt_image_cache_read_release(darktable.image_cache, img);

  if(!d->pixels)
  {
    d->first_imgid = imgid;
    d->first_filter = image.buf_dsc.filters;
    // sensor layout is just passed on to be written to dng.
    // we offset it to the crop of the image here, so we don't
    // need to load in the FCxtrans dependency into the dng writer.
    // for some stupid reason the dng needs this layout wrt cropped
    // offsets, not globally.
    dt_iop_roi_t roi = {0};
    roi.x = image.crop_x;
    roi.y = image.crop_y;
    for(int j=0;j<6;j++)
      for(int i = 0; i < 6; i++) d->first_xtrans[j][i] = FCxtrans(j, i, &roi, image.buf_dsc.xtrans);
    d->pixels = calloc((size_t)datai->width * datai->height, sizeof(float));
    d->weight = calloc((size_t)datai->width * datai->height, sizeof(float));
    d->wd = datai->width;
    d->ht = datai->height;
    d->orientation = image.orientation;
    for(int i = 0; i < 3; i++)
      d->wb_coeffs[i] = image.wb_coeffs[i];
    for(int k=0; k<4; k++)
      for(int i=0; i<3; i++)
        d->adobe_XYZ_to_CAM[k][i] = image.adobe_XYZ_to_CAM[k][i];
  }

  if(image.buf_dsc.filters == 0u || image.buf_dsc.channels != 1 || image.buf_dsc.datatype != TYPE_UINT16)
  {
    dt_control_log(_("exposure bracketing only works on raw images."));
    d->abort = TRUE;
    return 1;
  }
  else if(datai->width != d->wd || datai->height != d->ht || d->first_filter != image.buf_dsc.filters
          || d->orientation != image.orientation)
  {
    dt_control_log(_("images have to be of same size and orientation!"));
    d->abort = TRUE;
    return 1;
  }

  // if no valid exif data can be found, assume peleng fisheye at f/16, 8mm, with half of the light lost in
  // the system => f/22
  const float eap = image.exif_aperture > 0.0f ? image.exif_aperture : 22.0f;
  const float efl = image.exif_focal_length > 0.0f ? image.exif_focal_length : 8.0f;
  const float rad = .5f * efl / eap;
  const float aperture = M_PI * rad * rad;
  const float iso = image.exif_iso > 0.0f ? image.exif_iso : 100.0f;
  const float exp = image.exif_exposure > 0.0f ? image.exif_exposure : 1.0f;
  const float cal = 100.0f / (aperture * exp * iso);
  // about proportional to how many photons we can expect from this shot:
  const float photoncnt = 100.0f * aperture * exp / iso;
  float saturation = 1.0f;
  d->whitelevel = fmaxf(d->whitelevel, saturation * cal);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ivoid, cal, photoncnt) \
  shared(d, saturation) \
  schedule(static) collapse(2)
#endif
  for(int y = 0; y < d->ht; y++)
    for(int x = 0; x < d->wd; x++)
    {
      // read unclamped raw value with subtracted black and rescaled to 1.0 saturation.
      // this is the output of the rawprepare iop.
      const float in = ((float *)ivoid)[x + d->wd * y];
      // weights based on siggraph 12 poster
      // zijian zhu, zhengguo li, susanto rahardja, pasi fraenti
      // 2d denoising factor for high dynamic range imaging
      float w = photoncnt;

      // need some safety margin due to upsampling and 16-bit quantization + dithering?
      float offset = 3000.0f / (float)UINT16_MAX;

      // cannot do an envelope based on single pixel values here, need to get
      // maximum value of all color channels. to find that, go through the
      // pattern block (we conservatively do a 3x3 for bayer or xtrans):
      int xx = x & ~1, yy = y & ~1;
      float M = 0.0f, m = FLT_MAX;
      if(xx < d->wd - 2 && yy < d->ht - 2)
      {
        for(int i = 0; i < 3; i++)
          for(int j = 0; j < 3; j++)
          {
            M = MAX(M, ((float *)ivoid)[xx + i + d->wd * (yy + j)]);
            m = MIN(m, ((float *)ivoid)[xx + i + d->wd * (yy + j)]);
          }
        // move envelope a little to allow non-zero weight even for clipped regions.
        // this is because even if the 2x2 block is clipped somewhere, the other channels
        // might still prove useful. we'll check for individual channel saturation below.
        w *= d->epsw + envelope((M + offset) / saturation);
      }

      if(M + offset >= saturation)
      {
        if(d->weight[x + d->wd * y] <= 0.0f)
        { // only consider saturated pixels in case we have nothing better:
          if(d->weight[x + d->wd * y] == 0 || m < -d->weight[x + d->wd * y])
          {
            if(m + offset >= saturation)
              d->pixels[x + d->wd * y] = 1.0f; // let's admit we were completely clipped, too
            else
              d->pixels[x + d->wd * y] = in * cal / d->whitelevel;
            d->weight[x + d->wd * y]
                = -m; // could use -cal here, but m is per pixel and safer for varying illumination conditions
          }
        }
        // else silently ignore, others have filled in a better color here already
      }
      else
      {
        if(d->weight[x + d->wd * y] <= 0.0)
        { // cleanup potentially blown highlights from earlier images
          d->pixels[x + d->wd * y] = 0.0f;
          d->weight[x + d->wd * y] = 0.0f;
        }
        d->pixels[x + d->wd * y] += w * in * cal;
        d->weight[x + d->wd * y] += w;
      }
    }

  return 0;
}

static int32_t dt_control_merge_hdr_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  const guint total = g_list_length(t);
  char message[512] = { 0 };
  double fraction = 0;
  snprintf(message, sizeof(message), ngettext("merging %d image", "merging %d images", total), total);

  dt_control_job_set_progress_message(job, message);

  dt_control_merge_hdr_t d = (dt_control_merge_hdr_t){.epsw = 1e-8f, .abort = FALSE };

  dt_imageio_module_format_t buf = (dt_imageio_module_format_t){.mime = dt_control_merge_hdr_mime,
                                                                .levels = dt_control_merge_hdr_levels,
                                                                .bpp = dt_control_merge_hdr_bpp,
                                                                .write_image = dt_control_merge_hdr_process };

  dt_control_merge_hdr_format_t dat = (dt_control_merge_hdr_format_t){.parent = { 0 }, .d = &d };

  int num = 1;
  while(t)
  {
    if(d.abort) goto end;

    const uint32_t imgid = GPOINTER_TO_INT(t->data);

    const gboolean is_scaling =
      dt_conf_is_equal("plugins/lighttable/export/resizing", "scaling");

    dt_imageio_export_with_flags(imgid, "unused", &buf, (dt_imageio_module_data_t *)&dat, TRUE, FALSE, FALSE, is_scaling,
                                 FALSE, "pre:rawprepare", FALSE, FALSE, DT_COLORSPACE_NONE, NULL, DT_INTENT_LAST, NULL,
                                 NULL, num, total, NULL);

    t = g_list_next(t);

    /* update the progress bar */
    fraction += 1.0 / (total + 1);
    dt_control_job_set_progress(job, fraction);
    num++;
  }

  if(d.abort) goto end;

// normalize by white level to make clipping at 1.0 work as expected

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(d)
#endif
  for(size_t k = 0; k < (size_t)d.wd * d.ht; k++)
  {
    if(d.weight[k] > 0.0) d.pixels[k] = fmaxf(0.0f, d.pixels[k] / (d.whitelevel * d.weight[k]));
  }

  // output hdr as digital negative with exif data.
  uint8_t *exif = NULL;
  char pathname[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(d.first_imgid,  pathname,  sizeof(pathname),  &from_cache, __FUNCTION__);

  // last param is dng mode
  const int exif_len = dt_exif_read_blob(&exif, pathname, d.first_imgid, 0, d.wd, d.ht, 1);
  char *c = pathname + strlen(pathname);
  while(*c != '.' && c > pathname) c--;
  g_strlcpy(c, "-hdr.dng", sizeof(pathname) - (c - pathname));
  dt_imageio_write_dng(pathname,
                       d.pixels,
                       d.wd,
                       d.ht,
                       exif,
                       exif_len,
                       d.first_filter,
                       (const uint8_t (*)[6])d.first_xtrans,
                       1.0f,
                       (const float (*))d.wb_coeffs,
                       d.adobe_XYZ_to_CAM);
  free(exif);

  dt_control_job_set_progress(job, 1.0);

  while(*c != '/' && c > pathname) c--;
  dt_control_log(_("wrote merged HDR `%s'"), c + 1);

  // import new image
  gchar *directory = g_path_get_dirname((const gchar *)pathname);
  dt_film_t film;
  const int filmid = dt_film_new(&film, directory);
  const uint32_t imageid = dt_image_import(filmid, pathname, TRUE);
  g_free(directory);

  // refresh the thumbtable view
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_prepend(NULL, GINT_TO_POINTER(imageid)));
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();

end:
  free(d.pixels);
  free(d.weight);

  return 0;
}

static int32_t dt_control_duplicate_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  const guint total = g_list_length(t);
  double fraction = 0.0f;
  char message[512] = { 0 };

  dt_undo_start_group(darktable.undo, DT_UNDO_DUPLICATE);

  snprintf(message, sizeof(message), ngettext("duplicating %d image", "duplicating %d images", total), total);
  dt_control_job_set_progress_message(job, message);
  while(t)
  {
    const int imgid = GPOINTER_TO_INT(t->data);
    const int newimgid = dt_image_duplicate(imgid);
    if(newimgid != -1)
    {
      if(GPOINTER_TO_INT(params->data))
        dt_history_delete_on_image(newimgid);
      else
        dt_history_copy_and_paste_on_image(imgid, newimgid, FALSE, NULL, TRUE, TRUE);

      // a duplicate should keep the change time stamp of the original
      dt_image_cache_set_change_timestamp_from_image(darktable.image_cache, newimgid, imgid);

      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
    }
    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }

  dt_undo_end_group(darktable.undo);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}

static int32_t dt_control_flip_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  const int cw = params->flag;
  GList *t = params->index;
  const guint total = g_list_length(t);
  double fraction = 0.0f;
  char message[512] = { 0 };

  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);

  snprintf(message, sizeof(message), ngettext("flipping %d image", "flipping %d images", total), total);
  dt_control_job_set_progress_message(job, message);
  while(t)
  {
    const int imgid = GPOINTER_TO_INT(t->data);
    dt_image_flip(imgid, cw);
    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_image_set_aspect_ratio(imgid, FALSE);
    dt_control_job_set_progress(job, fraction);
  }

  dt_undo_end_group(darktable.undo);

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_ASPECT_RATIO,
                             g_list_copy(params->index));
  dt_control_queue_redraw_center();
  return 0;
}
static int32_t dt_control_monochrome_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  const int32_t mode = params->flag;
  GList *t = params->index;
  const guint total = g_list_length(t);
  char message[512] = { 0 };
  double fraction = 0.0f;

  dt_undo_start_group(darktable.undo, DT_UNDO_FLAGS);

  if(mode == 0)
    snprintf(message, sizeof(message), ngettext("set %d color image", "setting %d color images", total), total);
  else
    snprintf(message, sizeof(message), ngettext("set %d monochrome image", "setting %d monochrome images", total), total);

  dt_control_job_set_progress_message(job, message);
  while(t)
  {
    const int imgid = GPOINTER_TO_INT(t->data);

    if(imgid >= 0)
    {
      dt_image_set_monochrome_flag(imgid, mode == 2);
    }
    else
      fprintf(stderr,"[dt_control_monochrome_images_job_run] got illegal imgid %i\n", imgid);

    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }

  dt_undo_end_group(darktable.undo);

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_copy(params->index));
  dt_control_queue_redraw_center();
  return 0;
}

static char *_get_image_list(GList *l)
{
  const guint size = g_list_length(l);
  char num[8];
  char *buffer = calloc(size, sizeof(num));
  gboolean first = TRUE;

  buffer[0] = '\0';

  while(l)
  {
    const int imgid = GPOINTER_TO_INT(l->data);
    snprintf(num, sizeof(num), "%s%6d", first ? "" : ",", imgid);
    g_strlcat(buffer, num, size * sizeof(num));
    l = g_list_next(l);
    first = FALSE;
  }
  return buffer;
}

static void _set_remove_flag(char *imgs)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET flags = (flags|?1) WHERE id IN (?2)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, DT_IMAGE_REMOVE);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgs, -1, SQLITE_STATIC);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static GList *_get_full_pathname(char *imgs)
{
  sqlite3_stmt *stmt = NULL;
  GList *list = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT DISTINCT folder || '" G_DIR_SEPARATOR_S "' || filename FROM "
                              "main.images i, main.film_rolls f "
                              "ON i.film_id = f.id WHERE i.id IN (?1)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, imgs, -1, SQLITE_STATIC);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    list = g_list_prepend(list, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(list);  // list was built in reverse order, so un-reverse it
}

static int32_t dt_control_remove_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  char *imgs = _get_image_list(t);
  const guint total = g_list_length(t);
  char message[512] = { 0 };
  snprintf(message, sizeof(message), ngettext("removing %d image", "removing %d images", total), total);
  dt_control_job_set_progress_message(job, message);
  sqlite3_stmt *stmt = NULL;

  // check that we can safely remove the image
  gboolean remove_ok = TRUE;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.images WHERE id IN (?2) AND flags&?1=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, DT_IMAGE_LOCAL_COPY);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgs, -1, SQLITE_STATIC);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int imgid = sqlite3_column_int(stmt, 0);
    if(!dt_image_safe_remove(imgid))
    {
      remove_ok = FALSE;
      break;
    }
  }
  sqlite3_finalize(stmt);

  if(!remove_ok)
  {
    dt_control_log(_("cannot remove local copy when the original file is not accessible."));
    free(imgs);
    return 0;
  }

  // update remove status
  _set_remove_flag(imgs);

  dt_collection_update(darktable.collection);

  // We need a list of files to regenerate .xmp files if there are duplicates
  GList *list = _get_full_pathname(imgs);

  free(imgs);

  double fraction = 0.0f;
  while(t)
  {
    int imgid = GPOINTER_TO_INT(t->data);
    dt_image_remove(imgid);
    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }

  while(list)
  {
    char *imgname = (char *)list->data;
    dt_image_synch_all_xmp(imgname);
    list = g_list_delete_link(list, list);
  }
  dt_film_remove_empty();
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_copy(params->index));
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();

  return 0;
}

typedef struct _dt_delete_modal_dialog_t
{
  int send_to_trash;
  const char *filename;
  const char *error_message;

  gint dialog_result;

  dt_pthread_mutex_t mutex;
  pthread_cond_t cond;
} _dt_delete_modal_dialog_t;

enum _dt_delete_status
{
  _DT_DELETE_STATUS_UNKNOWN = 0,
  _DT_DELETE_STATUS_OK_TO_REMOVE = 1,
  _DT_DELETE_STATUS_SKIP_FILE = 2,
  _DT_DELETE_STATUS_STOP_PROCESSING = 3
};

enum _dt_delete_dialog_choice
{
  _DT_DELETE_DIALOG_CHOICE_DELETE = 1,
  _DT_DELETE_DIALOG_CHOICE_DELETE_ALL = 2,
  _DT_DELETE_DIALOG_CHOICE_REMOVE = 3,
  _DT_DELETE_DIALOG_CHOICE_CONTINUE = 4,
  _DT_DELETE_DIALOG_CHOICE_STOP = 5
};

static gboolean _dt_delete_dialog_main_thread(gpointer user_data)
{
  _dt_delete_modal_dialog_t* modal_dialog = (_dt_delete_modal_dialog_t*)user_data;
  dt_pthread_mutex_lock(&modal_dialog->mutex);

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
      GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_NONE,
      modal_dialog->send_to_trash
        ? _("could not send %s to trash.%s%s")
        : _("could not physically delete %s.%s%s"),
      modal_dialog->filename,
      modal_dialog->error_message != NULL ? "\n" : "",
      modal_dialog->error_message != NULL ? modal_dialog->error_message : "");
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  if(modal_dialog->send_to_trash)
  {
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("physically delete"), _DT_DELETE_DIALOG_CHOICE_DELETE);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("physically delete all files"), _DT_DELETE_DIALOG_CHOICE_DELETE_ALL);
  }
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("only remove from the image library"), _DT_DELETE_DIALOG_CHOICE_REMOVE);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("skip to next file"), _DT_DELETE_DIALOG_CHOICE_CONTINUE);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("stop process"), _DT_DELETE_DIALOG_CHOICE_STOP);

  gtk_window_set_title(
      GTK_WINDOW(dialog),
      modal_dialog->send_to_trash
        ? _("trashing error")
        : _("deletion error"));
  modal_dialog->dialog_result = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  pthread_cond_signal(&modal_dialog->cond);

  dt_pthread_mutex_unlock(&modal_dialog->mutex);

  // Don't call again on next idle time
  return FALSE;
}

static gint _dt_delete_file_display_modal_dialog(int send_to_trash, const char *filename, const char *error_message)
{
  _dt_delete_modal_dialog_t modal_dialog;
  modal_dialog.send_to_trash = send_to_trash;
  modal_dialog.filename = filename;
  modal_dialog.error_message = error_message;

  modal_dialog.dialog_result = GTK_RESPONSE_NONE;

  dt_pthread_mutex_init(&modal_dialog.mutex, NULL);
  pthread_cond_init(&modal_dialog.cond, NULL);

  dt_pthread_mutex_lock(&modal_dialog.mutex);

  gdk_threads_add_idle(_dt_delete_dialog_main_thread, &modal_dialog);
  while (modal_dialog.dialog_result == GTK_RESPONSE_NONE)
    dt_pthread_cond_wait(&modal_dialog.cond, &modal_dialog.mutex);

  dt_pthread_mutex_unlock(&modal_dialog.mutex);
  dt_pthread_mutex_destroy(&modal_dialog.mutex);
  pthread_cond_destroy(&modal_dialog.cond);

  return modal_dialog.dialog_result;
}

static enum _dt_delete_status delete_file_from_disk(const char *filename, gboolean *delete_on_trash_error)
{
  enum _dt_delete_status delete_status = _DT_DELETE_STATUS_UNKNOWN;

  GFile *gfile = g_file_new_for_path(filename);
  int send_to_trash = dt_conf_get_bool("send_to_trash");

  while (delete_status == _DT_DELETE_STATUS_UNKNOWN)
  {
    gboolean delete_success = FALSE;
    GError *gerror = NULL;
    if(send_to_trash)
    {
#ifdef __APPLE__
      delete_success = dt_osx_file_trash(filename, &gerror);
#elif defined(_WIN32)
      delete_success = dt_win_file_trash(gfile, NULL /*cancellable*/, &gerror);
#else
      delete_success = g_file_trash(gfile, NULL /*cancellable*/, &gerror);
#endif
    }
    else
    {
      delete_success = g_file_delete(gfile, NULL /*cancellable*/, &gerror);
    }

    // Delete is a success or the file does not exists: OK to remove from darktable
    if(delete_success
        || g_error_matches(gerror, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      delete_status = _DT_DELETE_STATUS_OK_TO_REMOVE;
    }
    else if(send_to_trash && *delete_on_trash_error)
    {
      // Loop again, this time delete instead of trashing
      delete_status = _DT_DELETE_STATUS_UNKNOWN;
      send_to_trash = FALSE;
    }
    else
    {
      const char *filename_display = NULL;
      GFileInfo *gfileinfo = g_file_query_info(
          gfile,
          G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
          G_FILE_QUERY_INFO_NONE,
          NULL /*cancellable*/,
          NULL /*error*/);
      if(gfileinfo != NULL)
        filename_display = g_file_info_get_attribute_string(
            gfileinfo,
            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);

      gint res = _dt_delete_file_display_modal_dialog(
          send_to_trash,
          filename_display == NULL ? filename : filename_display,
          gerror == NULL ? NULL : gerror->message);
      g_object_unref(gfileinfo);
      if(send_to_trash && res == _DT_DELETE_DIALOG_CHOICE_DELETE)
      {
        // Loop again, this time delete instead of trashing
        delete_status = _DT_DELETE_STATUS_UNKNOWN;
        send_to_trash = FALSE;
      }
      else if(send_to_trash && res == _DT_DELETE_DIALOG_CHOICE_DELETE_ALL)
      {
        // Loop again, this time delete instead of trashing
        delete_status = _DT_DELETE_STATUS_UNKNOWN;
        send_to_trash = FALSE;
        *delete_on_trash_error = TRUE;
      }
      else if(res == _DT_DELETE_DIALOG_CHOICE_REMOVE)
      {
        delete_status = _DT_DELETE_STATUS_OK_TO_REMOVE;
      }
      else if(res == _DT_DELETE_DIALOG_CHOICE_CONTINUE)
      {
        delete_status = _DT_DELETE_STATUS_SKIP_FILE;
      }
      else
      {
        delete_status = _DT_DELETE_STATUS_STOP_PROCESSING;
      }
    }
    if(gerror != NULL)
      g_error_free(gerror);
  }

  if(gfile != NULL)
    g_object_unref(gfile);

  return delete_status;
}


static int32_t dt_control_delete_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  char *imgs = _get_image_list(t);
  char imgidstr[25] = { 0 };
  const guint total = g_list_length(t);
  double fraction = 0.0f;
  char message[512] = { 0 };
  gboolean delete_on_trash_error = FALSE;
  if(dt_conf_get_bool("send_to_trash"))
    snprintf(message, sizeof(message), ngettext("trashing %d image", "trashing %d images", total), total);
  else
    snprintf(message, sizeof(message), ngettext("deleting %d image", "deleting %d images", total), total);
  dt_control_job_set_progress_message(job, message);

  sqlite3_stmt *stmt;

  dt_collection_update(darktable.collection);

  // We need a list of files to regenerate .xmp files if there are duplicates
  GList *list = _get_full_pathname(imgs);

  free(imgs);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.images WHERE filename IN (SELECT filename FROM "
                              "main.images WHERE id = ?1) AND film_id IN (SELECT film_id FROM main.images WHERE "
                              "id = ?1)", -1, &stmt, NULL);
  while(t)
  {
    enum _dt_delete_status delete_status = _DT_DELETE_STATUS_UNKNOWN;
    const int imgid = GPOINTER_TO_INT(t->data);
    char filename[PATH_MAX] = { 0 };
    gboolean from_cache = FALSE;
    dt_image_full_path(imgid,  filename,  sizeof(filename),  &from_cache, __FUNCTION__);

#ifdef _WIN32
    char *dirname = g_path_get_dirname(filename);
#endif

    int duplicates = 0;
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW) duplicates = sqlite3_column_int(stmt, 0);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    // remove from disk:
    if(duplicates == 1)
    {
      // first check for local copies, never delete a file whose original file is not accessible
      if(dt_image_local_copy_reset(imgid))
        goto delete_next_file;

      snprintf(imgidstr, sizeof(imgidstr), "%d", imgid);
      _set_remove_flag(imgidstr);
      dt_image_remove(imgid);

      // there are no further duplicates so we can remove the source data file
      delete_status = delete_file_from_disk(filename, &delete_on_trash_error);
      if(delete_status != _DT_DELETE_STATUS_OK_TO_REMOVE)
        goto delete_next_file;

      // all sidecar files - including left-overs - can be deleted;
      // left-overs can result when previously duplicates have been REMOVED;
      // no need to keep them as the source data file is gone.

      GList *files = dt_image_find_duplicates(filename);

      for(GList *file_iter = files; file_iter; file_iter = g_list_next(file_iter))
      {
        delete_status = delete_file_from_disk(file_iter->data, &delete_on_trash_error);
        if(delete_status != _DT_DELETE_STATUS_OK_TO_REMOVE)
          break;
      }

      g_list_free_full(files, g_free);
    }
    else
    {
      // don't remove the actual source data if there are further duplicates using it;
      // just delete the xmp file of the duplicate selected.

      dt_image_path_append_version(imgid, filename, sizeof(filename));
      g_strlcat(filename, ".xmp", sizeof(filename));

      // remove image from db first ...
      snprintf(imgidstr, sizeof(imgidstr), "%d", imgid);
      _set_remove_flag(imgidstr);
      dt_image_remove(imgid);

      // ... and delete afterwards because removing will re-write the XMP
      delete_status = delete_file_from_disk(filename, &delete_on_trash_error);
    }

delete_next_file:
#ifdef _WIN32
    g_free(dirname);
#endif
    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
    if(delete_status == _DT_DELETE_STATUS_STOP_PROCESSING)
      break;
  }

  sqlite3_finalize(stmt);

  while(list)
  {
    char *imgname = (char *)list->data;
    dt_image_synch_all_xmp(imgname);
    list = g_list_delete_link(list, list);
  }
  g_list_free(list);
  dt_film_remove_empty();
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_copy(params->index));
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}

static int32_t dt_control_gpx_apply_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = dt_control_job_get_params(job);
  GList *t = params->index;
  struct dt_gpx_t *gpx = NULL;
  uint32_t cntr = 0;
  const dt_control_gpx_apply_t *d = params->data;
  const gchar *filename = d->filename;
  const gchar *tz = d->tz;
  /* do we have any selected images */
  if(!t) goto bail_out;

  /* try parse the gpx data */
  gpx = dt_gpx_new(filename);
  if(!gpx)
  {
    dt_control_log(_("failed to parse GPX file"));
    goto bail_out;
  }

  GTimeZone *tz_camera = (tz == NULL) ? g_time_zone_new_utc() : g_time_zone_new(tz);
  if(!tz_camera) goto bail_out;

  GList *imgs = NULL;
  GArray *gloc = g_array_new(FALSE, FALSE, sizeof(dt_image_geoloc_t));
  /* go thru each selected image and lookup location in gpx */
  do
  {
    dt_image_geoloc_t geoloc;
    int imgid = GPOINTER_TO_INT(t->data);

    /* get image */
    const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(!cimg) continue;

    GDateTime *exif_time = dt_datetime_img_to_gdatetime(cimg, tz_camera);

    /* release the lock */
    dt_image_cache_read_release(darktable.image_cache, cimg);
    if(!exif_time) continue;
    GDateTime *utc_time = g_date_time_to_timezone(exif_time, darktable.utc_tz);
    g_date_time_unref(exif_time);
    if(!utc_time) continue;

    /* only update image location if time is within gpx tack range */
    if(dt_gpx_get_location(gpx, utc_time, &geoloc))
    {
      // takes the option to include the grouped images
      GList *grps = dt_grouping_get_group_images(imgid);
      for(GList *grp = grps; grp; grp = g_list_next(grp))
      {
        imgs = g_list_prepend(imgs, grp->data);
        g_array_append_val(gloc, geoloc);
        cntr++;
      }
      g_list_free(grps);
    }
    g_date_time_unref(utc_time);
  } while((t = g_list_next(t)) != NULL);
  imgs = g_list_reverse(imgs);

  dt_image_set_images_locations(imgs, gloc, TRUE);

  dt_control_log(ngettext("applied matched GPX location onto %d image",
                          "applied matched GPX location onto %d images", cntr), cntr);

  g_time_zone_unref(tz_camera);
  dt_gpx_destroy(gpx);
  g_array_unref(gloc);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, imgs, 0);
  return 0;

bail_out:
  if(gpx) dt_gpx_destroy(gpx);

  return 1;
}

static int32_t dt_control_move_images_job_run(dt_job_t *job)
{
  return _generic_dt_control_fileop_images_job_run(job, &dt_image_move, _("moving %d image"),
                                                   _("moving %d images"));
}

static int32_t dt_control_copy_images_job_run(dt_job_t *job)
{
  return _generic_dt_control_fileop_images_job_run(job, &dt_image_copy, _("copying %d image"),
                                                   _("copying %d images"));
}

static int32_t dt_control_local_copy_images_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  GList *t = params->index;
  guint tagid = 0;
  const guint total = g_list_length(t);
  double fraction = 0;
  const gboolean is_copy = params->flag == 1;
  char message[512] = { 0 };

  if(is_copy)
    snprintf(message, sizeof(message),
             ngettext("creating local copy of %d image", "creating local copies of %d images", total), total);
  else
    snprintf(message, sizeof(message),
             ngettext("removing local copy of %d image", "removing local copies of %d images", total), total);

  dt_control_log("%s", message);
  dt_control_job_set_progress_message(job, message);

  dt_tag_new("darktable|local-copy", &tagid);

  gboolean tag_change = FALSE;
  while(t && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
  {
    const int imgid = GPOINTER_TO_INT(t->data);
    if(is_copy)
    {
      if(dt_image_local_copy_set(imgid) == 0)
      {
        if(dt_tag_attach(tagid, imgid, FALSE, FALSE)) tag_change = TRUE;
      }
    }
    else
    {
      if(dt_image_local_copy_reset(imgid) == 0)
      {
        if(dt_tag_detach(tagid, imgid, FALSE, FALSE)) tag_change = TRUE;
      }
    }
    t = g_list_next(t);

    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_LOCAL_COPY,
                             g_list_copy(params->index));
  if(tag_change) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}

static int32_t dt_control_refresh_exif_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  GList *t = params->index;
  GList *imgs = g_list_copy(t);
  const guint total = g_list_length(t);
  double fraction = 0.0f;
  char message[512] = { 0 };
  snprintf(message, sizeof(message), ngettext("refreshing info for %d image", "refreshing info for %d images", total), total);
  dt_control_job_set_progress_message(job, message);
  while(t)
  {
    const int imgid = GPOINTER_TO_INT(t->data);
    if(imgid >= 0)
    {
      gboolean from_cache = TRUE;
      char sourcefile[PATH_MAX];
      dt_image_full_path(imgid,  sourcefile,  sizeof(sourcefile),  &from_cache, __FUNCTION__);

      dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
      if(img)
      {
        // Overwrite EXIF flags with our own, to preserve user-defined star ratings over EXIF.
        // All the rest should be synced with EXIF.
        const uint32_t flags = img->flags;
        dt_exif_read(img, sourcefile);
        img->flags = flags;
        dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
      }
      else
        fprintf(stderr,"[dt_control_refresh_exif_run] couldn't dt_image_cache_get for imgid %i\n", imgid);

      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED);
    }
    else
      fprintf(stderr,"[dt_control_refresh_exif_run] illegal imgid %i\n", imgid);

    t = g_list_next(t);
    fraction += 1.0 / total;
    dt_control_job_set_progress(job, fraction);
  }
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_copy(params->index));
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, imgs);
  dt_control_queue_redraw_center();
  return 0;
}

void _get_max_dimensions(uint32_t *w, uint32_t *h, dt_control_export_t *data, dt_imageio_module_data_t *fdata)
{
  uint32_t fw, fh, sw, sh;
  fw = fh = sw = sh = 0;
  data->module_storage->dimension(data->module_storage, data->module_data, &sw, &sh);
  data->module_format->dimension(data->module_format, fdata, &fw, &fh);

  if(sw == 0 || fw == 0)
    *w = sw > fw ? sw : fw;
  else
    *w = sw < fw ? sw : fw;

  if(sh == 0 || fh == 0)
    *h = sh > fh ? sh : fh;
  else
    *h = sh < fh ? sh : fh;
}

dt_export_metadata_t _export_init_metadata(const dt_control_export_t *data)
{
  dt_export_metadata_t metadata;
  metadata.flags = 0;
  metadata.list = dt_util_str_to_glist("\1", data->metadata_export);
  if(metadata.list)
  {
    metadata.flags = (int32_t) strtol(metadata.list->data, NULL, 16);
    metadata.list = g_list_remove(metadata.list, metadata.list->data);
  }
  return metadata;
}

gboolean _init_module_storage(GList **param_index, dt_control_export_t *data)
{
  dt_imageio_module_format_t *module_format = data->module_format;
  dt_imageio_module_storage_t *module_storage = data->module_storage;
  dt_imageio_module_data_t *module_data = data->module_data;
  dt_imageio_module_data_t *fdata = data->module_format->get_params(data->module_format);
  
  if(module_storage->initialize_store)
  {
    if(module_storage->initialize_store(module_storage, module_data, &module_format, &fdata, param_index, data->high_quality))
    {
      // bail out, something went wrong
      return 0;
    }
    module_format->set_params(module_format, fdata, (int)module_format->params_size(module_format));
    module_storage->set_params(module_storage, module_data, (int)module_storage->params_size(module_storage));
  }
  return 1;
}

static int32_t _control_export_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  dt_control_export_t *data = (dt_control_export_t *)params->data;
  GList *param_index = params->index;
  dt_imageio_module_format_t *module_format = data->module_format;
  dt_imageio_module_storage_t *module_storage = data->module_storage;
  dt_imageio_module_data_t *module_data = data->module_data;
  g_assert(module_format);
  g_assert(module_storage);
  
  dt_imageio_module_data_t *fdata = data->module_format->get_params(data->module_format);
  dt_export_metadata_t metadata = _export_init_metadata(data);

  gboolean tag_change = FALSE;

  fprintf(stdout, "\nEXPORT VAL:\n"
                  "Total img: %i\n"
                  "First imgID: %i\n"
                  "max_width: %i, max_height: %i\n"
                  "format_index: %i\n"
                  "storage_index: %i\n"
                  "high_quality: %i\n"
                  "export_masks: %i\n"
                  "style: %s\n"
                  "style_append: %i\n"
                  "icc_type: %i\n"
                  "icc_filename: %s\n"
                  "icc_intent: %i\n"
                  "metadata_export: %s\n\n",
                  data->total,
                  GPOINTER_TO_INT(data->imgid_list->data),
                  data->max_width, data->max_height,
                  data->format_index, data->storage_index,
                  data->high_quality, data->export_masks,
                  data->style, data->style_append,
                  data->icc_type, data->icc_filename, data->icc_intent,
                  data->metadata_export);

  // get a thread-safe fdata struct (one jpeg struct per thread etc):

  const guint total = g_list_length(data->imgid_list);


  if(!_init_module_storage(&param_index, data))
  {
    // bail out, something went wrong
    goto end;
  }
  
  // Get max dimensions...
  uint32_t img_width, img_height;
  _get_max_dimensions(&img_width, &img_height, data, fdata);

  // set up the fdata struct
  fdata->max_width = (data->max_width != 0 && img_width != 0) ? MIN(img_width, data->max_width) : MAX(img_width, data->max_width);
  fdata->max_height = (data->max_height != 0 && img_height != 0) ? MIN(img_height, data->max_height) : MAX(img_height, data->max_height);
  g_strlcpy(fdata->style, data->style, sizeof(fdata->style));
  fdata->style_append = data->style_append;
  // Invariant: the tagid for 'darktable|changed' will not change while this function runs. Is this a
  // sensible assumption?
  guint tagid = 0, etagid = 0;
  dt_tag_new("darktable|exported", &etagid);

  int index = 0;

  for(GList *img = g_list_first(data->imgid_list); img && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED; img = g_list_next(img))
  {
    const int imgid = GPOINTER_TO_INT(img->data);

    _refresh_progress_counter(job, total, index, EXPORT);
    
    // remove 'changed' tag from image
    if(dt_tag_detach(tagid, imgid, FALSE, FALSE)) tag_change = TRUE;
    // make sure the 'exported' tag is set on the image
    if(dt_tag_attach(etagid, imgid, FALSE, FALSE)) tag_change = TRUE;

    /* register export timestamp in cache */
    dt_image_cache_set_export_timestamp(darktable.image_cache, imgid);

    // check if image still exists:
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(image)
    {
      char imgfilename[PATH_MAX] = { 0 };
      gboolean from_cache = TRUE;
      dt_image_full_path(image->id,  imgfilename,  sizeof(imgfilename),  &from_cache, __FUNCTION__);
      if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
      {
        dt_control_log(_("image `%s' is currently unavailable"), image->filename);
        fprintf(stderr, "image `%s' is currently unavailable\n", imgfilename);
        // dt_image_remove(imgid);
        dt_image_cache_read_release(darktable.image_cache, image);
      }
      else
      {
        dt_image_cache_read_release(darktable.image_cache, image);
        if(module_storage->store(module_storage, module_data, imgid, module_format, fdata, index, total, data->high_quality,
           data->export_masks, data->icc_type, data->icc_filename, data->icc_intent,
           &metadata) != 0)
          dt_control_job_cancel(job);
      }
    }
    index++;
  }

  g_list_free_full(metadata.list, g_free);

  if(data->module_storage->finalize_store)
    data->module_storage->finalize_store(data->module_storage, data->module_data);

end:
  // all threads free their fdata
  data->module_format->free_params(data->module_format, fdata);

  // notify the user via the window manager
  dt_ui_notify_user();

  if(tag_change)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  fprintf(stdout,"end export run\n");
  return 0;
}

static dt_control_image_enumerator_t *dt_control_gpx_apply_alloc()
{
  dt_control_image_enumerator_t *params = dt_control_image_enumerator_alloc();
  if(!params) return NULL;

  params->data = calloc(1, sizeof(dt_control_gpx_apply_t));
  if(!params->data)
  {
    dt_control_image_enumerator_cleanup(params);
    return NULL;
  }

  return params;
}

static void dt_control_gpx_apply_job_cleanup(void *p)
{
  dt_control_image_enumerator_t *params = p;

  dt_control_gpx_apply_t *data = params->data;
  params->data = NULL;
  g_free(data->filename);
  g_free(data->tz);

  free(data);

  dt_control_image_enumerator_cleanup(params);
}

static dt_job_t *_control_gpx_apply_job_create(const gchar *filename, int32_t filmid,
                                               const gchar *tz, GList *imgs)
{
  dt_job_t *job = dt_control_job_create(&dt_control_gpx_apply_job_run, "gpx apply");
  if(!job) return NULL;
  dt_control_image_enumerator_t *params = dt_control_gpx_apply_alloc();
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  dt_control_job_set_params(job, params, dt_control_gpx_apply_job_cleanup);

  if(filmid != -1)
    dt_control_image_enumerator_job_film_init(params, filmid);
  else if(!imgs)
    params->index = dt_act_on_get_images(TRUE, TRUE, FALSE);
  else
    params->index = imgs;
  dt_control_gpx_apply_t *data = params->data;
  data->filename = g_strdup(filename);
  data->tz = g_strdup(tz);

  return job;
}

void dt_control_merge_hdr()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_merge_hdr_job_run, N_("merge hdr image"), 0,
                                                          NULL, PROGRESS_CANCELLABLE, TRUE));
}

void dt_control_gpx_apply(const gchar *filename, int32_t filmid, const gchar *tz, GList *imgs)
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     _control_gpx_apply_job_create(filename, filmid, tz, imgs));
}

void dt_control_duplicate_images(gboolean virgin)
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_duplicate_images_job_run,
                                                          N_("duplicate images"), 0, GINT_TO_POINTER(virgin), PROGRESS_SIMPLE, TRUE));
}

void dt_control_flip_images(const int32_t cw)
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_flip_images_job_run, N_("flip images"), cw,
                                                          NULL, PROGRESS_SIMPLE, TRUE));
}

void dt_control_monochrome_images(const int32_t mode)
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_monochrome_images_job_run, N_("set monochrome images"), mode,
                                                          NULL, PROGRESS_SIMPLE, TRUE));
}

gboolean dt_control_remove_images()
{
  // get all selected images now, to avoid the set changing during ui interaction
  dt_job_t *job = dt_control_generic_images_job_create(&dt_control_remove_images_job_run, N_("remove images"), 0,
                                                       NULL, PROGRESS_SIMPLE, FALSE);
  if(dt_conf_get_bool("ask_before_remove"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    const dt_control_image_enumerator_t *e = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
    const int number = g_list_length(e->index);
    if(number == 0)
    {
      dt_control_job_dispose(job);
      return TRUE;
    }

    dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        ngettext("Do you really want to remove %d image from Ansel library ?\nThe files will not be deleted on disk.",
                 "Do you really want to remove %d images from Ansel library ?\nThe files will not be deletetd on disk.", number),
        number);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), ngettext(_("Remove image from library ?"), _("Remove images from library ?"), number));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES)
    {
      dt_control_job_dispose(job);
      return FALSE;
    }
  }
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
  return TRUE;
}

void dt_control_delete_images()
{
  // first get all selected images, to avoid the set changing during ui interaction
  dt_job_t *job = dt_control_generic_images_job_create(&dt_control_delete_images_job_run, N_("delete images"), 0,
                                                       NULL, PROGRESS_SIMPLE, FALSE);
  if(dt_conf_get_bool("ask_before_delete"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    const dt_control_image_enumerator_t *e = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
    const int number = g_list_length(e->index);

    // Do not show the dialog if no image is selected:
    if(number == 0)
    {
      dt_control_job_dispose(job);
      return;
    }

    dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        ngettext("Do you really want to physically delete %d image ?\nThe system trash bin will be used if possible.",
                 "Do you really want to physically delete %d images ?\nThe system trash bin will be used if possible.", number),
        number);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), ngettext(_("Remove image from disk ?"), _("Remove images from disk ?"), number));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES)
    {
      dt_control_job_dispose(job);
      return;
    }
  }
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
}

void dt_control_delete_image(int imgid)
{
  // first get all selected images, to avoid the set changing during ui interaction
  dt_job_t *job = dt_control_generic_image_job_create(&dt_control_delete_images_job_run, N_("delete images"), 0,
                                                      NULL, PROGRESS_SIMPLE, imgid);
  int send_to_trash = dt_conf_get_bool("send_to_trash");
  if(dt_conf_get_bool("ask_before_delete"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    // Do not show the dialog if no valid image
    if(imgid < 1)
    {
      dt_control_job_dispose(job);
      return;
    }

    dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        send_to_trash ? _("do you really want to physically delete selected image (using trash if possible)?")
                      : _("do you really want to physically delete selected image from disk?"));
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), _("delete image?"));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES)
    {
      dt_control_job_dispose(job);
      return;
    }
  }
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
}

void dt_control_move_images()
{
  // Open file chooser dialog
  gchar *dir = NULL;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

  dt_job_t *job = dt_control_generic_images_job_create(&dt_control_move_images_job_run, N_("move images"), 0, dir,
                                                       PROGRESS_CANCELLABLE, FALSE);
  const dt_control_image_enumerator_t *e = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  const int number = g_list_length(e->index);
  if(number == 0)
  {
    dt_control_job_dispose(job);
    return;
  }

  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
        _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        _("_select as destination"), _("_cancel"));

  dt_conf_get_folder_to_file_chooser("ui_last/move_path", GTK_FILE_CHOOSER(filechooser));
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_folder_from_file_chooser("ui_last/move_path", GTK_FILE_CHOOSER(filechooser));
  }
  g_object_unref(filechooser);

  if(!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR)) goto abort;

  // ugly, but we need to set this after constructing the job:
  ((dt_control_image_enumerator_t *)dt_control_job_get_params(job))->data = dir;
  // the job's cleanup function is responsible for freeing dir, so we don't do that here

  if(dt_conf_get_bool("ask_before_move"))
  {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                               ngettext("do you really want to physically move %d image to %s?\n"
                                                        "(all duplicates will be moved along)",
                                                        "do you really want to physically move %d images to %s?\n"
                                                        "(all duplicates will be moved along)",
                                                        number),
                                               number, dir);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), ngettext("move image?", "move images?", number));

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if(res != GTK_RESPONSE_YES) goto abort;
  }

  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
  return;

abort:
  g_free(dir);
  dt_control_job_dispose(job);
}

void dt_control_copy_images()
{
  // Open file chooser dialog
  gchar *dir = NULL;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  dt_job_t *job = dt_control_generic_images_job_create(&dt_control_copy_images_job_run, N_("copy images"), 0, dir,
                                                       PROGRESS_CANCELLABLE, FALSE);
  const dt_control_image_enumerator_t *e = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  const int number = g_list_length(e->index);
  if(number == 0)
  {
    dt_control_job_dispose(job);
    return;
  }

  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
        _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        _("_select as destination"), _("_cancel"));

  dt_conf_get_folder_to_file_chooser("ui_last/copy_path", GTK_FILE_CHOOSER(filechooser));
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_folder_from_file_chooser("ui_last/copy_path", GTK_FILE_CHOOSER(filechooser));
  }
  g_object_unref(filechooser);

  if(!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR)) goto abort;

  // ugly, but we need to set this after constructing the job:
  ((dt_control_image_enumerator_t *)dt_control_job_get_params(job))->data = dir;
  // the job's cleanup function is responsible for freeing dir, so we don't do that here

  if(dt_conf_get_bool("ask_before_copy"))
  {
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        ngettext("do you really want to physically copy %d image to %s?",
                 "do you really want to physically copy %d images to %s?", number),
        number, dir);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), ngettext("copy image?", "copy images?", number));

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if(res != GTK_RESPONSE_YES) goto abort;
  }

  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
  return;

abort:
  g_free(dir);
  dt_control_job_dispose(job);
}

void dt_control_set_local_copy_images()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_local_copy_images_job_run,
                                                          N_("local copy images"), 1, NULL, PROGRESS_CANCELLABLE,
                                                          FALSE));
}

void dt_control_reset_local_copy_images()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_local_copy_images_job_run,
                                                          N_("local copy images"), 0, NULL, PROGRESS_CANCELLABLE,
                                                          FALSE));
}

void dt_control_refresh_exif()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_refresh_exif_run, N_("refresh exif"), 0,
                                                          NULL, PROGRESS_CANCELLABLE, FALSE));
}

static void _control_export_job_cleanup(void *p)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)p;
  dt_control_export_t *data = (dt_control_export_t *)params->data;
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage_by_index(data->storage_index);
  dt_imageio_module_data_t *sdata = data->module_data;
  
  mstorage->free_params(mstorage, sdata);

  g_free(data->style);
  g_free(data->icc_filename);
  g_free(data->metadata_export);

  g_list_free(data->imgid_list);
  free(data);
  dt_control_image_enumerator_cleanup(params);
}

static void _control_import_job_cleanup(void *p)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)p;
  dt_control_import_t *data = (dt_control_import_t *)params->data;

  // Display a recap of files that weren't copied
  if(g_list_length(g_list_last(data->discarded)) > 0)
  {
    // Create the window
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Message",
                                          GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                          GTK_DIALOG_DESTROY_WITH_PARENT,
                                          _("_OK"),
                                          GTK_RESPONSE_NONE,
                                          NULL);
    gtk_window_set_title(GTK_WINDOW(dialog), _("Some files have not been copied"));
    gtk_window_set_default_size(GTK_WINDOW(dialog), DT_PIXEL_APPLY_DPI(800), DT_PIXEL_APPLY_DPI(800));

    // Create the label
    GtkWidget *label = gtk_label_new(_("The following source files have not been copied "
                                       "because similarly-named files already exist on the destination. "
                                       "This may be because the files have already been imported "
                                       "or the naming pattern leads to non-unique file names."));
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);

    // Create the scrolled window internal container
    GtkWidget *scrolled_window = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled_window), TRUE);

    // Create the treeview model from the list of discarded file pathes
    GtkListStore *store = gtk_list_store_new(1, G_TYPE_STRING);
    GtkTreeIter iter;
    for(GList *file = g_list_first(data->discarded); file; file = g_list_next(file))
    {
      if(file->data)
      {
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, g_strdup((char *)file->data), -1);
      }
    }

    // Create the treeview view. Sooooo verbose... it's only a flat list.
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, _("Origin path"));
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, renderer, TRUE);
    gtk_tree_view_column_set_attributes(col, renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    g_object_unref(store);

    // Pack widgets to an unified box
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), scrolled_window, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(scrolled_window), view);

    // Pack the box to the dialog internal container
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_add(GTK_CONTAINER(content_area), box);
    gtk_widget_show_all(dialog);

#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  g_list_free_full(data->discarded, g_free);

  for(GList *img = g_list_first(data->imgs); img; img = g_list_next(img))
    free(img->data);

  free(data);
  dt_control_image_enumerator_cleanup(params);
}

static void *_control_import_export_alloc(dt_job_type_t job_type)
{
  dt_control_image_enumerator_t *params = dt_control_image_enumerator_alloc();
  if(!params) return NULL;

  if(job_type == IMPORT)
    params->data = g_malloc0(sizeof(dt_control_import_t));
  else if(job_type == EXPORT)
    params->data = g_malloc0(sizeof(dt_control_export_t));

  if(!params->data)
  {
    if(job_type == IMPORT)
      _control_import_job_cleanup(params);
    else if(job_type == EXPORT)
      _control_export_job_cleanup(params);

    return NULL;
  }
  return params;
}

/**
 * @brief 
 * @param data 
 * which contains:
 * @param imgid_list 
 * @param max_width 
 * @param max_height 
 * @param format_index 
 * @param storage_index 
 * @param high_quality 
 * @param upscale 
 * @param export_masks 
 * @param style 
 * @param style_append 
 * @param icc_type 
 * @param icc_filename 
 * @param icc_intent 
 * @param metadata_export 
 */
static dt_job_t *_control_export_job_create(dt_control_export_t data)
{
  dt_job_t *job = dt_control_job_create(&_control_export_job_run, "export");
  if(!job) return NULL;
  dt_control_image_enumerator_t *params = _control_import_export_alloc(EXPORT);
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  memcpy(params->data, &data, sizeof(dt_control_export_t));
  params->index = ((dt_control_export_t *)params->data)->imgid_list;
  dt_control_job_add_progress(job, _("export images"), TRUE);
  dt_control_job_set_params(job, params, _control_export_job_cleanup);
  fprintf(stdout,"end create export\n");
  return job;
}

void dt_control_export(dt_control_export_t data) 
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_EXPORT, _control_export_job_create(data));
  fprintf(stdout,"end dt_control_export\n");
}

static void _add_datetime_offset(const char *odt, const long int offset, char *ndt)
{
  // get the datetime_taken and calculate the new time
  GDateTime *datetime_original = dt_datetime_exif_to_gdatetime(odt, darktable.utc_tz);
  if(!datetime_original)
    return;

  // let's add our offset
  GDateTime *datetime_new = g_date_time_add(datetime_original, offset);
  g_date_time_unref(datetime_original);

  if(!datetime_new)
    return;
  gchar *datetime = g_date_time_format(datetime_new, "%Y:%m:%d %H:%M:%S,%f");
  datetime[DT_DATETIME_LENGTH - 1] = '\0';  // limit to milliseconds
  g_date_time_unref(datetime_new);

  if(datetime)
    g_strlcpy(ndt, datetime, DT_DATETIME_LENGTH);
  g_free(datetime);
}

static int32_t dt_control_datetime_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  uint32_t cntr = 0;
  GList *t = params->index;
  const GTimeSpan offset = ((dt_control_datetime_t *)params->data)->offset;
  const char *datetime = ((dt_control_datetime_t *)params->data)->datetime;
  char message[512] = { 0 };

  /* do we have any selected images and is offset != 0 */
  if(!t || (offset == 0 && !datetime[0]))
  {
    return 1;
  }

  const guint total = g_list_length(t);

  const char *mes11 = offset ? N_("adding time offset to %d image") : N_("setting date/time of %d image");
  const char *mes12 = offset ? N_("adding time offset to %d images") : N_("setting date/time of %d images");
  snprintf(message, sizeof(message), ngettext(mes11, mes12, total), total);
  dt_control_job_set_progress_message(job, message);

  GList *imgs = NULL;
  if(offset)
  {
    GArray *dtime = g_array_new(FALSE, TRUE, DT_DATETIME_LENGTH);

    for(GList *img = t; img; img = g_list_next(img))
    {
      char odt[DT_DATETIME_LENGTH] = {0};
      dt_image_get_datetime(GPOINTER_TO_INT(img->data), odt);
      if(!odt[0]) continue;

      char ndt[DT_DATETIME_LENGTH] = {0};
      _add_datetime_offset(odt, offset, ndt);
      if(!ndt[0]) continue;

      // takes the option to include the grouped images
      GList *grps = dt_grouping_get_group_images(GPOINTER_TO_INT(img->data));
      for(GList *grp = grps; grp; grp = g_list_next(grp))
      {
        imgs = g_list_prepend(imgs, grp->data);
        g_array_append_val(dtime, ndt);
        cntr++;
      }
      g_list_free(grps);
    }
    imgs = g_list_reverse(imgs);
    dt_image_set_datetimes(imgs, dtime, TRUE);
  }
  else
  {
    imgs = g_list_copy(t);
    // takes the option to include the grouped images
    dt_grouping_add_grouped_images(&imgs);
    cntr = g_list_length(imgs);
    dt_image_set_datetime(imgs, datetime, TRUE);
  }

  const char *mes21 = offset ? N_("added time offset to %d image") : N_("set date/time of %d image");
  const char *mes22 = offset ? N_("added time offset to %d images") : N_("set date/time of %d images");
  dt_control_log(ngettext(mes21, mes22, cntr), cntr);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, imgs);
  return 0;
}

static void *dt_control_datetime_alloc()
{
  dt_control_image_enumerator_t *params = dt_control_image_enumerator_alloc();
  if(!params) return NULL;

  params->data = calloc(1, sizeof(dt_control_datetime_t));
  if(!params->data)
  {
    dt_control_image_enumerator_cleanup(params);
    return NULL;
  }

  return params;
}

static void dt_control_datetime_job_cleanup(void *p)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)p;

  free(params->data);

  dt_control_image_enumerator_cleanup(params);
}

static dt_job_t *dt_control_datetime_job_create(const GTimeSpan offset, const char *datetime, GList *imgs)
{
  dt_job_t *job = dt_control_job_create(&dt_control_datetime_job_run, "time offset");
  if(!job) return NULL;
  dt_control_image_enumerator_t *params = dt_control_datetime_alloc();
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  dt_control_job_add_progress(job, _("time offset"), FALSE);
  dt_control_job_set_params(job, params, dt_control_datetime_job_cleanup);

  if(imgs)
    params->index = imgs;
  else
    params->index = dt_act_on_get_images(TRUE, TRUE, FALSE);

  dt_control_datetime_t *data = params->data;
  data->offset = offset;
  if(datetime)
    memcpy(data->datetime, datetime, sizeof(data->datetime));
  else
    data->datetime[0] = '\0';
  params->data = data;
  return job;
}

void dt_control_datetime(const GTimeSpan offset, const char *datetime, GList *imgs)
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_datetime_job_create(offset, datetime, imgs));
}

void dt_control_write_sidecar_files()
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_control_generic_images_job_create(&dt_control_write_sidecar_files_job_run,
                                                          N_("write sidecar files"), 0, NULL, PROGRESS_NONE,
                                                          FALSE));
}

/**
 * @brief Creates folders from path.
 * Returns TRUE if success.
 *
 * @param path a valid folders path to create.
 * @return gboolean
 */
gboolean create_dir(const char *path)
{
  if(g_mkdir_with_parents(path, 0755) == -1)
  {
    fprintf(stderr, "failed to create path %s.\n", path);
    return TRUE;
  }
  return FALSE;
}

/**
 * @brief Replaces separator depending of the current OS
 * and removes whitespaces.
 *
 * @param path
 * @return gchar*
 */
gchar *_path_cleanup(gchar *path_in)
{
  gchar *clean = dt_cleanup_separators(path_in);
  gchar *path_out = dt_util_remove_whitespace(clean);
  g_free(clean);
  return path_out;
}

gchar *dt_build_filename_from_pattern(const char *const filename, const int index, dt_image_t *img, dt_control_import_t *data)
{
  dt_variables_params_t *params;
  dt_variables_params_init(&params);
  params->filename = g_strdup(filename);
  params->sequence = index;
  params->jobcode = g_strdup(data->jobcode);
  params->imgid = -1;
  params->img = img;
  dt_variables_set_datetime(params, data->datetime);

  gchar *file_expand = dt_variables_expand(params, data->target_file_pattern, FALSE);
  gchar *path_expand = dt_variables_expand(params, data->target_subfolder_pattern, FALSE);

  // remove this if we decide to do the correction on user's settings directly
  gchar *file = _path_cleanup(file_expand);
  gchar *path = _path_cleanup(path_expand);
  g_free(file_expand);
  g_free(path_expand);
  fprintf(stdout, "+Dir: %s\n", data->target_folder);
  fprintf(stdout, "+Path: %s\n", path);
  fprintf(stdout, "+File: %s\n", file);

  gchar *dir = g_build_path(G_DIR_SEPARATOR_S, data->target_folder, path, (char *) NULL);
  data->target_dir = dt_util_normalize_path(dir);
  gchar *res = g_build_path(G_DIR_SEPARATOR_S, data->target_dir, file, (char *) NULL);

  g_free(file);
  g_free(path);
  g_free(dir);
  dt_variables_params_destroy(params);
  return res;
}

/**
 * @brief Tests if file exist. Returns 1 if so.
 *
 * @param dest_file_path
 * @return gboolean
 */
gboolean _file_exist(const char *dest_file_path)
{
  return dest_file_path != NULL && dest_file_path[0] && g_file_test(dest_file_path, G_FILE_TEST_EXISTS);
}

/**
 * @brief just create a folder. Returns 0 if success.
 *
 * @param target_dir
 * @return gboolean
 */
gboolean _create_folder(const char *target_dir)
{
  if(create_dir(target_dir))
  {
    fprintf(stdout, "Error while creating folder.\n");
    return 1;
  }

  fprintf(stdout, "Target folder(s) created successfully.\n");
  return 0;
}

/**
 * @brief Just copy a file. Returns 1 if success.
 *
 * @param filename
 * @param dest_file_path
 * @return gboolean
 */
gboolean _copy_file(const char *filename, const char *dest_file_path)
{
  GFile *in = g_file_new_for_path(filename);
  GFile *out = g_file_new_for_path(dest_file_path);

  gboolean res = g_file_copy(in, out, G_FILE_COPY_NONE, 0, 0, 0, NULL);
  if(res)
    fprintf(stdout, "File copied successfully.\n");
  else
    fprintf(stdout, "Error while copying file.\n");

  g_object_unref(in);
  g_object_unref(out);

  return res;
}

/**
 * @brief Add an image entry in the database and returns its imgID
 *
 * @param data informations from the import module
 * @param img_path_to_db the file path to import
 * @return const int32_t
 */
const int32_t _import_job(dt_control_import_t *data, gchar *img_path_to_db)
{
  fprintf(stdout, "::IMPORT FILE::\n%s to DB\n", img_path_to_db);

  dt_conf_set_int("ui_last/import_last_image", -1);
  gchar *dirname = g_strdup(dt_util_path_get_dirname(img_path_to_db));

  dt_film_t film;
  data->filmid = dt_film_new(&film, dirname);

  fprintf(stdout, "dirname: %s\tfilmid: %i\n", dirname, data->filmid);

  const int32_t imgid = dt_image_import(data->filmid, img_path_to_db, FALSE);
  g_free(dirname);
  return imgid;
}


/**
 * @brief Attempt to find a sidecar XMP file along an image file and import (copy) it to destination.
 * Warning: this does not support versions, so only the first version is imported.
 *
 * @param filename full path of original image file
 * @param dest_file_path full path of destination image file
 * @return int TRUE if XMP imported
 */
int _import_copy_xmp(const char *const filename, gchar *dest_file_path)
{
  char *xmp_source = g_strdup_printf("%s.xmp", dt_util_normalize_path(filename));
  char *xmp_destination = g_strdup_printf("%s.xmp", dt_util_normalize_path(dest_file_path));

  if(!xmp_source || !xmp_destination || !dt_util_test_image_file(xmp_source))
  {
    g_free(xmp_source);
    g_free(xmp_destination);
    return 0;
  }

  int success = _copy_file(xmp_source, xmp_destination);

  if(success)
    fprintf(stdout, "Sidecar XMP found and copied\n");
  else
    fprintf(stdout, "Sidecar XMP found but copy failed");

  g_free(xmp_source);
  g_free(xmp_destination);
  return success;
}

/**
 * @brief copy a file to a destination path after checking if everything is allright.
 *
 * @param params job informations.
 * @param data import module information.
 * @param img_path_to_db will be set to the file path for import.
 * @param pathname_len the `img_path_to_db` size.
 * @param discarded the list of file pathes discarded because the target already exists
 * @return int
 */
int _import_copy_file(const char *const filename, const int index, dt_control_import_t *data, gchar *img_path_to_db, size_t pathname_len, GList **discarded)
{
  dt_image_t *img = malloc(sizeof(dt_image_t));
  dt_image_init(img);

  // Generate file I/O only if the pattern is using EXIF variables.
  // Otherwise, discard it since it's really expensive if the file is on external/remote storage.
  // This is mandatory BEFORE expanding variables in pattern
  if(strstr(data->target_file_pattern, "$(EXIF") != NULL
    || strstr(data->target_subfolder_pattern, "$(EXIF") != NULL )
  {
    fprintf(stdout, "reading EXIF\n");
    dt_exif_read(img, filename);
  }

  gchar *dest_file_path = dt_build_filename_from_pattern(filename, index, img, data);
  fprintf(stdout, "Pattern to path: %s\n", dest_file_path);
  free(img);

  int process = TRUE;

  if(!_file_exist(dest_file_path))
  {
    if(!dt_util_dir_exist(data->target_dir))
      process = !_create_folder(data->target_dir); // _create_folder returns 0 when OK
    else
      fprintf(stdout, "The folder already exist.\n");

    if(process)
      process = dt_util_test_writable_dir(data->target_dir);
    else
      fprintf(stdout, "Unable to create the target folder.\n");

    if(process)
      process = _copy_file(filename, dest_file_path);
    else
      fprintf(stdout, "Not allowed to write in the folder.\n");

    if(process) _import_copy_xmp(filename, dest_file_path);

    if(process)
      g_strlcpy(img_path_to_db, dest_file_path, pathname_len);
    else
      fprintf(stderr, "Unable to copy the file.\n");
  }
  else
  {
    *discarded = g_list_prepend(*discarded, g_strdup(filename));
    g_strlcpy(img_path_to_db, dest_file_path, pathname_len);
    fprintf(stderr, "File copy skipped, the target file already exist.\n");
  }

  g_free(dest_file_path);
  return !process;
}

void _write_xmp_id(const char *filename, int32_t imgid)
{
  GList *res = dt_metadata_get(imgid, "Xmp.darktable.image_id", NULL);
  if(res != NULL)
  {
    // Image ID is already set in metadata, don't overwrite it
    g_list_free_full(res, g_free);
    return;
  }
  // else : init it
  GError *error = NULL;
  GFile *gfile = g_file_new_for_path(filename);
  GFileInfo *info = g_file_query_info(gfile,
                            G_FILE_ATTRIBUTE_STANDARD_NAME ","
                            G_FILE_ATTRIBUTE_TIME_MODIFIED,
                            G_FILE_QUERY_INFO_NONE, NULL, &error);
  const char *fn = g_file_info_get_name(info);

  const time_t datetime = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  char dt_txt[DT_DATETIME_EXIF_LENGTH];
  dt_datetime_unix_to_exif(dt_txt, sizeof(dt_txt), &datetime);
  const char *id = g_strconcat(fn, "-", dt_txt, NULL);
  dt_metadata_set(imgid, "Xmp.darktable.image_id", id, FALSE);
  g_object_unref(info);
  g_object_unref(gfile);
  g_clear_error(&error);
}

/**
 * @brief process to copy (or not) and import an image to database.
 *
 * @param img the current image.
 * @param data info from import module.
 * @param index current loop's index.
 * @return gboolean
 */
gboolean _import_image(const GList *img, dt_control_import_t *data, const int index, GList **discarded)
{
  const char *filename = (const char*) img->data;
  fprintf(stdout, "Filename: %s\n", filename);

  gchar img_path_to_db[PATH_MAX] = { 0 };
  gboolean process_error = 0;

  if(data->copy)
    // Copy the file to destination folder, expanding variables internally
    process_error = _import_copy_file(filename, index, data, img_path_to_db, sizeof(img_path_to_db), discarded);
  else
    // destination = origin, nothing to do
    g_strlcpy(img_path_to_db, filename, sizeof(img_path_to_db));

  process_error |= (*img_path_to_db == 0);

  if(process_error)
    fprintf(stdout, "Process Error\n");
  else
  {
    const int32_t imgid = _import_job(data, img_path_to_db);

    if(imgid == -1)
    {
      dt_control_log(_("Error importing file in collection (imgid: %i)."), imgid);
      process_error = 1;
    }
    else
    {
      _write_xmp_id(filename, imgid);
      fprintf(stdout, "imgid: %i\n", imgid);
      dt_conf_set_int("ui_last/import_last_image", imgid);
    }
  }

  fprintf(stdout, "::End of import_image.\n");

  return process_error;
}

static int32_t _control_import_job_run(dt_job_t *job)
{
  fprintf(stdout,"\n:::Control_Job_run:::\n");
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  dt_control_import_t *data = params->data;

  int index = 0;

  for(GList *img = g_list_first(data->imgs); img; img = g_list_next(img))
  {
    fprintf(stdout, "\nIMG %i.\n", index + 1);

    _refresh_progress_counter(job, data->elements, index, IMPORT);

    if(_import_image(img, data, index, &data->discarded))
      fprintf(stderr, "Skipping this one.\n");
    else
    {
      data->total_imported_elements += 1;
      fprintf(stdout, "N: %i\n", data->total_imported_elements);
    }
    index++;

    fprintf(stdout, "BOTTOM LOOP.\n\n");
  }

  if(data->total_imported_elements == 0 && data->filmid == -1)
  {
    dt_control_log(_("No image imported!"));
    fprintf(stderr, "No image imported!\n\n");
  }
  else if(data->filmid > -1)
  {
    dt_control_log(ngettext("imported %d image", "imported %d images", data->total_imported_elements), data->total_imported_elements);
    fprintf(stdout, "%d files imported in database.\n\n", data->total_imported_elements);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED, data->filmid); // refresh lighttable
  }

  fprintf(stdout,":::END OF IMPORT:::\n\n");

  dt_conf_set_int("ui_last/nb_imported", data->total_imported_elements);

  return data->total_imported_elements >= 1 ? 0 : 1;
}

static dt_job_t *_control_import_job_create(dt_control_import_t data)
{
  dt_job_t *job = dt_control_job_create(&_control_import_job_run, "import");
  if(!job) return NULL;
  dt_control_image_enumerator_t *params = _control_import_export_alloc(TRUE);
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  memcpy(params->data, &data, sizeof(dt_control_import_t));
  params->index = ((dt_control_import_t *)params->data)->imgs;
  dt_control_job_add_progress(job, _("import"), FALSE);
  dt_control_job_set_params(job, params, _control_import_job_cleanup);
  fprintf(stdout, "END Job create.\n");
  return job;
}

void dt_control_import(dt_control_import_t data)
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, _control_import_job_create(data));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
