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
#include <unistd.h>
#include <sys/stat.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <vte/vte.h>

#include <geanyplugin.h>


/* ------------------------------------------------------------------ */

GeanyPlugin  *geany_plugin;
GeanyData    *geany_data;

static VteTerminal *agent_term  = NULL;
static GtkWidget   *agent_hbox  = NULL;
static GtkWidget   *agent_label = NULL;
static gint         tab_index   = -1;
static gboolean     running     = FALSE;  /* FALSE during cleanup to suppress restart */

static gchar *agent_cmd    = NULL;
static gchar *config_file  = NULL;

/* askpass: temp dir prepended to PATH containing a sudo wrapper + askpass script */
static gboolean use_askpass = FALSE;  /* opt-in; disabled by default */
static gchar   *askpass_dir  = NULL;
static gchar   *askpass_prog = NULL;

#define DEFAULT_CMD "claude"

static const gchar SKILL_TEMPLATE[] =
    "---\n"
    "name: %s\n"
    "description: \n"
    "---\n"
    "\n"
    "<objective>\n"
    "\n"
    "</objective>\n"
    "\n"
    "<process>\n"
    "\n"
    "</process>\n";

#define AGENT_TOOLS_CONFIG "agenttools.conf"
#define AGENT_TOOLS_GROUP  "tools"

static GKeyFile  *agent_tools_config    = NULL;
static GtkWidget *agent_tools_menu_item = NULL;


/* ------------------------------------------------------------------ */
/* Askpass setup                                                       */
/*
 * Creates a private temp dir (0700) prepended to PATH containing:
 *   sudo             — wrapper that forces -A so sudo never prompts on the
 *                      terminal; the AI agent cannot see or handle the password
 *   geanyagent-askpass — zenity dialog (or delegates to a system askpass)
 *
 * sudo reads SUDO_ASKPASS and calls the helper; the helper shows a GTK
 * password dialog and writes the password to stdout.  Nothing is logged.
 */

static void setup_askpass(void)
{
    /* Find a system askpass program */
    const gchar *candidates[] = {
        "/usr/lib/ssh/x11-ssh-askpass",
        "/usr/libexec/ssh-askpass",
        "/usr/lib/openssh/gnome-ssh-askpass",
        "/usr/lib/openssh/x11-ssh-askpass",
        "/usr/bin/ksshaskpass",
        "/usr/bin/seahorse-askpass",
        NULL
    };
    for (gint i = 0; candidates[i]; i++)
    {
        if (g_file_test(candidates[i], G_FILE_TEST_IS_EXECUTABLE))
        {
            askpass_prog = g_strdup(candidates[i]);
            break;
        }
    }

    /* No system askpass — try to build a zenity-based one */
    if (!askpass_prog)
    {
        gchar *zenity = g_find_program_in_path("zenity");
        if (!zenity)
            return; /* nothing we can do */
        g_free(zenity);
    }

    /* Private temp dir — mode 0700 so other users cannot replace scripts */
    gchar *tmpl = g_build_filename(g_get_tmp_dir(), "geanyagent-XXXXXX", NULL);
    askpass_dir = g_mkdtemp(tmpl);  /* modifies tmpl in-place on success */
    if (!askpass_dir)
    {
        g_free(tmpl);
        g_free(askpass_prog);
        askpass_prog = NULL;
        return;
    }

    /* Write zenity askpass helper if no system one was found */
    if (!askpass_prog)
    {
        askpass_prog = g_build_filename(askpass_dir, "geanyagent-askpass", NULL);
        const gchar *script =
            "#!/bin/sh\n"
            "zenity --password --title 'Agent needs sudo' -- \"${1:-Password:}\" 2>/dev/null\n";
        utils_write_file(askpass_prog, script);
        chmod(askpass_prog, S_IRWXU);
    }

    /* Write sudo wrapper: forces -A so sudo never prompts on the PTY.
     * The AI sees only the exit code, never the password. */
    gchar *sudo_path = g_build_filename(askpass_dir, "sudo", NULL);
    gchar *content   = g_strdup_printf(
        "#!/bin/sh\n"
        "SUDO_ASKPASS='%s' exec /usr/bin/sudo -A \"$@\"\n",
        askpass_prog);
    utils_write_file(sudo_path, content);
    g_free(content);
    chmod(sudo_path, S_IRWXU);
    g_free(sudo_path);
}

static void cleanup_askpass(void)
{
    if (askpass_dir)
    {
        gchar *p;
        p = g_build_filename(askpass_dir, "sudo", NULL);
        g_unlink(p); g_free(p);
        p = g_build_filename(askpass_dir, "geanyagent-askpass", NULL);
        g_unlink(p); g_free(p);
        g_rmdir(askpass_dir);
        g_free(askpass_dir);
        askpass_dir = NULL;
    }
    g_free(askpass_prog);
    askpass_prog = NULL;
}


/* ------------------------------------------------------------------ */
/* Agent command dispatch                                              */

static void ga_send_command(const gchar *cmd)
{
	if (!agent_term || !cmd || !*cmd)
		return;
	if (tab_index >= 0)
		gtk_notebook_set_current_page(
		    GTK_NOTEBOOK(geany_data->main_widgets->message_window_notebook),
		    tab_index);
#if VTE_CHECK_VERSION(0, 70, 0)
	VtePty *pty = vte_terminal_get_pty(agent_term);
	if (pty) {
		int fd = vte_pty_get_fd(pty);
		write(fd, "\x15", 1);
		write(fd, cmd, strlen(cmd));
		write(fd, "\n", 1);
	}
#else
	vte_terminal_feed_child(agent_term, "\x15", 1);
	vte_terminal_feed_child(agent_term, cmd, (gssize)strlen(cmd));
	vte_terminal_feed_child(agent_term, "\n", 1);
#endif
}

/* Split expanded command on ';' and send each part to the agent. */
static void ga_run_agent_cmd(const gchar *expanded)
{
	gchar **parts = g_strsplit(expanded, ";", -1);
	for (gint i = 0; parts[i]; i++) {
		gchar *part = g_strstrip(parts[i]);
		if (*part)
			ga_send_command(part);
	}
	g_strfreev(parts);
}


/* ------------------------------------------------------------------ */
/* Agent tools config                                                  */

static gchar *agent_tools_format_cmd(const gchar *tmpl, const gchar *filepath)
{
	GString     *out  = g_string_sized_new(256);
	gchar       *dir  = g_path_get_dirname(filepath);
	gchar       *base = g_path_get_basename(filepath);
	gchar       *root;
	GeanyApp    *app  = geany->app;

	if (app->project && app->project->base_path && *app->project->base_path)
		root = g_strdup(app->project->base_path);
	else
		root = g_strdup(g_get_home_dir());

	for (const gchar *p = tmpl; *p; p++) {
		if (*p != '%' || !*(p + 1)) {
			g_string_append_c(out, *p);
			continue;
		}
		p++;
		switch (*p) {
			case 'p': g_string_append(out, filepath); break;
			case 'd': g_string_append(out, dir);      break;
			case 'f': g_string_append(out, base);     break;
			case 'r': g_string_append(out, root);     break;
			case '%': g_string_append_c(out, '%');    break;
			default:
				g_string_append_c(out, '%');
				g_string_append_c(out, *p);
		}
	}
	g_free(root);
	g_free(dir);
	g_free(base);
	return g_string_free(out, FALSE);
}

static void on_agent_tool_item_activate(GtkMenuItem *item,
                                        G_GNUC_UNUSED gpointer data)
{
	const gchar   *tmpl = g_object_get_data(G_OBJECT(item), "agent-tmpl");
	GeanyDocument *doc  = document_get_current();
	if (!tmpl)
		return;
	const gchar *filepath = (doc && doc->file_name) ? doc->file_name : "";
	gchar *expanded = agent_tools_format_cmd(tmpl, filepath);
	ga_run_agent_cmd(expanded);
	g_free(expanded);
}

static void agent_tools_load(void)
{
	if (agent_tools_config)
		g_key_file_free(agent_tools_config);
	agent_tools_config = g_key_file_new();

	/* User-level: ~/.config/geany/agenttools.conf */
	gchar *user_path = g_build_filename(geany->app->configdir,
	                                    AGENT_TOOLS_CONFIG, NULL);
	g_key_file_load_from_file(agent_tools_config, user_path, G_KEY_FILE_NONE, NULL);
	g_free(user_path);

	/* Project-level: <project>/config/geany/agenttools.conf — overlays user config */
	GeanyProject *proj = geany->app->project;
	if (!proj || !proj->base_path)
		return;

	gchar *proj_path = g_build_filename(proj->base_path, "config", "geany",
	                                    AGENT_TOOLS_CONFIG, NULL);
	GKeyFile *pkf = g_key_file_new();
	if (g_key_file_load_from_file(pkf, proj_path, G_KEY_FILE_NONE, NULL)) {
		gsize   ng;
		gchar **groups = g_key_file_get_groups(pkf, &ng);
		for (gsize gi = 0; gi < ng; gi++) {
			gsize   nk;
			gchar **keys = g_key_file_get_keys(pkf, groups[gi], &nk, NULL);
			for (gsize ki = 0; ki < nk; ki++) {
				gchar *val = g_key_file_get_string(pkf, groups[gi], keys[ki], NULL);
				g_key_file_set_string(agent_tools_config, groups[gi], keys[ki], val);
				g_free(val);
			}
			g_strfreev(keys);
		}
		g_strfreev(groups);
	}
	g_key_file_free(pkf);
	g_free(proj_path);
}

static void agent_tools_menu_populate(const gchar *filepath)
{
	if (!agent_tools_menu_item)
		return;

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(agent_tools_menu_item), NULL);

	if (!filepath || !agent_tools_config || !g_str_has_suffix(filepath, ".md") ||
	    !g_key_file_has_group(agent_tools_config, AGENT_TOOLS_GROUP)) {
		gtk_widget_set_sensitive(agent_tools_menu_item, FALSE);
		return;
	}

	GtkWidget *submenu = gtk_menu_new();
	gboolean   any     = FALSE;

	for (gint i = 0; ; i++) {
		gchar *agent_key = g_strdup_printf("agent_%d", i);
		gchar *tmpl      = g_key_file_get_string(agent_tools_config,
		                                          AGENT_TOOLS_GROUP, agent_key, NULL);
		g_free(agent_key);
		if (!tmpl)
			break;

		gchar *name_key = g_strdup_printf("name_%d", i);
		gchar *name     = g_key_file_get_string(agent_tools_config,
		                                         AGENT_TOOLS_GROUP, name_key, NULL);
		g_free(name_key);

		GtkWidget *mi = gtk_menu_item_new_with_label(name ? name : tmpl);
		g_object_set_data_full(G_OBJECT(mi), "agent-tmpl", tmpl, g_free);
		g_signal_connect(mi, "activate", G_CALLBACK(on_agent_tool_item_activate), NULL);
		gtk_menu_shell_append(GTK_MENU_SHELL(submenu), mi);
		gtk_widget_show(mi);

		g_free(name);
		any = TRUE;
	}

	if (any) {
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(agent_tools_menu_item), submenu);
		gtk_widget_set_sensitive(agent_tools_menu_item, TRUE);
		gtk_widget_show(submenu);
	} else {
		gtk_widget_destroy(submenu);
		gtk_widget_set_sensitive(agent_tools_menu_item, FALSE);
	}
}

static void on_agent_doc_activate(G_GNUC_UNUSED GObject *obj,
                                   GeanyDocument *doc,
                                   G_GNUC_UNUSED gpointer data)
{
	agent_tools_menu_populate(doc ? doc->file_name : NULL);
}

static void on_agent_project_open(G_GNUC_UNUSED GObject *obj,
                                   G_GNUC_UNUSED GKeyFile *config,
                                   G_GNUC_UNUSED gpointer data)
{
	agent_tools_load();
	GeanyDocument *doc = document_get_current();
	agent_tools_menu_populate(doc ? doc->file_name : NULL);
}

static void on_agent_project_close(G_GNUC_UNUSED GObject *obj,
                                    G_GNUC_UNUSED gpointer data)
{
	agent_tools_load();
	GeanyDocument *doc = document_get_current();
	agent_tools_menu_populate(doc ? doc->file_name : NULL);
}


/* ------------------------------------------------------------------ */
/* New Prompt dialog                                                    */

static void on_new_prompt_activate(G_GNUC_UNUSED GtkMenuItem *item,
                                   G_GNUC_UNUSED gpointer data)
{
	GtkWidget *dialog = gtk_dialog_new_with_buttons(
	    "New Prompt",
	    GTK_WINDOW(geany_data->main_widgets->window),
	    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	    "_Cancel", GTK_RESPONSE_CANCEL,
	    "_OK",     GTK_RESPONSE_OK,
	    NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "My Prompt Name");

	GtkWidget *err_label = gtk_label_new("");

	GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_set_border_width(GTK_CONTAINER(content_area), 8);
	gtk_box_pack_start(GTK_BOX(content_area), gtk_label_new("Prompt name:"), FALSE, FALSE, 2);
	gtk_box_pack_start(GTK_BOX(content_area), entry, FALSE, FALSE, 2);
	gtk_box_pack_start(GTK_BOX(content_area), err_label, FALSE, FALSE, 2);
	gtk_widget_show_all(dialog);

	while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK)
	{
		const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));
		if (!name || !*name) {
			gtk_label_set_text(GTK_LABEL(err_label), "Name cannot be empty.");
			continue;
		}

		/* Slugify: lowercase, spaces → underscores */
		gchar *slug = g_ascii_strdown(name, -1);
		for (gchar *c = slug; *c; c++) {
			if (*c == ' ')
				*c = '_';
		}

		gchar *base_path;
		GeanyApp *app = geany->app;
		if (app->project && app->project->base_path && *app->project->base_path)
			base_path = g_strdup(app->project->base_path);
		else
			base_path = g_strdup(g_get_home_dir());

		gchar *prompts_dir  = g_build_filename(base_path, "ai-prompts", NULL);
		gchar *filename     = g_strdup_printf("%s.prompt.md", slug);
		gchar *prompt_path  = g_build_filename(prompts_dir, filename, NULL);

		g_mkdir_with_parents(prompts_dir, 0755);
		if (!g_file_test(prompt_path, G_FILE_TEST_EXISTS))
			utils_write_file(prompt_path, "");

		document_open_file(prompt_path, FALSE, NULL, NULL);

		g_free(prompt_path);
		g_free(filename);
		g_free(slug);
		g_free(prompts_dir);
		g_free(base_path);
		break;
	}
	gtk_widget_destroy(dialog);
}


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

	/* Build PATH: prepend askpass_dir so our sudo wrapper takes priority */
	const gchar *old_path = g_getenv("PATH");
	gchar *new_path = askpass_dir
	    ? g_strconcat(askpass_dir, ":", old_path ? old_path : "/usr/bin:/bin", NULL)
	    : g_strdup(old_path ? old_path : "/usr/bin:/bin");

	/* strip vars that confuse nested terminals; rebuild PATH explicitly */
	const gchar *exclude[] = { "COLUMNS", "LINES", "TERM", "TERM_PROGRAM", "PATH", NULL };
	gchar **env;
	if (askpass_prog)
		env = utils_copy_environment(exclude,
		                             "TERM",         "xterm-256color",
		                             "PATH",         new_path,
		                             "SUDO_ASKPASS", askpass_prog,
		                             NULL);
	else
		env = utils_copy_environment(exclude,
		                             "TERM", "xterm-256color",
		                             "PATH", new_path,
		                             NULL);
	g_free(new_path);

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

typedef struct { VteTerminal *vte; gchar *text; } ClipCtxData;

static void on_clip_ctx_data_free(gpointer data, G_GNUC_UNUSED GClosure *closure)
{
	ClipCtxData *ctx = (ClipCtxData *)data;
	g_free(ctx->text);
	g_free(ctx);
}

static void on_add_to_context_activate(G_GNUC_UNUSED GtkMenuItem *item, gpointer user_data)
{
	ClipCtxData *ctx     = (ClipCtxData *)user_data;
	gchar       *payload = g_strconcat("@", ctx->text, NULL);
	gsize        len     = strlen(payload);

#if VTE_CHECK_VERSION(0, 70, 0)
	VtePty *pty = vte_terminal_get_pty(ctx->vte);
	if (pty)
		write(vte_pty_get_fd(pty), payload, len);
#else
	vte_terminal_feed_child(ctx->vte, payload, (gssize)len);
#endif
	g_free(payload);
}

/* Returns TRUE if text looks like a file-path argument suitable for @ context. */
static gboolean clip_looks_like_context_ref(const gchar *text)
{
	if (!text || !*text)
		return FALSE;
	if (text[0] == '/')
		return TRUE;
	/* single line, no spaces, under 255 chars */
	gsize len = strlen(text);
	return len < 255 && !strchr(text, ' ') && !strchr(text, '\n');
}

static void on_rename_tab_activate(G_GNUC_UNUSED GtkMenuItem *item,
                                   G_GNUC_UNUSED gpointer data)
{
	if (!agent_label)
		return;

	GtkWidget *dialog = gtk_dialog_new_with_buttons(
	    "Rename Tab",
	    GTK_WINDOW(geany_data->main_widgets->window),
	    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	    "_Cancel", GTK_RESPONSE_CANCEL,
	    "_OK",     GTK_RESPONSE_OK,
	    NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(entry), gtk_label_get_text(GTK_LABEL(agent_label)));
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);

	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_set_border_width(GTK_CONTAINER(content), 8);
	gtk_box_pack_start(GTK_BOX(content), gtk_label_new("Tab title:"), FALSE, FALSE, 2);
	gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 2);
	gtk_widget_show_all(dialog);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK)
	{
		const gchar *new_title = gtk_entry_get_text(GTK_ENTRY(entry));
		if (new_title && *new_title)
			gtk_label_set_text(GTK_LABEL(agent_label), new_title);
	}
	gtk_widget_destroy(dialog);
}

static void on_new_skill_activate(G_GNUC_UNUSED GtkMenuItem *item,
                                  G_GNUC_UNUSED gpointer data)
{
	GtkWidget *dialog = gtk_dialog_new_with_buttons(
	    "New Skill",
	    GTK_WINDOW(geany_data->main_widgets->window),
	    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	    "_Cancel", GTK_RESPONSE_CANCEL,
	    "_OK",     GTK_RESPONSE_OK,
	    NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "my-skill");

	GtkWidget *err_label = gtk_label_new("");

	GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_set_border_width(GTK_CONTAINER(content_area), 8);
	gtk_box_pack_start(GTK_BOX(content_area), gtk_label_new("Skill name (no spaces):"), FALSE, FALSE, 2);
	gtk_box_pack_start(GTK_BOX(content_area), entry, FALSE, FALSE, 2);
	gtk_box_pack_start(GTK_BOX(content_area), err_label, FALSE, FALSE, 2);
	gtk_widget_show_all(dialog);

	while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK)
	{
		const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));
		if (!name || !*name)
		{
			gtk_label_set_text(GTK_LABEL(err_label), "Name cannot be empty.");
			continue;
		}
		if (strchr(name, ' '))
		{
			gtk_label_set_text(GTK_LABEL(err_label), "Name cannot contain spaces.");
			continue;
		}

		gchar *base_path;
		GeanyApp *app = geany->app;
		if (app->project && app->project->base_path && *app->project->base_path)
			base_path = g_strdup(app->project->base_path);
		else
			base_path = g_strdup(g_get_home_dir());

		gchar *skill_dir  = g_build_filename(base_path, ".claude", "skills", name, NULL);
		gchar *skill_path = g_build_filename(skill_dir, "SKILL.md", NULL);

		g_mkdir_with_parents(skill_dir, 0755);

		gchar *file_content = g_strdup_printf(SKILL_TEMPLATE, name);
		utils_write_file(skill_path, file_content);
		g_free(file_content);

		document_open_file(skill_path, FALSE, NULL, NULL);

		g_free(skill_path);
		g_free(skill_dir);
		g_free(base_path);
		break;
	}
	gtk_widget_destroy(dialog);
}

static gboolean on_vte_button_press(GtkWidget *widget,
                                    GdkEventButton *event,
                                    G_GNUC_UNUSED gpointer data)
{
	if (event->button != 3)
		return FALSE;

	VteTerminal *vte        = VTE_TERMINAL(widget);
	gboolean     has_sel    = vte_terminal_get_has_selection(vte);

	/* Capture selection to CLIPBOARD now, before the menu event loop may
	 * clear VTE's selection state. */
	if (has_sel)
	{
#if VTE_CHECK_VERSION(0, 50, 0)
		vte_terminal_copy_clipboard_format(vte, VTE_FORMAT_TEXT);
#else
		vte_terminal_copy_clipboard(vte);
#endif
	}

	GtkWidget *menu = gtk_menu_new();

	GtkWidget *copy_item = gtk_menu_item_new_with_label("Copy");
	gtk_widget_set_sensitive(copy_item, has_sel);
	g_signal_connect(copy_item, "activate", G_CALLBACK(on_copy_activate), vte);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), copy_item);

	GtkWidget *paste_item = gtk_menu_item_new_with_label("Paste");
	g_signal_connect(paste_item, "activate", G_CALLBACK(on_paste_activate), vte);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), paste_item);

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

	GtkWidget *rename_item = gtk_menu_item_new_with_label("Rename Tab");
	g_signal_connect(rename_item, "activate", G_CALLBACK(on_rename_tab_activate), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), rename_item);

	GtkWidget *new_skill_item = gtk_menu_item_new_with_label("New Skill");
	g_signal_connect(new_skill_item, "activate", G_CALLBACK(on_new_skill_activate), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), new_skill_item);

	GtkWidget *new_prompt_item = gtk_menu_item_new_with_label("New Prompt");
	g_signal_connect(new_prompt_item, "activate", G_CALLBACK(on_new_prompt_activate), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), new_prompt_item);

	GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	gchar        *clip_text = gtk_clipboard_wait_for_text(clipboard);
	if (clip_text)
	{
		g_strstrip(clip_text);
		if (clip_looks_like_context_ref(clip_text))
		{
			gtk_menu_shell_append(GTK_MENU_SHELL(menu),
			                      gtk_separator_menu_item_new());

			ClipCtxData *ctx = g_new(ClipCtxData, 1);
			ctx->vte  = vte;
			ctx->text = g_strdup(clip_text);

			GtkWidget *ctx_item = gtk_menu_item_new_with_label("Add to Context");
			g_signal_connect_data(ctx_item, "activate",
			                      G_CALLBACK(on_add_to_context_activate),
			                      ctx, on_clip_ctx_data_free, 0);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), ctx_item);
		}
		g_free(clip_text);
	}

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
	agent_label = label;
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
		use_askpass = g_key_file_get_boolean(kf, "agent", "use_askpass", NULL);
	}
	g_key_file_free(kf);

	if (!agent_cmd)
		agent_cmd = g_strdup(DEFAULT_CMD);
}

static void ga_save_config(void)
{
	GKeyFile *kf   = g_key_file_new();
	gchar    *data = NULL;

	g_key_file_set_string(kf,  "agent", "cmd",        agent_cmd ? agent_cmd : DEFAULT_CMD);
	g_key_file_set_boolean(kf, "agent", "use_askpass", use_askpass);
	data = g_key_file_to_data(kf, NULL, NULL);
	utils_write_file(config_file, data);
	g_free(data);
	g_key_file_free(kf);
}


/* ------------------------------------------------------------------ */
/* Configure dialog                                                    */

typedef struct { GtkWidget *cmd_entry; GtkWidget *askpass_check; } ConfigWidgets;

static void on_configure_response(G_GNUC_UNUSED GtkDialog *dialog,
                                  gint response, ConfigWidgets *cw)
{
	if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY)
	{
		const gchar *new_cmd = gtk_entry_get_text(GTK_ENTRY(cw->cmd_entry));
		g_free(agent_cmd);
		agent_cmd   = g_strdup(new_cmd);
		use_askpass = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cw->askpass_check));
		ga_save_config();
	}
	g_free(cw);
}

static GtkWidget *ga_configure(G_GNUC_UNUSED GeanyPlugin *plugin,
                               GtkDialog *dialog,
                               G_GNUC_UNUSED gpointer data)
{
	ConfigWidgets *cw   = g_new0(ConfigWidgets, 1);
	GtkWidget     *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

	GtkWidget *cmd_label = gtk_label_new("Agent command:");
	gtk_widget_set_halign(cmd_label, GTK_ALIGN_START);
	cw->cmd_entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(cw->cmd_entry),
	                   agent_cmd ? agent_cmd : DEFAULT_CMD);

	cw->askpass_check = gtk_check_button_new_with_label(
	    "Intercept sudo with GUI password dialog (Linux only, requires zenity or ssh-askpass)");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cw->askpass_check), use_askpass);
	gtk_widget_set_tooltip_text(cw->askpass_check,
	    "When enabled, a sudo wrapper is injected into PATH so that sudo commands\n"
	    "issued by the agent show a graphical password dialog instead of prompting\n"
	    "on the terminal. This prevents passwords appearing in command strings or history.");

	gtk_box_pack_start(GTK_BOX(vbox), cmd_label,        FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), cw->cmd_entry,    FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), cw->askpass_check, FALSE, FALSE, 4);

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
	if (use_askpass)
		setup_askpass();
	create_agent_tab();

	running = TRUE;
	g_idle_add(ga_spawn_idle, NULL);

	/* Agent tools menu */
	agent_tools_load();
	agent_tools_menu_item = gtk_menu_item_new_with_label("Agent Tools");
	gtk_widget_show(agent_tools_menu_item);
	gtk_menu_shell_append(GTK_MENU_SHELL(geany_data->main_widgets->tools_menu),
	                      agent_tools_menu_item);
	GeanyDocument *cur = document_get_current();
	agent_tools_menu_populate(cur ? cur->file_name : NULL);

	plugin_signal_connect(plugin, geany->object, "document-activate", FALSE,
	                      G_CALLBACK(on_agent_doc_activate), NULL);
	plugin_signal_connect(plugin, geany->object, "document-open", FALSE,
	                      G_CALLBACK(on_agent_doc_activate), NULL);
	plugin_signal_connect(plugin, geany->object, "project-open", FALSE,
	                      G_CALLBACK(on_agent_project_open), NULL);
	plugin_signal_connect(plugin, geany->object, "project-close", FALSE,
	                      G_CALLBACK(on_agent_project_close), NULL);

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
		agent_hbox  = NULL;
		agent_label = NULL;
		tab_index   = -1;
	}

	cleanup_askpass();

	if (agent_tools_menu_item) {
		gtk_widget_destroy(agent_tools_menu_item);
		agent_tools_menu_item = NULL;
	}
	if (agent_tools_config) {
		g_key_file_free(agent_tools_config);
		agent_tools_config = NULL;
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
