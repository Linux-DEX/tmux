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
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Layout-preserving server upgrade path.
 *
 * True in-place restart (keeping pane PTYs alive across exec) is a large
 * upstream effort. This command instead:
 *   1. verifies the new binary runs (tmux -V)
 *   2. queues save-layout to the default layout file
 *   3. writes an autoload marker and stops the server
 *
 * Pane processes exit with the old server. Layouts reload automatically when
 * a client starts the new binary (see layout_maybe_autoload in cfg.c).
 */

static enum cmd_retval	cmd_upgrade_server_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_upgrade_server_entry = {
	.name = "upgrade-server",
	.description = "Save layouts and restart into a new tmux binary.",
	.alias = NULL,

	.args = { "f:y", 0, 0, NULL },
	.usage = "[-y] [-f binary]",

	.flags = 0,
	.exec = cmd_upgrade_server_exec
};

static char *
cmd_upgrade_server_self_path(void)
{
	char	 buf[PATH_MAX];
	ssize_t	 n;

#ifdef __linux__
	n = readlink("/proc/self/exe", buf, sizeof buf - 1);
	if (n > 0) {
		buf[n] = '\0';
		return (xstrdup(buf));
	}
#endif
	return (xstrdup(getprogname()));
}

static int
cmd_upgrade_server_check_binary(const char *path, char **cause)
{
	pid_t	 pid;
	int	 status;
	char	*argv[3];

	argv[0] = (char *)path;
	argv[1] = (char *)"-V";
	argv[2] = NULL;

	switch (pid = fork()) {
	case -1:
		xasprintf(cause, "fork failed: %s", strerror(errno));
		return (-1);
	case 0:
		if (dup2(open("/dev/null", O_RDWR), STDOUT_FILENO) == -1)
			_exit(127);
		if (dup2(STDOUT_FILENO, STDERR_FILENO) == -1)
			_exit(127);
		execv(path, argv);
		_exit(127);
	default:
		break;
	}

	if (waitpid(pid, &status, 0) == -1) {
		xasprintf(cause, "waitpid failed: %s", strerror(errno));
		return (-1);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		xasprintf(cause, "%s: not a usable tmux binary "
		    "(tmux -V failed)", path);
		return (-1);
	}
	return (0);
}

static struct cmdq_item *
cmd_upgrade_server_queue(struct cmdq_item *after, const char *command,
    char **cause)
{
	struct cmd_parse_result	*pr;
	struct cmdq_item	*new_item;
	struct cmdq_state	*state;

	pr = cmd_parse_from_string(command, NULL);
	if (pr->status == CMD_PARSE_ERROR) {
		*cause = pr->error;
		return (NULL);
	}
	state = cmdq_copy_state(cmdq_get_state(after), NULL);
	new_item = cmdq_get_command(pr->cmdlist, state);
	cmdq_insert_after(after, new_item);
	cmd_list_free(pr->cmdlist);
	cmdq_free_state(state);
	return (new_item);
}

static enum cmd_retval
cmd_upgrade_server_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	const char		*bin;
	char			*freeme = NULL, *layout_path = NULL;
	char			*cause = NULL, *cmd = NULL;
	struct cmdq_item	*save_item, *stop_item;
	struct session		*s;
	u_int			 n = 0;

	if (!args_has(args, 'y')) {
		RB_FOREACH(s, sessions, &sessions)
			n++;
		if (n == 0) {
			cmdq_error(item, "no sessions to upgrade");
			return (CMD_RETURN_ERROR);
		}
	}

	if ((bin = args_get(args, 'f')) == NULL)
		bin = freeme = cmd_upgrade_server_self_path();

	if (access(bin, X_OK) != 0) {
		cmdq_error(item, "%s: %s", bin, strerror(errno));
		free(freeme);
		return (CMD_RETURN_ERROR);
	}
	if (cmd_upgrade_server_check_binary(bin, &cause) != 0) {
		cmdq_error(item, "%s", cause);
		free(cause);
		free(freeme);
		return (CMD_RETURN_ERROR);
	}

	if ((layout_path = layout_get_path()) == NULL) {
		cmdq_error(item, "could not determine layout file location");
		free(freeme);
		return (CMD_RETURN_ERROR);
	}

	xasprintf(&cmd, "save-layout -f %s", layout_path);
	save_item = cmd_upgrade_server_queue(item, cmd, &cause);
	free(cmd);
	if (save_item == NULL) {
		cmdq_error(item, "%s", cause);
		free(cause);
		free(layout_path);
		free(freeme);
		return (CMD_RETURN_ERROR);
	}

	/*
	 * After save-layout completes, drop the autoload marker and stop the
	 * server. run-shell -b is intentional so SIGTERM is not blocked by the
	 * command queue waiting on itself.
	 */
	xasprintf(&cmd,
	    "run-shell -b \"printf '' > %s.autoload && kill -TERM %ld\"",
	    layout_path, (long)getpid());
	stop_item = cmd_upgrade_server_queue(save_item, cmd, &cause);
	free(cmd);
	if (stop_item == NULL) {
		cmdq_error(item, "%s", cause);
		free(cause);
		free(layout_path);
		free(freeme);
		return (CMD_RETURN_ERROR);
	}

	cmdq_print(item,
	    "upgrade-server: saving layouts then stopping server "
	    "(new binary: %s). Reattach after restart to restore layouts. "
	    "Note: processes inside panes will exit.",
	    bin);

	free(layout_path);
	free(freeme);
	return (CMD_RETURN_NORMAL);
}
