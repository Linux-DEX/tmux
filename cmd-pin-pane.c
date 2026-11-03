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
 * Toggle PANE_PINNED on a floating pane so it stays above other floats.
 */

static enum cmd_retval	cmd_pin_pane_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_pin_pane_entry = {
	.name = "pin-pane",
	.description = "Pin or unpin a floating pane.",
	.alias = NULL,

	.args = { "t:", 0, 0, NULL },
	.usage = CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_pin_pane_exec
};

static enum cmd_retval
cmd_pin_pane_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window_pane	*wp = target->wp;
	struct window		*w = target->wl->window;
	struct client		*c = cmdq_get_client(item);

	if (!window_pane_is_floating(wp)) {
		cmdq_error(item, "pane is not floating");
		return (CMD_RETURN_ERROR);
	}

	if (wp->flags & PANE_PINNED) {
		wp->flags &= ~PANE_PINNED;
		if (c != NULL)
			status_message_set(c, -1, 1, 0, 0, "pane unpinned");
	} else {
		wp->flags |= PANE_PINNED;
		TAILQ_REMOVE(&w->z_index, wp, zentry);
		TAILQ_INSERT_HEAD(&w->z_index, wp, zentry);
		if (c != NULL)
			status_message_set(c, -1, 1, 0, 0, "pane pinned");
	}

	wp->flags |= PANE_REDRAW;
	server_redraw_window(w);
	return (CMD_RETURN_NORMAL);
}
