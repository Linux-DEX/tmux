#!/bin/sh
# Minimal round-trip check for layout JSON v2 + float-layer / stacked.
# Uses a private socket so it does not touch a running server.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMUX="${ROOT}/tmux"
SOCK="dex-layout-check-$$"
TMP=$(mktemp -d)
LAYOUT="$TMP/layout.json"
export TMUX_TMPDIR="$TMP"

cleanup() {
	"$TMUX" -L "$SOCK" kill-server 2>/dev/null || true
	rm -rf "$TMP"
}
trap cleanup EXIT

die() {
	echo "FAIL: $*" >&2
	exit 1
}

"$TMUX" -V >/dev/null || die "tmux binary missing at $TMUX"

"$TMUX" -L "$SOCK" -f /dev/null set-option -g exit-empty off \; \
	new-session -d -s check -n win -- sleep 120
"$TMUX" -L "$SOCK" split-window -t check:0 -d -- sleep 120
"$TMUX" -L "$SOCK" set-option -g layout-autosave off
"$TMUX" -L "$SOCK" save-layout -f "$LAYOUT" >/dev/null

grep -q '"version":2' "$LAYOUT" || die "expected version 2 in $LAYOUT"
grep -q '"sessions"' "$LAYOUT" || die "missing sessions"
grep -q '"cwd"' "$LAYOUT" || die "missing cwd"

"$TMUX" -L "$SOCK" kill-server 2>/dev/null || true

# Restore as shells only so panes stay alive for the rest of the check.
"$TMUX" -L "$SOCK" -f /dev/null set-option -g exit-empty off \; \
	set-option -g layout-restore-commands off \; \
	load-layout -f "$LAYOUT" >/dev/null \
	|| die "load-layout failed"

"$TMUX" -L "$SOCK" has-session -t check 2>/dev/null \
	|| die "session check not restored"

"$TMUX" -L "$SOCK" list-commands | grep -q toggle-float-layer \
	|| die "toggle-float-layer not registered"
"$TMUX" -L "$SOCK" list-commands | grep -q pin-pane \
	|| die "pin-pane not registered"

"$TMUX" -L "$SOCK" new-pane -t check:win >/dev/null 2>&1 || true
"$TMUX" -L "$SOCK" toggle-float-layer -t check:win >/dev/null \
	|| die "toggle-float-layer failed"
"$TMUX" -L "$SOCK" select-layout -t check:win stacked >/dev/null \
	|| die "select-layout stacked failed"

echo "OK: layout v2 round-trip, float-layer, pin-pane, stacked"
