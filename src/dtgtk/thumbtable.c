/*
    This file is part of darktable,
    Copyright (C) 2019-2021 darktable developers.
    Copyright (C) 2022-2025 Aurélien PIERRE.

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
/** a class to manage a table of thumbnail for lighttable and filmstrip.  */

#include "dtgtk/thumbtable.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/undo.h"
#include "control/control.h"

#include "gui/drag_and_drop.h"
#include "views/view.h"
#include "bauhaus/bauhaus.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

/**
 * @file thumbtable.c
 *
 * We keep a double reference of thumbnail objects for the current collection:
 *  - as a linked list of variable length, in table->list
 *  - as an array of fixed length, in table->lut.
 *
 * The linked list is used to keep track of allocated objects to update, redraw and free.
 * Its length is limited to 210 elements or whatever is visible inside viewport
 * at current scroll level. It's garbage-collected.
 *
 * The LUT is used to speed up lookups for thumbnails at known, bounded positions in sequential
 * order (position in collection = (rowid - 1) in SQLite result = order in GUI = index in the LUT).
 * This LUT prevents us from re-querying the collection in SQLite all the time using:
 * "SELECT rowid, imgid FROM main.collected_images WHERE rowid > min_row AND rowid <= max_row ORDER BY rowid ASC".
 * Note though that SQLite starts indexing at 1, so there is an unit offset.
 * It also keeps a reference to the thumbnail objects, but objets should never be freed from there.
 * Given that collections set on root folders contain all the images from their children,
 * the number of elements in a LUT can be anything from 1 to several 100k images.
 *
 * It is expected that thumbnails alloc/free always happen using table->list,
 * and that table->lut only updates its references accordingly, because table->list
 * will typically lead to fewer loop incrementations.
 *
 * Keep that in mind if/when extending features.
 *
 * For image collections having up to 1000 items,
 * we could just statically reset/init the list of thumbnails once when the collection changes, then only resize
 * thumbnails at runtime. But for collections of thousands of images, while adding child widgets is fairly fast,
 * considering, detaching those widgets from the parent takes ages (several orders of magnitude more than
 * attaching). So we have no choice here but to attach and detach dynamically, as to keep the number of children
 * reasonable. "Reasonable" is we populate the current viewport page (at current scrolling position),
 * the previous and the next ones, as to ensure smooth scrolling.
 *
 * The dimensions of the full collection grid are only ever virtual, but we need to make them real for the scrollbars to behave properly
 * through dynamic loading and unloading of thumbnails.
 * So we set the grid area to what it would be if we loaded all thumbnails.
 **/


void dt_thumbtable_configure(dt_thumbtable_t *table);
void _dt_thumbtable_empty_list(dt_thumbtable_t *table);

// get the class name associated with the overlays mode
static gchar *_thumbs_get_overlays_class(dt_thumbnail_overlay_t over)
{
  switch(over)
  {
    case DT_THUMBNAIL_OVERLAYS_NONE:
      return g_strdup("dt_overlays_none");
    case DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL:
      return g_strdup("dt_overlays_always");
    case DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL:
    default:
      return g_strdup("dt_overlays_hover");
  }
}

// update thumbtable class and overlays mode, depending on size category
static void _thumbs_update_overlays_mode(dt_thumbtable_t *table)
{
  // we change the overlay mode
  gchar *txt = g_strdup("plugins/lighttable/overlays/global");
  dt_thumbnail_overlay_t over = sanitize_overlays(dt_conf_get_int(txt));
  g_free(txt);

  dt_thumbtable_set_overlays_mode(table, over);
}

// change the type of overlays that should be shown
void dt_thumbtable_set_overlays_mode(dt_thumbtable_t *table, dt_thumbnail_overlay_t over)
{
  if(!table) return;
  if(over == table->overlays) return;
  dt_conf_set_int("plugins/lighttable/overlays/global", sanitize_overlays(over));
  gchar *cl0 = _thumbs_get_overlays_class(table->overlays);
  gchar *cl1 = _thumbs_get_overlays_class(over);

  dt_gui_remove_class(table->grid, cl0);
  dt_gui_add_class(table->grid, cl1);
    // we need to change the overlay content if we pass from normal to extended overlays
  // this is not done on the fly with css to avoid computing extended msg for nothing and to reserve space if needed
  dt_pthread_mutex_lock(&table->lock);
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    // and we resize the bottom area
    dt_thumbnail_resize(th, th->width, th->height, TRUE, IMG_TO_FIT);
  }
  dt_pthread_mutex_unlock(&table->lock);

  table->overlays = over;
  g_free(cl0);
  g_free(cl1);
}

// We can't trust the mouse enter/leave events on thumnbails to properly
// update active thumbnail styling, so we need to catch the signal here and update the whole list.
void _mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  dt_pthread_mutex_lock(&table->lock);
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    dt_thumbnail_set_mouseover(thumb, thumb->imgid == dt_control_get_mouse_over_id());
  }
  dt_pthread_mutex_unlock(&table->lock);
}


static void _rowid_to_position(dt_thumbtable_t *table, int index, int *x, int *y)
{
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    int row = index / table->thumbs_per_row;  // euclidean division
    int col = index % table->thumbs_per_row;
    *x = col * table->thumb_width;
    *y = row * table->thumb_height;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    *x = index * table->thumb_width;
    *y = 0;
  }
}

// Find the x, y coordinates of any given thumbnail
// Return TRUE if a position could be computed
// thumb->rowid and table->thumbs_per_row need to have been inited before calling this
static gboolean _set_thumb_position(dt_thumbtable_t *table, dt_thumbnail_t *thumb)
{
  if(table->thumbs_per_row < 1) return FALSE;
  _rowid_to_position(table, thumb->rowid, &thumb->x, &thumb->y);
  return TRUE;
}

static int dt_thumbtable_scroll_to_imgid(dt_thumbtable_t *table, int imgid)
{
  if(!table->collection_inited || imgid < 0) return 1;

  // Find the rowid
  int rowid = -1;

  dt_pthread_mutex_lock(&table->lock);
  for(size_t i = 0; i < table->collection_count; i++)
  {
    if(table->lut[i].imgid == imgid)
    {
      rowid = i;
      break;
    }
  }
  dt_pthread_mutex_unlock(&table->lock);

  if(rowid == -1) return 0;

  // Find (x, y) of the current thumbnail (north-west corner)
  int x = 0, y = 0;
  _rowid_to_position(table, rowid, &x, &y);

  // Put the image always in the center of the view, if possible
  x += table->thumb_width / 2;
  y += table->thumb_height / 2;

  // Scroll there
  gtk_adjustment_set_value(table->v_scrollbar, (double)y - table->view_height / 2);
  gtk_adjustment_set_value(table->h_scrollbar, (double)x - table->view_width / 2);
  table->x_position = x;
  table->y_position = y;

  return 0;
}


static int dt_thumbtable_scroll_to_selection(dt_thumbtable_t *table)
{
  int id = dt_selection_get_first_id(darktable.selection);
  if(id < 0) id = dt_control_get_keyboard_over_id();
  if(id < 0) id = dt_control_get_mouse_over_id();
  dt_thumbtable_scroll_to_imgid(table, id);
  return 0;
}

// Find the row ids (as in SQLite indices) of the images contained within viewport at current scrolling stage
static gboolean _get_row_ids(dt_thumbtable_t *table, int *rowid_min, int *rowid_max)
{
  if(!table->configured || !table->v_scrollbar || !table->h_scrollbar) return FALSE;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    // Pixel coordinates of the viewport:
    int page_size = gtk_adjustment_get_page_size(table->v_scrollbar);
    int position = gtk_adjustment_get_value(table->v_scrollbar);

    // what is currently visible lies between position and position + page_size.
    int row_min = (position) / table->thumb_height - 2;
    int row_max = (position + page_size) / table->thumb_height + 2;

    // rowid is the positional ID of the image in the SQLite collection, indexed from 0.
    // SQLite indexes from 1 but then be use our own array to cache results.
    *rowid_min = row_min * table->thumbs_per_row;
    *rowid_max = row_max * table->thumbs_per_row;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    int page_size = gtk_adjustment_get_page_size(table->h_scrollbar);
    int position = gtk_adjustment_get_value(table->h_scrollbar);

    int row_min = (position - page_size) / table->thumb_width;
    int row_max = (position + 2 * page_size) / table->thumb_width;

    *rowid_min = row_min * table->thumbs_per_row;
    *rowid_max = row_max * table->thumbs_per_row;
  }

  return TRUE;
}

// Find out if a given row id is visible at current scroll step
gboolean _is_rowid_visible(dt_thumbtable_t *table, int rowid)
{
  if(!table->configured || !table->v_scrollbar || !table->h_scrollbar) return FALSE;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    // Pixel coordinates of the viewport:
    int page_size = gtk_adjustment_get_page_size(table->v_scrollbar);
    int position = gtk_adjustment_get_value(table->v_scrollbar);
    int page_bottom = page_size + position;

    int img_top = (rowid / table->thumbs_per_row) * table->thumb_height;
    int img_bottom = img_top + table->thumb_height;
    return img_top >= position && img_bottom <= page_bottom;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    int page_size = gtk_adjustment_get_page_size(table->h_scrollbar);
    int position = gtk_adjustment_get_value(table->h_scrollbar);
    int page_right = page_size + position;

    int img_left = rowid * table->thumb_height;
    int img_right = img_left + table->thumb_width;
    return img_left >= position && img_right <= page_right;
  }

  return FALSE;
}


void _update_row_ids(dt_thumbtable_t *table)
{
  int rowid_min = 0;
  int rowid_max = 210;
  _get_row_ids(table, &rowid_min, &rowid_max);
  if(rowid_min != table->min_row_id || rowid_max != table->max_row_id)
  {
    table->min_row_id = rowid_min;
    table->max_row_id = rowid_max;
    table->thumbs_inited = FALSE;
  }
}

void _update_grid_area(dt_thumbtable_t *table)
{
  if(!table->configured || !table->collection_inited) return;

  double main_dimension = 0.;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    double height = main_dimension = ceilf((float)table->collection_count / (float)table->thumbs_per_row) * table->thumb_height;
    gtk_widget_set_size_request(table->grid, -1, height);
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    double width = main_dimension = table->collection_count * table->thumb_height;
    gtk_widget_set_size_request(table->grid, width, -1);
  }
  else
    gtk_widget_set_size_request(table->grid, -1, -1);

  dt_print(DT_DEBUG_LIGHTTABLE, "Configuring grid size main dimension: %.f\n", main_dimension);
}


void _grid_configure(dt_thumbtable_t *table, int width, int height, int cols)
{
  if(width < 32 || height < 32) return;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    table->thumbs_per_row = cols;
    table->view_width = width;
    table->view_height = height;
    table->thumb_width = (int)floorf((float)width / (float)table->thumbs_per_row);
    table->thumb_height = (table->thumbs_per_row == 1) ? height : table->thumb_width;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    table->thumbs_per_row = 1;
    table->view_width = width;
    table->view_height = height;
    table->thumb_height = height;
    table->thumb_width = height;
  }

  table->configured = TRUE;

  dt_print(DT_DEBUG_LIGHTTABLE, "Configuring thumbtable w=%i h=%i thumbs/row=%i thumb_width=%i\n",
           table->view_width, table->view_height, table->thumbs_per_row, table->thumb_width);
}

// Track size changes of the container or number of thumbs per row
// and recomputed the size of individual thumbnails accordingly
void dt_thumbtable_configure(dt_thumbtable_t *table)
{
  int cols;
  int new_width;
  int new_height;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    GtkWidget *parent = table->overlay_center;
    new_width = gtk_widget_get_allocated_width(parent);
    new_height = gtk_widget_get_allocated_height(parent);
    GtkWidget *v_scroll = gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(table->scroll_window));
    new_width -= gtk_widget_get_allocated_width(v_scroll);
    cols = dt_conf_get_int("plugins/lighttable/images_in_row");
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    GtkWidget *parent = table->overlay_filmstrip;
    new_width = gtk_widget_get_allocated_width(parent);
    new_height = dt_conf_get_int("darkroom/ui/0/bottom_size");
    GtkWidget *h_scroll = gtk_scrolled_window_get_hscrollbar(GTK_SCROLLED_WINDOW(table->scroll_window));
    new_height -= gtk_widget_get_allocated_height(h_scroll);
    cols = table->thumbs_per_row; // whatever that doesn't make the next if think layout changed
  }
  else
  {
    gtk_widget_set_size_request(table->grid, -1, -1);
    return;
  }

  if((new_width > 0 && new_width != table->view_width) ||
     (new_height > 0 && new_height != table->view_height) ||
     (cols != table->thumbs_per_row))
  {
    // new sizes: update everything
    table->thumbs_inited = FALSE;
    _grid_configure(table, new_width, new_height, cols);
  }

  if(!table->thumbs_inited)
  {
    _update_grid_area(table);
    _update_row_ids(table);
  }
}

// Remove invisible thumbs at current scrolling level, only when we have more than 210.
// That's because freeing widgets slows down the scrolling and 210 is no issue to handle at once.
// 210 = 2*3*5*7, so we ensure full rows up to 11 thumbs/row.
void _garbage_collection(dt_thumbtable_t *table, int *num_thumb)
{
  dt_pthread_mutex_lock(&table->lock);
  GList *link = g_list_first(table->list);
  while(link)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)link->data;
    GList *l = link;
    link = g_list_next(link);

    gboolean collect_garbage = (table->thumb_nb + *num_thumb > 210)
                                && (thumb->rowid < table->min_row_id || thumb->rowid > table->max_row_id);

    // rowid doesn't match imgid: this belongs to a previous collection
    gboolean is_in_collection = (thumb->imgid == table->lut[thumb->rowid].imgid);

    if(collect_garbage && is_in_collection) table->lut[thumb->rowid].thumb = NULL;
    // else if(collect_garbage && !is_incollection)
    // the cache was reinited when loading the new collection, so it's NULL already

    if(collect_garbage || !is_in_collection)
    {
      dt_thumbnail_destroy(thumb);
      table->list = g_list_delete_link(table->list, l);
      *num_thumb -= 1;
    }
  }
  dt_pthread_mutex_unlock(&table->lock);
}

// Add and/or resize thumbnails within visible viewort at current scroll level
void _populate_thumbnails(dt_thumbtable_t *table, int *num_thumb)
{
  dt_pthread_mutex_lock(&table->lock);
  for(size_t i = MAX(table->min_row_id, 0); i < MIN(table->max_row_id, table->collection_count); i++)
  {
    const int nrow = i;
    const int nid = table->lut[i].imgid;

    dt_thumbnail_t *thumb = (table->lut[i].thumb) ? (table->lut[i].thumb) : NULL;
    gboolean new_item = (thumb == NULL);

    if(new_item)
    {
      thumb = dt_thumbnail_new(IMG_TO_FIT, nid, nrow, table->overlays, table);
      table->lut[i].thumb = thumb;
      table->list = g_list_prepend(table->list, thumb);
      *num_thumb += 1;
    }

    if(!thumb) continue; // not sure why that would happen

    // In case it's not a new item, ensure those are still up-to-date
    // to validate the coding logic and assumptions.
    assert(thumb->rowid == nrow);
    assert(thumb->imgid == nid);

    // There is no circumstance yet in which we simply want to move a thumbnail
    // at constant size, moving is always assumed to come with resizing.
    gboolean size_changed = (table->thumb_height != thumb->height || table->thumb_width != thumb->width);
    if(new_item || size_changed)
    {
      dt_thumbnail_resize(thumb, table->thumb_width, table->thumb_height, TRUE, IMG_TO_FIT);
      _set_thumb_position(table, thumb);
    }

    if(new_item)
      gtk_fixed_put(GTK_FIXED(table->grid), thumb->widget, thumb->x, thumb->y);
    else if(size_changed)
      gtk_fixed_move(GTK_FIXED(table->grid), thumb->widget, thumb->x, thumb->y);

    // Update visual states and flags. Mouse over is not connected to a signal and cheap to update
    dt_thumbnail_set_mouseover(thumb, (dt_control_get_mouse_over_id() == thumb->imgid));
  }
  dt_pthread_mutex_unlock(&table->lock);
}

// Resize the thumbnails that are still existing but outside of visible viewport at current scroll level
void _resize_thumbnails(dt_thumbtable_t *table)
{
  dt_pthread_mutex_lock(&table->lock);
  for(GList *link = table->list; link; link = g_list_next(link))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)link->data;
    gboolean already_processed = (thumb->rowid >= table->min_row_id && thumb->rowid < table->max_row_id);
    gboolean size_changed = (table->thumb_height != thumb->height || table->thumb_width != thumb->width);

    if(!already_processed && size_changed)
    {
      dt_thumbnail_resize(thumb, table->thumb_width, table->thumb_height, TRUE, IMG_TO_FIT);
      _set_thumb_position(table, thumb);
      gtk_fixed_move(GTK_FIXED(table->grid), thumb->widget, thumb->x, thumb->y);
    }
  }
  dt_pthread_mutex_unlock(&table->lock);
}


void dt_thumbtable_update(dt_thumbtable_t *table)
{
  if(!table->lut || !table->configured || !table->collection_inited || table->thumbs_inited) return;

  if(table->reset_collection)
  {
    _dt_thumbtable_empty_list(table);
    table->reset_collection = FALSE;
  }

  int num_thumb = 0;
  const double start = dt_get_wtime();
  gboolean empty_list = (table->list == NULL);

  _populate_thumbnails(table, &num_thumb);

  // Remove unneeded thumbnails: out of viewport or out of current collection
  if(!empty_list)
  {
    _garbage_collection(table, &num_thumb);
    _resize_thumbnails(table);
  }

  table->thumb_nb += num_thumb;
  table->thumbs_inited = TRUE;

  dt_print(DT_DEBUG_LIGHTTABLE, "Populated %d thumbs between %i and %i in %0.04f sec \n", num_thumb, table->min_row_id, table->max_row_id, dt_get_wtime() - start);
}


static void _dt_profile_change_callback(gpointer instance, int type, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  dt_pthread_mutex_lock(&table->lock);
  for(const GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_image_refresh(th);
  }
  dt_pthread_mutex_unlock(&table->lock);
}

static void _dt_selection_changed_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  dt_pthread_mutex_lock(&table->lock);
  for(const GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    dt_thumbnail_update_selection(thumb, dt_selection_is_id_selected(darktable.selection, thumb->imgid));
  }
  dt_pthread_mutex_unlock(&table->lock);
}

static void _dt_mipmaps_updated_callback(gpointer instance, int imgid, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  dt_pthread_mutex_lock(&table->lock);
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    if(thumb->imgid == imgid)
    {
      dt_thumbnail_image_refresh(thumb);
      break;
    }
  }
  dt_pthread_mutex_unlock(&table->lock);
}

// this is called each time the images info change
static void _dt_image_info_changed_callback(gpointer instance, gpointer imgs, gpointer user_data)
{
  if(!user_data || !imgs) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  dt_pthread_mutex_lock(&table->lock);
  for(GList *i = g_list_first(imgs); i; i = g_list_next(i))
  {
    const int imgid_to_update = GPOINTER_TO_INT(i->data);
    for(GList *l = table->list; l; l = g_list_next(l))
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
      if(thumb->imgid == imgid_to_update)
      {
        dt_thumbnail_update_infos(thumb);
        break;
      }
    }
  }
  dt_pthread_mutex_unlock(&table->lock);
}

static void _dt_collection_lut(dt_thumbtable_t *table)
{
  // Because the "culling" mode swaps selection with collection directly in memory,
  // we need to fetch the current collection directly from the SQLite memory DB
  // and can't rely on the regular collection API
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM memory.collected_images ORDER BY rowid ASC", -1, &stmt, NULL);
  GList *collection = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    collection = g_list_prepend(collection, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);

  if(collection == NULL)
  {
    table->collection_count = 0;
    return;
  }

  table->collection_count = g_list_length(collection);
  collection = g_list_reverse(collection);

  dt_pthread_mutex_lock(&table->lock);

  if(table->lut) free(table->lut);
  table->lut = malloc(table->collection_count * sizeof(dt_thumbtable_cache_t));

  if(!table->lut)
  {
    g_list_free(collection);
    dt_pthread_mutex_unlock(&table->lock);
    return;
  }

  GList *collection_item = g_list_first(collection);
  for(size_t i = 0; i < table->collection_count; i++)
  {
    // i is our SQLite rowid -1, aka position in DB result
    // NOTE: SQLite indexes from 1
    table->lut[i].imgid = GPOINTER_TO_INT(collection_item->data);
    table->lut[i].thumb = NULL;
    collection_item = g_list_next(collection_item);
  }

  dt_pthread_mutex_unlock(&table->lock);

  g_list_free(collection);
}

static int _dt_collection_get_hash(dt_thumbtable_t *table)
{
  // Hash the collection query string
  const char *const query = dt_collection_get_query(darktable.collection);
  size_t len = strlen(query);
  uint64_t hash = dt_hash(5384, query, len);

  // Factor in the number of images in the collection result
  uint32_t num_pics = dt_collection_get_count(darktable.collection);
  hash = dt_hash(hash, (char *)&num_pics, sizeof(uint32_t));

  if(hash != table->collection_hash || table->reset_collection)
  {
    // Collection changed: reset everything
    table->collection_hash = hash;
    table->collection_count = num_pics;
    table->collection_inited = TRUE;
    table->thumbs_inited = FALSE;
    _dt_collection_lut(table);
    return 1;
  }
  return 0;
}

static int _grab_focus(dt_thumbtable_t *table)
{
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    // Grab focus here otherwise, on first click over the grid,
    // scrolled window gets scrolled all the way to the top and it's annoying.
    // This can work only if the grid is mapped and realized, which we ensure
    // by wrapping that in a g_idle() method.
    gtk_widget_grab_focus(table->grid);
  }
  return 0;
}

// this is called each time collected images change
static void _dt_collection_changed_callback(gpointer instance, dt_collection_change_t query_change,
                                            dt_collection_properties_t changed_property, gpointer imgs,
                                            const int next, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  // See if the collection changed
  int changed = _dt_collection_get_hash(table);

  dt_thumbtable_configure(table);
  if(changed) g_idle_add((GSourceFunc) dt_thumbtable_scroll_to_selection, table);
  dt_thumbtable_update(table);
  g_idle_add((GSourceFunc) _grab_focus, table);
}

static void _event_dnd_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection_data,
                           const guint target_type, const guint time, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  g_assert(selection_data != NULL);

  switch(target_type)
  {
    case DND_TARGET_IMGID:
    {
      const int imgs_nb = g_list_length(table->drag_list);
      if(imgs_nb)
      {
        uint32_t *imgs = malloc(sizeof(uint32_t) * imgs_nb);
        GList *l = table->drag_list;
        for(int i = 0; i < imgs_nb; i++)
        {
          imgs[i] = GPOINTER_TO_INT(l->data);
          l = g_list_next(l);
        }
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                               _DWORD, (guchar *)imgs, imgs_nb * sizeof(uint32_t));
        free(imgs);
      }
      break;
    }
    default: // return the location of the file as a last resort
    case DND_TARGET_URI:
    {
      GList *l = table->drag_list;
      if(g_list_is_singleton(l))
      {
        gchar pathname[PATH_MAX] = { 0 };
        gboolean from_cache = TRUE;
        const int id = GPOINTER_TO_INT(l->data);
        dt_image_full_path(id,  pathname,  sizeof(pathname),  &from_cache, __FUNCTION__);
        gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                               _BYTE, (guchar *)uri, strlen(uri));
        g_free(uri);
      }
      else
      {
        GList *images = NULL;
        for(; l; l = g_list_next(l))
        {
          const int id = GPOINTER_TO_INT(l->data);
          gchar pathname[PATH_MAX] = { 0 };
          gboolean from_cache = TRUE;
          dt_image_full_path(id,  pathname,  sizeof(pathname),  &from_cache, __FUNCTION__);
          gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
          images = g_list_prepend(images, uri);
        }
        images = g_list_reverse(images); // list was built in reverse order, so un-reverse it
        gchar *uri_list = dt_util_glist_to_str("\r\n", images);
        g_list_free_full(images, g_free);
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _BYTE,
                               (guchar *)uri_list, strlen(uri_list));
        g_free(uri_list);
      }
      break;
    }
  }
}

static void _event_dnd_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  const int ts = DT_PIXEL_APPLY_DPI(128);

  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  table->drag_list = dt_act_on_get_images();

#ifdef HAVE_MAP
  dt_view_manager_t *vm = darktable.view_manager;
  dt_view_t *view = vm->current_view;
  if(!strcmp(view->module_name, "map"))
  {
    if(table->drag_list)
      dt_view_map_drag_set_icon(darktable.view_manager, context,
                                GPOINTER_TO_INT(table->drag_list->data),
                                g_list_length(table->drag_list));
  }
  else
#endif
  {
    // if we are dragging a single image -> use the thumbnail of that image
    // otherwise use the generic d&d icon
    // TODO: have something pretty in the 2nd case, too.
    if(g_list_is_singleton(table->drag_list))
    {
      const int id = GPOINTER_TO_INT(table->drag_list->data);
      dt_mipmap_buffer_t buf;
      dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, ts, ts);
      dt_mipmap_cache_get(darktable.mipmap_cache, &buf, id, mip, DT_MIPMAP_BLOCKING, 'r');

      if(buf.buf)
      {
        for(size_t i = 3; i < (size_t)4 * buf.width * buf.height; i += 4) buf.buf[i] = UINT8_MAX;

        int w = ts, h = ts;
        if(buf.width < buf.height)
          w = (buf.width * ts) / buf.height; // portrait
        else
          h = (buf.height * ts) / buf.width; // landscape

        GdkPixbuf *source = gdk_pixbuf_new_from_data(buf.buf, GDK_COLORSPACE_RGB, TRUE, 8, buf.width, buf.height,
                                                     buf.width * 4, NULL, NULL);
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(source, w, h, GDK_INTERP_HYPER);
        gtk_drag_set_icon_pixbuf(context, scaled, 0, h);

        if(source) g_object_unref(source);
        if(scaled) g_object_unref(scaled);
      }

      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    }
  }

  if(darktable.collection->params.sort == DT_COLLECTION_SORT_CUSTOM_ORDER)
    dt_gui_add_class(table->grid, "dt_thumbtable_reorder");
}

void dt_thumbtable_event_dnd_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                GtkSelectionData *selection_data, guint target_type, guint time,
                                gpointer user_data)
{
  // AUREL FIXME: clean that fucking mess
  gboolean success = FALSE;

  if((target_type == DND_TARGET_URI) && (selection_data != NULL)
     && (gtk_selection_data_get_length(selection_data) >= 0))
  {
    gchar **uri_list = g_strsplit_set((gchar *)gtk_selection_data_get_data(selection_data), "\r\n", 0);
    if(uri_list)
    {
      gchar **image_to_load = uri_list;
      while(*image_to_load)
      {
        if(**image_to_load)
        {
          dt_load_from_string(*image_to_load, FALSE, NULL); // TODO: do we want to open the image in darkroom mode?
                                                            // If yes -> set to TRUE.
        }
        image_to_load++;
      }
    }
    g_strfreev(uri_list);
    success = TRUE;
  }
  else if((target_type == DND_TARGET_IMGID) && (selection_data != NULL)
          && (gtk_selection_data_get_length(selection_data) >= 0))
  {
    dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
    if(table->drag_list)
    {
      if(darktable.collection->params.sort == DT_COLLECTION_SORT_CUSTOM_ORDER)
      {
        // source = dest = thumbtable => we are reordering
        // set order to "user defined" (this shouldn't trigger anything)
        const int32_t mouse_over_id = dt_control_get_mouse_over_id();
        dt_collection_move_before(mouse_over_id, table->drag_list);
        dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                                   g_list_copy(table->drag_list));
        success = TRUE;
      }
    }
    else
    {
      // we don't catch anything here at the moment
    }
  }
  gtk_drag_finish(context, success, FALSE, time);
}

static void _event_dnd_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(table->drag_list)
  {
    g_list_free(table->drag_list);
    table->drag_list = NULL;
  }
  // in any case, with reset the reordering class if any
  dt_gui_remove_class(table->grid, "dt_thumbtable_reorder");
}


void _adjust_value_changed(GtkAdjustment *self, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  _update_row_ids(table);
  gtk_widget_queue_draw(table->grid);
}

int _imgid_to_rowid(dt_thumbtable_t *table, int imgid)
{
  if(!table->lut) return -1;

  int rowid = -1;

  dt_pthread_mutex_lock(&table->lock);
  for(int i = 0; i < table->collection_count; i++)
  {
    if(table->lut[i].imgid == imgid)
    {
      rowid = i;
      break;
    }
  }
  dt_pthread_mutex_unlock(&table->lock);

  return rowid;
}

typedef enum dt_thumbtable_direction_t
{
  DT_TT_MOVE_UP,
  DT_TT_MOVE_DOWN,
  DT_TT_MOVE_LEFT,
  DT_TT_MOVE_RIGHT,
  DT_TT_MOVE_PREVIOUS_PAGE,
  DT_TT_MOVE_NEXT_PAGE,
  DT_TT_MOVE_START,
  DT_TT_MOVE_END
} dt_thumbtable_direction_t;


void _move_in_grid(dt_thumbtable_t *table, dt_thumbtable_direction_t direction, int origin_imgid)
{
  int current_rowid = _imgid_to_rowid(table, origin_imgid);
  int offset = 0;

  switch(direction)
  {
    case DT_TT_MOVE_UP:
      offset = - table->thumbs_per_row;
      break;
    case DT_TT_MOVE_DOWN:
      offset = + table->thumbs_per_row;
      break;
    case DT_TT_MOVE_LEFT:
      offset = - 1;
      break;
    case DT_TT_MOVE_RIGHT:
      offset = + 1;
      break;
    case DT_TT_MOVE_PREVIOUS_PAGE:
      offset = - table->view_height / table->thumb_height * table->thumbs_per_row;
      break;
    case DT_TT_MOVE_NEXT_PAGE:
      offset = + table->view_height / table->thumb_height * table->thumbs_per_row;
      break;
    case DT_TT_MOVE_START:
      offset = - origin_imgid;
      break;
    case DT_TT_MOVE_END:
      offset = +table->collection_count; // will be clamped below, don't care
      break;
  }

  int new_rowid = CLAMP(current_rowid + offset, 0, table->collection_count - 1);

  dt_pthread_mutex_lock(&table->lock);
  int new_imgid = table->lut[new_rowid].imgid;
  dt_pthread_mutex_unlock(&table->lock);

  dt_control_set_mouse_over_id(new_imgid);
  dt_control_set_keyboard_over_id(new_imgid);

  if(!_is_rowid_visible(table, new_rowid))
  {
    // GUI update will be handled through the value-changed event of the GtkAdjustment
    dt_thumbtable_scroll_to_imgid(table, new_imgid);
  }
  else
  {
    // We still need to update all visible thumbs to keep mouse_over states in sync
    table->thumbs_inited = FALSE;
    dt_thumbtable_update(table);
  }
}


gboolean _key_pressed_grid(GtkWidget *self, GdkEventKey *event, gpointer user_data)
{
  if(!user_data) return FALSE;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  // Find out the current image
  // NOTE: when moving into the grid from key arrow events,
  // if the cursor is still overlaying the grid when scrolling, it can collide
  // with key event and set the mouse_over focus elsewhere.
  // For this reason, we use our own private keyboard_over event,
  // and use the mouse_over as a fall-back only.
  // Key events are "knobby", therefore more reliale than "hover",
  // so they always take precedence.
  int imgid = dt_control_get_keyboard_over_id();
  if(imgid < 0) imgid = dt_control_get_mouse_over_id();
  if(imgid < 0) imgid = dt_selection_get_first_id(darktable.selection);
  if(imgid < 0 && table->lut)
  {
    dt_pthread_mutex_lock(&table->lock);
    imgid = table->lut[0].imgid;
    dt_pthread_mutex_unlock(&table->lock);
  }

  //fprintf(stdout, "%s\n", gtk_accelerator_name(event->keyval, event->state));

  switch(event->keyval)
  {
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
    {
      if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
      {
        _move_in_grid(table, DT_TT_MOVE_UP, imgid);
        return TRUE;
      }
      else
      {
        return FALSE;
      }
    }
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
    {
      if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
      {
        _move_in_grid(table, DT_TT_MOVE_DOWN, imgid);
        return TRUE;
      }
      else
      {
        return FALSE;
      }
    }
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
    {
      _move_in_grid(table, DT_TT_MOVE_LEFT, imgid);
      return TRUE;
    }
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
    {
      _move_in_grid(table, DT_TT_MOVE_RIGHT, imgid);
      return TRUE;
    }
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:
    {
      _move_in_grid(table, DT_TT_MOVE_PREVIOUS_PAGE, imgid);
      return TRUE;
    }
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:
    {
      _move_in_grid(table, DT_TT_MOVE_NEXT_PAGE, imgid);
      return TRUE;
    }
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
    {
      _move_in_grid(table, DT_TT_MOVE_START, imgid);
      return TRUE;
    }
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
    {
      _move_in_grid(table, DT_TT_MOVE_END, imgid);
      return TRUE;
    }
    case GDK_KEY_space:
    {
      if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
        dt_selection_select_range(darktable.selection, imgid);
      else if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
        dt_selection_toggle(darktable.selection, imgid);
      else
        dt_selection_select_single(darktable.selection, imgid);
      return TRUE;
    }
    case GDK_KEY_nobreakspace:
    {
      // Shift + space is decoded as nobreakspace on BÉPO keyboards
      dt_selection_select_range(darktable.selection, imgid);
      return TRUE;
    }
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, imgid);
      return TRUE;
    }
  }
  return FALSE;
}


static gboolean _draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  // Ensure the background color is painted
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);
  gtk_render_frame(context, cr, 0, 0, allocation.width, allocation.height);

  dt_thumbtable_configure(table);
  dt_thumbtable_update(table);
  return FALSE;
}

void dt_thumbtable_reset_collection(dt_thumbtable_t *table)
{
  table->reset_collection = TRUE;
}

gboolean _event_main_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_control_set_mouse_over_id(-1);
  return FALSE;
}

dt_thumbtable_t *dt_thumbtable_new()
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)calloc(1, sizeof(dt_thumbtable_t));

  table->scroll_window = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(table->scroll_window), FALSE);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(table->scroll_window), GTK_SHADOW_ETCHED_IN);
  gtk_widget_set_can_focus(table->scroll_window, TRUE);
  gtk_widget_set_focus_on_click(table->scroll_window, TRUE);

  table->v_scrollbar = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(table->scroll_window));
  table->h_scrollbar = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(table->scroll_window));
  g_signal_connect(G_OBJECT(table->v_scrollbar), "value-changed", G_CALLBACK(_adjust_value_changed), table);
  g_signal_connect(G_OBJECT(table->h_scrollbar), "value-changed", G_CALLBACK(_adjust_value_changed), table);

  table->grid = gtk_fixed_new();
  dt_gui_add_class(table->grid, "dt_thumbtable");
  gtk_container_add(GTK_CONTAINER(table->scroll_window), table->grid);
  gtk_widget_set_can_focus(table->grid, TRUE);
  gtk_widget_set_focus_on_click(table->grid, TRUE);
  gtk_widget_add_events(table->grid, GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(table->grid), "leave-notify-event", G_CALLBACK(_event_main_leave), table);

  // drag and drop : used for reordering, interactions with maps, exporting uri to external apps, importing images
  // in filmroll...
  gtk_drag_source_set(table->grid, GDK_BUTTON1_MASK, target_list_all, n_targets_all, GDK_ACTION_MOVE);
  gtk_drag_dest_set(table->grid, GTK_DEST_DEFAULT_ALL, target_list_all, n_targets_all, GDK_ACTION_MOVE);
  g_signal_connect_after(table->grid, "drag-begin", G_CALLBACK(_event_dnd_begin), table);
  g_signal_connect_after(table->grid, "drag-end", G_CALLBACK(_event_dnd_end), table);
  g_signal_connect(table->grid, "drag-data-get", G_CALLBACK(_event_dnd_get), table);
  g_signal_connect(table->grid, "drag-data-received", G_CALLBACK(dt_thumbtable_event_dnd_received), table);

  gtk_widget_add_events(table->grid, GDK_STRUCTURE_MASK | GDK_EXPOSURE_MASK | GDK_KEY_PRESS_MASK);
  g_signal_connect(table->grid, "draw", G_CALLBACK(_draw_callback), table);
  g_signal_connect(table->grid, "key-press-event", G_CALLBACK(_key_pressed_grid), table);
  gtk_widget_show(table->grid);

  table->thumb_nb = 0;
  table->grid_cols = 0;
  table->collection_inited = FALSE;
  table->configured = FALSE;
  table->thumbs_inited = FALSE;
  table->collection_hash = -1;
  table->list = NULL;
  table->collection_count = 0;
  table->min_row_id = 0;
  table->max_row_id = 0;
  table->lut = NULL;
  table->reset_collection = FALSE;
  table->x_position = 0.;
  table->y_position = 0.;

  dt_pthread_mutex_init(&table->lock, NULL);

  dt_gui_add_help_link(table->grid, dt_get_help_url("lighttable_filemanager"));

  // set css name and class
  gtk_widget_set_name(table->grid, "thumbtable-filemanager");
  if(dt_conf_get_bool("lighttable/ui/expose_statuses")) dt_gui_add_class(table->grid, "dt_show_overlays");

  // overlays mode
  _thumbs_update_overlays_mode(table);

  // we register globals signals
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_dt_collection_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_dt_selection_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_dt_profile_change_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_dt_mipmaps_updated_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED,
                            G_CALLBACK(_dt_image_info_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), table);


  table->overlay_center = gtk_overlay_new();
  table->overlay_filmstrip = gtk_overlay_new();

  return table;
}


// Be careful where you call this because we loop on the list
// in many places, so you might free something being looped on
void _dt_thumbtable_empty_list(dt_thumbtable_t *table)
{
  // Cleanup existing stuff
  const double start = dt_get_wtime();

  dt_pthread_mutex_lock(&table->lock);
  if(table->list)
  {
    // WARNING: we need to detach children from parent starting from the last
    // otherwise, Gtk updates the index of all the next children in sequence
    // and that takes forever when thumb_nb > 1000
    for(GList *l = table->list; l; l = g_list_next(l))
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
      dt_thumbnail_destroy(thumb);
    }

    g_list_free(g_steal_pointer(&table->list));
  }
  dt_pthread_mutex_unlock(&table->lock);

  dt_print(DT_DEBUG_LIGHTTABLE, "Cleaning the list of %i elements in %0.04f sec\n", table->thumb_nb,
           dt_get_wtime() - start);

  table->list = NULL;
  table->thumb_nb = 0;
  table->thumbs_inited = FALSE;
}


void dt_thumbtable_cleanup(dt_thumbtable_t *table)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_mipmaps_updated_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_selection_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_profile_change_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_image_info_changed_callback), table);

  _dt_thumbtable_empty_list(table);

  dt_pthread_mutex_destroy(&table->lock);

  free(table->lut);

  free(table);
  table = NULL;
}

// change thumbtable parent widget. Typically from center screen to filmstrip lib
void dt_thumbtable_set_parent(dt_thumbtable_t *table, dt_thumbtable_mode_t mode)
{
  if(table->mode == mode) return;

  GtkWidget *parent = gtk_widget_get_parent(table->scroll_window);
  if(parent)
  {
    // Relax size constaints
    gtk_widget_set_size_request(table->overlay_center, -1, -1);
    gtk_widget_set_size_request(parent, -1, -1);
    gtk_widget_set_size_request(table->grid, -1, -1);

    // Re-init everything
    g_object_ref(table->scroll_window);
    gtk_container_remove(GTK_CONTAINER(parent), table->scroll_window);
    table->mode = DT_THUMBTABLE_MODE_NONE;
    _update_grid_area(table);
  }

  table->mode = mode;

  // Ensure the default drawing area for views is hidden for lighttable and shown otherwise
  GtkWidget *drawing_area = dt_ui_center(darktable.gui->ui);

  if(mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    gtk_widget_set_name(table->grid, "thumbtable-filemanager");
    dt_gui_add_help_link(table->grid, dt_get_help_url("lighttable_filemanager"));
    gtk_widget_hide(drawing_area);
    gtk_widget_show(table->overlay_center);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(table->scroll_window), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_overlay_add_overlay(GTK_OVERLAY(table->overlay_center), table->scroll_window);
    dt_control_set_mouse_over_id(dt_selection_get_first_id(darktable.selection));
  }
  else if(mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    gtk_widget_set_name(table->grid, "thumbtable-filmstrip");
    dt_gui_add_help_link(table->grid, dt_get_help_url("filmstrip"));
    gtk_widget_show(drawing_area);
    gtk_widget_hide(table->overlay_center);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(table->scroll_window), GTK_POLICY_ALWAYS, GTK_POLICY_NEVER);
    gtk_overlay_add_overlay(GTK_OVERLAY(table->overlay_filmstrip), table->scroll_window);
  }

  gtk_widget_show_all(table->scroll_window);

  dt_pthread_mutex_lock(&table->lock);

  for(const GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    if(mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
      // In filmstrip view, the overlay controls are too small to be
      // usable, so we remove actions on them to prevent accidents
      thumb->disable_actions = FALSE;

      // There is no selection in filmstrip, only active images,
      // but we need to pass on the CSS states anyway.
      dt_thumbnail_update_selection(thumb, (thumb->imgid == dt_control_get_mouse_over_id()));
    }
    else
    {
      // Restore selection when existing filmstrip
      dt_thumbnail_update_selection(thumb, dt_selection_is_id_selected(darktable.selection, thumb->imgid));
    }

  }
  dt_pthread_mutex_unlock(&table->lock);

  dt_thumbtable_configure(table);
  g_idle_add((GSourceFunc)dt_thumbtable_scroll_to_selection, table);
  dt_thumbtable_update(table);
  g_idle_add((GSourceFunc) _grab_focus, table);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
