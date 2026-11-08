#!/bin/sh

# One-column side status: #{v:} stacks a value downward, host_* are sampled
# in-process, and a normal start shows that column. -f/dev/null keeps the
# previous tree side status so the rest of the suite's geometry stays put.

. ./regress-common.inc

PATH=/bin:/usr/bin
LC_ALL=C.UTF-8
export PATH LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
EMPTY=$(mktemp)
TMUXE="$TEST_TMUX -LtestE$$ -f$EMPTY"
OUT="$TEST_TMUX -LtestO$$ -f/dev/null"

fail() {
	echo "$*" >&2
	exit 1
}

cleanup() {
	stop_server $TMUX || exit 1
	stop_server $TMUXE || exit 1
	stop_server $OUT || exit 1
	rm -f "$EMPTY"
}
trap cleanup 0
trap 'exit 1' 1 2 15

$TMUX new-session -d -s col -x 40 -y 12 || exit 1

out=$($TMUX display-message -p '#{v;l:main}') || fail "v expand failed"
[ "$out" = "$(printf 'm\na\ni\nn')" ] || fail "v;l:main was '$out'"

out=$($TMUX display-message -p '#{v;l:é}') || fail "v utf8 failed"
[ "$out" = "é" ] || fail "v split a utf-8 character: '$out'"

out=$($TMUX display-message -p '#{host_cpu}') || fail "host_cpu failed"
printf '%s' "$out" | grep -Eq '^[0-9]+$' || fail "host_cpu '$out'"
[ "$out" -le 100 ] || fail "host_cpu '$out' over 100"

out=$($TMUX display-message -p '#{host_ram}') || fail "host_ram failed"
printf '%s' "$out" | grep -Eq '^[0-9]+$' || fail "host_ram '$out'"
[ "$out" -le 100 ] || fail "host_ram over 100"

out=$($TMUX display-message -p '#{host_disk}') || fail "host_disk failed"
printf '%s' "$out" | grep -Eq '^[0-9]+$' || fail "host_disk '$out'"
[ "$out" -le 100 ] || fail "host_disk over 100"

out=$($TMUX display-message -p '#{host_net}') || fail "host_net failed"
printf '%s' "$out" | grep -Eq '^[0-9]+[KMG]?$' || fail "host_net '$out'"

out=$($TMUX display-message -p '#{host_ram_usage}') || fail "host_ram_usage failed"
printf '%s' "$out" | grep -Eq '^[0-9]+/[0-9]+[BKMGT]$|^[0-9]+[BKMGT]/[0-9]+[BKMGT]$' ||
    fail "host_ram_usage '$out'"

out=$($TMUX display-message -p '#{host_disk_usage}') || fail "host_disk_usage failed"
printf '%s' "$out" | grep -Eq '^[0-9]+/[0-9]+[BKMGT]$|^[0-9]+[BKMGT]/[0-9]+[BKMGT]$' ||
    fail "host_disk_usage '$out'"

out=$($TMUX display-message -p '#{host_net_down}') || fail "host_net_down failed"
printf '%s' "$out" | grep -Eq '^[0-9]+[KMG]?$' || fail "host_net_down '$out'"
out=$($TMUX display-message -p '#{host_net_up}') || fail "host_net_up failed"
printf '%s' "$out" | grep -Eq '^[0-9]+[KMG]?$' || fail "host_net_up '$out'"

[ "$($TMUX show -gv side-status)" = off ] || fail "devnull side-status"
[ "$($TMUX show -gv side-status-width)" = 14 ] || fail "devnull width"
[ "$($TMUX show -gv status-interval)" = 15 ] || fail "devnull interval"

$TMUXE new-session -d -s col -x 40 -y 12 || exit 1
[ "$($TMUXE show -gv side-status)" = left ] || fail "column side-status"
[ "$($TMUXE show -gv side-status-width)" = 2 ] || fail "column width"
[ "$($TMUXE show -gv status-interval)" = 2 ] || fail "column interval"

# Letters of the session name run down the first column of the screen.
$TMUXE new-session -d -s work -n ed -x 40 -y 24 'sleep 60' || exit 1
$TMUXE set -g status off || exit 1
$OUT new-session -d -x 40 -y 24 || exit 1
$OUT send-keys "exec $TEST_TMUX -LtestE$$ -f$EMPTY attach -t work" Enter
sleep 2
col=$($OUT capture-pane -p | cut -c 1 | tr -d '\n')
printf '%s' "$col" | grep -q 'work' || fail "column was '$col'"
printf '%s' "$col" | grep -q 'C' || fail "cpu label missing in '$col'"
printf '%s' "$col" | grep -q 'N' || fail "net label missing in '$col'"

exit 0
