/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Mohammad Alfi Sharin Rizvi <asrizvi2357@gmail.com>
 * Copyright (c) 2026 Linux-DEX
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tmux.h"

/*
 * Write the layout (sessions, windows, panes, splits, per-window layout trees,
 * per-pane working directories, titles and commands) of every session on the
 * server to a JSON file (version 2), so that it can later be recreated with
 * 'load-layout'. Shell-only panes omit the command field.
 */

static enum cmd_retval cmd_save_layout_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_save_layout_entry = {
	.name = "save-layout",
	.description = "Save session layouts to a file.",
	.alias = NULL,

	.args = { "f:", 0, 0, NULL },
	.usage = "[-f path]",

	.flags = CMD_AFTERHOOK,
	.exec = cmd_save_layout_exec
};

struct cmd_save_layout_data {
	struct cmdq_item	*item;
	char			*path;
	u_int			 nsessions;
	u_int			 nwindows;
	u_int			 npanes;
};

/* Escape a string for embedding in a JSON string literal. */
static char *
cmd_save_layout_json_escape(const char *s)
{
	struct evbuffer	*evb;
	char		*out;
	unsigned char	 c;

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	for (; *s != '\0'; s++) {
		c = (unsigned char)*s;
		switch (c) {
		case '"':
			evbuffer_add(evb, "\\\"", 2);
			break;
		case '\\':
			evbuffer_add(evb, "\\\\", 2);
			break;
		case '\n':
			evbuffer_add(evb, "\\n", 2);
			break;
		case '\r':
			evbuffer_add(evb, "\\r", 2);
			break;
		case '\t':
			evbuffer_add(evb, "\\t", 2);
			break;
		default:
			if (c < 0x20)
				evbuffer_add_printf(evb, "\\u%04x", c);
			else
				evbuffer_add(evb, &c, 1);
			break;
		}
	}
	evbuffer_add(evb, "", 1);
	out = xstrdup((const char *)evbuffer_pullup(evb, -1));
	evbuffer_free(evb);
	return (out);
}

static char *
cmd_save_layout_pane_cwd(struct window_pane *wp)
{
	char	*cwd;

	cwd = osdep_get_cwd(wp->fd);
	if (cwd != NULL)
		return (xstrdup(cwd));
	if (wp->cwd != NULL)
		return (xstrdup(wp->cwd));
	return (xstrdup(""));
}

/* Return a pane command worth restoring, or NULL for a plain shell. */
static char *
cmd_save_layout_pane_command(struct window_pane *wp)
{
	char		*cmd, *base;
	const char	*shell;

	/*
	 * Prefer argv (keeps arguments). Fall back to the live process name
	 * when argv is empty (interactive shells that replaced themselves).
	 */
	if (wp->argc > 0)
		cmd = cmd_stringify_argv(wp->argc, wp->argv);
	else
		cmd = NULL;
	if (cmd == NULL || *cmd == '\0') {
		free(cmd);
		cmd = osdep_get_name(wp->fd, wp->tty);
	}
	if (cmd == NULL || *cmd == '\0') {
		free(cmd);
		return (NULL);
	}

	shell = wp->shell;
	if (shell != NULL) {
		base = strrchr(shell, '/');
		base = (base != NULL) ? base + 1 : (char *)shell;
		if (strcmp(cmd, shell) == 0 || strcmp(cmd, base) == 0) {
			free(cmd);
			return (NULL);
		}
	}
	if (strcmp(cmd, "bash") == 0 || strcmp(cmd, "zsh") == 0 ||
	    strcmp(cmd, "sh") == 0 || strcmp(cmd, "fish") == 0 ||
	    strcmp(cmd, "ksh") == 0 || strcmp(cmd, "tcsh") == 0 ||
	    strcmp(cmd, "dash") == 0) {
		free(cmd);
		return (NULL);
	}
	return (cmd);
}

static void
cmd_save_layout_done(__unused struct client *c, const char *path, int error,
    int closed, __unused struct evbuffer *buffer, void *data)
{
	struct cmd_save_layout_data	*cdata = data;
	struct cmdq_item		*item = cdata->item;

	if (!closed)
		return;

	if (error != 0)
		cmdq_error(item, "%s: %s", path, strerror(error));
	else
		cmdq_print(item,
		    "saved %u session%s / %u window%s / %u pane%s to %s",
		    cdata->nsessions, cdata->nsessions == 1 ? "" : "s",
		    cdata->nwindows, cdata->nwindows == 1 ? "" : "s",
		    cdata->npanes, cdata->npanes == 1 ? "" : "s",
		    path);
	cmdq_continue(item);

	free(cdata->path);
	free(cdata);
}

static enum cmd_retval
cmd_save_layout_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args			*args = cmd_get_args(self);
	struct cmd_save_layout_data	*cdata;
	const char			*path;
	char				*freeme = NULL;
	struct evbuffer			*evb;
	struct session			*s;
	struct winlink			*wl;
	struct window			*w;
	struct window_pane		*wp;
	char				*name_esc, *cwd, *cwd_esc, *layout;
	char				*cmd, *cmd_esc, *title_esc;
	u_int				 pane_index;
	int				 first_session, first_window, first_pane;
	time_t				 now;
	char				 tbuf[32];
	void				*data;
	size_t				 size;

	if ((path = args_get(args, 'f')) == NULL) {
		if ((path = freeme = layout_get_path()) == NULL) {
			cmdq_error(item, "could not determine layout file "
			    "location");
			return (CMD_RETURN_ERROR);
		}
	}

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");

	time(&now);
	strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
	evbuffer_add_printf(evb,
	    "{\"version\":2,\"generated\":\"%s\",\"tmux_version\":\"%s\","
	    "\"sessions\":[", tbuf, getversion());

	cdata = xcalloc(1, sizeof *cdata);
	cdata->item = item;
	cdata->path = xstrdup(path);

	first_session = 1;
	RB_FOREACH(s, sessions, &sessions) {
		if (!first_session)
			evbuffer_add(evb, ",", 1);
		first_session = 0;
		cdata->nsessions++;

		name_esc = cmd_save_layout_json_escape(s->name);
		evbuffer_add_printf(evb,
		    "{\"name\":\"%s\",\"current_window\":"
		    "%u,\"windows\":[", name_esc,
		    s->curw != NULL ? s->curw->idx : 0);
		free(name_esc);

		first_window = 1;
		RB_FOREACH(wl, winlinks, &s->windows) {
			w = wl->window;
			if (!first_window)
				evbuffer_add(evb, ",", 1);
			first_window = 0;
			cdata->nwindows++;

			name_esc = cmd_save_layout_json_escape(w->name);
			evbuffer_add_printf(evb, "{\"index\":%u,\"name\":"
			    "\"%s\"", wl->idx, name_esc);
			free(name_esc);

			layout = layout_dump(w, w->layout_root, 0);
			if (layout != NULL) {
				evbuffer_add_printf(evb, ",\"layout\":%s",
				    layout);
				free(layout);
			}

			evbuffer_add(evb, ",\"panes\":[", 10);
			first_pane = 1;
			TAILQ_FOREACH(wp, &w->panes, entry) {
				if (!first_pane)
					evbuffer_add(evb, ",", 1);
				first_pane = 0;
				cdata->npanes++;

				window_pane_index(wp, &pane_index);
				cwd = cmd_save_layout_pane_cwd(wp);
				cwd_esc = cmd_save_layout_json_escape(cwd);
				free(cwd);

				evbuffer_add_printf(evb,
				    "{\"index\":%u,\"cwd\":\"%s\"",
				    pane_index, cwd_esc);
				free(cwd_esc);

				if (wp->base.title != NULL &&
				    *wp->base.title != '\0') {
					title_esc = cmd_save_layout_json_escape(
					    wp->base.title);
					evbuffer_add_printf(evb,
					    ",\"title\":\"%s\"", title_esc);
					free(title_esc);
				}

				cmd = cmd_save_layout_pane_command(wp);
				if (cmd != NULL) {
					cmd_esc = cmd_save_layout_json_escape(
					    cmd);
					evbuffer_add_printf(evb,
					    ",\"command\":\"%s\"", cmd_esc);
					free(cmd_esc);
					free(cmd);
				}
				evbuffer_add(evb, "}", 1);
			}
			evbuffer_add(evb, "]}", 2);
		}
		evbuffer_add(evb, "]}", 2);
	}
	evbuffer_add(evb, "]}", 2);

	data = evbuffer_pullup(evb, -1);
	size = evbuffer_get_length(evb);

	file_write(cmdq_get_client(item), path, O_TRUNC, data, size,
	    cmd_save_layout_done, cdata);

	evbuffer_free(evb);
	free(freeme);
	return (CMD_RETURN_WAIT);
}
