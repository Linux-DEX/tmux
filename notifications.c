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
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Server notifications: tuios-style toasts drawn in the status line.
 *
 * Alerts (bell, activity, silence) and pane exits are shown as a transient
 * message block at the right end of the status line: a severity cap glyph, an
 * icon, the text, then a countdown bar which burns down as the notification
 * ages (or "esc" for sticky notifications). The prefix followed by Escape
 * clears the queue; prefix followed by Enter jumps to the toast on screen.
 * Clicking the message switches to the pane it came from - going there is
 * the acknowledgment - and so does switching there by any other route,
 * through notifications_arrived.
 */

#include <sys/types.h>
#include <sys/time.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

#define NOTIFICATION_BAR_CELLS 5
#define NOTIFICATION_MIN_WIDTH 24

/* Jump target kinds for an external alert: none, bell, pane exited. */
#define NOTIFICATION_EXTERNAL_NONE 0
#define NOTIFICATION_EXTERNAL_BELL 1
#define NOTIFICATION_EXTERNAL_EXIT 2

/* No pane was stored. Pane ids start at 0, so 0 is a real pane. */
#define NOTIFICATION_NO_PANE ((u_int)-1)

/* Severity cap, mark and colour. */
static const char	*notifications_caps[] = { "▎", "▌", "▊" };
static const char	*notifications_marks[] = { "●", "⚠", "✕" };
static int		 notifications_fg[] = { 245, 3, 1 };

static const char	*notifications_duration_option(int);
static u_int		 notifications_count(struct client *);
static void		 notifications_delete(struct client *,
			     struct notification *);
static void		 notifications_push(struct session *, struct window *,
			     struct window_pane *, int, const char *, int);
static void		 notifications_external(int, const char *);
static void		 notifications_external_timer(__unused int,
			     __unused short, void *);
static void		 notifications_tick(__unused int, __unused short,
			     __unused void *);
static void		 notifications_expire(struct client *,
			     struct timeval *);
static void		 notifications_arm_timer(void);
static char		*notifications_squote(const char *);
static char		*notifications_ascript(const char *);
static char		*notifications_escape(const char *, const char *);
static char		*notifications_fit_text(const char *, u_int, u_int *);

/*
 * One event per server for the countdown bar: a one-second tick while any
 * notification can expire.
 */
static struct event	 notifications_timer;
static int		 notifications_timer_running;

/* Initialize the notification queue for a client. */
void
notifications_init(struct client *c)
{
	TAILQ_INIT(&c->notifications);
	c->notification_revision = 0;
}

/* Free the notification queue for a client. */
void
notifications_free(struct client *c)
{
	struct notification	*n, *n1;

	TAILQ_FOREACH_SAFE(n, &c->notifications, entry, n1)
		notifications_delete(c, n);
}

/* Count notifications for a client. */
static u_int
notifications_count(struct client *c)
{
	struct notification	*n;
	u_int			 count = 0;

	TAILQ_FOREACH(n, &c->notifications, entry)
		count++;
	return (count);
}

/* Remove and free a notification and note the change. */
static void
notifications_delete(struct client *c, struct notification *n)
{
	TAILQ_REMOVE(&c->notifications, n, entry);
	free(n->text);
	free(n);
	c->notification_revision++;
	server_status_client(c);
}

/*
 * Arm the tick timer: run every second while any notification can expire,
 * so the countdown bar is redrawn.
 */
static void
notifications_arm_timer(void)
{
	struct client		*c;
	struct notification	*n;
	struct timeval		 tv;
	int			 live = 0;

	if (!notifications_timer_running) {
		evtimer_set(&notifications_timer, notifications_tick, NULL);
		notifications_timer_running = 1;
	}

	TAILQ_FOREACH(c, &clients, entry) {
		TAILQ_FOREACH(n, &c->notifications, entry) {
			if (n->expiry.tv_sec != 0 || n->expiry.tv_usec != 0)
				live++;
		}
	}

	timerclear(&tv);
	tv.tv_sec = 1;
	if (live != 0)
		evtimer_add(&notifications_timer, &tv);
	else
		evtimer_del(&notifications_timer);
}

/* Tick: drop expired notifications for every client and re-arm. */
static void
notifications_tick(__unused int fd, __unused short events, __unused void *arg)
{
	struct client		*c;
	struct notification	*n;
	struct timeval		 now;

	gettimeofday(&now, NULL);
	TAILQ_FOREACH(c, &clients, entry) {
		notifications_expire(c, &now);
		/* The countdown bar only moves when the status line redraws. */
		TAILQ_FOREACH(n, &c->notifications, entry) {
			if (n->expiry.tv_sec == 0 && n->expiry.tv_usec == 0)
				continue;
			server_status_client(c);
			break;
		}
	}
	notifications_arm_timer();
}

/* Drop expired notifications for one client. */
static void
notifications_expire(struct client *c, struct timeval *now)
{
	struct notification	*n, *n1;

	TAILQ_FOREACH_SAFE(n, &c->notifications, entry, n1) {
		if (n->expiry.tv_sec == 0 && n->expiry.tv_usec == 0)
			continue;
		if (timercmp(now, &n->expiry, <))
			continue;
		notifications_delete(c, n);
	}
}

/* Shell single quote a string for use inside a command. */
static char *
notifications_squote(const char *s)
{
	char	*out, *p;

	p = out = xmalloc(4 * strlen(s) + 3);
	*p++ = '\'';
	while (*s != '\0') {
		if (*s == '\'') {
			*p++ = '\'';
			*p++ = '\\';
			*p++ = '\'';
			*p++ = '\'';
		} else
			*p++ = *s;
		s++;
	}
	*p++ = '\'';
	*p = '\0';
	return (out);
}

/* Escape a string for use inside an AppleScript double-quoted string. */
static char *
notifications_ascript(const char *s)
{
	char	*out, *p;
	size_t	 n = strlen(s);

	p = out = xmalloc(2 * n + 1);
	while (*s != '\0') {
		if (*s == '"' || *s == '\\')
			*p++ = '\\';
		*p++ = *s++;
	}
	*p = '\0';
	return (out);
}

/*
 * Kick off an external alert: a sound and a desktop notification, at most
 * once every three seconds. The shell always runs from the event loop, so
 * alerts that arrive inside the SIGCHLD handler are safe too.
 */
static void
notifications_external(int which, const char *text)
{
	static struct timeval	last_alert = { 0, 0 };
	struct timeval		now, tv;
	char			*cmd, *sound, *notify, *quoted, *script;

	if (!options_get_number(global_options, "notifications-sound") &&
	    !options_get_number(global_options, "notifications-desktop"))
		return;

	gettimeofday(&now, NULL);
	timersub(&now, &last_alert, &tv);
	if (tv.tv_sec >= 0 && tv.tv_sec < 3)
		return;

	if (options_get_number(global_options, "notifications-sound")) {
#ifdef __APPLE__
		xasprintf(&sound, "afplay /System/Library/Sounds/%s.aiff "
		    "2>/dev/null",
		    which == NOTIFICATION_EXTERNAL_EXIT ? "Tink" : "Glass");
#else
		sound = xstrdup("(if command -v paplay >/dev/null; then paplay "
		    "/usr/share/sounds/freedesktop/stereo/complete.oga; "
		    "elif command -v pw-play >/dev/null; then pw-play "
		    "/usr/share/sounds/freedesktop/stereo/complete.oga; "
		    "fi) 2>/dev/null");
#endif
	} else
		sound = xstrdup("");

	if (options_get_number(global_options, "notifications-desktop")) {
		/* Quote the message for the shell on both platforms. */
#ifdef __APPLE__
		script = notifications_ascript(text);
		xasprintf(&quoted, "display notification \"%s\" with title "
		    "\"tmux\"", script);
		free(script);
		script = quoted;
#else
		script = xstrdup(text);
#endif
		quoted = notifications_squote(script);
		free(script);
#ifdef __APPLE__
		xasprintf(&notify, "osascript -e %s 2>/dev/null", quoted);
#else
		xasprintf(&notify, "notify-send tmux %s 2>/dev/null", quoted);
#endif
		free(quoted);
	} else
		notify = xstrdup("");
	if (*sound != '\0' && *notify != '\0')
		xasprintf(&cmd, "%s; %s", sound, notify);
	else if (*sound != '\0')
		cmd = xstrdup(sound);
	else
		cmd = xstrdup(notify);
	free(sound);
	free(notify);
	if (*cmd == '\0') {
		free(cmd);
		return;
	}

	/* Deferred fork: alerts may arrive inside the SIGCHLD handler. */
	timerclear(&tv);
	if (event_once(-1, EV_TIMEOUT, notifications_external_timer, cmd,
	    &tv) != 0) {
		free(cmd);
		return;
	}
	memcpy(&last_alert, &now, sizeof last_alert);
}

/* Run the prepared external alert command from the event loop. */
static void
notifications_external_timer(__unused int fd, __unused short events,
    void *arg)
{
	char	*cmd = arg;

	/* job_run keeps its own copy of the command string. */
	job_run(cmd, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	    0, -1, -1);
	free(cmd);
}

/* Duration option for a severity. */
static const char *
notifications_duration_option(int severity)
{
	switch (severity) {
	case NOTIFICATION_WARNING:
		return ("notifications-warning-duration");
	case NOTIFICATION_ERROR:
		return ("notifications-error-duration");
	default:
		return ("notifications-duration");
	}
}

/*
 * Push a notification for every attached client of a session that is not
 * already looking at the window it came from.
 */
static void
notifications_push(struct session *s, struct window *w, struct window_pane *wp,
    int severity, const char *text, int external)
{
	struct client		*c;
	struct notification	*n, *n1;
	struct timeval		 now, tv;
	int			 pushed = 0;

	if (severity < NOTIFICATION_INFO || severity > NOTIFICATION_ERROR)
		severity = NOTIFICATION_INFO;

	gettimeofday(&now, NULL);
	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != s ||
		    c->flags & (CLIENT_UNATTACHEDFLAGS|CLIENT_CONTROL))
			continue;
		if (c->session->curw != NULL &&
		    c->session->curw->window == w)
			continue;	/* already there: not news */

		/* Drop an identical message already live so the new copy
		 * does not stack on top of it. */
		TAILQ_FOREACH_SAFE(n, &c->notifications, entry, n1) {
			if (strcmp(n->text, text) == 0) {
				notifications_delete(c, n);
				break;
			}
		}

		n = xcalloc(1, sizeof *n);
		n->severity = severity;
		n->text = xstrdup(text);
		n->count = 1;
		n->sticky = (severity == NOTIFICATION_ERROR &&
		    options_get_number(global_options,
		    "notifications-error-sticky"));
		n->duration = options_get_number(global_options,
		    notifications_duration_option(severity));
		n->pane_id = NOTIFICATION_NO_PANE;
		if (wp != NULL)
			n->pane_id = wp->id;
		if (w != NULL)
			n->window_id = w->id;
		if (s != NULL)
			n->session_id = s->id;

		/* Sticky notifications only go away with esc or a jump. */
		if (!n->sticky && n->duration != 0) {
			timerclear(&tv);
			tv.tv_sec = n->duration;
			timeradd(&now, &tv, &n->expiry);
		}

		/* Cap the queue, dropping the oldest first. */
		while (notifications_count(c) >= NOTIFICATION_MAX_COUNT &&
		    TAILQ_LAST(&c->notifications, notifications) != NULL)
			notifications_delete(c,
			    TAILQ_LAST(&c->notifications, notifications));

		TAILQ_INSERT_HEAD(&c->notifications, n, entry);
		c->notification_revision++;
		server_status_client(c);
		pushed = 1;
	}

	notifications_arm_timer();
	if (pushed && external != NOTIFICATION_EXTERNAL_NONE)
		notifications_external(external, text);
}

/*
 * An alert (bell, activity or silence) has been processed for a session:
 * show a toast, and forward the terminal bell if the option says so. This
 * replaces the visual-{bell,activity,silence} feedback while notifications
 * are on.
 */
void
notifications_alert(struct session *s, struct window *w,
    struct window_pane *wp, int severity, int which, const char *text)
{
	struct client		*c;
	int			 forward, external;

	if (!options_get_number(global_options, "notifications"))
		return;

	switch (which) {
	case NOTIFICATION_ALERT_BELL:
		forward = options_get_number(global_options,
		    "notifications-bell");
		external = NOTIFICATION_EXTERNAL_BELL;
		break;
	case NOTIFICATION_ALERT_ACTIVITY:
		forward = options_get_number(global_options,
		    "notifications-activity");
		external = NOTIFICATION_EXTERNAL_NONE;
		break;
	default:
		forward = options_get_number(global_options,
		    "notifications-silence");
		external = NOTIFICATION_EXTERNAL_NONE;
		break;
	}

	/* Forward the terminal bell to attached clients' terminals. */
	if (forward == NOTIFICATION_FORWARD ||
	    forward == NOTIFICATION_BOTH) {
		TAILQ_FOREACH(c, &clients, entry) {
			if (c->session != s ||
			    c->flags & (CLIENT_UNATTACHEDFLAGS|CLIENT_CONTROL))
				continue;
			tty_putcode(&c->tty, TTYC_BEL);
		}
	}
	if (forward == NOTIFICATION_NOTHING)
		return;

	notifications_push(s, w, wp, severity, text, external);
}

/*
 * A pane has exited: push a warning toast for it and play the exit sound.
 * Called from the SIGCHLD handler, so this only queues; the fork happens
 * from the event loop.
 */
void
notifications_pane_exited(struct window_pane *wp)
{
	struct winlink	*wl;
	char		*text;

	if (!options_get_number(global_options, "notifications"))
		return;

	xasprintf(&text, "Pane exited in %s", wp->window->name);
	TAILQ_FOREACH(wl, &wp->window->winlinks, wentry)
		notifications_push(wl->session, wp->window, wp,
		    NOTIFICATION_WARNING, text, NOTIFICATION_EXTERNAL_EXIT);
	free(text);
}

/*
 * A client has arrived at a window: notifications for that window are no
 * longer news, drop them. Called from session_set_current, so a jump from a
 * click or a manual switch is the acknowledgment.
 */
void
notifications_arrived(struct session *s, struct window *w)
{
	struct client		*c;
	struct notification	*n, *n1;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != s)
			continue;
		TAILQ_FOREACH_SAFE(n, &c->notifications, entry, n1) {
			if (n->window_id == w->id)
				notifications_delete(c, n);
		}
	}
}

/* Clear the whole notification queue for a client. */
void
notifications_clear(struct client *c)
{
	struct notification	*n, *n1;

	TAILQ_FOREACH_SAFE(n, &c->notifications, entry, n1)
		notifications_delete(c, n);
}

/*
 * Jump to the toast on screen (the newest one) and dismiss every toast for
 * that window. Prefers the client's own session when it has the target
 * window; otherwise any session that has it.
 */
void
notifications_jump(struct client *c)
{
	struct notification	*n, *n1;
	struct window_pane	*wp = NULL;
	struct window		*w = NULL;
	struct session		*ts, *best = NULL;
	struct winlink		*wl;

	n = TAILQ_FIRST(&c->notifications);
	if (n == NULL)
		return;

	/* Resolve the target session from the stored ids. */
	if (n->pane_id != NOTIFICATION_NO_PANE)
		wp = window_pane_find_by_id(n->pane_id);
	if (wp != NULL)
		w = wp->window;
	else {
		w = window_find_by_id(n->window_id);
		if (w == NULL)
			best = session_find_by_id(n->session_id);
	}

	/* Prefer the client's own session if it has the target window. */
	if (w != NULL) {
		TAILQ_FOREACH(wl, &w->winlinks, wentry) {
			ts = wl->session;
			if (ts == c->session) {
				best = ts;
				break;
			}
			if (best == NULL)
				best = ts;
		}
	}

	/*
	 * Going there is the acknowledgment. Drop this client's toasts for
	 * the window before any session switch, so a client that is about to
	 * move still loses them.
	 */
	TAILQ_FOREACH_SAFE(n, &c->notifications, entry, n1) {
		if (w == NULL) {
			notifications_delete(c, n);
			break;
		}
		if (n->window_id == w->id)
			notifications_delete(c, n);
	}

	if (best == NULL || c->flags & CLIENT_UNATTACHEDFLAGS)
		return;

	/* Find the target window in the chosen session and switch to it. */
	if (w != NULL) {
		wl = winlink_find_by_window(&best->windows, w);
		if (wl != NULL) {
			if (wp != NULL && wp->window == w)
				window_set_active_pane(w, wp, 1);
			session_set_current(best, wl);
		}
	}
	if (c->session != best)
		server_client_set_session(c, best);
	else
		server_redraw_client(c);
}

/*
 * Change signature of the notification queue for a client, used to force a
 * redraw when notifications change: the revision, plus how soon the first
 * notification expires so the countdown bar is redrawn every second.
 */
u_int64_t
notifications_signature(struct client *c)
{
	struct notification	*n = TAILQ_FIRST(&c->notifications);
	struct timeval		 now, left;
	u_int			 secs = 0;

	if (n != NULL) {
		if (n->sticky)
			secs = 1;
		else {
			gettimeofday(&now, NULL);
			timersub(&n->expiry, &now, &left);
			if (left.tv_sec < 0)
				secs = 0;
			else
				secs = (u_int)left.tv_sec + 1;
		}
	}
	return (((u_int64_t)c->notification_revision << 32) |
	    (u_int64_t)secs);
}

/*
 * Escape a string for insertion into a format string: any character in the
 * set given is doubled, so format_draw treats it as literal text.
 */
static char *
notifications_escape(const char *s, const char *set)
{
	char	*out, *p;
	size_t	 n = strlen(s) + 1;
	size_t	 i;

	p = out = xmalloc(2 * n + 1);
	while (*s != '\0') {
		for (i = 0; set[i] != '\0'; i++) {
			if (*s == set[i]) {
				*p++ = set[i];
				break;
			}
		}
		*p++ = *s++;
	}
	*p = '\0';
	return (out);
}

/*
 * Fit text into a wanted width in cells, cutting with an ellipsis if
 * needed. Returns the width actually used.
 */
static char *
notifications_fit_text(const char *text, u_int wanted, u_int *used)
{
	struct utf8_data	*ud;
	char			*out;
	const char		*ellipsis = "…";
	u_int			 ellipsis_w = 1;
	u_int			 w = 0, speed;
	size_t			 i, len;

	ud = utf8_fromcstr(text);
	if (ud == NULL || ud[0].have == 0) {
		*used = 0;
		free(ud);
		return (xstrdup(""));
	}

	/* Reserve the ellipsis when the whole string does not fit. */
	speed = 0;
	len = 0;
	for (i = 0; ud[i].have != 0; i++) {
		speed = ud[i].width;
		if (speed == 0xff)
			speed = 1;
		w += speed;
	}
	if (w > wanted && wanted > ellipsis_w)
		wanted -= ellipsis_w;
	else if (w > wanted)
		wanted = 0;
	w = 0;
	for (i = 0; ud[i].have != 0; i++) {
		speed = ud[i].width;
		if (speed == 0xff)
			speed = 1;
		if (w + speed > wanted)
			break;
		w += speed;
		len += ud[i].size;
	}

	out = xmalloc(len + strlen(ellipsis) + 1);
	memcpy(out, text, len);
	out[len] = '\0';
	if (ud[i].have != 0) {
		strlcat(out, ellipsis, len + strlen(ellipsis) + 1);
		w += ellipsis_w;
	}

	free(ud);
	*used = w;
	return (out);
}

/*
 * Draw the notification toast at the right end of the status line on top of
 * the already drawn status formats. Called from status_redraw.
 */
void
notifications_status_draw(struct client *c, struct screen_write_ctx *ctx,
    u_int width, struct grid_cell *gc)
{
	struct notification	*n;
	struct style_ranges	 tmp;
	struct style_range	*sr;
	struct status_line	*sl = &c->status;
	struct window_pane	*wp = NULL;
	const char		*cap, *mark;
	char			*fmt, *text, *escaped, *clean, *meta, *meta2;
	size_t			 i;
	u_int			 max_w, meta_w, text_w, text_max, block_w, xoff;
	u_int			 count, cells, rem, fg, lead;
	int			 use_range;
	struct timeval		 now, left;

	n = TAILQ_FIRST(&c->notifications);
	if (n == NULL)
		return;
	if (width < NOTIFICATION_MIN_WIDTH)
		return;

	/* Never wider than the cap; leave room to see the window list. */
	max_w = width - 4;
	if (max_w > NOTIFICATION_MAX_WIDTH)
		max_w = NOTIFICATION_MAX_WIDTH;

	/* Clean newlines and control characters out of the message. */
	clean = xstrdup(n->text);
	for (i = 0; clean[i] != '\0'; i++) {
		if (clean[i] == '\n' || clean[i] == '\r' || clean[i] == '\t')
			clean[i] = ' ';
	}

	/* The countdown bar (or esc for sticky), and the queued count. */
	count = notifications_count(c);
	if (n->sticky) {
		meta = xstrdup("esc");
	} else {
		gettimeofday(&now, NULL);
		timersub(&n->expiry, &now, &left);
		if (left.tv_sec < 0)
			rem = 0;
		else
			rem = (u_int)left.tv_sec;
		cells = 0;
		if (n->duration == 0)
			cells = NOTIFICATION_BAR_CELLS;
		else {
			cells = (rem * NOTIFICATION_BAR_CELLS +
			    n->duration - 1) / n->duration;
			if (cells > NOTIFICATION_BAR_CELLS)
				cells = NOTIFICATION_BAR_CELLS;
		}
		meta = xstrdup("");
		for (i = 0; i < NOTIFICATION_BAR_CELLS; i++) {
			xasprintf(&meta2, "%s%s", meta,
			    (i < cells ? "▰" : "▱"));
			free(meta);
			meta = meta2;
		}
	}

	/* "+N" for the queued notifications behind this one. */
	if (count > 1) {
		xasprintf(&meta2, "%s +%u", meta, count - 1);
		free(meta);
		meta = meta2;
	}
	meta_w = utf8_cstrwidth(meta);

	cap = notifications_caps[n->severity];
	mark = notifications_marks[n->severity];
	fg = notifications_fg[n->severity];

	/* cap, space, mark, space, then a space before the meta. */
	lead = utf8_cstrwidth(cap) + 1 + utf8_cstrwidth(mark) + 1;
	if (max_w < lead + 1 + meta_w + 6) {
		free(clean);
		goto out;
	}
	text_max = max_w - lead - 1 - meta_w;

	/* The text runs under the style stack, so double any "#". */
	escaped = notifications_escape(clean, "#");
	free(clean);
	text = notifications_fit_text(escaped, text_max, &text_w);
	free(escaped);

	/* One click target across the whole block when the pane exists. */
	if (n->pane_id != NOTIFICATION_NO_PANE)
		wp = window_pane_find_by_id(n->pane_id);
	use_range = (wp != NULL);
	if (use_range)
		xasprintf(&fmt, "#[range=pane|%u fg=colour%u]%s %s %s "
		    "#[fg=colour8]%s", wp->id, fg, cap, mark, text, meta);
	else
		xasprintf(&fmt, "#[fg=colour%u]%s %s %s #[fg=colour8]%s",
		    fg, cap, mark, text, meta);

	block_w = lead + text_w + 1 + meta_w;

	/*
	 * Draw the block, right aligned on the first status line.
	 * format_draw records ranges from the left of the block, not from
	 * the screen column, so shift them onto the status line.
	 */
	xoff = width - block_w;
	screen_write_cursormove(ctx, xoff, 0, 0);
	style_ranges_init(&tmp);
	format_draw(ctx, gc, block_w, fmt, &tmp, 0);

	/* Hand the ranges to the status line click machinery, inserted at
	 * the head so clicks on the block win over the underlying status
	 * ranges. */
	while ((sr = TAILQ_FIRST(&tmp)) != NULL) {
		TAILQ_REMOVE(&tmp, sr, entry);
		sr->start += xoff;
		sr->end += xoff;
		TAILQ_INSERT_HEAD(&sl->entries[0].ranges, sr, entry);
	}

	free(fmt);
	free(text);
out:
	free(meta);
}

/* Clear the queue, or jump to the toast on screen. */
static enum cmd_retval
cmd_clear_notifications_exec(struct cmd *self, struct cmdq_item *item)
{
	struct client	*c = cmdq_get_target_client(item);

	if (c == NULL)
		return (CMD_RETURN_NORMAL);
	if (args_has(cmd_get_args(self), 'j'))
		notifications_jump(c);
	else
		notifications_clear(c);
	return (CMD_RETURN_NORMAL);
}

const struct cmd_entry cmd_clear_notifications_entry = {
	.name = "clear-notifications",
	.description = "Clear notification toasts, or jump to the latest.",
	.alias = NULL,

	.args = { "c:j", 0, 0, NULL },
	.usage = "[-c target-client] [-j]",

	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_clear_notifications_exec
};
