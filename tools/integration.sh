#!/bin/bash
# Integration checks against the real server over DDS - the behaviour unit
# tests cannot reach, because it needs a live server and several clients.
# Run with `make integration`, which builds ./server and the tools first.
# Takes about a minute; prints PASS/FAIL per check and exits non-zero if
# any check failed.
#
#   1. a repeated JoinRequest gets the same slot; a slot that never sends
#      Input is freed after 10 s and its nonce is then refused
#   2. a replayed Input moves the square only once
#   3. a client that joins late sees every current player, and not one who
#      has already left
#   4. the server exits cleanly (code 0) on Ctrl+C

cd "$(dirname "$0")/.."
FAILED=0
LOG=$(mktemp)

start_server() {
	./server > "$LOG" 2>&1 &
	SERVER=$!
	sleep 1
}

stop_server() {
	kill -INT $SERVER
	wait $SERVER
	SERVER_EXIT=$?
}

echo "== 1-2. join and replay protection"
start_server
tools/join_test retry 7001 12 &
RETRY=$!
sleep 0.5
tools/join_test replay 7002 || FAILED=1
wait $RETRY || FAILED=1
stop_server

echo "== 3. late joiner"
start_server
tools/bench idle 7101 20 > /dev/null 2>&1 &
P1=$!
sleep 1.5
tools/bench idle 7102 5 > /dev/null 2>&1 &
P2=$!
sleep 1.5
tools/bench idle 7103 20 > /dev/null 2>&1 &
P3=$!
# Player 2 leaves; 1 and 3 stay
wait $P2
sleep 3
SEEN=$(tools/late_joiner 2> /dev/null | grep 'late joiner sees')
echo "$SEEN (expected: 1 3)"
if [ "$SEEN" = "late joiner sees: 1 3" ]; then
	echo "PASS: late joiner gets the current players and not the one who left"
else
	echo "FAIL: late joiner saw the wrong players"
	FAILED=1
fi
kill -INT $P1 $P3 2> /dev/null
wait $P1 $P3 2> /dev/null

echo "== 4. clean shutdown"
stop_server
if [ "$SERVER_EXIT" = 0 ] && grep -q "Server stopped" "$LOG"; then
	echo "PASS: server exits cleanly on Ctrl+C"
else
	echo "FAIL: server exit code $SERVER_EXIT"
	FAILED=1
fi
rm -f "$LOG"

echo
[ $FAILED = 0 ] && echo "All integration checks passed" || echo "Some integration checks FAILED"
exit $FAILED
