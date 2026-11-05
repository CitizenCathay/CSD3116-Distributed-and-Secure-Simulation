#!/bin/bash
# Builds two versions of the game side by side, with the same compiler
# flags, for before/after profiling:
#   build/old  - an older commit (default 5c982eb: the code before the
#                optimization pass)
#   build/new  - the current working tree, including uncommitted changes
# Each gets its own server, client and tools/bench.c, all compiled against
# that version's messages.idl - clients and bench must match the server
# they talk to, because the messages changed between versions.
#
# Usage: tools/build_versions.sh [old_commit] [cflags]
#   eg   tools/build_versions.sh 5c982eb "-O2 -g -fno-omit-frame-pointer"
# The default flags keep symbols and frame pointers, so perf/gprof/callgrind
# show readable call stacks.
set -e

OLD=${1:-5c982eb}
FLAGS=${2:-"-O2 -g -fno-omit-frame-pointer"}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIBS_DDS=$(pkg-config --cflags --libs CycloneDDS)
LIBS_CRYPTO=$(pkg-config --cflags --libs libcrypto 2>/dev/null || echo -lcrypto)
LIBS_GLFW=$(pkg-config --cflags --libs glfw3)

# Builds the sources in $1 (server, client, bench) into $1
build() {
	local dir=$1
	cd "$dir"
	idlc -l c messages.idl 2> /dev/null
	# Later versions add files - net_config.c (cross-machine play) and
	# render.c (render split) - so build with whichever exist
	local common="security.c game_logic.c grid_logic.c messages.c"
	[ -f net_config.c ] && common="$common net_config.c"
	gcc $FLAGS -o server server.c $common $LIBS_DDS $LIBS_CRYPTO -pthread
	local client_src="client.c $common"
	[ -f render.c ] && client_src="$client_src render.c"
	gcc $FLAGS -o client $client_src $LIBS_GLFW -lGL $LIBS_DDS $LIBS_CRYPTO -pthread -lm
	gcc $FLAGS -I. -o bench "$ROOT/tools/bench.c" security.c messages.c $LIBS_DDS $LIBS_CRYPTO
	echo "built $dir: server client bench"
}

rm -rf "$ROOT/build/old" "$ROOT/build/new"
mkdir -p "$ROOT/build/old" "$ROOT/build/new"

# Old version: exactly what was committed at $OLD
git -C "$ROOT" archive "$OLD" | tar -x -C "$ROOT/build/old"
# New version: the working tree's sources, so uncommitted changes count
cp "$ROOT"/*.c "$ROOT"/*.h "$ROOT"/messages.idl "$ROOT/build/new/"
rm -f "$ROOT/build/new/messages.c" "$ROOT/build/new/messages.h"

echo "flags: $FLAGS"
build "$ROOT/build/old"
build "$ROOT/build/new"
