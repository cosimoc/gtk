/* GDK - The GIMP Drawing Kit
 * Copyright (C) 1995-1997 Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Modified by the GTK+ Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GTK+ Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GTK+ at ftp://ftp.gtk.org/pub/gtk/. 
 */

#include "config.h"

#include "gdkwindowimpl.h"

#include "gdkinternals.h"


G_DEFINE_TYPE (GdkWindowImpl, gdk_window_impl, G_TYPE_OBJECT);

static gboolean
gdk_window_impl_beep (GdkWindow *window)
{
  /* FALSE means windows can't beep, so the display will be
   * made to beep instead. */
  return FALSE;
}

static GdkDisplay *
get_display_for_window (GdkWindow *primary,
                        GdkWindow *secondary)
{
  GdkDisplay *display = gdk_window_get_display (primary);

  if (display)
    return display;

  display = gdk_window_get_display (secondary);

  if (display)
    return display;

  return gdk_display_get_default ();
}

static GdkMonitor *
get_monitor_for_rect (GdkDisplay         *display,
                      const GdkRectangle *rect)
{
  gint biggest_area = G_MININT;
  GdkMonitor *best_monitor = NULL;
  GdkMonitor *monitor;
  GdkRectangle workarea;
  GdkRectangle intersection;
  gint x;
  gint y;
  gint i;

  for (i = 0; i < gdk_display_get_n_monitors (display); i++)
    {
      monitor = gdk_display_get_monitor (display, i);
      gdk_monitor_get_workarea (monitor, &workarea);

      if (gdk_rectangle_intersect (&workarea, rect, &intersection))
        {
          if (intersection.width * intersection.height > biggest_area)
            {
              biggest_area = intersection.width * intersection.height;
              best_monitor = monitor;
            }
        }
    }

  if (best_monitor)
    return best_monitor;

  x = rect->x + rect->width / 2;
  y = rect->y + rect->height / 2;

  return gdk_display_get_monitor_at_point (display, x, y);
}

static gint
get_anchor_x (GdkGravity anchor)
{
  switch (anchor)
    {
    case GDK_GRAVITY_STATIC:
    case GDK_GRAVITY_NORTH_WEST:
    case GDK_GRAVITY_WEST:
    case GDK_GRAVITY_SOUTH_WEST:
      return -1;

    default:
    case GDK_GRAVITY_NORTH:
    case GDK_GRAVITY_CENTER:
    case GDK_GRAVITY_SOUTH:
      return 0;

    case GDK_GRAVITY_NORTH_EAST:
    case GDK_GRAVITY_EAST:
    case GDK_GRAVITY_SOUTH_EAST:
      return 1;
    }
}

static gint
get_anchor_y (GdkGravity anchor)
{
  switch (anchor)
    {
    case GDK_GRAVITY_STATIC:
    case GDK_GRAVITY_NORTH_WEST:
    case GDK_GRAVITY_NORTH:
    case GDK_GRAVITY_NORTH_EAST:
      return -1;

    default:
    case GDK_GRAVITY_WEST:
    case GDK_GRAVITY_CENTER:
    case GDK_GRAVITY_EAST:
      return 0;

    case GDK_GRAVITY_SOUTH_WEST:
    case GDK_GRAVITY_SOUTH:
    case GDK_GRAVITY_SOUTH_EAST:
      return 1;
    }
}

static gint
choose_position (gint      bounds_x,
                 gint      bounds_width,
                 gint      rect_x,
                 gint      rect_width,
                 gint      window_width,
                 gint      rect_anchor,
                 gint      window_anchor,
                 gint      rect_anchor_dx,
                 gboolean  flip,
                 gboolean *flipped)
{
  gint x;

  *flipped = FALSE;
  x = rect_x + (1 + rect_anchor) * rect_width / 2 + rect_anchor_dx - (1 + window_anchor) * window_width / 2;

  if (!flip || (x >= bounds_x && x + window_width <= bounds_x + bounds_width))
    return x;

  *flipped = TRUE;
  x = rect_x + (1 - rect_anchor) * rect_width / 2 - rect_anchor_dx - (1 - window_anchor) * window_width / 2;

  if (x >= bounds_x && x + window_width <= bounds_x + bounds_width)
    return x;

  *flipped = FALSE;
  return rect_x + (1 + rect_anchor) * rect_width / 2 + rect_anchor_dx - (1 + window_anchor) * window_width / 2;
}

static void
gdk_window_impl_move_to_rect (GdkWindow          *window,
                              GdkWindow          *transient_for,
                              const GdkRectangle *rect,
                              GdkGravity          rect_anchor,
                              GdkGravity          window_anchor,
                              GdkAnchorHints      anchor_hints,
                              gint                rect_anchor_dx,
                              gint                rect_anchor_dy)
{
  GdkDisplay *display;
  GdkMonitor *monitor;
  GdkRectangle bounds;
  GdkRectangle root_rect = *rect;
  GdkRectangle flipped_rect;
  GdkRectangle slid_rect;
  gboolean flipped_x;
  gboolean flipped_y;

  gdk_window_get_root_coords (transient_for,
                              root_rect.x,
                              root_rect.y,
                              &root_rect.x,
                              &root_rect.y);

  display = get_display_for_window (window, transient_for);
  monitor = get_monitor_for_rect (display, &root_rect);
  gdk_monitor_get_workarea (monitor, &bounds);

  flipped_rect.width = window->width - window->shadow_left - window->shadow_right;
  flipped_rect.height = window->height - window->shadow_top - window->shadow_bottom;
  flipped_rect.x = choose_position (bounds.x,
                                    bounds.width,
                                    root_rect.x,
                                    root_rect.width,
                                    flipped_rect.width,
                                    get_anchor_x (rect_anchor),
                                    get_anchor_x (window_anchor),
                                    rect_anchor_dx,
                                    anchor_hints & GDK_ANCHOR_FLIP_X,
                                    &flipped_x);
  flipped_rect.y = choose_position (bounds.y,
                                    bounds.height,
                                    root_rect.y,
                                    root_rect.height,
                                    flipped_rect.height,
                                    get_anchor_y (rect_anchor),
                                    get_anchor_y (window_anchor),
                                    rect_anchor_dy,
                                    anchor_hints & GDK_ANCHOR_FLIP_Y,
                                    &flipped_y);

  slid_rect = flipped_rect;

  if (anchor_hints & GDK_ANCHOR_SLIDE_X)
    {
      if (slid_rect.x + slid_rect.width > bounds.x + bounds.width)
        slid_rect.x = bounds.x + bounds.width - slid_rect.width;

      if (slid_rect.x < bounds.x)
        slid_rect.x = bounds.x;
    }

  if (anchor_hints & GDK_ANCHOR_SLIDE_Y)
    {
      if (slid_rect.y + slid_rect.height > bounds.y + bounds.height)
        slid_rect.y = bounds.y + bounds.height - slid_rect.height;

      if (slid_rect.y < bounds.y)
        slid_rect.y = bounds.y;
    }

  if (anchor_hints & GDK_ANCHOR_RESIZE_X)
    {
      if (slid_rect.x < bounds.x)
        {
          slid_rect.width -= bounds.x - slid_rect.x;
          slid_rect.x = bounds.x;
        }

      if (slid_rect.x + slid_rect.width > bounds.x + bounds.width)
        slid_rect.width = bounds.x + bounds.width - slid_rect.x;
    }

  if (anchor_hints & GDK_ANCHOR_RESIZE_Y)
    {
      if (slid_rect.y < bounds.y)
        {
          slid_rect.height -= bounds.y - slid_rect.y;
          slid_rect.y = bounds.y;
        }

      if (slid_rect.y + slid_rect.height > bounds.y + bounds.height)
        slid_rect.height = bounds.y + bounds.height - slid_rect.y;
    }

  flipped_rect.x -= window->shadow_left;
  flipped_rect.y -= window->shadow_top;
  flipped_rect.width += window->shadow_left + window->shadow_right;
  flipped_rect.height += window->shadow_top + window->shadow_bottom;

  slid_rect.x -= window->shadow_left;
  slid_rect.y -= window->shadow_top;
  slid_rect.width += window->shadow_left + window->shadow_right;
  slid_rect.height += window->shadow_top + window->shadow_bottom;

  if (slid_rect.width != window->width || slid_rect.height != window->height)
    gdk_window_move_resize (window, slid_rect.x, slid_rect.y, slid_rect.width, slid_rect.height);
  else
    gdk_window_move (window, slid_rect.x, slid_rect.y);

  g_signal_emit_by_name (window,
                         "moved-to-rect",
                         &flipped_rect,
                         &slid_rect,
                         flipped_x,
                         flipped_y);
}

static void
gdk_window_impl_process_updates_recurse (GdkWindow      *window,
                                         cairo_region_t *region)
{
  _gdk_window_process_updates_recurse (window, region);
}

static void
gdk_window_impl_class_init (GdkWindowImplClass *impl_class)
{
  impl_class->beep = gdk_window_impl_beep;
  impl_class->move_to_rect = gdk_window_impl_move_to_rect;
  impl_class->process_updates_recurse = gdk_window_impl_process_updates_recurse;
}

static void
gdk_window_impl_init (GdkWindowImpl *impl)
{
}
