#!/bin/bash
# Before/after benchmark of build/old vs build/new (see
# tools/build_versions.sh). For each version:
#   - key press -> new position latency, with 2 and with 5 players
#   - server CPU per thread over 10 s with 5 players not moving
#   - client CPU per thread over 10 s (only if a display is available)
# These are the measurements behind the README's Optimizations table.
#
# Usage: tools/compare.sh      (after tools/build_versions.sh)
#
# Processes are stopped with SIGTERM, not SIGINT: a script's background
# jobs ignore SIGINT, and the old server has no handler to undo that

ROOT=$(cd "$(dirname "$0")/.." && pwd)

# Prints the CPU use of each of $1's own threads (not DDS's internal ones,
# which have their own names) and of the whole process, over $2 seconds
cpu() {
	local pid=$1 secs=$2 name hz t
	name=$(cat /proc/$pid/comm)
	hz=$(getconf CLK_TCK)
	declare -A start
	for t in /proc/$pid/task/*; do
		start[$t]=$(awk '{print $14 + $15}' $t/stat 2> /dev/null)
	done
	local total_start; total_start=$(awk '{print $14 + $15}' /proc/$pid/stat)
	sleep "$secs"
	local total_end; total_end=$(awk '{print $14 + $15}' /proc/$pid/stat)
	for t in /proc/$pid/task/*; do
		[ "$(cat $t/comm 2> /dev/null)" = "$name" ] || continue
		local role=worker
		[ "$(basename $t)" = "$pid" ] && role=main
		awk -v a="${start[$t]:-0}" -v b="$(awk '{print $14 + $15}' $t/stat)" \
			-v s="$secs" -v hz="$hz" -v n="$name" -v r="$role" \
			'BEGIN { c = (b - a) * 100 / (hz * s); if (c > 0 || r == "main") printf "    %s %s thread: %.1f%% CPU\n", n, r, c }'
	done
	awk -v a="$total_start" -v b="$total_end" -v s="$secs" -v hz="$hz" -v n="$name" \
		'BEGIN { printf "    %s whole process: %.1f%% CPU\n", n, (b - a) * 100 / (hz * s) }'
}

for version in old new; do
	DIR="$ROOT/build/$version"
	[ -x "$DIR/server" ] || { echo "no $DIR/server - run tools/build_versions.sh first"; exit 1; }
	echo "===== $version ====="

	for players in 2 5; do
		"$DIR/server" > /dev/null 2>&1 &
		SERVER=$!
		sleep 1
		IDLE=()
		for i in $(seq 2 $players); do
			"$DIR/bench" idle $((6000 + i)) 40 > /dev/null 2>&1 &
			IDLE+=($!)
		done
		sleep 3
		echo "  $players players, key press -> new position (40 moves):"
		"$DIR/bench" moves 6999 40 2>&1 | grep -v xtensibility | sed 's/^/    /'
		if [ "$players" = 5 ]; then
			echo "  5 players, nobody moving, server CPU over 10 s:"
			cpu $SERVER 10
		fi
		kill $SERVER "${IDLE[@]}" 2> /dev/null
		wait 2> /dev/null
		sleep 2
	done

	if [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; then
		"$DIR/server" > /dev/null 2>&1 &
		SERVER=$!
		sleep 1
		"$DIR/bench" idle 6100 30 > /dev/null 2>&1 &
		BENCH=$!
		"$DIR/client" > /dev/null 2>&1 &
		CLIENT=$!
		sleep 5
		echo "  client idle in a running game, CPU over 10 s:"
		cpu $CLIENT 10
		kill $CLIENT
		sleep 1
		kill $SERVER $BENCH 2> /dev/null
		wait 2> /dev/null
	fi
done
