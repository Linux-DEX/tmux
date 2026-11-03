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
 * Hide or show all floating panes in the current window without killing
 * their PTYs. Hidden floats keep running and come back on the next toggle.
 */

static enum cmd_retval	cmd_toggle_float_layer_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_toggle_float_layer_entry = {
	.name = "toggle-float-layer",
	.description = "Hide or show all floating panes in a window.",
	.alias = NULL,

	.args = { "t:", 0, 0, NULL },
	.usage = CMD_TARGET_WINDOW_USAGE,

	.target = { 't', CMD_FIND_WINDOW, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_toggle_float_layer_exec
};

static enum cmd_retval
cmd_toggle_float_layer_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window		*w;

	if (target->wl == NULL) {
		cmdq_error(item, "no current window");
		return (CMD_RETURN_ERROR);
	}
	w = target->wl->window;
	struct window_pane	*wp, *next_active = NULL;
	struct client		*c = cmdq_get_client(item);
	u_int			 n = 0;
	int			 hide;

	hide = (~w->flags & WINDOW_FLOATLAYERHIDDEN);

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!window_pane_is_floating(wp))
			continue;
		n++;
		if (hide)
			wp->flags |= PANE_LAYERHIDDEN;
		else
			wp->flags &= ~PANE_LAYERHIDDEN;
	}

	if (n == 0) {
		if (c != NULL)
			status_message_set(c, -1, 1, 0, 0, "no floating panes");
		return (CMD_RETURN_NORMAL);
	}

	if (hide) {
		w->flags |= WINDOW_FLOATLAYERHIDDEN;
		if (w->active != NULL && window_pane_is_floating(w->active)) {
			TAILQ_FOREACH(wp, &w->panes, entry) {
				if (!window_pane_is_floating(wp) &&
				    (~wp->flags & PANE_LAYERHIDDEN)) {
					next_active = wp;
					break;
				}
			}
			if (next_active != NULL)
				window_set_active_pane(w, next_active, 1);
		}
	} else
		w->flags &= ~WINDOW_FLOATLAYERHIDDEN;

	server_redraw_window(w);
	if (c != NULL)
		status_message_set(c, -1, 1, 0, 0,
		    hide ? "float layer hidden (%u)" : "float layer shown (%u)",
		    n);
	return (CMD_RETURN_NORMAL);
}
