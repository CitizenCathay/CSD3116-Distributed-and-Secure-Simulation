#!/bin/bash
# Puts a server under a fixed, repeatable load without opening any game
# windows: 4 idle players plus 1 player moving the whole time. Use it to
# profile the old and new server under exactly the same conditions.
#
# Usage: tools/load.sh <build dir> <secs>
#   eg, in one terminal:  perf record -g build/old/server
#       in another:       tools/load.sh build/old 20
#       then Ctrl+C the server and run perf report
# <build dir> must hold a bench built against the same version as the
# server (tools/build_versions.sh does this).

DIR=${1:?usage: tools/load.sh <build dir> <secs>}
SECS=${2:-20}
BENCH="$DIR/bench"
[ -x "$BENCH" ] || { echo "no $BENCH - run tools/build_versions.sh first"; exit 1; }

for i in 1 2 3 4; do
	"$BENCH" idle $((5100 + i)) "$SECS" > /dev/null 2>&1 &
done
# One mover, sending moves until the time is up
END=$(( $(date +%s) + SECS ))
ROUND=0
while [ "$(date +%s)" -lt "$END" ]; do
	"$BENCH" moves $((5200 + ROUND)) 40 2>&1 | grep -v xtensibility
	ROUND=$((ROUND + 1))
done
wait
