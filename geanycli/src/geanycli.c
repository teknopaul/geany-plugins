/*
 * geanycli.c — Geany plugin: tabbed terminals in the message window
 *
 * Adds a "Terminal" tab to the message window.  Inside that tab a second
 * GtkNotebook holds multiple VTE terminal instances.  Each terminal opens
 * a login shell at the current project root (or $HOME when no project is
 * open).  If the shell exits it is automatically restarted.
 *
 * Inter-plugin IPC — two equivalent mechanisms are available:
 *
 *   1) GLib signal (no header required):
 *        g_signal_emit_by_name(geany->object,
 *                              "geanycli-run-command",
 *                              "make", FALSE);
 *
 *   2) Direct symbol call (resolves via GModule at runtime):
 *        typedef void (*GtRunCmdFn)(const gchar *, gboolean);
 *        GtRunCmdFn fn;
 *        if (g_module_symbol(other_plugin->module,
 *                            "geanycli_run_command",
 *                            (gpointer *)&fn))
 *            fn("make", FALSE);
 *
 *   Signal / function parameters:
 *     cmd     – shell command string to execute
 *     new_tab – TRUE  → open a fresh terminal tab then run cmd
 *               FALSE → run cmd in the currently active tab
 *
 * Copyright 2025 teknopaul
 * License: GPL-2.0-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <vte/vte.h>

#include <geanyplugin.h>

#include "geanycli.h"


/* ------------------------------------------------------------------ */

GeanyPlugin  *geany_plugin;
GeanyData    *geany_data;


/* ------------------------------------------------------------------ */
/* Per-tab state                                                       */

typedef struct {
    VteTerminal *vte;
    GtkWidget   *hbox;   /* terminal + scrollbar; used as the notebook page */
} TermTab;

/* Carries (tab, cmd) across the async spawn boundary */
typedef struct {
    TermTab *tab;
    gchar   *cmd;
} SpawnData;


/* ------------------------------------------------------------------ */
/* Module globals                                                      */

static GtkNotebook *term_nb      = NULL;  /* inner notebook with terminal pages */
static GtkWidget   *outer_widget = NULL;  /* the widget appended to msg notebook */
static gint         outer_idx    = -1;    /* page index in the message notebook   */
static gint         tab_counter  = 0;     /* monotonically-increasing label counter */
static gboolean     active       = FALSE; /* set FALSE during cleanup to stop restarts */

static GKeyFile  *tools_config    = NULL; /* merged user + project filetypetools.conf */
static GtkWidget *tools_menu_item = NULL; /* "File Tools" entry appended to Tools menu */


/* ------------------------------------------------------------------ */
/* Helpers                                                             */

static gchar *get_work_dir(void)
{
    GeanyApp *app = geany->app;
    if (app->project && app->project->base_path && *app->project->base_path)
        return g_strdup(app->project->base_path);
    return g_strdup(g_get_home_dir());
}

static void on_spawn_ready(VteTerminal *vte,
                           GPid         pid,
                           GError      *error,
                           gpointer     user_data)
{
    SpawnData *sd = user_data;
    if (!error && sd->cmd && *sd->cmd)
    {
        vte_terminal_feed_child(VTE_TERMINAL(vte), "\x15", 1);
        vte_terminal_feed_child(VTE_TERMINAL(vte), sd->cmd, -1);
        vte_terminal_feed_child(VTE_TERMINAL(vte), "\n", 1);
    }
    g_free(sd->cmd);
    g_free(sd);
}

static void spawn_shell(TermTab *tab, const gchar *cmd)
{
    const gchar *shell = g_getenv("SHELL");
    if (!shell || !*shell)
        shell = "/bin/sh";

    gchar *argv[2] = { (gchar *)shell, NULL };

    gchar *wd = get_work_dir();

    const gchar *exclude[] = { "COLUMNS", "LINES", "TERM", "TERM_PROGRAM", NULL };
    gchar **env = utils_copy_environment(exclude, "TERM", "xterm-256color", NULL);

    SpawnData *sd = g_new0(SpawnData, 1);
    sd->tab = tab;
    sd->cmd = g_strdup(cmd);

    vte_terminal_spawn_async(tab->vte,
                             VTE_PTY_DEFAULT,
                             wd,
                             argv,
                             env,
                             G_SPAWN_SEARCH_PATH,
                             NULL, NULL, NULL,
                             -1, NULL,
                             on_spawn_ready, sd);
    g_strfreev(env);
    g_free(wd);
}


/* ------------------------------------------------------------------ */
/* Signal handlers                                                     */

static void on_child_exited(VteTerminal *vte,
                             G_GNUC_UNUSED gint status,
                             gpointer user_data)
{
    if (!active)
        return;
    TermTab *tab = user_data;
    vte_terminal_feed(vte, "\r\n[process exited — restarting shell]\r\n", -1);
    spawn_shell(tab, NULL);
}

static void on_close_btn_clicked(G_GNUC_UNUSED GtkButton *btn, gpointer user_data)
{
    if (!term_nb)
        return;
    if (gtk_notebook_get_n_pages(term_nb) <= 1)
        return; /* always keep at least one terminal */

    TermTab *tab = user_data;
    g_signal_handlers_disconnect_by_func(tab->vte,
                                         G_CALLBACK(on_child_exited), tab);

    gint page = gtk_notebook_page_num(term_nb, tab->hbox);
    if (page >= 0)
        gtk_notebook_remove_page(term_nb, page);

    g_free(tab);
}


/* ------------------------------------------------------------------ */
/* Right-click context menu                                            */

static void on_copy_activate(G_GNUC_UNUSED GtkMenuItem *item, gpointer user_data)
{
#if VTE_CHECK_VERSION(0, 50, 0)
    vte_terminal_copy_clipboard_format(VTE_TERMINAL(user_data), VTE_FORMAT_TEXT);
#else
    vte_terminal_copy_clipboard(VTE_TERMINAL(user_data));
#endif
}

static void on_paste_activate(G_GNUC_UNUSED GtkMenuItem *item, gpointer user_data)
{
    vte_terminal_paste_clipboard(VTE_TERMINAL(user_data));
}

static gboolean on_vte_button_press(GtkWidget *widget,
                                    GdkEventButton *event,
                                    G_GNUC_UNUSED gpointer data)
{
    if (event->button != 3)
        return FALSE;

    VteTerminal *vte  = VTE_TERMINAL(widget);
    GtkWidget   *menu = gtk_menu_new();

    GtkWidget *copy_item = gtk_menu_item_new_with_label("Copy");
    g_signal_connect(copy_item, "activate", G_CALLBACK(on_copy_activate), vte);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), copy_item);

    GtkWidget *paste_item = gtk_menu_item_new_with_label("Paste");
    g_signal_connect(paste_item, "activate", G_CALLBACK(on_paste_activate), vte);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), paste_item);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);

    return TRUE;
}


/* ------------------------------------------------------------------ */
/* Tab creation                                                        */

static TermTab *create_tab(const gchar *cmd)
{
    TermTab *tab = g_new0(TermTab, 1);

    /* Terminal widget */
    GtkWidget *vte_widget = vte_terminal_new();
    tab->vte = VTE_TERMINAL(vte_widget);

    vte_terminal_set_size(tab->vte, 80, 24);
    vte_terminal_set_mouse_autohide(tab->vte, TRUE);
    vte_terminal_set_scrollback_lines(tab->vte, 10000);
    gtk_widget_set_size_request(vte_widget, 10, 10);

    g_signal_connect(tab->vte, "child-exited",
                     G_CALLBACK(on_child_exited), tab);
    g_signal_connect(vte_widget, "button-press-event",
                     G_CALLBACK(on_vte_button_press), NULL);

    /* Scrollbar */
    GtkWidget *scrollbar = gtk_scrollbar_new(
        GTK_ORIENTATION_VERTICAL,
        gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte_widget)));
    gtk_widget_set_can_focus(scrollbar, FALSE);

    /* Page widget */
    tab->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(tab->hbox), vte_widget, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tab->hbox), scrollbar, FALSE, FALSE, 0);
    gtk_widget_show_all(tab->hbox);

    /* Store back-pointer so cleanup can find the TermTab from the page widget */
    g_object_set_data(G_OBJECT(tab->hbox), "termtab", tab);

    /* Tab label: "Term N  [×]" */
    tab_counter++;
    gchar *title = g_strdup_printf("Term %d", tab_counter);

    GtkWidget *lbl_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *label    = gtk_label_new(title);
    GtkWidget *close_btn = gtk_button_new();
    GtkWidget *close_img = gtk_image_new_from_icon_name("window-close-symbolic",
                                                          GTK_ICON_SIZE_MENU);
    g_free(title);

    gtk_button_set_image(GTK_BUTTON(close_btn), close_img);
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    gtk_button_set_focus_on_click(GTK_BUTTON(close_btn), FALSE);
    g_signal_connect(close_btn, "clicked",
                     G_CALLBACK(on_close_btn_clicked), tab);

    gtk_box_pack_start(GTK_BOX(lbl_hbox), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(lbl_hbox), close_btn, FALSE, FALSE, 0);
    gtk_widget_show_all(lbl_hbox);

    gtk_notebook_append_page(term_nb, tab->hbox, lbl_hbox);
    gtk_notebook_set_tab_reorderable(term_nb, tab->hbox, TRUE);

    spawn_shell(tab, cmd);
    return tab;
}


/* ------------------------------------------------------------------ */
/* IPC implementation                                                  */

static void do_run_command(const gchar *cmd, gboolean new_tab)
{
    if (!cmd || !*cmd || !term_nb)
        return;

    /* Bring Terminal tab to front in the message window */
    if (outer_idx >= 0)
        gtk_notebook_set_current_page(
            GTK_NOTEBOOK(geany_data->main_widgets->message_window_notebook),
            outer_idx);

    if (new_tab) {
        create_tab(cmd);
        gtk_notebook_set_current_page(term_nb,
                                       gtk_notebook_get_n_pages(term_nb) - 1);
        return;  /* command will be fed once the shell spawns */
    }

    gint page = gtk_notebook_get_current_page(term_nb);
    if (page < 0)
        return;

    GtkWidget *page_widget = gtk_notebook_get_nth_page(term_nb, page);
    if (!page_widget)
        return;

    TermTab *tab = g_object_get_data(G_OBJECT(page_widget), "termtab");
    if (!tab)
        return;

    /* Ctrl-U clears any partial input already on the prompt line */
    vte_terminal_feed_child(tab->vte, "\x15", 1);
    vte_terminal_feed_child(tab->vte, cmd, -1);
    vte_terminal_feed_child(tab->vte, "\n", 1);
}

/* Signal handler — other plugins emit "geanycli-run-command" on geany->object */
static void on_ipc_signal(G_GNUC_UNUSED GObject *obj,
                          const gchar *cmd,
                          gboolean new_tab,
                          G_GNUC_UNUSED gpointer data)
{
    do_run_command(cmd, new_tab);
}

/* Exported symbol — other plugins can call this via GModule symbol lookup */
G_MODULE_EXPORT void geanycli_run_command(const gchar *cmd, gboolean new_tab)
{
    do_run_command(cmd, new_tab);
}


/* ------------------------------------------------------------------ */
/* New-tab button                                                      */

static void on_new_tab_clicked(G_GNUC_UNUSED GtkButton *btn,
                               G_GNUC_UNUSED gpointer data)
{
    create_tab(NULL);
    gtk_notebook_set_current_page(term_nb,
                                   gtk_notebook_get_n_pages(term_nb) - 1);
}


/* ------------------------------------------------------------------ */
/* File-type tools                                                     */

#define TOOLS_CONFIG_FILENAME "filetypetools.conf"

/* Load ~/.config/geany/filetypetools.conf, then overlay
 * <project-root>/filetypetools.conf on top. */
static void tools_config_load(void)
{
    if (tools_config)
        g_key_file_free(tools_config);
    tools_config = g_key_file_new();

    gchar *user_path = g_build_filename(geany->app->configdir,
                                         TOOLS_CONFIG_FILENAME, NULL);
    g_key_file_load_from_file(tools_config, user_path, G_KEY_FILE_NONE, NULL);
    g_free(user_path);

    GeanyProject *proj = geany->app->project;
    if (!proj || !proj->base_path)
        return;

    gchar *proj_path = g_build_filename(proj->base_path,
                                         TOOLS_CONFIG_FILENAME, NULL);
    GKeyFile *pkf = g_key_file_new();
    if (g_key_file_load_from_file(pkf, proj_path, G_KEY_FILE_NONE, NULL)) {
        gsize ng;
        gchar **groups = g_key_file_get_groups(pkf, &ng);
        for (gsize gi = 0; gi < ng; gi++) {
            gsize nk;
            gchar **keys = g_key_file_get_keys(pkf, groups[gi], &nk, NULL);
            for (gsize ki = 0; ki < nk; ki++) {
                gchar *val = g_key_file_get_string(pkf, groups[gi],
                                                    keys[ki], NULL);
                g_key_file_set_string(tools_config, groups[gi],
                                      keys[ki], val);
                g_free(val);
            }
            g_strfreev(keys);
        }
        g_strfreev(groups);
    }
    g_key_file_free(pkf);
    g_free(proj_path);
}

/* Returns a pointer into filepath for the matching section key, e.g.
 * ".c.snip" or ".snip", trying longest suffix first.
 * Returns ".*" (literal) for the wildcard section, NULL for no match. */
static const gchar *tools_find_section(const gchar *filepath)
{
    if (!tools_config || !filepath)
        return NULL;

    const gchar *base = strrchr(filepath, G_DIR_SEPARATOR);
    base = base ? base + 1 : filepath;

    /* Walk from first dot rightward — longest match first */
    const gchar *dot = strchr(base, '.');
    while (dot) {
        if (g_key_file_has_group(tools_config, dot))
            return dot;
        dot = strchr(dot + 1, '.');
    }
    return g_key_file_has_group(tools_config, ".*") ? ".*" : NULL;
}

/* Expand %p %d %f %e %l in tmpl using filepath. Caller frees result. */
static gchar *tools_format_cmd(const gchar *tmpl, const gchar *filepath)
{
    GString *out  = g_string_sized_new(256);
    gchar   *dir  = g_path_get_dirname(filepath);
    gchar   *base = g_path_get_basename(filepath);
    const gchar *first_dot = strchr(base, '.');
    gchar   *stem = first_dot ? g_strndup(base, (gsize)(first_dot - base))
                              : g_strdup(base);

    gint line = 1;
    GeanyDocument *doc = document_get_current();
    if (doc && doc->file_name && strcmp(doc->file_name, filepath) == 0)
        line = sci_get_current_line(doc->editor->sci) + 1;

    for (const gchar *p = tmpl; *p; p++) {
        if (*p != '%' || !*(p + 1)) {
            g_string_append_c(out, *p);
            continue;
        }
        p++;
        switch (*p) {
            case 'p': g_string_append(out, filepath);          break;
            case 'd': g_string_append(out, dir);               break;
            case 'f': g_string_append(out, base);              break;
            case 'e': g_string_append(out, stem);              break;
            case 'l': g_string_append_printf(out, "%d", line); break;
            case '%': g_string_append_c(out, '%');             break;
            default:
                g_string_append_c(out, '%');
                g_string_append_c(out, *p);
        }
    }

    g_free(dir);
    g_free(base);
    g_free(stem);
    return g_string_free(out, FALSE);
}

static void on_tool_item_activate(GtkMenuItem *item,
                                   G_GNUC_UNUSED gpointer data)
{
    const gchar *cmd = g_object_get_data(G_OBJECT(item), "tool-cmd");
    if (cmd)
        do_run_command(cmd, FALSE);
}

/* Rebuild the "File Tools" submenu for the given filepath.
 * Pass NULL to clear and disable the menu. */
static void tools_menu_populate(const gchar *filepath)
{
    if (!tools_menu_item)
        return;

    /* Use GTK's own mechanism to detach and release the old submenu.
     * gtk_widget_destroy() alone does not clear the menu item's pointer,
     * which prevents the replacement submenu from appearing. */
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(tools_menu_item), NULL);

    if (!filepath || !tools_config) {
        gtk_widget_set_sensitive(tools_menu_item, FALSE);
        return;
    }

    const gchar *section = tools_find_section(filepath);
    if (!section) {
        gtk_widget_set_sensitive(tools_menu_item, FALSE);
        return;
    }

    GtkWidget *submenu = gtk_menu_new();
    gboolean   any     = FALSE;

    for (gint i = 0; ; i++) {
        gchar *key  = g_strdup_printf("tool_%d", i);
        gchar *tmpl = g_key_file_get_string(tools_config, section, key, NULL);
        g_free(key);
        if (!tmpl)
            break;

        gchar     *cmd = tools_format_cmd(tmpl, filepath);
        GtkWidget *mi  = gtk_menu_item_new_with_label(tmpl);
        gtk_widget_set_tooltip_text(mi, cmd);
        g_object_set_data_full(G_OBJECT(mi), "tool-cmd", cmd, g_free);
        g_signal_connect(mi, "activate",
                         G_CALLBACK(on_tool_item_activate), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), mi);
        gtk_widget_show(mi);
        g_free(tmpl);
        any = TRUE;
    }

    if (any) {
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(tools_menu_item), submenu);
        gtk_widget_set_sensitive(tools_menu_item, TRUE);
        gtk_widget_show(submenu);
    } else {
        gtk_widget_destroy(submenu);
        gtk_widget_set_sensitive(tools_menu_item, FALSE);
    }
}

static void on_doc_activate(G_GNUC_UNUSED GObject *obj,
                             GeanyDocument *doc,
                             G_GNUC_UNUSED gpointer data)
{
    tools_menu_populate(doc ? doc->file_name : NULL);
}

static void on_project_open(G_GNUC_UNUSED GObject *obj,
                             G_GNUC_UNUSED GKeyFile *config,
                             G_GNUC_UNUSED gpointer data)
{
    tools_config_load();
    GeanyDocument *doc = document_get_current();
    tools_menu_populate(doc ? doc->file_name : NULL);
}

static void on_project_close(G_GNUC_UNUSED GObject *obj,
                              G_GNUC_UNUSED gpointer data)
{
    tools_config_load();
    GeanyDocument *doc = document_get_current();
    tools_menu_populate(doc ? doc->file_name : NULL);
}


/* ------------------------------------------------------------------ */
/* Plugin lifecycle                                                    */

static gboolean gt_init(GeanyPlugin *plugin, G_GNUC_UNUSED gpointer data)
{
    geany_plugin = plugin;
    geany_data   = plugin->geany_data;
    active       = TRUE;

    /* Inner notebook */
    term_nb = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_set_scrollable(term_nb, TRUE);
    gtk_notebook_set_show_border(term_nb, FALSE);

    /* "+" button appended to the inner tab bar */
    GtkWidget *add_btn = gtk_button_new_from_icon_name("list-add-symbolic",
                                                        GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_button_set_relief(GTK_BUTTON(add_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(add_btn, "New terminal tab");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_new_tab_clicked), NULL);
    gtk_widget_show(add_btn);
    gtk_notebook_set_action_widget(term_nb, add_btn, GTK_PACK_END);

    outer_widget = GTK_WIDGET(term_nb);
    gtk_widget_show(outer_widget);

    GtkWidget *tab_label = gtk_label_new("🗔 Cli");
    outer_idx = gtk_notebook_append_page(
        GTK_NOTEBOOK(geany_data->main_widgets->message_window_notebook),
        outer_widget, tab_label);

    /* First terminal tab */
    create_tab(NULL);

    /* File-type tools menu */
    tools_config_load();
    tools_menu_item = gtk_menu_item_new_with_label("File Tools");
    gtk_widget_show(tools_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(geany_data->main_widgets->tools_menu),
                          tools_menu_item);
    GeanyDocument *cur = document_get_current();
    tools_menu_populate(cur ? cur->file_name : NULL);

    plugin_signal_connect(plugin, geany->object, "document-activate", FALSE,
                          G_CALLBACK(on_doc_activate), NULL);
    plugin_signal_connect(plugin, geany->object, "document-open", FALSE,
                          G_CALLBACK(on_doc_activate), NULL);
    plugin_signal_connect(plugin, geany->object, "project-open", FALSE,
                          G_CALLBACK(on_project_open), NULL);
    plugin_signal_connect(plugin, geany->object, "project-close", FALSE,
                          G_CALLBACK(on_project_close), NULL);

    /* Register the IPC signal on the Geany application object.
     * g_signal_lookup guards against duplicate registration on reload. */
    GType obj_type = G_OBJECT_TYPE(geany->object);
    if (!g_signal_lookup("geanycli-run-command", obj_type))
        g_signal_new("geanycli-run-command",
                     obj_type,
                     G_SIGNAL_RUN_LAST,
                     0, NULL, NULL,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE,
                     2,
                     G_TYPE_STRING,
                     G_TYPE_BOOLEAN);

    plugin_signal_connect(plugin, geany->object,
                          "geanycli-run-command", FALSE,
                          G_CALLBACK(on_ipc_signal), NULL);

    return TRUE;
}

static void gt_cleanup(G_GNUC_UNUSED GeanyPlugin *plugin,
                       G_GNUC_UNUSED gpointer data)
{
    active = FALSE;

    /* Disconnect child-exited signals and free TermTab structs before the
     * notebook widget tree is destroyed by gtk_notebook_remove_page. */
    if (term_nb) {
        gint n = gtk_notebook_get_n_pages(term_nb);
        for (gint i = 0; i < n; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(term_nb, i);
            TermTab *tab = g_object_get_data(G_OBJECT(page), "termtab");
            if (tab) {
                g_signal_handlers_disconnect_by_func(tab->vte,
                                                     G_CALLBACK(on_child_exited),
                                                     tab);
                g_free(tab);
            }
        }
    }

    if (outer_idx >= 0) {
        gtk_notebook_remove_page(
            GTK_NOTEBOOK(geany_data->main_widgets->message_window_notebook),
            outer_idx);
        outer_widget = NULL;
        term_nb      = NULL;
        outer_idx    = -1;
    }

    if (tools_menu_item) {
        gtk_widget_destroy(tools_menu_item);
        tools_menu_item = NULL;
    }
    if (tools_config) {
        g_key_file_free(tools_config);
        tools_config = NULL;
    }

    tab_counter = 0;
}


/* ------------------------------------------------------------------ */

G_MODULE_EXPORT void geany_load_module(GeanyPlugin *plugin)
{
    plugin->info->name        = "Terminal";
    plugin->info->description =
        "Tabbed VTE terminals in the message window. "
        "Opens at the project root. "
        "Other plugins can run commands by emitting the "
        "\"geanycli-run-command\" signal on geany->object "
        "or by calling geanycli_run_command() via GModule.";
    plugin->info->version     = "0.1";
    plugin->info->author      = "teknopaul";

    plugin->funcs->init    = gt_init;
    plugin->funcs->cleanup = gt_cleanup;

    GEANY_PLUGIN_REGISTER(plugin, 235);
}
