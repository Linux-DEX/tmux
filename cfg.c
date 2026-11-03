/* $OpenBSD: cfg.c,v 1.92 2026/10/02 12:28:07 nicm Exp $ */

/*
 * Copyright (c) 2008 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

struct client		 *cfg_client;
int			  cfg_finished;
static char		**cfg_causes;
static u_int		  cfg_ncauses;
static struct cmdq_item	 *cfg_item;
static int		  cfg_started;
static struct event	  layout_autosave_ev;
static int		  layout_autosave_armed;

int                       cfg_quiet = 1;
char                    **cfg_files;
u_int                     cfg_nfiles;

/*
 * Path used by save-layout / load-layout / upgrade-server. Prefer a writable
 * directory next to the last configuration file; otherwise the home directory.
 */
char *
layout_get_path(void)
{
	const char	*home;
	char		*copy, *dir, *path;

	if (cfg_nfiles > 0) {
		copy = xstrdup(cfg_files[cfg_nfiles - 1]);
		dir = dirname(copy);
		if (access(dir, W_OK) == 0) {
			xasprintf(&path, "%s/%s", dir, TMUX_LAYOUT_FILE);
			free(copy);
			return (path);
		}
		free(copy);
	}

	if ((home = find_home()) != NULL) {
		xasprintf(&path, "%s/%s", home, TMUX_LAYOUT_FILE);
		return (path);
	}

	return (NULL);
}

/* Queue a background save-layout (used by autosave and detach). */
void
layout_autosave_now(void)
{
	struct cmd_parse_result	*pr;
	struct cmdq_item	*new_item;
	struct cmdq_state	*state;

	if (!options_get_number(global_options, "layout-autosave"))
		return;
	if (RB_EMPTY(&sessions))
		return;

	pr = cmd_parse_from_string("save-layout", NULL);
	if (pr->status == CMD_PARSE_ERROR) {
		free(pr->error);
		return;
	}
	state = cmdq_new_state(NULL, NULL, 0);
	new_item = cmdq_get_command(pr->cmdlist, state);
	cmdq_append(NULL, new_item);
	cmd_list_free(pr->cmdlist);
	cmdq_free_state(state);
}

static void
layout_autosave_timer(__unused int fd, __unused short events,
    __unused void *arg)
{
	struct timeval	tv;
	int		interval;

	layout_autosave_now();

	interval = options_get_number(global_options,
	    "layout-autosave-interval");
	if (interval < 5)
		interval = 5;
	timerclear(&tv);
	tv.tv_sec = interval;
	evtimer_add(&layout_autosave_ev, &tv);
}

void
layout_autosave_start(void)
{
	struct timeval	tv;
	int		interval;

	if (layout_autosave_armed)
		return;
	layout_autosave_armed = 1;

	evtimer_set(&layout_autosave_ev, layout_autosave_timer, NULL);
	interval = options_get_number(global_options,
	    "layout-autosave-interval");
	if (interval < 5)
		interval = 5;
	timerclear(&tv);
	tv.tv_sec = interval;
	evtimer_add(&layout_autosave_ev, &tv);
}

/* If upgrade-server left an autoload marker, queue load-layout once. */
static void
layout_maybe_autoload(void)
{
	char			*layout_path, *marker, *cmd;
	struct cmd_parse_result	*pr;
	struct cmdq_item	*new_item;
	struct cmdq_state	*state;

	if ((layout_path = layout_get_path()) == NULL)
		return;
	xasprintf(&marker, "%s.autoload", layout_path);
	if (access(marker, F_OK) != 0) {
		free(marker);
		free(layout_path);
		return;
	}
	unlink(marker);
	free(marker);

	{
		char	*escaped = args_escape(layout_path);

		xasprintf(&cmd, "load-layout -f %s", escaped);
		free(escaped);
	}
	pr = cmd_parse_from_string(cmd, NULL);
	free(cmd);
	free(layout_path);
	if (pr->status == CMD_PARSE_ERROR) {
		cfg_add_cause("autoload layout: %s", pr->error);
		free(pr->error);
		return;
	}
	state = cmdq_new_state(NULL, NULL, 0);
	new_item = cmdq_get_command(pr->cmdlist, state);
	cmdq_append(NULL, new_item);
	cmd_list_free(pr->cmdlist);
	cmdq_free_state(state);
}

static enum cmd_retval
cfg_client_done(struct cmdq_item *item, __unused void *data)
{
	if (cmdq_get_client(item)->flags & CLIENT_DEAD)
		return (CMD_RETURN_NORMAL);
	if (!cfg_finished)
		return (CMD_RETURN_WAIT);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cfg_done(__unused struct cmdq_item *item, __unused void *data)
{
	if (cfg_finished)
		return (CMD_RETURN_NORMAL);
	cfg_finished = 1;

	layout_maybe_autoload();
	layout_autosave_start();
	cfg_show_causes(NULL);

	if (cfg_item != NULL) {
		cmdq_continue(cfg_item);
		cfg_item = NULL;
	}

	prompt_load_history();

	return (CMD_RETURN_NORMAL);
}

void
cfg_client_lost(struct client *c)
{
	if (c != cfg_client)
		return;
	cfg_client = NULL;
	if (cfg_item != NULL) {
		cmdq_continue(cfg_item);
		cfg_item = NULL;
	}
}

void
start_cfg(void)
{
	struct client	 *c;
	u_int		  i;
	int		  flags = 0;

	/*
	 * Configuration files are loaded without a client, so commands are run
	 * in the global queue with item->client NULL.
	 *
	 * However, we must block the initial client (but just the initial
	 * client) so that its command runs after the configuration is loaded.
	 * Because start_cfg() is called so early, we can be sure the client's
	 * command queue is currently empty and our callback will be at the
	 * front - we need to get in before MSG_COMMAND.
	 *
	 * If the initial client is lost before the configuration finishes, the
	 * next client to identify is first in the list and ends up here. Block
	 * it instead, but do not load the files again.
	 */
	cfg_client = c = TAILQ_FIRST(&clients);
	if (c != NULL) {
		cfg_item = cmdq_get_callback(cfg_client_done, NULL);
		cmdq_append(c, cfg_item);
	}
	if (cfg_started)
		return;
	cfg_started = 1;

	if (cfg_quiet)
		flags = CMD_PARSE_QUIET;
	for (i = 0; i < cfg_nfiles; i++)
		load_cfg(cfg_files[i], c, NULL, NULL, flags, NULL);

	cmdq_append(NULL, cmdq_get_callback(cfg_done, NULL));
}

int
load_cfg(const char *path, struct client *c, struct cmdq_item *item,
    struct cmd_find_state *current, int flags, struct cmdq_item **new_item)
{
	FILE			*f;
	struct cmd_parse_input	 pi;
	struct cmd_parse_result	*pr;
	struct cmdq_item	*new_item0;
	struct cmdq_state	*state;

	if (new_item != NULL)
		*new_item = NULL;

	log_debug("loading %s", path);
	if ((f = fopen(path, "rb")) == NULL) {
		if (errno == ENOENT && (flags & CMD_PARSE_QUIET))
			return (0);
		cfg_add_cause("%s: %s", path, strerror(errno));
		return (-1);
	}

	memset(&pi, 0, sizeof pi);
	pi.flags = flags;
	pi.file = path;
	pi.line = 1;
	pi.item = item;
	pi.c = c;

	pr = cmd_parse_from_file(f, &pi);
	fclose(f);
	if (pr->status == CMD_PARSE_ERROR) {
		cfg_add_cause("%s", pr->error);
		free(pr->error);
		return (-1);
	}
	if (flags & CMD_PARSE_PARSEONLY) {
		cmd_list_free(pr->cmdlist);
		return (0);
	}

	if (item != NULL)
		state = cmdq_copy_state(cmdq_get_state(item), current);
	else
		state = cmdq_new_state(NULL, NULL, 0);
	cmdq_add_format(state, "current_file", "%s", pi.file);

	new_item0 = cmdq_get_command(pr->cmdlist, state);
	if (item != NULL)
		new_item0 = cmdq_insert_after(item, new_item0);
	else
		new_item0 = cmdq_append(NULL, new_item0);
	cmd_list_free(pr->cmdlist);
	cmdq_free_state(state);

	if (new_item != NULL)
		*new_item = new_item0;
	return (0);
}

int
load_cfg_from_buffer(const void *buf, size_t len, const char *path,
    struct client *c, struct cmdq_item *item, struct cmd_find_state *current,
    int flags, struct cmdq_item **new_item)
{
	struct cmd_parse_input	 pi;
	struct cmd_parse_result	*pr;
	struct cmdq_item	*new_item0;
	struct cmdq_state	*state;

	if (new_item != NULL)
		*new_item = NULL;

	log_debug("loading %s", path);

	memset(&pi, 0, sizeof pi);
	pi.flags = flags;
	pi.file = path;
	pi.line = 1;
	pi.item = item;
	pi.c = c;

	pr = cmd_parse_from_buffer(buf, len, &pi);
	if (pr->status == CMD_PARSE_ERROR) {
		cfg_add_cause("%s", pr->error);
		free(pr->error);
		return (-1);
	}
	if (flags & CMD_PARSE_PARSEONLY) {
		cmd_list_free(pr->cmdlist);
		return (0);
	}

	if (item != NULL)
		state = cmdq_copy_state(cmdq_get_state(item), current);
	else
		state = cmdq_new_state(NULL, NULL, 0);
	cmdq_add_format(state, "current_file", "%s", pi.file);

	new_item0 = cmdq_get_command(pr->cmdlist, state);
	if (item != NULL)
		new_item0 = cmdq_insert_after(item, new_item0);
	else
		new_item0 = cmdq_append(NULL, new_item0);
	cmd_list_free(pr->cmdlist);
	cmdq_free_state(state);

	if (new_item != NULL)
		*new_item = new_item0;
	return (0);
}

void
cfg_add_cause(const char *fmt, ...)
{
	va_list	 ap;
	char	*msg;

	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);

	cfg_ncauses++;
	cfg_causes = xreallocarray(cfg_causes, cfg_ncauses, sizeof *cfg_causes);
	cfg_causes[cfg_ncauses - 1] = msg;
}

void
cfg_print_causes(struct cmdq_item *item)
{
	struct client	*c = cmdq_get_client(item);
	u_int		 i;
	char		*cause;

	for (i = 0; i < cfg_ncauses; i++) {
		cause = cfg_causes[i];
		if (c != NULL && (c->flags & CLIENT_CONTROL))
			control_notify_write(c, "%%config-error %s", cause);
		else
			cmdq_print(item, "%s", cause);
		free(cause);
	}

	free(cfg_causes);
	cfg_causes = NULL;
	cfg_ncauses = 0;
}

void
cfg_show_causes(struct session *s)
{
	struct client			*c = TAILQ_FIRST(&clients);
	struct window_pane		*wp;
	struct window_mode_entry	*wme;
	u_int				 i;
	char				*cause;

	if (cfg_ncauses == 0)
		return;

	if (c != NULL && (c->flags & CLIENT_CONTROL)) {
		for (i = 0; i < cfg_ncauses; i++) {
			cause = cfg_causes[i];
			control_notify_write(c, "%%config-error %s", cause);
			free(cause);
		}
		goto out;
	}

	if (s == NULL) {
		if (c != NULL && c->session != NULL)
			s = c->session;
		else
			s = RB_MIN(sessions, &sessions);
	}
	if (s == NULL || s->attached == 0) /* wait for an attached session */
		return;
	wp = s->curw->window->active;

	wme = TAILQ_FIRST(&wp->modes);
	if (wme == NULL || wme->mode != &window_view_mode)
		window_pane_set_mode(wp, NULL, &window_view_mode, NULL, NULL,
		    NULL);
	for (i = 0; i < cfg_ncauses; i++) {
		window_copy_add(wp, 0, "%s", cfg_causes[i]);
		free(cfg_causes[i]);
	}

out:
	free(cfg_causes);
	cfg_causes = NULL;
	cfg_ncauses = 0;
}
