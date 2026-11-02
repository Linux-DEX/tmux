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
 * Toggle a modal floating scratch pane in the current window. If a pane titled
 * "scratch" already exists and is active, kill it; if it exists but is inactive,
 * select it; otherwise create a new modal floating scratch over the layout
 * (including above a zoomed pane).
 */

#define SCRATCH_TITLE "scratch"

static enum cmd_retval	cmd_toggle_scratch_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_toggle_scratch_entry = {
	.name = "toggle-scratch",
	.description = "Toggle a floating scratch pane.",
	.alias = NULL,

	.args = { "t:", 0, 0, NULL },
	.usage = CMD_TARGET_PANE_USAGE,

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

static enum cmd_retval
cmd_toggle_scratch_exec(struct cmd *self, struct cmdq_item *item)
{
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window		*w = target->wl->window;
	struct window_pane	*wp;
	struct cmd_parse_result	*pr;
	struct cmdq_item	*new_item;
	struct cmdq_state	*state;

	wp = cmd_toggle_scratch_find(w);
	if (wp != NULL) {
		if (wp == w->active) {
			server_kill_pane(wp);
			return (CMD_RETURN_NORMAL);
		}
		window_set_active_pane(w, wp, 1);
		server_redraw_window(w);
		return (CMD_RETURN_NORMAL);
	}

	/*
	 * Non-modal float on purpose: modal panes block select-pane to the
	 * tiled layout, so Prefix+u could not bring an existing scratch back
	 * to focus. Esc-to-dismiss remains available via a second toggle.
	 */
	pr = cmd_parse_from_string(
	    "new-pane -A -x80% -y60% -c '#{pane_current_path}' "
	    "-T " SCRATCH_TITLE " ; move-pane -P centre", NULL);
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
