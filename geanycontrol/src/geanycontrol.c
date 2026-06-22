/*
 * geanycontrol.c — Geany plugin: Unix socket + signal IPC for agent UI control
 *
 * Exposes a line-delimited command protocol on a Unix domain socket at
 * ~/.config/geany/geanycontrol.sock and registers matching GLib signals on
 * geany->object for in-process inter-plugin use.
 *
 * Commands (one per line, \n terminated; server replies ok\n or error: …\n):
 *
 *   open-file <path>
 *   close-file <path>
 *   save-file <path>
 *   save-all
 *   scroll-to-line <path>:<line>   (line is 1-based)
 *   get-current-file
 *   list-open-files
 *   activate-menu-item <label>     (case-insensitive, searches Tools menu)
 *   refresh                        (emits geanycontrol-refresh signal)
 *   ping
 *
 * Inter-plugin IPC — emit on geany->object:
 *   "geanycontrol-open-file"          (gchar *path)
 *   "geanycontrol-close-file"         (gchar *path)
 *   "geanycontrol-save-file"          (gchar *path)
 *   "geanycontrol-save-all"           (no args)
 *   "geanycontrol-scroll-to-line"     (gchar *"path:line")
 *   "geanycontrol-activate-menu-item" (gchar *label)
 *   "geanycontrol-refresh"            (no args)
 *
 * Copyright 2025 teknopaul
 * License: GPL-2.0-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <gtk/gtk.h>

#include <geanyplugin.h>


/* ------------------------------------------------------------------ */

GeanyPlugin  *geany_plugin;
GeanyData    *geany_data;

static GSocketService *socket_service = NULL;
static gchar          *socket_path    = NULL;


/* ------------------------------------------------------------------ */
/* Connection context                                                  */

typedef struct {
    GDataInputStream  *reader;
    GOutputStream     *out;
    GSocketConnection *conn;
} ConnCtx;

static void conn_ctx_free(ConnCtx *ctx)
{
    g_object_unref(ctx->reader);
    g_object_unref(ctx->out);
    g_object_unref(ctx->conn);
    g_free(ctx);
}


/* ------------------------------------------------------------------ */
/* File operation helper                                               */

typedef struct {
    gchar *path;
    gint   line;   /* -1 if not applicable */
} FileOpData;

static void file_op_data_free(gpointer p)
{
    FileOpData *d = p;
    g_free(d->path);
    g_free(d);
}


/* ------------------------------------------------------------------ */
/* Phase 2 — document operations                                       */

static gboolean idle_open_file(gpointer data)
{
    FileOpData *d = data;
    document_open_file(d->path, FALSE, NULL, NULL);
    file_op_data_free(d);
    return G_SOURCE_REMOVE;
}

static gboolean idle_close_file(gpointer data)
{
    FileOpData *d = data;
    guint i;
    GeanyDocument *doc;

    foreach_document(i) {
        doc = documents[i];
        if (doc->file_name && strcmp(doc->file_name, d->path) == 0) {
            document_close(doc);
            break;
        }
    }
    file_op_data_free(d);
    return G_SOURCE_REMOVE;
}

static gboolean idle_save_file(gpointer data)
{
    FileOpData *d = data;
    guint i;
    GeanyDocument *doc;

    foreach_document(i) {
        doc = documents[i];
        if (doc->file_name && strcmp(doc->file_name, d->path) == 0) {
            document_save_file(doc, FALSE);
            break;
        }
    }
    file_op_data_free(d);
    return G_SOURCE_REMOVE;
}

static gboolean idle_save_all(G_GNUC_UNUSED gpointer data)
{
    guint i;
    GeanyDocument *doc;

    foreach_document(i) {
        doc = documents[i];
        if (doc->file_name && doc->changed)
            document_save_file(doc, FALSE);
    }
    return G_SOURCE_REMOVE;
}

static gchar *cmd_open_file(const gchar *path)
{
    FileOpData *d = g_new0(FileOpData, 1);
    d->path = g_strdup(path);
    d->line = -1;
    g_idle_add(idle_open_file, d);
    return g_strdup("ok\n");
}

static gchar *cmd_close_file(const gchar *path)
{
    FileOpData *d = g_new0(FileOpData, 1);
    d->path = g_strdup(path);
    d->line = -1;
    g_idle_add(idle_close_file, d);
    return g_strdup("ok\n");
}

static gchar *cmd_save_file(const gchar *path)
{
    FileOpData *d = g_new0(FileOpData, 1);
    d->path = g_strdup(path);
    d->line = -1;
    g_idle_add(idle_save_file, d);
    return g_strdup("ok\n");
}

static gchar *cmd_save_all(void)
{
    g_idle_add(idle_save_all, NULL);
    return g_strdup("ok\n");
}


/* ------------------------------------------------------------------ */
/* Phase 3 — scroll, query, list                                       */

static gchar *cmd_scroll_to_line(const gchar *arg)
{
    /* Split on last colon to handle paths that may contain colons */
    const gchar *colon = strrchr(arg, ':');
    if (!colon || colon == arg)
        return g_strdup("error: expected path:line\n");

    gchar *path = g_strndup(arg, (gsize)(colon - arg));
    gint   line = (gint)g_ascii_strtoll(colon + 1, NULL, 10) - 1; /* 0-based */

    GeanyDocument *doc = document_open_file(path, FALSE, NULL, NULL);
    g_free(path);
    if (!doc)
        return g_strdup("error: could not open file\n");
    if (line >= 0)
        sci_goto_line(doc->editor->sci, line, TRUE);
    return g_strdup("ok\n");
}

static gchar *cmd_get_current_file(void)
{
    GeanyDocument *doc = document_get_current();
    if (!doc || !doc->file_name)
        return g_strdup("none\nok\n");
    return g_strdup_printf("%s\nok\n", doc->file_name);
}

static gchar *cmd_list_open_files(void)
{
    GString *buf = g_string_new(NULL);
    guint i;
    GeanyDocument *doc;

    foreach_document(i) {
        doc = documents[i];
        if (doc->file_name)
            g_string_append_printf(buf, "%s\n", doc->file_name);
    }
    g_string_append(buf, "ok\n");
    return g_string_free(buf, FALSE);
}


/* ------------------------------------------------------------------ */
/* Phase 4 — menu item activation                                      */

typedef struct {
    const gchar *target;   /* lower-cased label to find */
    gboolean     found;
} MenuWalk;

static void walk_menu(GtkWidget *widget, gpointer data)
{
    MenuWalk *w = data;
    if (w->found) return;

    if (GTK_IS_MENU_ITEM(widget)) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
        if (GTK_IS_LABEL(child)) {
            const gchar *text  = gtk_label_get_text(GTK_LABEL(child));
            gchar       *lower = g_utf8_strdown(text, -1);
            if (strcmp(lower, w->target) == 0) {
                w->found = TRUE;
                gtk_menu_item_activate(GTK_MENU_ITEM(widget));
            }
            g_free(lower);
        }
        GtkWidget *sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(widget));
        if (sub)
            gtk_container_foreach(GTK_CONTAINER(sub), walk_menu, data);
    }
}

static gchar *cmd_activate_menu_item(const gchar *label)
{
    gchar    *lower = g_utf8_strdown(label, -1);
    MenuWalk  w     = { lower, FALSE };

    gtk_container_foreach(
        GTK_CONTAINER(geany_data->main_widgets->tools_menu),
        walk_menu, &w);

    g_free(lower);
    return w.found
        ? g_strdup("ok\n")
        : g_strdup("error: menu item not found\n");
}


/* ------------------------------------------------------------------ */
/* Phase 5 — refresh                                                   */

static gboolean idle_refresh(G_GNUC_UNUSED gpointer data)
{
    g_signal_emit_by_name(geany->object, "geanycontrol-refresh");
    return G_SOURCE_REMOVE;
}

static gchar *cmd_refresh(void)
{
    g_idle_add(idle_refresh, NULL);
    return g_strdup("ok\n");
}


/* ------------------------------------------------------------------ */
/* Command dispatch                                                    */

static gchar *dispatch_command(const gchar *line)
{
    if (!line || !*line)
        return g_strdup("error: empty command\n");

    if (g_str_has_prefix(line, "open-file "))
        return cmd_open_file(line + 10);
    if (g_str_has_prefix(line, "close-file "))
        return cmd_close_file(line + 11);
    if (g_str_has_prefix(line, "save-file "))
        return cmd_save_file(line + 10);
    if (strcmp(line, "save-all") == 0)
        return cmd_save_all();
    if (g_str_has_prefix(line, "scroll-to-line "))
        return cmd_scroll_to_line(line + 15);
    if (strcmp(line, "get-current-file") == 0)
        return cmd_get_current_file();
    if (strcmp(line, "list-open-files") == 0)
        return cmd_list_open_files();
    if (g_str_has_prefix(line, "activate-menu-item "))
        return cmd_activate_menu_item(line + 19);
    if (strcmp(line, "refresh") == 0)
        return cmd_refresh();
    if (strcmp(line, "ping") == 0)
        return g_strdup("ok\n");

    return g_strdup("error: unknown command\n");
}


/* ------------------------------------------------------------------ */
/* Async line reader                                                   */

static void on_read_line(GObject *src, GAsyncResult *res, gpointer data)
{
    ConnCtx *ctx  = data;
    GError  *err  = NULL;
    gchar   *line = g_data_input_stream_read_line_finish(
                        G_DATA_INPUT_STREAM(src), res, NULL, &err);

    if (!line) {
        conn_ctx_free(ctx);
        if (err) g_error_free(err);
        return;
    }

    gchar *reply = dispatch_command(g_strstrip(line));
    g_free(line);

    g_output_stream_write_all(ctx->out, reply, strlen(reply), NULL, NULL, NULL);
    g_free(reply);

    g_data_input_stream_read_line_async(ctx->reader, G_PRIORITY_DEFAULT, NULL,
                                        on_read_line, ctx);
}


/* ------------------------------------------------------------------ */
/* Incoming connection                                                 */

static gboolean on_incoming_connection(G_GNUC_UNUSED GSocketService    *svc,
                                       GSocketConnection *conn,
                                       G_GNUC_UNUSED GObject           *source,
                                       G_GNUC_UNUSED gpointer           udata)
{
    GInputStream     *in     = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream    *out    = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    GDataInputStream *reader = g_data_input_stream_new(in);

    ConnCtx *ctx = g_new0(ConnCtx, 1);
    ctx->reader  = reader;
    ctx->out     = g_object_ref(out);
    ctx->conn    = g_object_ref(conn);

    g_data_input_stream_read_line_async(reader, G_PRIORITY_DEFAULT, NULL,
                                        on_read_line, ctx);
    return TRUE;
}


/* ------------------------------------------------------------------ */
/* Signal registration                                                 */

static void gc_register_signals(void)
{
    GType obj_type = G_OBJECT_TYPE(geany->object);

    static const struct { const gchar *name; GType arg; } sigs[] = {
        { "geanycontrol-open-file",          G_TYPE_STRING },
        { "geanycontrol-close-file",         G_TYPE_STRING },
        { "geanycontrol-save-file",          G_TYPE_STRING },
        { "geanycontrol-save-all",           G_TYPE_NONE   },
        { "geanycontrol-scroll-to-line",     G_TYPE_STRING },
        { "geanycontrol-activate-menu-item", G_TYPE_STRING },
        { "geanycontrol-refresh",            G_TYPE_NONE   },
        { NULL, 0 }
    };

    for (gint i = 0; sigs[i].name; i++) {
        if (g_signal_lookup(sigs[i].name, obj_type))
            continue;
        if (sigs[i].arg == G_TYPE_NONE)
            g_signal_new(sigs[i].name, obj_type, G_SIGNAL_RUN_LAST,
                         0, NULL, NULL, NULL, G_TYPE_NONE, 0);
        else
            g_signal_new(sigs[i].name, obj_type, G_SIGNAL_RUN_LAST,
                         0, NULL, NULL, NULL, G_TYPE_NONE, 1, sigs[i].arg);
    }
}


/* ------------------------------------------------------------------ */
/* Signal handlers (in-process IPC)                                   */

static void on_signal_open_file(G_GNUC_UNUSED GObject *obj, const gchar *path,
                                G_GNUC_UNUSED gpointer data)
{
    gchar *r = cmd_open_file(path);
    g_free(r);
}

static void on_signal_close_file(G_GNUC_UNUSED GObject *obj, const gchar *path,
                                 G_GNUC_UNUSED gpointer data)
{
    gchar *r = cmd_close_file(path);
    g_free(r);
}

static void on_signal_save_file(G_GNUC_UNUSED GObject *obj, const gchar *path,
                                G_GNUC_UNUSED gpointer data)
{
    gchar *r = cmd_save_file(path);
    g_free(r);
}

static void on_signal_save_all(G_GNUC_UNUSED GObject *obj,
                               G_GNUC_UNUSED gpointer data)
{
    gchar *r = cmd_save_all();
    g_free(r);
}

static void on_signal_scroll_to_line(G_GNUC_UNUSED GObject *obj,
                                     const gchar *arg,
                                     G_GNUC_UNUSED gpointer data)
{
    gchar *r = cmd_scroll_to_line(arg);
    g_free(r);
}


/* ------------------------------------------------------------------ */
/* Plugin lifecycle                                                    */

static gboolean gc_init(GeanyPlugin *plugin,
                        gpointer data G_GNUC_UNUSED)
{
    geany_plugin = plugin;
    geany_data   = plugin->geany_data;

    socket_path = g_build_filename(geany->app->configdir,
                                   "geanycontrol.sock", NULL);
    g_unlink(socket_path);   /* remove stale socket from previous session */

    GSocketAddress *addr = g_unix_socket_address_new(socket_path);
    socket_service = g_socket_service_new();

    GError *err = NULL;
    g_socket_listener_add_address(G_SOCKET_LISTENER(socket_service),
                                  addr, G_SOCKET_TYPE_STREAM,
                                  G_SOCKET_PROTOCOL_DEFAULT,
                                  NULL, NULL, &err);
    g_object_unref(addr);

    if (err) {
        g_warning("geanycontrol: socket bind failed: %s", err->message);
        g_error_free(err);
        g_clear_object(&socket_service);
        g_free(socket_path);
        socket_path = NULL;
        return TRUE;
    }

    g_signal_connect(socket_service, "incoming",
                     G_CALLBACK(on_incoming_connection), NULL);
    g_socket_service_start(socket_service);

    gc_register_signals();

    plugin_signal_connect(plugin, geany->object, "geanycontrol-open-file", FALSE,
                          G_CALLBACK(on_signal_open_file), NULL);
    plugin_signal_connect(plugin, geany->object, "geanycontrol-close-file", FALSE,
                          G_CALLBACK(on_signal_close_file), NULL);
    plugin_signal_connect(plugin, geany->object, "geanycontrol-save-file", FALSE,
                          G_CALLBACK(on_signal_save_file), NULL);
    plugin_signal_connect(plugin, geany->object, "geanycontrol-save-all", FALSE,
                          G_CALLBACK(on_signal_save_all), NULL);
    plugin_signal_connect(plugin, geany->object, "geanycontrol-scroll-to-line", FALSE,
                          G_CALLBACK(on_signal_scroll_to_line), NULL);

    return TRUE;
}

static void gc_cleanup(GeanyPlugin *plugin G_GNUC_UNUSED,
                       gpointer data G_GNUC_UNUSED)
{
    if (socket_service) {
        g_socket_service_stop(socket_service);
        g_clear_object(&socket_service);
    }
    if (socket_path) {
        g_unlink(socket_path);
        g_free(socket_path);
        socket_path = NULL;
    }
}

G_MODULE_EXPORT void geany_load_module(GeanyPlugin *plugin)
{
    plugin->info->name        = "GeanyControl";
    plugin->info->description = "Unix socket + signal IPC for agent-driven UI control.";
    plugin->info->version     = "0.1";
    plugin->info->author      = "teknopaul";
    plugin->funcs->init       = gc_init;
    plugin->funcs->cleanup    = gc_cleanup;
    GEANY_PLUGIN_REGISTER(plugin, 235);
}
