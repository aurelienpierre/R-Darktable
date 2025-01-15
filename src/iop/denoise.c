/*
   This file is part of Ansel,
   Copyright (C) 2023 Aurélien Pierre.

   Ansel is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Ansel is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/extra_optimizations.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/bspline.h"
#include "common/darktable.h"
#include "common/dwt.h"
#include "common/gaussian.h"
#include "common/image.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/noiseprofiles.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/noise_generator.h"
#include "develop/openmp_maths.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"


DT_MODULE_INTROSPECTION(1, dt_iop_denoise_params_t)

#define MAX_NUM_SCALES 10
typedef struct dt_iop_denoise_params_t
{
  int iterations;                    // $MIN: 1    $MAX: 32   $DEFAULT: 5     $DESCRIPTION: "Iterations"
  int radius;                        // $MIN: 1    $MAX: 32   $DEFAULT: 12    $DESCRIPTION: "Radius"
  float denoise_RGB;                 // $MIN: 0.0  $MAX: 1.0  $DEFAULT: 0.05  $DESCRIPTION: "Denoise"
  float denoise_chroma;              // $MIN: 0.0  $MAX: 1.0  $DEFAULT: 1.0   $DESCRIPTION: "Denoise"
  float edges_threshold_RGB;         // $MIN: 0.0  $MAX: 15.0  $DEFAULT: 6.0   $DESCRIPTION: "Edges/noise threshold"
  float edges_threshold_chroma;      // $MIN: 0.0  $MAX: 15.0  $DEFAULT: 1.0   $DESCRIPTION: "Edges/noise threshold
  float edges_sensibility_RGB;       // $MIN: 0.0  $MAX: 15.0 $DEFAULT: 6.0   $DESCRIPTION: "Edges protection"
  float edges_sensibility_chroma;    // $MIN: 0.0  $MAX: 15.0 $DEFAULT: 0.0   $DESCRIPTION: "Edges protection"
  float sharpness;                   // $MIN: 0.0  $MAX: 1.0  $DEFAULT: 0.07  $DESCRIPTION: "Sharpness"
  float edges_sensibility_sharpness; // $MIN: 0.0  $MAX: 15.0 $DEFAULT: 6.   $DESCRIPTION: "Sharpness edges sensibility"
  float hot_pixels_threshold;        // $MIN: 0.0  $MAX: 15.0  $DEFAULT: 8.0   $DESCRIPTION: "Hot pixels threshold"
  dt_aligned_pixel_t a;
  dt_aligned_pixel_t b;
} dt_iop_denoise_params_t;


typedef struct dt_iop_denoise_gui_data_t
{
  GtkWidget *iterations, *radius, *denoise_RGB, *denoise_chroma, *edges_threshold_RGB, *edges_threshold_chroma,
      *edges_sensibility_RGB, *edges_sensibility_chroma, *sharpness, *edges_sensibility_sharpness, *hot_pixels,
      *profile;
} dt_iop_denoise_gui_data_t;


typedef enum diffuse_reconstruct_variant_t
{
  DIFFUSE_RECONSTRUCT_RGB = 0,
  DIFFUSE_RECONSTRUCT_CHROMA
} diffuse_reconstruct_variant_t;


const char *name()
{
  return _("Pixel cleaner");
}

const char *aliases()
{
  return _("diffusion|deconvolution|blur|sharpening");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("simulate directional diffusion of light with heat transfer model\n"
                                  "to apply an iterative edge-oriented blur,\n"
                                  "inpaint damaged parts of the image,"
                                  "or to remove blur with blind deconvolution."),
                                _("corrective and creative"),
                                _("linear, RGB, scene-referred"),
                                _("linear, RGB"),
                                _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void reload_defaults(dt_iop_module_t *module)
{
  // get matching profiles:
  GList *profiles = dt_noiseprofile_get_matching(&module->dev->image_storage);
  dt_noiseprofile_t interpolated = dt_noiseprofile_generic;

  const int iso = module->dev->image_storage.exif_iso;
  dt_noiseprofile_t *last = NULL;

  for(GList *iter = profiles; iter; iter = g_list_next(iter))
  {
    dt_noiseprofile_t *current = (dt_noiseprofile_t *)iter->data;

    if(current->iso == iso)
    {
      interpolated = *current;
      break;
    }
    if(last && last->iso < iso && current->iso > iso)
    {
      interpolated.iso = iso;
      dt_noiseprofile_interpolate(last, current, &interpolated);
      break;
    }
    last = current;
  }

  dt_iop_denoise_params_t *p = module->default_params;

  for(int k = 0; k < 4; k++)
  {
    p->a[k] = interpolated.a[k];
    p->b[k] = interpolated.b[k];
  }
}

// Discretization parameters for the Partial Derivative Equation solver
#define H 1         // spatial step
#define KAPPA 0.25f // 0.25 if h = 1, 1 if h = 2

enum wavelets_scale_t
{
  ANY_SCALE   = 1 << 0, // any wavelets scale   : reconstruct += HF
  FIRST_SCALE = 1 << 1, // first wavelets scale : reconstruct = 0
  LAST_SCALE  = 1 << 2, // last wavelets scale  : reconstruct += residual
};


static uint8_t scale_type(const int s, const int scales)
{
  uint8_t scale = ANY_SCALE;
  if(s == 0) scale |= FIRST_SCALE;
  if(s == scales - 1) scale |= LAST_SCALE;
  return scale;
}

static inline gboolean invert_matrix(const dt_colormatrix_t in, dt_colormatrix_t out)
{
  // use same notation as https://en.wikipedia.org/wiki/Invertible_matrix#Inversion_of_3_%C3%97_3_matrices
  const double biga =  (in[1][1] * in[2][2] - in[1][2] * in[2][1]);
  const double bigb = -(in[1][0] * in[2][2] - in[1][2] * in[2][0]);
  const double bigc =  (in[1][0] * in[2][1] - in[1][1] * in[2][0]);
  const double bigd = -(in[0][1] * in[2][2] - in[0][2] * in[2][1]);
  const double bige =  (in[0][0] * in[2][2] - in[0][2] * in[2][0]);
  const double bigf = -(in[0][0] * in[2][1] - in[0][1] * in[2][0]);
  const double bigg =  (in[0][1] * in[1][2] - in[0][2] * in[1][1]);
  const double bigh = -(in[0][0] * in[1][2] - in[0][2] * in[1][0]);
  const double bigi =  (in[0][0] * in[1][1] - in[0][1] * in[1][0]);

  const double det = in[0][0] * biga + in[0][1] * bigb + in[0][2] * bigc;
  if(fabs(det) < 1e-9) // empirical threshold for valid output
    return FALSE;

  out[0][0] = biga / det;
  out[0][1] = bigb / det;
  out[0][2] = bigc / det;
  out[0][3] = 0.0;
  out[1][0] = bigd / det;
  out[1][1] = bige / det;
  out[1][2] = bigf / det;
  out[1][3] = 0.0;
  out[2][0] = bigg / det;
  out[2][1] = bigh / det;
  out[2][2] = bigi / det;
  out[2][3] = 0.0;
  out[3][0] = 0.0;
  out[3][1] = 0.0;
  out[3][2] = 0.0;
  out[3][3] = 0.0;

  return TRUE;
}

#define FILTER_RADIUS 3
#define FILTER_WIDTH ((2 * FILTER_RADIUS) + 1)
#define FILTER_SIZE (FILTER_WIDTH * FILTER_WIDTH)

//#define DEBUG

#pragma omp declare simd uniform(in, width, height)
static inline void basic_guided_filter_3D(dt_aligned_pixel_t RGB, const float *const restrict in,
                                          const int i, const int j,
                                          const size_t width, const size_t height,
                                          const float threshold)
{
  // Get the neighbours in the filter window
  dt_aligned_pixel_t neighbours[FILTER_SIZE] = { { 0.f } };
  dt_aligned_pixel_t average = { 0.f };
  int DT_ALIGNED_ARRAY mask[FILTER_SIZE] = { 0 };
  int num_elem = 0;

  for(int ii = 0; ii < FILTER_WIDTH; ++ii)
  {
    const int index_v = i + ii - FILTER_RADIUS;
    if(index_v > -1 && index_v < height)
    {
      for(int jj = 0; jj < FILTER_WIDTH; ++jj)
      {
        const int index_h = j + jj - FILTER_RADIUS;
        if(index_h > -1 && index_h < width)
        {
          // Mark this pixel as counting for the average/variance
          mask[ii * FILTER_WIDTH + jj] = 1;
          num_elem++;

          for_four_channels(c, aligned(in, neighbours, average))
          {
            neighbours[ii * FILTER_WIDTH + jj][c] = in[4 * (index_v * width + index_h) + c];
            average[c] += in[4 * (index_v * width + index_h) + c];
          }
        }
      }
    }
  }

  // Get the average per channel
  for_four_channels(c, aligned(neighbours, average)) average[c] /= (float)num_elem;

  // Get the covariance matrix per channel with regard to other channels
  dt_colormatrix_t covariance = { { 0.f } };
  for(size_t k = 0; k < FILTER_SIZE; k++)
    for(size_t ii = 0; ii < 3; ii++)
      for(size_t jj = ii; jj < 3; jj++)
        covariance[ii][jj] += mask[k] ? (average[ii] - neighbours[k][ii]) * (average[jj] - neighbours[k][jj]) / (float)num_elem : 0.f;
  // We divide by num_elem before aggregating into covariance to avoid floating point arithmetic problems (cancellation of decimals)
  // Reason is we work on HF wavelets coeffs, aka laplacian, and the average is close to 0.

  // Note : the covariance matrix is symetric but the lower triangle is not inited.
  covariance[1][0] = covariance[0][1];
  covariance[2][0] = covariance[0][2];
  covariance[2][1] = covariance[1][2];

  // sigma = covariance matrix + eps * I
  dt_colormatrix_t sigma = { { 0.f } };
  for(size_t ii = 0; ii < 3; ii++)
    for(size_t jj = 0; jj < 3; jj++)
      sigma[ii][jj] = (ii == jj) ? covariance[ii][jj] + threshold
                                 : covariance[ii][jj];

  dt_colormatrix_t sigma_inv = { { 0.f } };
  if(invert_matrix(sigma, sigma_inv))
  {
    // Non-singular matrix :
    // Solve the 3D linear problem y = a x + b
    dt_colormatrix_t a = { { 0.f } };
    for(size_t c = 0; c < 3; c++)
      dot_product(covariance[c], sigma_inv, a[c]);
    // channel-wise linear coeffs are writen on rows
    // aka `new R = a[0][0] * old R + a[0][1] * old G + a[0][2] * old B`
    for_each_channel(c, aligned(a, RGB))
    {
      const float b = average[c] - a[c][0] * average[0] - a[c][1] * average[1] - a[c][2] * average[2];
      RGB[c] = a[c][0] * RGB[0] + a[c][1] * RGB[1] + a[c][2] * RGB[2] + b;
    }
  }
}

static inline void prepare_image(const float *const restrict input, float *const restrict output,
                                 const size_t width, const size_t height, const float edge_threshold)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) collapse(2) \
    dt_omp_firstprivate(output, input, height, width, edge_threshold) \
    schedule(static)
#endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = ((i * width) + j) * 4;
      dt_aligned_pixel_t RGB = { input[index + RED], input[index + GREEN], input[index + BLUE], input[index + ALPHA] };
      basic_guided_filter_3D(RGB, input, i, j, width, height, edge_threshold);
      RGB[ALPHA] = fmaxf(sqrtf(sqf(RGB[RED]) + sqf(RGB[GREEN]) + sqf(RGB[BLUE])) / sqrtf(3.f), 1e-6f);
      for_four_channels(c, aligned(RGB, output)) output[index + c] = RGB[c];
    }
}


#pragma omp declare simd uniform(HF, width, height, threshold, denoise, regularization, radius, first_order, hot_threshold, sharpen, regularization_sharpen)
static inline float guided_filter_3D(dt_aligned_pixel_t RGB, const float *const restrict HF,
                                    const int i, const int j,
                                    const size_t width, const size_t height,
                                    const float threshold, const float denoise,
                                    const float regularization, const float radius,
                                    const int first_order, const float hot_threshold,
                                    const float sharpen, const float regularization_sharpen)
{
  // Get the neighbours in the filter window
  dt_aligned_pixel_t neighbours[FILTER_SIZE] = { { 0.f } };
  dt_aligned_pixel_t average = { 0.f };

  // Manage the edge pixels with a mask recording what neighbours are recorded in the window
  int DT_ALIGNED_ARRAY mask[FILTER_SIZE] = { 0 };
  int num_elem = 0;

  // Local variance is computed on the euclidean norm as a metric of sharpness/details less biased with noise
  float local_variance = 0.f;

  for(int ii = 0; ii < FILTER_WIDTH; ++ii)
  {
    const int index_v = i + ii - FILTER_RADIUS;
    if(index_v > -1 && index_v < height)
    {
      for(int jj = 0; jj < FILTER_WIDTH; ++jj)
      {
        const int index_h = j + jj - FILTER_RADIUS;
        if(index_h > -1 && index_h < width)
        {
          // Mark this pixel as counting for the average/variance
          mask[ii * FILTER_WIDTH + jj] = 1;
          num_elem++;

          for_four_channels(c, aligned(HF, neighbours, average))
          {
            neighbours[ii * FILTER_WIDTH + jj][c] = HF[4 * (index_v * width + index_h) + c];
            average[c] += HF[4 * (index_v * width + index_h) + c];
          }

          local_variance += sqf(HF[4 * (index_v * width + index_h) + ALPHA]);
        }
      }
    }
  }

  // Finish average/variance computation
  for_four_channels(c, aligned(neighbours, average)) average[c] /= (float)num_elem;
  local_variance /= (float)num_elem;

  // Get the bi-laplacian = laplacian of HF wavelets coeffs
  dt_aligned_pixel_t bilaplacian;
  for_four_channels(c, aligned(RGB, average, bilaplacian))
    bilaplacian[c] = (average[c] - RGB[c]) * 24.f / (float)FILTER_SIZE;

  // Hot pixels are detected as a ratio between bi-laplacian and local variance
  const int is_hot
      =    (sqf(bilaplacian[0]) / (local_variance + 1e-9f)) > hot_threshold
        || (sqf(bilaplacian[1]) / (local_variance + 1e-9f)) > hot_threshold
        || (sqf(bilaplacian[2]) / (local_variance + 1e-9f)) > hot_threshold;

  if(is_hot)
  {
    for_four_channels(c, aligned(neighbours, average)) RGB[c] = average[c];
  }
  else
  {
    int guided_filter_success = FALSE;
    const float norm_backup = RGB[ALPHA];

    // Get the covariance matrix per channel with regard to other channels
    dt_colormatrix_t covariance = { { 0.f } };
    for(size_t k = 0; k < FILTER_SIZE; k++)
      for(size_t ii = 0; ii < 3; ii++)
        for(size_t jj = ii; jj < 3; jj++)
          covariance[ii][jj]
              += mask[k] ? (average[ii] - neighbours[k][ii]) * (average[jj] - neighbours[k][jj]) / (float)num_elem
                         : 0.f;

    // We divide by num_elem before aggregating into covariance to avoid floating point arithmetic problems
    // (cancellation of decimals) Reason is we work on HF wavelets coeffs, aka laplacian, and the average is close
    // to 0.

    if(first_order) // actually, first order == chroma process
    {
      // Note : the covariance matrix is symetric but the lower triangle is not inited.
      covariance[1][0] = covariance[0][1];
      covariance[2][0] = covariance[0][2];
      covariance[2][1] = covariance[1][2];

      // sigma = covariance matrix + eps * I
      dt_colormatrix_t sigma = { { 0.f } };
      for(size_t ii = 0; ii < 3; ii++)
        for(size_t jj = 0; jj < 3; jj++)
          sigma[ii][jj] = (ii == jj) ? covariance[ii][jj] + threshold
                                     : covariance[ii][jj];

      dt_colormatrix_t sigma_inv = { { 0.f } };
      guided_filter_success = invert_matrix(sigma, sigma_inv);

      if(guided_filter_success)
      {
        // Non-singular matrix :
        // Solve the 3D linear problem y = a x + b
        // Typical case for edges.
        dt_colormatrix_t a = { { 0.f } };
        for(size_t c = 0; c < 3; c++)
          dot_product(covariance[c], sigma_inv, a[c]);

        // channel-wise linear coeffs are writen on rows
        // aka `new R = a[0][0] * old R + a[0][1] * old G + a[0][2] * old B`
        int DT_ALIGNED_PIXEL local_success[4];
        for_each_channel(c, aligned(a, RGB, local_success))
        {
          const float b = average[c] - a[c][0] * average[0] - a[c][1] * average[1] - a[c][2] * average[2];
          const float test_value = a[c][0] * RGB[0] + a[c][1] * RGB[1] + a[c][2] * RGB[2] + b;

          // Discard the solution if it's not within original HF ± 200 %, then average
          // This prevents ringing and overshooting at edges with poorly-conditionned matrices.
          local_success[c] = (fabsf(test_value - RGB[c]) / (fabsf(RGB[c]) + 1e-9f) < 1.f);
          RGB[c] = local_success[c] ? test_value : RGB[c];
        }

        guided_filter_success &= local_success[0] && local_success[1] && local_success[2];
      }
    }

    if(!guided_filter_success)
    {
      // Singular matrix : can't find a cross-channels linear model. Diffuse within the channel.
      // Typical case for flat surfaces
      if(first_order)
      {
        // Variance-based penalty used as edge-avoiding factor. Normalize variance for scale
        // such that it stays constant
        const float penalty = fminf(1.f / (1.f + regularization * local_variance), 1.f);
        const float factor = denoise * penalty / radius;

        // First-order diffusion : laplacian = 0. Aggressive denoising for chroma only.
        for_each_channel(c, aligned(RGB)) RGB[c] += factor * (-0.5f * RGB[c] + bilaplacian[c]);
      }
      else
      {
        // Second-order diffusion : laplacian(laplacian) = 0. Better preservation of edges for RGB.
        for_each_channel(c, aligned(RGB, bilaplacian, covariance))
          RGB[c] += denoise * fminf(1.f / (1.f + regularization * covariance[c][c]), 1.f) * bilaplacian[c] / radius;
      }
    }

    RGB[ALPHA] = norm_backup;

    if(first_order && i > 4 && j > 4 && i < height - 4 && j < width - 4)
    {
      // first_order == TRUE is assumed to be triggered only from the chroma variant.
      // need to keep in sync if that changes in the future.
      // This should be triggered for the chroma variant on the norm channel.
      //const float sharp_penalty = expf(-sqf(1.12631139e-06f - local_variance) / regularization_sharpen) /
      const float sharp_penalty= fminf((regularization_sharpen * local_variance * fabsf(1.f - regularization_sharpen * local_variance)), 1.f);
      RGB[ALPHA] -= 0.5f * sharpen * sharp_penalty * bilaplacian[ALPHA] / radius;
    }
  }

  // variance of the norm channel
  return local_variance;
}

static inline void guided_laplacians(const float *const restrict high_freq, float *const restrict low_freq,
                                      float *const restrict output,
                                      const size_t width, const size_t height,
                                      const uint8_t scale, const float radius,
                                      const float regularization, const float edge_threshold,
                                      const float denoise, const float hot_threshold)
{
  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);

#ifdef _OPENMP
#pragma omp parallel for default(none)                                                     \
    dt_omp_firstprivate(out, HF, LF, height, width, scale, radius, denoise, regularization, \
    edge_threshold, hot_threshold) collapse(2) \
    schedule(static)
#endif
  for(size_t i = 0; i < height; ++i)
    for(size_t j = 0; j < width; ++j)
    {
      const size_t index = (i * width + j) * 4;

      dt_aligned_pixel_t high_frequency = { HF[index + 0], HF[index + 1], HF[index + 2], HF[index + 3] };
      const float norm_backup = high_frequency[ALPHA];
      guided_filter_3D(high_frequency, HF, i, j, width, height, 0.f, denoise, regularization, radius, FALSE, hot_threshold, 0.f, 0.f);
      high_frequency[ALPHA] = norm_backup;

      if((scale & FIRST_SCALE))
      {
        // out is not inited yet
        for_four_channels(c, aligned(out, high_frequency : 64))
          out[index + c] = high_frequency[c];
      }
      else
      {
        // just accumulate HF
        for_four_channels(c, aligned(out, high_frequency : 64))
          out[index + c] += high_frequency[c];
      }

      if((scale & LAST_SCALE))
      {
        // add the residual and clamp
        for_each_channel(c, aligned(out, LF, high_frequency : 64))
          out[index + c] = fmaxf(out[index + c] + LF[index + c], 0.f);
      }
    }
}

static inline void heat_PDE_diffusion(const float *const restrict high_freq, const float *const restrict low_freq,
                                      float *const restrict output, const size_t width, const size_t height,
                                      const uint8_t scale,
                                      const float radius,
                                      const float regularization,
                                      const float edge_threshold,
                                      const float denoise,
                                      const float sharpen, const float regularization_sharpen,
                                      const float hot_threshold)
{
  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);

  //float avg_variance = 0.f;

#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                               \
    dt_omp_firstprivate(out, HF, LF, height, width, scale, regularization, edge_threshold, denoise, \
    sharpen, regularization_sharpen, hot_threshold, radius) \
    schedule(static) collapse(2)
    //shared(avg_variance)
#endif
  for(size_t i = 0; i < height; ++i)
  {
    for(size_t j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;

      dt_aligned_pixel_t high_frequency = { HF[index + 0], HF[index + 1], HF[index + 2], HF[index + 3] };
      const float local_variance = guided_filter_3D(high_frequency, HF, i, j, width, height, edge_threshold, denoise, regularization, radius, TRUE, hot_threshold, sharpen, regularization_sharpen);

      //avg_variance += variance / (float)(height * width);

      if(i > 2 && j > 2 && i < height - 2 && j < width - 2)
      {
        // Compute the laplacian
        static const float DT_ALIGNED_ARRAY isotropic_kernel[5][5]
            = { { -0.00833333f, 0.f,         -0.0666666f,  0.f,         -0.00833333f },
                {  0.f,         0.13333333f,  1.06666667f, 0.13333333f,  0.f },
                { -0.06666667f, 1.06666667f, -4.5f,        1.06666667f, -0.06666667f },
                {  0.f,         0.13333333f,  1.06666667f, 0.13333333f,  0.f },
                { -0.00833333f, 0.f,         -0.06666667f, 0.f,         -0.00833333f } };

        // Convolve the filter to get the laplacian, ignoring borders
        if(i > 2 && j > 2 && i < height - 2 && j < width - 2)
        {
          float laplacian_LF = 0.f ;

          for(int ii = 0; ii < 5; ii++)
            for(int jj = 0; jj < 5; jj++)
            {
              const size_t index_v = i + ii - 2;
              const size_t index_h = j + jj - 2;
              laplacian_LF += LF[4 * (index_v * width + index_h) + ALPHA] * isotropic_kernel[ii][jj];
            }

          // Assuming HF is the modulation of the signal around LF, we need to scale the HF boost
          // accordingly with the base signal to avoid over-sharpening near black
          //const float penalty = expf(-sqf(1.12631139e-06f - local_variance) / regularization_sharpen) / expf(sqf(local_variance) / regularization_sharpen);
          const float sharp_penalty= fminf((regularization_sharpen * local_variance * fabsf(1.f - regularization_sharpen * local_variance)), 1.f);
          high_frequency[ALPHA] -= sharpen * sharp_penalty * laplacian_LF / radius;
        }
      }

      if((scale & FIRST_SCALE))
      {
        // out is not inited yet
        for_each_channel(c, aligned(out, high_frequency : 64)) out[index + c] = high_frequency[c];
      }
      else
      {
        // just accumulate HF
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] += high_frequency[c];
      }

      if((scale & LAST_SCALE))
      {
        // add the residual and clamp
        for_four_channels(c, aligned(out, LF, high_frequency : 64))
          out[index + c] = fmaxf(out[index + c] + LF[index + c], 0.f);

        // Last scale : reconstruct RGB from ratios and norm - norm stays in the 4th channel
        // we need it to evaluate the gradient
        for_four_channels(c, aligned(out))
          out[index + c] = (c == ALPHA) ? out[index + ALPHA] : out[index + c] * out[index + ALPHA];

        // Update the norm
        const float norm = fmaxf(sqrtf(sqf(out[index + 0]) + sqf(out[index + 1]) + sqf(out[index + 2])) / sqrtf(3.f), 1e-6f);
        out[index + ALPHA] = norm;
      }
    }
  }

  //fprintf(stdout, "avg variance : %.12f for scale %d at radius %f\n", avg_variance, scale, radius);
}


static inline gint wavelets_process(const float *const restrict in,
                                    float *const restrict reconstructed,
                                    const size_t width, const size_t height,
                                    const float zoom, const int scales,
                                    float *const restrict HF,
                                    float *const restrict LF_odd,
                                    float *const restrict LF_even,
                                    const diffuse_reconstruct_variant_t variant,
                                    const float denoise, const float edges_sensibility,
                                    const float edges_threshold,
                                    const float sharpness, const float edges_sensibility_sharpness,
                                    const float hot_threshold)
{
  gint success = TRUE;

  // À trous decimated wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,

  // allocate a one-row temporary buffer for the decomposition
  size_t padded_size;
  float *const DT_ALIGNED_ARRAY tempbuf = dt_alloc_perthread_float(4 * width, &padded_size);
  float *const DT_ALIGNED_ARRAY temp_HF = dt_alloc_align_float(4 * width * height);

  for(int s = 0; s < scales; ++s)
  {
    //fprintf(stderr, "CPU Wavelet decompose : scale %i\n", s);
    const int mult = 1 << s;

    const float *restrict buffer_in;
    float *restrict buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    decompose_2D_Bspline(buffer_in, HF, buffer_out, width, height, mult, tempbuf, padded_size);

    uint8_t current_scale_type = scale_type(s, scales);
    const float radius = (float)mult * zoom;

    const float threshold = powf(10.f, -edges_threshold);
    const float regularization = powf(10.f, edges_sensibility) - 1.f;
    const float hot_pixels_threshold = powf(10.f, hot_threshold);

    if(variant == DIFFUSE_RECONSTRUCT_RGB)
    {
      guided_laplacians(HF, buffer_out, reconstructed, width, height, current_scale_type, radius,
                        regularization, threshold, denoise, hot_pixels_threshold);
    }
    else
    {
      const float regularization_sharpen = powf(10.f, edges_sensibility_sharpness);
      heat_PDE_diffusion(HF, buffer_out, reconstructed, width, height, current_scale_type, radius,
                         regularization, threshold, denoise, sharpness, regularization_sharpen, hot_pixels_threshold);
    }
  }
  dt_free_align(tempbuf);
  dt_free_align(temp_HF);
  return success;
}

static inline void precondition(const float *const in, float *const buf, const int wd, const int ht, const dt_aligned_pixel_t a,
                                const dt_aligned_pixel_t sigma2_plus_3_8)
{
  // MAKITALO AND FOI, OPTIMAL INVERSION OF THE GENERALIZED ANSCOMBE TRANSFORMATION FOR POISSON-GAUSSIAN NOISE
  // https://webpages.tuni.fi/foi/papers/OptGenAnscombeInverse-doublecolumn-preprint.pdf
  const size_t npixels = (size_t)wd * ht;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(buf, npixels, in, sigma2_plus_3_8, a) \
  schedule(static)
#endif
  for(size_t j = 0; j < 4U * npixels; j += 4)
  {
    for_each_channel(c,aligned(in,buf,a,sigma2_plus_3_8))
    {
      const float d = fmaxf(0.0f, in[j+c] / a[c] + sigma2_plus_3_8[c]);
      buf[j+c] = 2.0f * sqrtf(d);
    }
    buf[j + 3] = in[j + 3];
  }
}

static inline void backtransform(float *const buf, const int wd, const int ht, const dt_aligned_pixel_t a, const dt_aligned_pixel_t sigma2_plus_1_8)
{
  // MAKITALO AND FOI, OPTIMAL INVERSION OF THE GENERALIZED ANSCOMBE TRANSFORMATION FOR POISSON-GAUSSIAN NOISE
  // https://webpages.tuni.fi/foi/papers/OptGenAnscombeInverse-doublecolumn-preprint.pdf
  const size_t npixels = (size_t)wd * ht;
  const float sqrt_3_2 = sqrtf(3.0f / 2.0f);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(buf, npixels, sigma2_plus_1_8, sqrt_3_2, a)   \
  schedule(static)
#endif
  for(size_t j = 0; j < 4U * npixels; j += 4)
  {
    for_each_channel(c,aligned(buf,sigma2_plus_1_8))
    {
      const float x = buf[j+c], x2 = sqf(x);
      // closed form approximation to unbiased inverse (input range was 0..200 for fit, not 0..1)
      buf[j+c] = (x < 0.5f) ? 0.0f
                            : a[c] * (0.25f * x2 + 0.25f * sqrt_3_2 / x - 1.375f / x2
                                      + 0.625f * sqrt_3_2 / (x * x2) - sigma2_plus_1_8[c]);
    }

    // Break the RGB channels into ratios/norm for the next step of reconstruction
    const float norm = fmaxf(sqrtf(sqf(buf[j + RED]) + sqf(buf[j + GREEN]) + sqf(buf[j + BLUE])) / sqrtf(3.f), 1e-6f);
    for_each_channel(c, aligned(buf : 64)) buf[j + c] /= norm;
    buf[j + ALPHA] = norm;
  }
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                    const void *const restrict ivoid, void *const restrict ovoid,
                    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_denoise_params_t *data = (dt_iop_denoise_params_t *)piece->data;

  const size_t height = roi_in->height;
  const size_t width = roi_in->width;
  const size_t size = roi_in->width * roi_in->height;

  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  float *const restrict LF_odd = dt_alloc_align_float(size * 4);
  float *const restrict LF_even = dt_alloc_align_float(size * 4);

  const float scale = fmaxf(piece->iscale / (roi_in->scale), 1.f);
  const float final_radius = (float)(data->radius) / scale;
  const int iterations = MAX(ceilf((float)data->iterations), 1);
  const int diffusion_scales = num_steps_to_reach_equivalent_sigma(B_SPLINE_SIGMA, final_radius);
  const int scales = CLAMP(diffusion_scales, 1, MAX_NUM_SCALES);

  // wavelets scales buffers
  float *restrict HF = dt_alloc_align_float(size * 4);
  float *restrict temp = dt_alloc_align_float(size * 4);

  const float *const restrict input = (const float *const restrict)ivoid;
  float *const restrict output = (float *const restrict)ovoid;

  // Gauss-Poisson mixed model params for Anscombe transform
  dt_aligned_pixel_t wb = { 1.f };
  for(int i = 0; i < 3; i++) wb[i] = piece->pipe->dsc.temperature.coeffs[i];

  // Variance increases proportionnaly with WB coeffs and scaling factor
  dt_aligned_pixel_t sigma2_plus_1_8 = { 0.f };
  for_each_channel(c) sigma2_plus_1_8[c] = sqf(data->b[c] / (data->a[c] * wb[c] * scale)) + 1.f / 8.f;

  dt_aligned_pixel_t sigma2_plus_3_8 = { 0.f };
  for_each_channel(c) sigma2_plus_3_8[c] = sqf(data->b[c] / (data->a[c] * wb[c] * scale)) + 3.f / 8.f;

  // For RGB processing, apply Anscombe variance stabilisation to account for Poisson noise
  prepare_image(input, temp, width, height, powf(10.f, -data->edges_threshold_RGB));

  for(int i = 0; i < iterations; i++)
  {
    precondition(temp, temp, width, height, data->a, sigma2_plus_3_8);
    wavelets_process(temp, output, width, height, scale, scales, HF, LF_odd, LF_even,
                     DIFFUSE_RECONSTRUCT_RGB,
                     data->denoise_RGB, data->edges_sensibility_RGB,
                     data->edges_threshold_RGB,
                     data->sharpness, data->edges_sensibility_sharpness,
                     data->hot_pixels_threshold);
    backtransform(output, width, height, data->a, sigma2_plus_1_8);

    // RGB ratios make no sense regarding scene-referred light in Anscombe space, so we need to go back
    wavelets_process(output, temp, width, height, scale, scales, HF, LF_odd, LF_even,
                     DIFFUSE_RECONSTRUCT_CHROMA,
                     data->denoise_chroma, data->edges_sensibility_chroma,
                     data->edges_threshold_chroma,
                     data->sharpness, data->edges_sensibility_sharpness,
                     data->hot_pixels_threshold);

    prepare_image(temp, output, width, height, powf(10.f, -data->edges_threshold_RGB));
  }


  dt_free_align(temp);
  dt_free_align(LF_even);
  dt_free_align(LF_odd);
  dt_free_align(HF);
}


void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_denoise_gui_data_t *g = IOP_GUI_ALLOC(denoise);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->iterations = dt_bauhaus_slider_from_params(self, "iterations");
  gtk_widget_set_tooltip_text(g->iterations,
                              _("more iterations make the effect stronger but the module slower.\n"
                                "this is analogous to giving more time to the diffusion reaction.\n"
                                "if you plan on sharpening or inpainting, \n"
                                "more iterations help reconstruction."));

  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  dt_bauhaus_slider_set_format(g->radius, " px");
  gtk_widget_set_tooltip_text(
      g->radius, _("width of the diffusion around the central radius.\n"
                   "high values diffuse on a large band of radii.\n"
                   "low values diffuse closer to the central radius.\n"
                   "if you plan on deblurring, \n"
                   "the radius should be around the width of your lens blur."));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("RGB noise")), FALSE, FALSE, 0);

  g->denoise_RGB = dt_bauhaus_slider_from_params(self, "denoise_RGB");
  dt_bauhaus_slider_set_format(g->denoise_RGB, "%");
  g->edges_sensibility_RGB = dt_bauhaus_slider_from_params(self, "edges_sensibility_RGB");
  g->edges_threshold_RGB = dt_bauhaus_slider_from_params(self, "edges_threshold_RGB");

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("Chroma aberrations & noise")), FALSE, FALSE, 0);

  g->denoise_chroma = dt_bauhaus_slider_from_params(self, "denoise_chroma");
  dt_bauhaus_slider_set_format(g->denoise_chroma, "%");
  g->edges_sensibility_chroma = dt_bauhaus_slider_from_params(self, "edges_sensibility_chroma");
  g->edges_threshold_chroma = dt_bauhaus_slider_from_params(self, "edges_threshold_chroma");

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("Sharpness")), FALSE, FALSE, 0);

  g->sharpness = dt_bauhaus_slider_from_params(self, "sharpness");
  dt_bauhaus_slider_set_format(g->sharpness, "%");
  g->edges_sensibility_sharpness = dt_bauhaus_slider_from_params(self, "edges_sensibility_sharpness");

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("Hot & Dead pixels")), FALSE, FALSE, 0);
  g->hot_pixels = dt_bauhaus_slider_from_params(self, "hot_pixels_threshold");
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
