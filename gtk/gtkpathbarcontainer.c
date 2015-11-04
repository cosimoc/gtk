/*
 * Copyright (C) 2016 Carlos Soriano <csoriano@gnome.org>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include "glib.h"

#include "gtkpathbarcontainer.h"
#include "gtkwidgetprivate.h"
#include "gtkintl.h"
#include "gtksizerequest.h"
#include "gtkbuildable.h"
#include "gtkrevealer.h"
#include "gtkbox.h"

//TODO remove
#include "gtkbutton.h"

#define REVEALER_ANIMATION_TIME 2000 //ms
#define INVERT_ANIMATION_SPEED 1.2 //px/ms
#define INVERT_ANIMATION_MAX_TIME 750 //px/ms

struct _GtkPathBarContainerPrivate
{
  GList *children;
  gint inverted :1;
  GList *widgets_to_hide;
  GList *widgets_to_show;
  GList *widgets_to_remove;
  gint current_width;
  gint current_height;

  gboolean invert_animation;

  GdkWindow *bin_window;
  GdkWindow *view_window;

  GtkWidget *children_box;

  guint invert_animation_tick_id;
  double invert_animation_progress;
  gint invert_animation_initial_children_width;
  guint64 invert_animation_initial_time;
  gint allocated_children_width;
  gint total_children_width;
  gint previous_child_width;
};

G_DEFINE_TYPE_WITH_PRIVATE (GtkPathBarContainer, gtk_path_bar_container, GTK_TYPE_BIN)

enum {
  PROP_0,
  PROP_INVERTED,
  LAST_PROP
};

static GParamSpec *path_bar_container_properties[LAST_PROP] = { NULL, };

static void
gtk_path_bar_container_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  GtkPathBarContainer *children_box = GTK_PATH_BAR_CONTAINER (object);

  switch (prop_id)
    {
    case PROP_INVERTED:
      gtk_path_bar_container_set_inverted (children_box, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gtk_path_bar_container_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  GtkPathBarContainer *children_box = GTK_PATH_BAR_CONTAINER (object);
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (children_box);

  switch (prop_id)
    {
    case PROP_INVERTED:
      g_value_set_boolean (value, priv->inverted);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

void
gtk_path_bar_container_add (GtkPathBarContainer *self,
                           GtkWidget           *widget)
{
  GtkPathBarContainer *children_box = GTK_PATH_BAR_CONTAINER (self);
  GtkWidget *revealer;
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (children_box);
  GtkStyleContext *style_context;

  revealer = gtk_revealer_new ();
  style_context = gtk_widget_get_style_context (revealer);
  //gtk_style_context_add_class (style_context, "pathbar-initial-opacity");
  gtk_revealer_set_transition_type (GTK_REVEALER (revealer),
                                    GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
  gtk_container_add (GTK_CONTAINER (revealer), widget);
  gtk_container_add (GTK_CONTAINER (priv->children_box), revealer);
  gtk_revealer_set_transition_duration (GTK_REVEALER (revealer),
                                            REVEALER_ANIMATION_TIME);
  priv->children = g_list_append (priv->children, widget);
  gtk_widget_show_all (revealer);
}

static void
really_remove_child (GtkPathBarContainer *self,
                     GtkWidget           *widget)
{
  GList *child;
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);

  for (child = priv->widgets_to_remove; child != NULL; child = child->next)
    {
      GtkWidget *revealer;

      revealer = gtk_widget_get_parent (child->data);
      if (child->data == widget && !gtk_revealer_get_child_revealed (GTK_REVEALER (revealer)))
        {
          gboolean was_visible = gtk_widget_get_visible (widget);

          priv->widgets_to_remove = g_list_remove (priv->widgets_to_remove,
                                                   child->data);
          gtk_container_remove (GTK_CONTAINER (priv->children_box), revealer);

          if (was_visible)
            gtk_widget_queue_resize (GTK_WIDGET (self));

          break;
        }
    }
}

static void
unrevealed_really_remove_child (GObject    *widget,
                                GParamSpec *pspec,
                                gpointer    user_data)
{
  GtkPathBarContainer *self = GTK_PATH_BAR_CONTAINER (user_data);

  g_signal_handlers_disconnect_by_func (widget, unrevealed_really_remove_child, self);
  really_remove_child (self, gtk_bin_get_child (GTK_BIN (widget)));
}

void
gtk_path_bar_container_remove (GtkPathBarContainer *self,
                               GtkWidget    *widget)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GtkWidget *to_remove;

  if (GTK_IS_REVEALER (widget) && gtk_widget_get_parent (widget) == priv->children_box)
    to_remove = gtk_bin_get_child (GTK_BIN (widget));
  else
    to_remove = widget;

  priv->widgets_to_remove = g_list_append (priv->widgets_to_remove, to_remove);
  priv->children = g_list_remove (priv->children, to_remove);

  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static gboolean
get_children_preferred_size_for_requisition (GtkPathBarContainer *self,
                                             GtkRequisition      *available_size,
                                             gboolean             inverted,
                                             GtkRequisition      *minimum_size,
                                             GtkRequisition      *natural_size)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GtkWidget *child_widget;
  GList *child;
  GtkRequestedSize child_width;
  GtkRequestedSize child_height;
  GtkRequestedSize revealer_width;
  GtkWidget *revealer;
  gint i;
  GList *children;
  gint current_children_min_width = 0;
  gint current_child_min_width = 0;
  gint current_children_nat_width = 0;
  gint current_child_nat_width = 0;
  gint current_children_min_height = 0;
  gint current_children_nat_height = 0;
  gint full_children_current_width = 0;

  children = g_list_copy (priv->children);

  if (inverted)
    children = g_list_reverse (children);

  /* Retrieve desired size for visible children. */
  for (i = 0, child = children; child != NULL; i++, child = child->next)
    {
      child_widget = GTK_WIDGET (child->data);
      revealer = gtk_widget_get_parent (child_widget);

      gtk_widget_get_preferred_width_for_height (child_widget,
                                                 available_size->height,
                                                 &child_width.minimum_size,
                                                 &child_width.natural_size);

      gtk_widget_get_preferred_height_for_width (child_widget,
                                                 current_children_nat_width,
                                                 &child_height.minimum_size,
                                                 &child_height.natural_size);

      gtk_widget_get_preferred_width_for_height (revealer,
                                                 available_size->height,
                                                 &revealer_width.minimum_size,
                                                 &revealer_width.natural_size);

      /* If we are in the middle of a revealer animation, get the revealer
       * allocation */
          current_child_min_width = revealer_width.minimum_size;
          current_child_nat_width = revealer_width.natural_size;

      current_children_min_height = MAX (current_children_min_height, child_height.minimum_size);
      current_children_nat_height = MAX (current_children_nat_height, child_height.natural_size);
      current_children_min_width += current_child_min_width;
      current_children_nat_width += current_child_nat_width;

      full_children_current_width += current_child_min_width;
      g_print ("children container i: %d reversed: %i current width %d available %d\n", i, inverted, child_width.natural_size, available_size->width);
      if (!gtk_revealer_get_reveal_child (revealer))
        {
          break;
        }
    }

  if (minimum_size)
    {
      minimum_size->width = current_children_min_width;
      minimum_size->height = current_children_min_height;
    }

  if (natural_size)
    {
      natural_size->width = current_children_nat_width;
      natural_size->height = current_children_nat_height;
    }

  g_list_free (children);

  return current_children_nat_width > available_size->width;
}

static void
update_children_visibility (GtkPathBarContainer *self,
                            GtkRequisition      *available_size)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GtkWidget *child_widget;
  GList *child;
  GtkRequestedSize *sizes_temp;
  gint i;
  GList *children;
  gboolean allocate_more_children = TRUE;
  gint current_children_width = 0;

  g_list_free (priv->widgets_to_show);
  priv->widgets_to_show = NULL;
  g_list_free (priv->widgets_to_hide);
  priv->widgets_to_hide = NULL;
  children = g_list_copy (priv->children);
  sizes_temp = g_newa (GtkRequestedSize, g_list_length (priv->children));
  if (priv->inverted)
    children = g_list_reverse (children);

  /* Retrieve desired size for visible children. */
  for (i = 0, child = children; child != NULL; i++, child = child->next)
    {
      child_widget = GTK_WIDGET (child->data);

      gtk_widget_get_preferred_width_for_height (child_widget,
                                                 available_size->height,
                                                 &sizes_temp[i].minimum_size,
                                                 &sizes_temp[i].natural_size);

      current_children_width += sizes_temp[i].minimum_size;

      if (!allocate_more_children || current_children_width > available_size->width)
        {
          if (allocate_more_children)
            priv->allocated_children_width = current_children_width - sizes_temp[i].minimum_size;
          allocate_more_children = FALSE;
          priv->previous_child_width = sizes_temp[i].minimum_size;
          if (gtk_revealer_get_child_revealed (GTK_REVEALER (gtk_widget_get_parent (child_widget))))
            priv->widgets_to_hide = g_list_append (priv->widgets_to_hide, child_widget);

          continue;
        }

      if (!g_list_find (priv->widgets_to_remove, child_widget))
        priv->widgets_to_show = g_list_append (priv->widgets_to_show, child_widget);
    }
g_print ("####### i %d\n", g_list_length (priv->widgets_to_show));

  priv->total_children_width = current_children_width;
  g_list_free (children);
}

static void
add_opacity_class (GtkWidget   *widget,
                   const gchar *class_name)
{
  GtkStyleContext *style_context;

  style_context = gtk_widget_get_style_context (widget);

  gtk_style_context_add_class (style_context, class_name);
}

static void
remove_opacity_classes (GtkWidget *widget)
{
  GtkStyleContext *style_context;

  style_context = gtk_widget_get_style_context (widget);

  gtk_style_context_remove_class (style_context, "pathbar-initial-opacity");
  gtk_style_context_remove_class (style_context, "pathbar-opacity-on");
  gtk_style_context_remove_class (style_context, "pathbar-opacity-off");
}

static void
revealer_on_show_completed (GObject    *widget,
                            GParamSpec *pspec,
                            gpointer    user_data)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (GTK_PATH_BAR_CONTAINER (user_data));

  remove_opacity_classes (GTK_WIDGET (widget));
  g_signal_handlers_disconnect_by_func (widget, revealer_on_show_completed, user_data);
  priv->widgets_to_show = g_list_remove (priv->widgets_to_show,
                                         gtk_bin_get_child (GTK_BIN (widget)));
}

static void
revealer_on_hide_completed (GObject    *widget,
                            GParamSpec *pspec,
                            gpointer    user_data)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (GTK_PATH_BAR_CONTAINER (user_data));

  remove_opacity_classes (GTK_WIDGET (widget));
  g_signal_handlers_disconnect_by_func (widget, revealer_on_hide_completed,
                                        user_data);
  priv->widgets_to_hide = g_list_remove (priv->widgets_to_hide,
                                         gtk_bin_get_child (GTK_BIN (widget)));
}

static void
idle_update_revealers (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GList *l;

  /* The invert animation is handled in a tick callback, do nothing here */
  if (priv->invert_animation)
    return;

  for (l = priv->widgets_to_hide; l != NULL; l = l->next)
    {
      GtkWidget *revealer;

      revealer = gtk_widget_get_parent (l->data);
      if (gtk_revealer_get_child_revealed (GTK_REVEALER (revealer)) &&
          gtk_revealer_get_reveal_child (GTK_REVEALER (revealer)))
        {
          g_signal_handlers_disconnect_by_func (revealer, revealer_on_hide_completed, self);
          g_signal_connect (revealer, "notify::child-revealed", (GCallback) revealer_on_hide_completed, self);

          remove_opacity_classes (revealer);
          add_opacity_class (revealer, "pathbar-opacity-off");

          gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), FALSE);
        }
    }

  for (l = priv->widgets_to_remove; l != NULL; l = l->next)
    {
      GtkWidget *revealer;

      revealer = gtk_widget_get_parent (l->data);
      if (gtk_revealer_get_child_revealed (GTK_REVEALER (revealer)))
        {
          g_signal_handlers_disconnect_by_func (revealer, revealer_on_hide_completed, self);
          g_signal_connect (revealer, "notify::child-revealed",
                            (GCallback) unrevealed_really_remove_child, self);

          remove_opacity_classes (revealer);
          add_opacity_class (revealer, "pathbar-opacity-off");

          gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), FALSE);
        }
      else
        {
          really_remove_child (self, l->data);
        }
    }

  /* We want to defer to show revealers until the animation of those that needs
   * to be hidden or removed are done
   */
  if (priv->widgets_to_remove || priv->widgets_to_hide)
    return;

  for (l = priv->widgets_to_show; l != NULL; l = l->next)
    {
      GtkWidget *revealer;

      revealer = gtk_widget_get_parent (l->data);
      if (!gtk_revealer_get_reveal_child (GTK_REVEALER (revealer)))
        {
          g_signal_handlers_disconnect_by_func (revealer, revealer_on_show_completed, self);
          g_signal_connect (revealer, "notify::child-revealed", (GCallback) revealer_on_show_completed, self);

          remove_opacity_classes (revealer);
          add_opacity_class (revealer, "pathbar-opacity-on");

          gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), TRUE);
        }
    }

}

static gint
get_max_scroll (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GtkRequisition children_used_min_size;
  GtkRequisition children_used_nat_size;
  gboolean overflows;
  GtkAllocation allocation;
  GtkRequisition available_size;
  gint children_width;
  gint children_used_width;
  gdouble max_scroll;


  gtk_widget_get_allocation (GTK_WIDGET (self), &allocation);
  available_size.width = allocation.width;
  available_size.height = allocation.height;
  overflows = get_children_preferred_size_for_requisition (self, &available_size,
                                                           TRUE,
                                                           &children_used_min_size,
                                                           &children_used_nat_size);

  children_used_width = overflows ? MAX (children_used_min_size.width, allocation.width) :
                                    children_used_nat_size.width;

  children_width = gtk_widget_get_allocated_width (priv->children_box);

  if (priv->invert_animation)
    {
      if (priv->inverted)
        {
          max_scroll = MAX (0, children_width - children_used_width);
        }
      else
        {
          max_scroll = children_width - children_used_width;
        }
    }
  else
    {
      max_scroll = MAX (0, children_width - children_used_width);
    }

  return max_scroll;
}

static void
update_scrolling (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GtkAllocation allocation;
  GtkAllocation child_allocation;
  gint scroll_value;
  gint max_scroll;

  gtk_widget_get_allocation (GTK_WIDGET (self), &allocation);
  gtk_widget_get_allocation (priv->children_box, &child_allocation);
  max_scroll = get_max_scroll (self);

  if (gtk_widget_get_realized (GTK_WIDGET (self)))
    {
      if (priv->invert_animation)
        {
          /* We only move the window to the left of the allocation, so negative values */
          if (priv->inverted)
            scroll_value = - priv->invert_animation_progress * max_scroll;
          else
            scroll_value = - (1 - priv->invert_animation_progress) * max_scroll;
        }
      else
        {
          scroll_value = 0;
        }

      gdk_window_move_resize (priv->bin_window,
                              scroll_value, 0,
                              child_allocation.width, child_allocation.height);
    }
}

static void
gtk_path_bar_container_size_allocate (GtkWidget     *widget,
                                      GtkAllocation *allocation)
{
  GtkPathBarContainer *self = GTK_PATH_BAR_CONTAINER (widget);
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GtkAllocation child_allocation;
  GtkRequestedSize *sizes;
  GtkRequisition minimum_size;
  GtkRequisition natural_size;

  sizes = g_newa (GtkRequestedSize, g_list_length (priv->children));

  gtk_widget_set_allocation (widget, allocation);

  idle_update_revealers (self);

  gtk_widget_get_preferred_size (priv->children_box, &minimum_size, &natural_size);
  child_allocation.x = 0;
  child_allocation.y = 0;
  child_allocation.width = MAX (minimum_size.width,
                                MIN (allocation->width, natural_size.width));
  child_allocation.height = MAX (minimum_size.height,
                                 MIN (allocation->height, natural_size.height));
  gtk_widget_size_allocate (priv->children_box, &child_allocation);

  update_scrolling (self);

  if (gtk_widget_get_realized (widget))
    {
      gdk_window_move_resize (priv->view_window,
                              allocation->x, allocation->y,
                              allocation->width, allocation->height);
      gdk_window_show (priv->view_window);
    }
}

gint
gtk_path_bar_container_get_unused_width (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GtkAllocation allocation;
  GtkAllocation child_allocation;

  gtk_widget_get_allocation (priv->children_box, &child_allocation);
  gtk_widget_get_allocation (GTK_WIDGET (self), &allocation);

  return allocation.width - child_allocation.width;
}


static void
finish_invert_animation (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GtkAllocation allocation;
  GtkRequisition available_size;
  GList *widgets_to_hide_copy;
  GList *l;

  /* Hide the revealers that need to be hidden now. */
  gtk_widget_get_allocation (GTK_WIDGET (self), &allocation);
  available_size.width = allocation.width;
  available_size.height = allocation.height;
  update_children_visibility (self, &available_size);

  widgets_to_hide_copy = g_list_copy (priv->widgets_to_hide);
  for (l = widgets_to_hide_copy; l != NULL; l = l->next)
    {
      GtkWidget *revealer;
      double animation_time;

      revealer = gtk_widget_get_parent (l->data);
      remove_opacity_classes (revealer);

      add_opacity_class (revealer, "pathbar-opacity-off");
      g_signal_handlers_disconnect_by_func (revealer, revealer_on_hide_completed, self);
      g_signal_connect (revealer, "notify::child-revealed", (GCallback) revealer_on_hide_completed, self);

      /* If the animation we just did was to the inverted state, we
       * have the revealers that need to be hidden out of the view, so
       * there's no point on animating them.
       * Not only that, we want to update the scroll in a way that takes
       * into account the state when the animation is finished, if not
       * we are going to show the animation of the revealers next time
       * the scroll is updated
       */
      animation_time =  priv->inverted ? 0 : REVEALER_ANIMATION_TIME;
      gtk_revealer_set_transition_duration (GTK_REVEALER (revealer),
                                            animation_time);
      gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), FALSE);
      gtk_revealer_set_transition_duration (GTK_REVEALER (revealer),
                                            REVEALER_ANIMATION_TIME);
    }

  priv->invert_animation = FALSE;
  priv->invert_animation_progress = 0;
  priv->invert_animation_initial_time = 0;
  gtk_widget_remove_tick_callback (priv->children_box,
                                   priv->invert_animation_tick_id);
  priv->invert_animation_tick_id = 0;
}


static gboolean
invert_animation_on_tick (GtkWidget     *widget,
                          GdkFrameClock *frame_clock,
                          gpointer       user_data)
{
  GtkPathBarContainer *self = GTK_PATH_BAR_CONTAINER (user_data);
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  guint64 elapsed;
  gint max_scroll;
  double animation_speed;
  GtkAllocation child_allocation;
  GtkAllocation allocation;

  /* Initialize the frame clock the first time this is called */
  if (priv->invert_animation_initial_time == 0)
    priv->invert_animation_initial_time = gdk_frame_clock_get_frame_time (frame_clock);

  gtk_widget_get_allocation (GTK_WIDGET (self), &allocation);
  gtk_widget_get_allocation (priv->children_box, &child_allocation);

  max_scroll = get_max_scroll (self);
  if (!max_scroll)
    return TRUE;

  /* If there are several items the animation can take some time, so let's limit
   * it to some extend
   */
  if (max_scroll / INVERT_ANIMATION_SPEED > INVERT_ANIMATION_MAX_TIME)
    animation_speed = max_scroll / INVERT_ANIMATION_MAX_TIME;
  else
    animation_speed = INVERT_ANIMATION_SPEED;

  elapsed = gdk_frame_clock_get_frame_time (frame_clock) - priv->invert_animation_initial_time;
  priv->invert_animation_progress = MIN (1, elapsed * animation_speed / (1000. * max_scroll));
  g_print ("################animation progres %d %d %f %f\n", gtk_widget_get_allocated_width (GTK_WIDGET (self)), max_scroll, elapsed / 1000., priv->invert_animation_progress);
  update_scrolling (self);

  if (priv->invert_animation_progress >= 1)
    {
      finish_invert_animation (self);

      return FALSE;
    }

  return TRUE;
}

static void
start_invert_animation (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GList *child;

  if (priv->invert_animation)
    finish_invert_animation (self);

  priv->invert_animation = TRUE;
  priv->invert_animation_progress = 0;
  priv->invert_animation_initial_children_width = gtk_widget_get_allocated_width (GTK_WIDGET (self));

  for (child = priv->children; child != NULL; child = child->next)
    {
      GtkWidget *revealer;

      revealer = gtk_widget_get_parent (GTK_WIDGET (child->data));

      remove_opacity_classes (revealer);
      if (!gtk_revealer_get_child_revealed (GTK_REVEALER (revealer)))
        add_opacity_class (revealer, "pathbar-opacity-on");

      gtk_revealer_set_transition_duration (GTK_REVEALER (revealer), 0);
      gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), TRUE);
      gtk_revealer_set_transition_duration (GTK_REVEALER (revealer), REVEALER_ANIMATION_TIME);
    }

  priv->invert_animation_tick_id = gtk_widget_add_tick_callback (priv->children_box,
                                                                 (GtkTickCallback) invert_animation_on_tick,
                                                                 self, NULL);
}

static void
gtk_path_bar_container_get_preferred_width (GtkWidget *widget,
                                            gint      *minimum_width,
                                            gint      *natural_width)
{
  GtkPathBarContainer *self = GTK_PATH_BAR_CONTAINER (widget);
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  gint child_minimum_width;
  gint child_natural_width;
  GList *child;
  gint n_visible_children;
  gboolean have_min = FALSE;
  GList *children;

  *minimum_width = 0;
  *natural_width = 0;

  children = g_list_copy (priv->children);
  if (priv->inverted)
    children = g_list_reverse (children);

  n_visible_children = 0;
  for (child = children; child != NULL; child = child->next)
    {
      if (!gtk_widget_is_visible (child->data))
        continue;

      ++n_visible_children;
      gtk_widget_get_preferred_width (child->data, &child_minimum_width, &child_natural_width);
      /* Minimum is a minimum of the first visible child */
      if (!have_min)
        {
          *minimum_width = child_minimum_width;
          have_min = TRUE;
        }
      /* Natural is a sum of all visible children */
      *natural_width += child_natural_width;
    }

  g_list_free (children);
}

static void
gtk_path_bar_container_get_preferred_width_for_height (GtkWidget *widget,
                                                      gint       height,
                                                      gint      *minimum_width_out,
                                                      gint      *natural_width_out)
{
  gtk_path_bar_container_get_preferred_width (widget, minimum_width_out, natural_width_out);
}

static void
gtk_path_bar_container_get_preferred_height (GtkWidget *widget,
                                            gint      *minimum_height,
                                            gint      *natural_height)
{
  GtkPathBarContainer *children_box = GTK_PATH_BAR_CONTAINER (widget);
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (children_box);
  gint child_minimum_height;
  gint child_natural_height;
  GList *child;

  *minimum_height = 0;
  *natural_height = 0;
  for (child = priv->children; child != NULL; child = child->next)
    {
      if (!gtk_widget_is_visible (child->data))
        continue;

      gtk_widget_get_preferred_height (child->data, &child_minimum_height, &child_natural_height);
      *minimum_height = MAX (*minimum_height, child_minimum_height);
      *natural_height = MAX (*natural_height, child_natural_height);
    }
}

static GtkSizeRequestMode
gtk_path_bar_container_get_request_mode (GtkWidget *self)
{
  return GTK_SIZE_REQUEST_WIDTH_FOR_HEIGHT;
}

static void
gtk_path_bar_container_container_add (GtkContainer *container,
                              GtkWidget    *child)
{
  g_return_if_fail (child != NULL);

  g_error ("Path bar cannot add children. Use the path bar API instead");

  return;
}

static void
gtk_path_bar_container_container_remove (GtkContainer *container,
                                 GtkWidget    *child)
{
  g_return_if_fail (child != NULL);

  //g_error ("Path bar cannot remove children. Use the path bar API instead");

  return;
}

static void
gtk_path_bar_container_unrealize (GtkWidget *widget)
{
  GtkPathBarContainer *self = GTK_PATH_BAR_CONTAINER (widget);
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);

  gtk_widget_unregister_window (widget, priv->bin_window);
  gdk_window_destroy (priv->bin_window);
  priv->view_window = NULL;

  GTK_WIDGET_CLASS (gtk_path_bar_container_parent_class)->unrealize (widget);
}

static void
gtk_path_bar_container_realize (GtkWidget *widget)
{
  GtkPathBarContainer *self = GTK_PATH_BAR_CONTAINER (widget);
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GtkAllocation allocation;
  GdkWindowAttr attributes = { 0 };
  GdkWindowAttributesType attributes_mask;
  GtkRequisition children_used_min_size;
  GtkRequisition children_used_nat_size;
  GtkRequisition available_size;
  gboolean overflows;
  gint children_used_width;

  gtk_widget_set_realized (widget, TRUE);

  gtk_widget_get_allocation (widget, &allocation);

  attributes.x = allocation.x;
  attributes.y = allocation.y;
  attributes.width = allocation.width;
  attributes.height = allocation.height;
  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.wclass = GDK_INPUT_OUTPUT;
  attributes.visual = gtk_widget_get_visual (widget);
  attributes.event_mask = gtk_widget_get_events (widget);
  attributes_mask = (GDK_WA_X | GDK_WA_Y) | GDK_WA_VISUAL;

  priv->view_window = gdk_window_new (gtk_widget_get_parent_window (GTK_WIDGET (self)),
                                      &attributes, attributes_mask);
  gtk_widget_set_window (widget, priv->view_window);
  gtk_widget_register_window (widget, priv->view_window);

  available_size.width = allocation.width;
  available_size.height = allocation.height;
  overflows = get_children_preferred_size_for_requisition (self, &available_size,
                                                           priv->inverted,
                                                           &children_used_min_size,
                                                           &children_used_nat_size);

  children_used_width = overflows ? MAX (children_used_min_size.width, allocation.width) :
                                    children_used_nat_size.width;
  attributes.x = 0;
  attributes.y = 0;
  attributes.width = children_used_width;
  attributes.height = children_used_nat_size.height;

  priv->bin_window = gdk_window_new (priv->view_window, &attributes,
                                     attributes_mask);
  gtk_widget_register_window (widget, priv->bin_window);

  gtk_widget_set_parent_window (priv->children_box, priv->bin_window);

  gdk_window_show (priv->bin_window);
  gdk_window_show (priv->view_window);
  gtk_widget_show_all (priv->children_box);
}

static gboolean
gtk_path_bar_container_draw (GtkWidget *widget,
                     cairo_t   *cr)
{
  GtkPathBarContainer *self = GTK_PATH_BAR_CONTAINER (widget);
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);

  if (gtk_cairo_should_draw_window (cr, priv->bin_window))
    {
      GTK_WIDGET_CLASS (gtk_path_bar_container_parent_class)->draw (widget, cr);
    }

  return GDK_EVENT_PROPAGATE;
}

static gboolean
real_get_preferred_size_for_requisition (GtkWidget      *widget,
                                         GtkRequisition *available_size,
                                         GtkRequisition *minimum_size,
                                         GtkRequisition *natural_size)
{
  GtkPathBarContainer *self = GTK_PATH_BAR_CONTAINER (widget);
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);

  return  get_children_preferred_size_for_requisition (self, available_size,
                                                       priv->inverted,
                                                       minimum_size,
                                                       natural_size);
}

gboolean
gtk_path_bar_container_get_preferred_size_for_requisition (GtkWidget      *widget,
                                                           GtkRequisition *available_size,
                                                           GtkRequisition *minimum_size,
                                                           GtkRequisition *natural_size)
{

  return real_get_preferred_size_for_requisition (widget,
                                                  available_size,
                                                  minimum_size,
                                                  natural_size);
}

static void
gtk_path_bar_container_init (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);

  gtk_widget_set_has_window (GTK_WIDGET (self), TRUE);
  gtk_widget_set_redraw_on_allocate (GTK_WIDGET (self), TRUE);

  priv->children_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_parent_window (priv->children_box, priv->bin_window);
  GTK_CONTAINER_CLASS (gtk_path_bar_container_parent_class)->add (GTK_CONTAINER (self), priv->children_box);

  gtk_widget_show (priv->children_box);

  priv->invert_animation = FALSE;
  priv->inverted = FALSE;
  priv->invert_animation_tick_id = 0;
  priv->widgets_to_hide = NULL;
  priv->widgets_to_show = NULL;
  priv->widgets_to_remove = NULL;
}

static void
gtk_path_bar_container_class_init (GtkPathBarContainerClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);
  GtkContainerClass *container_class = GTK_CONTAINER_CLASS (class);

  object_class->set_property = gtk_path_bar_container_set_property;
  object_class->get_property = gtk_path_bar_container_get_property;

  widget_class->size_allocate = gtk_path_bar_container_size_allocate;
  widget_class->get_preferred_width = gtk_path_bar_container_get_preferred_width;
  widget_class->get_preferred_height = gtk_path_bar_container_get_preferred_height;
  widget_class->get_preferred_width_for_height = gtk_path_bar_container_get_preferred_width_for_height;
  widget_class->get_request_mode = gtk_path_bar_container_get_request_mode;
  widget_class->realize = gtk_path_bar_container_realize;
  widget_class->unrealize = gtk_path_bar_container_unrealize;
  widget_class->draw = gtk_path_bar_container_draw;

  container_class->add = gtk_path_bar_container_container_add;
  container_class->remove = gtk_path_bar_container_container_remove;

  class->get_preferred_size_for_requisition = real_get_preferred_size_for_requisition;

  path_bar_container_properties[PROP_INVERTED] =
           g_param_spec_int ("inverted",
                             _("Direction of hiding children inverted"),
                             P_("If false the container will start hiding widgets from the end when there is not enough space, and the oposite in case inverted is true."),
                             0, G_MAXINT, 0,
                             G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, LAST_PROP, path_bar_container_properties);
}

/**
 * gtk_path_bar_container_new:
 *
 * Creates a new #GtkPathBarContainer.
 *
 * Returns: a new #GtkPathBarContainer.
 **/
GtkWidget *
gtk_path_bar_container_new (void)
{
  return g_object_new (GTK_TYPE_PATH_BAR_CONTAINER, NULL);
}

void
gtk_path_bar_container_adapt_to_size (GtkPathBarContainer *self,
                                      GtkRequisition      *available_size)
{
  update_children_visibility (self, available_size);
  idle_update_revealers (self);
}

void
gtk_path_bar_container_set_inverted (GtkPathBarContainer *self,
                                     gboolean             inverted)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  g_return_if_fail (GTK_IS_PATH_BAR_CONTAINER (self));

  if (priv->inverted != inverted)
    {
      priv->inverted = inverted != FALSE;

      g_object_notify (G_OBJECT (self), "inverted");

      if (_gtk_widget_get_mapped (GTK_WIDGET (self)))
        start_invert_animation (self);
      gtk_widget_queue_resize (GTK_WIDGET (self));
    }
}

GList *
gtk_path_bar_container_get_children (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GList *children = NULL;
  GList *l;

  g_return_val_if_fail (GTK_IS_PATH_BAR_CONTAINER (self), NULL);

  for (l = priv->children; l != NULL; l = l->next)
    {
      if (!g_list_find (priv->widgets_to_remove, l->data))
        children = g_list_append (children, l->data);
    }

  return children;
}

void
gtk_path_bar_container_remove_all_children (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);

  g_return_if_fail (GTK_IS_PATH_BAR_CONTAINER (self));

  gtk_container_foreach (GTK_CONTAINER (priv->children_box),
                         (GtkCallback) gtk_widget_destroy, NULL);

  g_list_free (priv->widgets_to_remove);
  priv->widgets_to_remove = NULL;

  g_list_free (priv->children);
  priv->children = NULL;
}

gboolean
gtk_path_bar_container_get_inverted (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  g_return_val_if_fail (GTK_IS_PATH_BAR_CONTAINER (self), 0);

  priv = gtk_path_bar_container_get_instance_private (self);

  return priv->inverted;
}

GList *
gtk_path_bar_container_get_overflow_children (GtkPathBarContainer *self)
{
  GtkPathBarContainerPrivate *priv = gtk_path_bar_container_get_instance_private (self);
  GList *result = NULL;
  GList *l;

  g_return_val_if_fail (GTK_IS_PATH_BAR_CONTAINER (self), 0);

  priv = gtk_path_bar_container_get_instance_private (self);

  for (l = priv->children; l != NULL; l = l->next)
    if (gtk_widget_is_visible (l->data) && !gtk_widget_get_child_visible (l->data))
      result = g_list_append (result, l->data);

  return result;
}
