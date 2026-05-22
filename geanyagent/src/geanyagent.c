/*
 * geanyagent.c — Geany plugin: AI agent terminal tab in the message window
 *
 * The configured command is run inside the user's login shell so that PATH
 * and profile scripts (nvm, pyenv, etc.) are honoured.  When the process
 * exits it is restarted automatically.
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


/* ------------------------------------------------------------------ */

GeanyPlugin  *geany_plugin;
GeanyData    *geany_data;

static VteTerminal *agent_term  = NULL;
static GtkWidget   *agent_hbox  = NULL;
static gint         tab_index   = -1;
static gboolean     running     = FALSE;  /* FALSE during cleanup to suppress restart */

static gchar *agent_cmd    = NULL;
static gchar *config_file  = NULL;

#define DEFAULT_CMD "claude"


/* ------------------------------------------------------------------ */
/* Spawn / restart                                                     */

static gboolean ga_spawn_idle(gpointer data);

static void on_spawn_done(G_GNUC_UNUSED VteTerminal *term,
                          G_GNUC_UNUSED GPid pid,
                          GError *error,
                          G_GNUC_UNUSED gpointer data)
{
	if (error)
		g_warning("geanyagent: spawn failed: %s", error->message);
}

static void on_child_exited(G_GNUC_UNUSED VteTerminal *term,
                             G_GNUC_UNUSED int status,
                             G_GNUC_UNUSED gpointer data)
{
	if (running)
		g_idle_add(ga_spawn_idle, NULL);
}

static void ga_spawn(void)
{
	const gchar *shell = g_getenv("SHELL");
	if (!shell || !*shell)
		shell = "/bin/sh";

	gchar **shell_parts = g_strsplit(shell, " ", -1);
	guint   n           = g_strv_length(shell_parts);
	gchar **argv        = g_new(gchar *, n + 3);
	guint   i;

	for (i = 0; i < n; i++)
		argv[i] = shell_parts[i];
	argv[n]     = (gchar *)"-c";
	argv[n + 1] = agent_cmd ? agent_cmd : (gchar *)DEFAULT_CMD;
	argv[n + 2] = NULL;

	/* prefer project base path; fall back to home dir */
	gchar *work_dir = NULL;
	GeanyApp *app = geany->app;
	if (app->project && app->project->base_path && *app->project->base_path)
		work_dir = g_strdup(app->project->base_path);
	else
		work_dir = g_strdup(g_get_home_dir());

	/* strip vars that confuse nested terminals */
	const gchar *exclude[] = { "COLUMNS", "LINES", "TERM", "TERM_PROGRAM", NULL };
	gchar **env = utils_copy_environment(exclude, "TERM", "xterm-256color", NULL);

	vte_terminal_spawn_async(agent_term,
	                         VTE_PTY_DEFAULT,
	                         work_dir,
	                         argv,
	                         env,
	                         G_SPAWN_SEARCH_PATH,
	                         NULL, NULL, NULL,
	                         -1,
	                         NULL,
	                         on_spawn_done,
	                         NULL);

	g_strfreev(env);
	g_free(work_dir);
	g_free(argv);
	g_strfreev(shell_parts);
}

static gboolean ga_spawn_idle(G_GNUC_UNUSED gpointer data)
{
	ga_spawn();
	return G_SOURCE_REMOVE;
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
/* Widget construction                                                 */

static void create_agent_tab(void)
{
	GtkWidget *vte, *scrollbar, *label;

	vte = vte_terminal_new();
	agent_term = VTE_TERMINAL(vte);

	scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL,
	                              gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte)));
	gtk_widget_set_can_focus(scrollbar, FALSE);

	agent_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_pack_start(GTK_BOX(agent_hbox), vte, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(agent_hbox), scrollbar, FALSE, FALSE, 0);

	gtk_widget_set_size_request(vte, 10, 10);
	vte_terminal_set_size(agent_term, 80, 24);
	vte_terminal_set_mouse_autohide(agent_term, TRUE);
	vte_terminal_set_scrollback_lines(agent_term, 10000);

	g_signal_connect(agent_term, "child-exited",
	                 G_CALLBACK(on_child_exited), NULL);
	g_signal_connect(vte, "button-press-event",
	                 G_CALLBACK(on_vte_button_press), NULL);

	gtk_widget_show_all(agent_hbox);

	/* 🤖 Agent — UTF-8 encoded inline */
	label = gtk_label_new("\xf0\x9f\xa4\x96 Agent");
	tab_index = gtk_notebook_append_page(
	    GTK_NOTEBOOK(geany_data->main_widgets->message_window_notebook),
	    agent_hbox, label);
}


/* ------------------------------------------------------------------ */
/* Config                                                              */

static void ga_load_config(void)
{
	GKeyFile *kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, config_file, G_KEY_FILE_NONE, NULL))
	{
		gchar *v = g_key_file_get_string(kf, "agent", "cmd", NULL);
		if (v)
		{
			g_free(agent_cmd);
			agent_cmd = v;
		}
	}
	g_key_file_free(kf);

	if (!agent_cmd)
		agent_cmd = g_strdup(DEFAULT_CMD);
}

static void ga_save_config(void)
{
	GKeyFile *kf   = g_key_file_new();
	gchar    *data = NULL;

	g_key_file_set_string(kf, "agent", "cmd",
	                      agent_cmd ? agent_cmd : DEFAULT_CMD);
	data = g_key_file_to_data(kf, NULL, NULL);
	utils_write_file(config_file, data);
	g_free(data);
	g_key_file_free(kf);
}


/* ------------------------------------------------------------------ */
/* Configure dialog                                                    */

typedef struct { GtkWidget *cmd_entry; } ConfigWidgets;

static void on_configure_response(G_GNUC_UNUSED GtkDialog *dialog,
                                  gint response, ConfigWidgets *cw)
{
	if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY)
	{
		const gchar *new_cmd = gtk_entry_get_text(GTK_ENTRY(cw->cmd_entry));
		g_free(agent_cmd);
		agent_cmd = g_strdup(new_cmd);
		ga_save_config();
	}
	g_free(cw);
}

static GtkWidget *ga_configure(G_GNUC_UNUSED GeanyPlugin *plugin,
                               GtkDialog *dialog,
                               G_GNUC_UNUSED gpointer data)
{
	ConfigWidgets *cw    = g_new0(ConfigWidgets, 1);
	GtkWidget     *vbox  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget     *label = gtk_label_new("Agent command:");

	gtk_widget_set_halign(label, GTK_ALIGN_START);
	cw->cmd_entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(cw->cmd_entry),
	                   agent_cmd ? agent_cmd : DEFAULT_CMD);

	gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), cw->cmd_entry, FALSE, FALSE, 0);

	g_signal_connect(dialog, "response",
	                 G_CALLBACK(on_configure_response), cw);
	return vbox;
}


/* ------------------------------------------------------------------ */
/* Plugin lifecycle                                                    */

static gboolean ga_init(GeanyPlugin *plugin, G_GNUC_UNUSED gpointer data)
{
	geany_plugin = plugin;
	geany_data   = plugin->geany_data;

	config_file = g_build_filename(geany->app->configdir,
	                               "plugins", "geanyagent",
	                               "geanyagent.conf", NULL);
	gchar *config_dir = g_path_get_dirname(config_file);
	g_mkdir_with_parents(config_dir, 0755);
	g_free(config_dir);

	ga_load_config();
	create_agent_tab();

	running = TRUE;
	g_idle_add(ga_spawn_idle, NULL);

	return TRUE;
}

static void ga_cleanup(G_GNUC_UNUSED GeanyPlugin *plugin,
                       G_GNUC_UNUSED gpointer data)
{
	running = FALSE;

	if (agent_term)
	{
		g_signal_handlers_disconnect_by_func(agent_term,
		                                     G_CALLBACK(on_child_exited), NULL);
		agent_term = NULL;
	}

	if (tab_index >= 0)
	{
		gtk_notebook_remove_page(
		    GTK_NOTEBOOK(geany_data->main_widgets->message_window_notebook),
		    tab_index);
		agent_hbox = NULL;
		tab_index  = -1;
	}

	g_free(agent_cmd);
	agent_cmd = NULL;
	g_free(config_file);
	config_file = NULL;
}


/* ------------------------------------------------------------------ */

G_MODULE_EXPORT void geany_load_module(GeanyPlugin *plugin)
{
	plugin->info->name        = "Agent";
	plugin->info->description = "AI agent terminal tab in the message window. "
	                            "Runs a configurable command (default: claude) "
	                            "and restarts it automatically on exit.";
	plugin->info->version     = "0.1";
	plugin->info->author      = "teknopaul";

	plugin->funcs->init      = ga_init;
	plugin->funcs->cleanup   = ga_cleanup;
	plugin->funcs->configure = ga_configure;

	GEANY_PLUGIN_REGISTER(plugin, 235);
}
