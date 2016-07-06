/* GTK - The GIMP Toolkit
 * gtkprintoperation-unix.c: Print Operation Details for Unix 
 *                           and Unix-like platforms
 * Copyright (C) 2006, Red Hat, Inc.
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

#include "config.h"

#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <cairo-pdf.h>
#include <cairo-ps.h>

#include <gio/gunixfdlist.h>

#include "gtkprintoperation-private.h"
#include "gtkprintoperation-portal.h"
#include "gtkprintsettings.h"
#include "gtkpagesetup.h"
#include "gtkprintbackend.h"
#include "gtkshow.h"
#include "printportal.h"

typedef struct {
  GtkPrintOperation *op;
  GXdpPrint *proxy;
  guint print_response_signal_id;
  gboolean do_print;
  GtkPrintOperationResult result;
  GtkPrintOperationPrintFunc print_cb;
  GtkWindow *parent;
  GMainLoop *loop;
  guint32 token;
  GDestroyNotify destroy;
} PortalData;

static void
portal_data_free (gpointer data)
{
  PortalData *portal = data;

  g_object_unref (portal->op);
  g_object_unref (portal->proxy);
  if (portal->loop)
    g_main_loop_unref (portal->loop);

  g_free (portal);
}

static GXdpPrint *
get_portal_proxy (void)
{
  return gxdp_print_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                            G_DBUS_PROXY_FLAGS_NONE,
                                            "org.freedesktop.portal.Desktop",
                                            "/org/freedesktop/portal/desktop",
                                            NULL, NULL);
}

typedef struct {
  GXdpPrint *proxy;
  GtkPrintJob *job;
  guint32 token;
  cairo_surface_t *surface;
  GMainLoop *loop;
  gboolean file_written;
} GtkPrintOperationPortal;

static void
op_portal_free (GtkPrintOperationPortal *op_portal)
{
  g_clear_object (&op_portal->proxy);
  g_clear_object (&op_portal->job);
  if (op_portal->loop)
    g_main_loop_unref (op_portal->loop);
  g_free (op_portal);
}

static void
portal_start_page (GtkPrintOperation *op,
                   GtkPrintContext   *print_context,
                   GtkPageSetup      *page_setup)
{
  GtkPrintOperationPortal *op_portal = op->priv->platform_data;
  GtkPaperSize *paper_size;
  cairo_surface_type_t type;
  gdouble w, h;

  paper_size = gtk_page_setup_get_paper_size (page_setup);

  w = gtk_paper_size_get_width (paper_size, GTK_UNIT_POINTS);
  h = gtk_paper_size_get_height (paper_size, GTK_UNIT_POINTS);

  type = cairo_surface_get_type (op_portal->surface);

  if ((op->priv->manual_number_up < 2) ||
      (op->priv->page_position % op->priv->manual_number_up == 0))
    {
      if (type == CAIRO_SURFACE_TYPE_PS)
        {
          cairo_ps_surface_set_size (op_portal->surface, w, h);
          cairo_ps_surface_dsc_begin_page_setup (op_portal->surface);
          switch (gtk_page_setup_get_orientation (page_setup))
            {
              case GTK_PAGE_ORIENTATION_PORTRAIT:
              case GTK_PAGE_ORIENTATION_REVERSE_PORTRAIT:
                cairo_ps_surface_dsc_comment (op_portal->surface, "%%PageOrientation: Portrait");
                break;

              case GTK_PAGE_ORIENTATION_LANDSCAPE:
              case GTK_PAGE_ORIENTATION_REVERSE_LANDSCAPE:
                cairo_ps_surface_dsc_comment (op_portal->surface, "%%PageOrientation: Landscape");
                break;
            }
         }
      else if (type == CAIRO_SURFACE_TYPE_PDF)
        {
          if (!op->priv->manual_orientation)
            {
              w = gtk_page_setup_get_paper_width (page_setup, GTK_UNIT_POINTS);
              h = gtk_page_setup_get_paper_height (page_setup, GTK_UNIT_POINTS);
            }
          cairo_pdf_surface_set_size (op_portal->surface, w, h);
        }
    }
}

static void
portal_end_page (GtkPrintOperation *op,
                 GtkPrintContext   *print_context)
{
  cairo_t *cr;

  cr = gtk_print_context_get_cairo_context (print_context);

  if ((op->priv->manual_number_up < 2) ||
      ((op->priv->page_position + 1) % op->priv->manual_number_up == 0) ||
      (op->priv->page_position == op->priv->nr_of_pages_to_print - 1))
    cairo_show_page (cr);
}

static void
print_file_done (GObject *source,
                 GAsyncResult *result,
                 gpointer data)
{
  GtkPrintOperation *op = data;
  GtkPrintOperationPortal *op_portal = op->priv->platform_data;
  GError *error = NULL;
  char *handle = NULL;

  if (!gxdp_print_call_print_file_finish (op_portal->proxy,
                                          &handle,
                                          NULL,
                                          result,
                                          &error))
    {
      if (op->priv->error == NULL)
        op->priv->error = g_error_copy (error);
      g_warning ("Print file failed: %s", error->message);
      g_error_free (error);
    }

  if (op_portal->loop)
    g_main_loop_quit (op_portal->loop);

  g_object_unref (op);
}

static void
portal_job_complete (GtkPrintJob  *job,
                     gpointer      data,
                     const GError *error)
{
  GtkPrintOperation *op = data;
  GtkPrintOperationPortal *op_portal = op->priv->platform_data;
  GtkPrintSettings *settings;
  const char *uri;
  const char *filename;
  int fd, idx;
  GVariantBuilder opt_builder;
  GUnixFDList *fd_list;

  if (error != NULL && op->priv->error == NULL)
    {
      g_warning ("Print job failed: %s", error->message);
      op->priv->error = g_error_copy (error);
    }

  op_portal->file_written = TRUE;

  settings = gtk_print_job_get_settings (job);
  uri = gtk_print_settings_get (settings, GTK_PRINT_SETTINGS_OUTPUT_URI);
  g_assert (g_str_has_prefix (uri, "file://"));
  filename = uri + strlen ("file://");

  fd = open (filename, O_RDONLY|O_CLOEXEC);
  fd_list = g_unix_fd_list_new ();
  idx = g_unix_fd_list_append (fd_list, fd, NULL);
  close (fd);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&opt_builder, "{sv}",  "token", g_variant_new_uint32 (op_portal->token));

  gxdp_print_call_print_file (op_portal->proxy,
                              "",
                              "",
                              g_variant_new_handle (idx),
                              g_variant_builder_end (&opt_builder),
                              fd_list,
                              NULL,
                              print_file_done,
                              op);
  g_object_unref (fd_list);
}

static void
portal_end_run (GtkPrintOperation *op,
                gboolean           wait,
                gboolean           cancelled)
{
  GtkPrintOperationPortal *op_portal = op->priv->platform_data;

  cairo_surface_finish (op_portal->surface);

  if (cancelled)
    return;

  if (wait)
    op_portal->loop = g_main_loop_new (NULL, FALSE);

  /* TODO: Check for error */
  if (op_portal->job != NULL)
    {
      g_object_ref (op);
      gtk_print_job_send (op_portal->job, portal_job_complete, op, NULL);
    }

  if (wait)
    {
      g_object_ref (op);
      if (!op_portal->file_written)
        {
          gdk_threads_leave ();
          g_main_loop_run (op_portal->loop);
          gdk_threads_enter ();
        }
      g_object_unref (op);
    }
}

static void
finish_print (PortalData        *portal,
              GtkPrinter        *printer,
              GtkPageSetup      *page_setup,
              GtkPrintSettings  *settings)
{
  GtkPrintOperation *op = portal->op;
  GtkPrintOperationPrivate *priv = op->priv;
  GtkPrintJob *job;
  GtkPrintOperationPortal *op_portal;
  cairo_t *cr;


  if (portal->do_print)
    {
      gtk_print_operation_set_print_settings (op, settings);
      priv->print_context = _gtk_print_context_new (op);

      _gtk_print_context_set_hard_margins (priv->print_context, 0, 0, 0, 0);

      gtk_print_operation_set_default_page_setup (op, page_setup);
      _gtk_print_context_set_page_setup (priv->print_context, page_setup);

      op_portal = g_new0 (GtkPrintOperationPortal, 1);
      priv->platform_data = op_portal;
      priv->free_platform_data = (GDestroyNotify) op_portal_free;

      priv->start_page = portal_start_page;
      priv->end_page = portal_end_page;
      priv->end_run = portal_end_run;

      job = gtk_print_job_new (priv->job_name, printer, settings, page_setup);
      op_portal->job = job;

      op_portal->proxy = g_object_ref (portal->proxy);
      op_portal->token = portal->token;

      op_portal->surface = gtk_print_job_get_surface (job, &priv->error);
      if (op_portal->surface == NULL)
        {
          portal->result = GTK_PRINT_OPERATION_RESULT_ERROR;
          portal->do_print = FALSE;
          goto out;
        }

      cr = cairo_create (op_portal->surface);
      gtk_print_context_set_cairo_context (priv->print_context, cr, 72, 72);
      cairo_destroy (cr);

      priv->print_pages = gtk_print_job_get_pages (job);
      priv->page_ranges = gtk_print_job_get_page_ranges (job, &priv->num_page_ranges);
      priv->manual_num_copies = gtk_print_job_get_num_copies (job);
      priv->manual_collation = gtk_print_job_get_collate (job);
      priv->manual_reverse = gtk_print_job_get_reverse (job);
      priv->manual_page_set = gtk_print_job_get_page_set (job);
      priv->manual_scale = gtk_print_job_get_scale (job);
      priv->manual_orientation = gtk_print_job_get_rotate (job);
      priv->manual_number_up = gtk_print_job_get_n_up (job);
      priv->manual_number_up_layout = gtk_print_job_get_n_up_layout (job);
    }

out:
  if (portal->print_cb)
    portal->print_cb (op, portal->parent, portal->do_print, portal->result);

  if (portal->destroy)
    portal->destroy (portal);
}

static GtkPrinter *
find_file_printer (void)
{
  GList *backends, *l, *printers;
  GtkPrinter *printer;

  printer = NULL;

  backends = gtk_print_backend_load_modules ();
  for (l = backends; l; l = l->next)
    {
      GtkPrintBackend *backend = l->data;
      if (strcmp (G_OBJECT_TYPE_NAME (backend), "GtkPrintBackendFile") == 0)
        {
          printers = gtk_print_backend_get_printer_list (backend);
          printer = printers->data;
          g_list_free (printers);
        }
    }
  g_list_free (backends);

  return printer;
}

static void
print_response (GDBusConnection *connection,
                const char      *sender_name,
                const char      *object_path,
                const char      *interface_name,
                const char      *signal_name,
                GVariant        *parameters,
                gpointer         data)
{
  PortalData *portal = data;
  guint32 response;
  GVariant *options;

  if (portal->print_response_signal_id != 0)
    {
      g_dbus_connection_signal_unsubscribe (connection,
                                            portal->print_response_signal_id);
      portal->print_response_signal_id = 0;
    }

  g_variant_get (parameters, "(u@a{sv})", &response, &options);

  portal->do_print = (response == 0);

  if (portal->do_print)
    {
      portal->result = GTK_PRINT_OPERATION_RESULT_APPLY;
      GVariant *v;
      GtkPrintSettings *settings;
      GtkPageSetup *page_setup;
      GtkPrinter *printer;
      char *filename;
      char *uri;
      int fd;

      v = g_variant_lookup_value (options, "settings", G_VARIANT_TYPE_VARDICT);
      settings = gtk_print_settings_new_from_gvariant (v);
      g_variant_unref (v);

      v = g_variant_lookup_value (options, "page-setup", G_VARIANT_TYPE_VARDICT);
      page_setup = gtk_page_setup_new_from_gvariant (v);
      g_variant_unref (v);

      g_variant_lookup (options, "token", "u", &portal->token);

      printer = find_file_printer ();

      fd = g_file_open_tmp ("gtkprintXXXXXX", &filename, NULL);
      uri = g_strconcat ("file://", filename, NULL);
      gtk_print_settings_set (settings, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
      g_free (uri);
      close (fd);

      finish_print (portal, printer, page_setup, settings);
      g_free (filename);
    }
  else
    portal->result = GTK_PRINT_OPERATION_RESULT_CANCEL;

  if (portal->loop)
    g_main_loop_quit (portal->loop);
}

static void
print_called (GObject *source,
              GAsyncResult *result,
              gpointer data)
{
  PortalData *portal = data;
  GError *error = NULL;
  char *handle = NULL;

  if (!gxdp_print_call_print_finish (portal->proxy, &handle, result, &error))
    {
      g_warning ("Error: %s", error->message);
      g_error_free (error);
      g_main_loop_quit (portal->loop);
      return;
    }

  portal->print_response_signal_id =
    g_dbus_connection_signal_subscribe (g_dbus_proxy_get_connection (G_DBUS_PROXY (portal->proxy)),
                                        "org.freedesktop.portal.Desktop",
                                        "org.freedesktop.portal.Request",
                                        "Response",
                                        handle,
                                        NULL,
                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                        print_response,
                                        portal, NULL);

  g_free (handle);
}

GtkPrintOperationResult
gtk_print_operation_portal_run_dialog (GtkPrintOperation *op,
                                       gboolean           show_dialog,
                                       GtkWindow         *parent,
                                       gboolean          *do_print)
{
  GtkPrintOperationPrivate *priv = op->priv;
  GVariant *settings;
  GVariant *setup;
  GVariantBuilder opt_builder;
  GVariant *options;
  guint signal_id;
  PortalData *portal;
  GtkPrintOperationResult result;

  portal = g_new0 (PortalData, 1);
  portal->proxy = get_portal_proxy ();
  portal->op = g_object_ref (op);
  portal->parent = parent;
  portal->result = GTK_PRINT_OPERATION_RESULT_CANCEL;
  portal->print_cb = NULL;
  portal->destroy = NULL;

  signal_id = g_signal_lookup ("create-custom-widget", GTK_TYPE_PRINT_OPERATION);
  if (g_signal_has_handler_pending (op, signal_id, 0, TRUE))
    g_warning ("GtkPrintOperation::create-custom-widget not supported with portal");

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  options = g_variant_builder_end (&opt_builder);

  if (priv->print_settings)
    settings = gtk_print_settings_to_gvariant (priv->print_settings);
  else
    {
      GVariantBuilder builder;
      g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
      settings = g_variant_builder_end (&builder);
    }

  if (priv->default_page_setup)
    setup = gtk_page_setup_to_gvariant (priv->default_page_setup);
  else
    {
      GtkPageSetup *page_setup = gtk_page_setup_new ();
      setup = gtk_page_setup_to_gvariant (page_setup);
      g_object_unref (page_setup);
    }

  portal->loop = g_main_loop_new (NULL, FALSE);

  gxdp_print_call_print (portal->proxy,
                         "",
                         "Print Test",
                         settings,
                         setup,
                         options,
                         NULL,
                         print_called,
                         portal);

  gdk_threads_leave ();
  g_main_loop_run (portal->loop);
  gdk_threads_enter ();

  *do_print = portal->do_print;
  result = portal->result;

  portal_data_free (portal);

  return result;
}

void
gtk_print_operation_portal_run_dialog_async (GtkPrintOperation          *op,
                                             gboolean                    show_dialog,
                                             GtkWindow                  *parent,
                                             GtkPrintOperationPrintFunc  print_cb)
{
  GtkPrintOperationPrivate *priv = op->priv;
  GVariant *settings;
  GVariant *setup;
  GVariantBuilder opt_builder;
  GVariant *options;
  guint signal_id;
  PortalData *portal;

  portal = g_new0 (PortalData, 1);
  portal->proxy = get_portal_proxy ();
  portal->op = g_object_ref (op);
  portal->parent = parent;
  portal->result = GTK_PRINT_OPERATION_RESULT_CANCEL;
  portal->print_cb = print_cb;
  portal->destroy = portal_data_free;

  signal_id = g_signal_lookup ("create-custom-widget", GTK_TYPE_PRINT_OPERATION);
  if (g_signal_has_handler_pending (op, signal_id, 0, TRUE))
    g_warning ("GtkPrintOperation::create-custom-widget not supported with portal");

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  options = g_variant_builder_end (&opt_builder);

  if (priv->print_settings)
    settings = gtk_print_settings_to_gvariant (priv->print_settings);
  else
    {
      GVariantBuilder builder;
      g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
      settings = g_variant_builder_end (&builder);
    }

  if (priv->default_page_setup)
    setup = gtk_page_setup_to_gvariant (priv->default_page_setup);
  else
    {
      GtkPageSetup *page_setup = gtk_page_setup_new ();
      setup = gtk_page_setup_to_gvariant (page_setup);
      g_object_unref (page_setup);
    }

  gxdp_print_call_print (portal->proxy,
                         "",
                         "Print Test",
                         settings,
                         setup,
                         options,
                         NULL,
                         print_called,
                         portal);
}

void
gtk_print_operation_portal_launch_preview (GtkPrintOperation *op,
                                           cairo_surface_t   *surface,
                                           GtkWindow         *parent,
                                           const char        *filename)
{
  char *uri;

  uri = g_strconcat ("file://", filename, NULL);
  gtk_show_uri_on_window (parent, uri, GDK_CURRENT_TIME, NULL);
  g_free (uri);
}
