/* $OpenBSD$ */

/*
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

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Toggle a floating scratch pane in the current window. If a pane titled
 * "scratch" already exists and is active, kill it; if it exists but is
 * inactive, select it; otherwise create a non-modal floating scratch.
 *
 * Size presets via -s: small (50%x40%), medium (80%x60%, default),
 * large (95%x85%).
 */

#define SCRATCH_TITLE "scratch"

static enum cmd_retval	cmd_toggle_scratch_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_toggle_scratch_entry = {
	.name = "toggle-scratch",
	.description = "Toggle a floating scratch pane.",
	.alias = NULL,

	.args = { "s:t:", 0, 0, NULL },
	.usage = "[-s small|medium|large] " CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_toggle_scratch_exec
};

static struct window_pane *
cmd_toggle_scratch_find(struct window *w)
{
	struct window_pane	*wp;

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (wp->base.title != NULL &&
		    strcmp(wp->base.title, SCRATCH_TITLE) == 0)
			return (wp);
	}
	return (NULL);
}

static void
cmd_toggle_scratch_size(const char *preset, const char **sx, const char **sy)
{
	if (preset != NULL && strcmp(preset, "small") == 0) {
		*sx = "50%";
		*sy = "40%";
	} else if (preset != NULL && strcmp(preset, "large") == 0) {
		*sx = "95%";
		*sy = "85%";
	} else {
		*sx = "80%";
		*sy = "60%";
	}
}

static enum cmd_retval
cmd_toggle_scratch_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window		*w = target->wl->window;
	struct window_pane	*wp;
	struct cmd_parse_result	*pr;
	struct cmdq_item	*new_item;
	struct cmdq_state	*state;
	struct client		*c = cmdq_get_client(item);
	const char		*preset, *sx, *sy;
	char			*cmd;

	wp = cmd_toggle_scratch_find(w);
	if (wp != NULL) {
		if (wp == w->active) {
			server_kill_pane(wp);
			if (c != NULL)
				status_message_set(c, -1, 1, 0, 0,
				    "scratch closed");
			return (CMD_RETURN_NORMAL);
		}
		window_set_active_pane(w, wp, 1);
		server_redraw_window(w);
		if (c != NULL)
			status_message_set(c, -1, 1, 0, 0, "scratch focused");
		return (CMD_RETURN_NORMAL);
	}

	preset = args_get(args, 's');
	cmd_toggle_scratch_size(preset, &sx, &sy);

	/*
	 * Non-modal float on purpose: modal panes block select-pane to the
	 * tiled layout, so Prefix+u could not bring an existing scratch back
	 * to focus. Yellow SCRATCH chrome comes from pane-border-format /
	 * pane-active-border-style defaults.
	 */
	xasprintf(&cmd,
	    "new-pane -A -x%s -y%s -c '#{pane_current_path}' "
	    "-T " SCRATCH_TITLE " ; move-pane -P centre ; "
	    "display-message 'scratch (%s)'", sx, sy,
	    preset != NULL ? preset : "medium");
	pr = cmd_parse_from_string(cmd, NULL);
	free(cmd);
	if (pr->status == CMD_PARSE_ERROR) {
		cmdq_error(item, "%s", pr->error);
		free(pr->error);
		return (CMD_RETURN_ERROR);
	}

	state = cmdq_copy_state(cmdq_get_state(item), target);
	new_item = cmdq_get_command(pr->cmdlist, state);
	cmdq_insert_after(item, new_item);
	cmd_list_free(pr->cmdlist);
	cmdq_free_state(state);
	return (CMD_RETURN_NORMAL);
}
