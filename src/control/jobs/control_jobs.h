/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#pragma once

#include "common/imageio_module.h"
#include "common/image.h"
#include "control/control.h"
#include "common/variables.h"
#include <inttypes.h>

#ifdef HAVE_PRINT
#include "common/cups_print.h"
#endif

typedef struct dt_control_import_t
{
  GList *imgs;
  GDateTime *datetime;
  gboolean copy;
  char *jobcode;
  char *target_folder;
  char *target_subfolder_pattern;
  char *target_file_pattern;
  char *target_dir;
  const int elements;
  int total_imported_elements;

  int32_t filmid;
  GList *discarded;

} dt_control_import_t;


typedef struct dt_control_export_t
{
  GList *imgid_list;
  int max_width;
  int max_height;
  int format_index;
  int storage_index;
  int total;

  gboolean high_quality;
  gboolean export_masks;
  gchar *style;
  gboolean style_append;
  dt_colorspaces_color_profile_type_t icc_type;
  gchar *icc_filename;
  dt_iop_color_intent_t icc_intent;

  gchar *metadata_export;

  dt_imageio_module_format_t *module_format;
  dt_imageio_module_storage_t *module_storage;

  /** 
  * Needed since the gui thread resets things like overwrite once the export
  * is dispatched, but we have to keep that information.
  */
  dt_imageio_module_data_t *module_data;

} dt_control_export_t;

void dt_control_gpx_apply(const gchar *filename, int32_t filmid, const gchar *tz, GList *imgs);

void dt_control_datetime(const GTimeSpan offset, const char *datetime, GList *imgs);

void dt_control_write_sidecar_files();
void dt_control_delete_images();
void dt_control_delete_image(int imgid);
void dt_control_duplicate_images(gboolean virgin);
void dt_control_flip_images(const int32_t cw);
void dt_control_monochrome_images(const int32_t mode);
gboolean dt_control_remove_images();
void dt_control_move_images();
void dt_control_copy_images();
void dt_control_set_local_copy_images();
void dt_control_reset_local_copy_images();
void dt_control_export(dt_control_export_t data);
void dt_control_merge_hdr();

/**
 * @brief Process a list of images to import with or without copying the files on an arbitrary hard-drive.
 *
 * @param data import informations to transmit through the functions
 */
void dt_control_import(dt_control_import_t data);
void dt_control_seed_denoise();
void dt_control_denoise();
void dt_control_refresh_exif();

/**
 * @brief Build a full path for a given image file, given a pattern.
 *
 * @param filename Full path of the original file
 * @param index Incremental number in a sequence
 * @param img dt_image_t object. Needs to be inited with EXIF fields prior to calling this function, otherwise EXIF variables are expanded to defaults/fallback.
 * @param data Import options
 * @return gchar* The full path after variables expansion
 */
gchar *dt_build_filename_from_pattern(const char *const filename, const int index, dt_image_t *img, dt_control_import_t *data);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
