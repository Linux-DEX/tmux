#!/bin/sh

. ./regress-common.inc

# Exercise switch-mode -C (fuzzy command palette): list population, fuzzy
# filter, selection with a custom command template, and -k teardown.

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(test_tmpdir) || exit 1
TMUX_TMPDIR=$DIR
export TMUX_TMPDIR
INNER="$TEST_TMUX -LtestI$$ -f/dev/null"
OUTER="$TEST_TMUX -LtestO$$ -f/dev/null"

fail()
{
	echo "$*" >&2
	exit 1
}

cleanup()
{
	stop_server $OUTER || exit 1
	stop_server $INNER || exit 1
	rm -rf "$DIR"
}
trap cleanup 0 1 15

capture()
{
	$OUTER capture-pane -p -t outer:0.0 2>/dev/null
}

wait_capture()
{
	marker=$1
	i=0
	while [ "$i" -lt 50 ]; do
		captured=$(capture)
		printf '%s\n' "$captured" | grep -Fq "$marker" && return 0
		sleep 0.1
		i=$((i + 1))
	done
	fail "timed out waiting for '$marker'"
}

wait_count()
{
	marker=$1
	want=$2
	i=0
	while [ "$i" -lt 50 ]; do
		captured=$(capture)
		count=$(printf '%s\n' "$captured" | grep -Fc "$marker")
		[ "$count" -eq "$want" ] && return 0
		sleep 0.1
		i=$((i + 1))
	done
	fail "found $count '$marker' rows, expected $want"
}

wait_format()
{
	target=$1
	format=$2
	want=$3
	i=0
	while [ "$i" -lt 50 ]; do
		got=$($INNER display-message -p -t "$target" "$format" \
		    2>/dev/null)
		[ "$got" = "$want" ] && return 0
		sleep 0.1
		i=$((i + 1))
	done
	fail "$target format $format is '$got', expected '$want'"
}

wait_client_session()
{
	want=$1
	i=0
	while [ "$i" -lt 50 ]; do
		got=$($INNER list-clients -F '#{client_session}' 2>/dev/null)
		[ "$got" = "$want" ] && return 0
		sleep 0.1
		i=$((i + 1))
	done
	fail "client session is '$got', expected '$want'"
}

$INNER new-session -d -s pal -n main -x80 -y24 'exec sleep 100' || exit 1
$INNER set-option -g status off || exit 1

$OUTER new-session -d -s outer -x80 -y24 "$INNER attach -t pal" || exit 1
$OUTER set-option -g status off || exit 1
wait_client_session pal

# Default border type for this fork.
got=$($INNER show -gv pane-border-type)
[ "$got" = separate ] || fail "default pane-border-type is '$got', expected separate"

# Palette lists commands; custom template records selection.
$INNER set-option -g @picked '' || exit 1
$INNER switch-mode -C -t pal:main.0 -F 'CMD #{command_list_name}' \
	"set-option -g @picked '%%'" || exit 1
wait_capture 'CMD attach-session'
wait_capture 'CMD bind-key'
wait_format pal:main.0 '#{pane_mode}' switch-mode

# Fuzzy filter down to list-sessions, then pick it.
$INNER send-keys -t pal:main.0 -l listsess || exit 1
wait_capture 'CMD list-sessions'
wait_count 'CMD ' 1

$INNER send-keys -t pal:main.0 Enter || exit 1
wait_format pal:main.0 '#{pane_in_mode}' 0
[ "$($INNER show-option -gv @picked)" = list-sessions ] ||
	fail "palette selection was '$($INNER show-option -gv @picked)'"

# Escape cancels without running the template.
$INNER set-option -g @picked unchanged || exit 1
$INNER switch-mode -C -t pal:main.0 -F 'CMD #{command_list_name}' \
	"set-option -g @picked '%%'" || exit 1
wait_capture 'CMD '
$INNER send-keys -t pal:main.0 Escape || exit 1
wait_format pal:main.0 '#{pane_in_mode}' 0
[ "$($INNER show-option -gv @picked)" = unchanged ] ||
	fail "cancelled command palette ran its command"

# -k kills the disposable pane on exit.
kill_pane=$($INNER split-window -d -P -F '#{pane_id}' -t pal:main \
	'exec sleep 100') || exit 1
$INNER switch-mode -Ck -t "$kill_pane" -F 'CMD #{command_list_name}' || exit 1
wait_format "$kill_pane" '#{pane_mode}' switch-mode
$INNER send-keys -t "$kill_pane" Escape || exit 1
i=0
while [ "$i" -lt 50 ]; do
	$INNER list-panes -s -t pal -F '#{pane_id}' 2>/dev/null | \
	    grep -q -x "$kill_pane" || break
	sleep 0.1
	i=$((i + 1))
done
[ "$i" -lt 50 ] || fail "switch-mode -Ck did not kill its pane"

# Cycle helper: separate -> separate-active -> joined -> separate
$INNER set -w -t pal:main pane-border-type separate || exit 1
$INNER if -F '#{==:#{pane-border-type},separate}' \
	'set -w pane-border-type separate-active' || exit 1
[ "$($INNER show -wv -t pal:main pane-border-type)" = separate-active ] ||
	fail "cycle step to separate-active failed"
$INNER if -F '#{==:#{pane-border-type},separate-active}' \
	'set -w pane-border-type joined' || exit 1
[ "$($INNER show -wv -t pal:main pane-border-type)" = joined ] ||
	fail "cycle step to joined failed"
$INNER if -F '#{==:#{pane-border-type},joined}' \
	'set -w pane-border-type separate' || exit 1
[ "$($INNER show -wv -t pal:main pane-border-type)" = separate ] ||
	fail "cycle step back to separate failed"

exit 0
