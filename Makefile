# Thin wrapper around the build commands in README.md.
# Needs idlc, pkg-config, GLFW3, CycloneDDS and OpenSSL (libcrypto) dev
# packages installed (see README.md "Quick start").

CC   := gcc
IDLC := idlc
# Compiler optimisation for every build (see README.md "Optimizations")
CFLAGS := -O2

CFLAGS_DDS    := $(shell pkg-config --cflags CycloneDDS)
LIBS_DDS      := $(shell pkg-config --libs CycloneDDS)
CFLAGS_GLFW   := $(shell pkg-config --cflags glfw3)
LIBS_GLFW     := $(shell pkg-config --libs glfw3)
CFLAGS_CRYPTO := $(shell pkg-config --cflags libcrypto 2>/dev/null)
LIBS_CRYPTO   := $(shell pkg-config --libs libcrypto 2>/dev/null || echo -lcrypto)

# Headless DDS tools for benchmarking and integration checks (see tools/)
TOOLS := tools/bench tools/join_test tools/late_joiner

.PHONY: all test clean tools integration bench

all: server client

# messages.h and messages.c are generated from messages.idl - do not edit by hand
messages.h messages.c: messages.idl
	$(IDLC) -l c messages.idl

server: server.c security.c game_logic.c grid_logic.c messages.c
	$(CC) $(CFLAGS) -o server server.c security.c game_logic.c grid_logic.c messages.c \
		$(CFLAGS_DDS) $(LIBS_DDS) $(CFLAGS_CRYPTO) $(LIBS_CRYPTO) -pthread

client: client.c render.c render.h stb_image.h security.c game_logic.c grid_logic.c messages.c
	$(CC) $(CFLAGS) client.c render.c security.c game_logic.c grid_logic.c messages.c -o client \
		$(CFLAGS_GLFW) -lGL $(LIBS_GLFW) $(CFLAGS_DDS) $(LIBS_DDS) \
		$(CFLAGS_CRYPTO) $(LIBS_CRYPTO) -pthread -lm

test_security: test_security.c security.c
	$(CC) $(CFLAGS) test_security.c security.c -o test_security $(CFLAGS_CRYPTO) $(LIBS_CRYPTO)

test_game_logic: test_game_logic.c game_logic.c
	$(CC) $(CFLAGS) test_game_logic.c game_logic.c -o test_game_logic

test_grid_logic: test_grid_logic.c grid_logic.c game_logic.c
	$(CC) $(CFLAGS) test_grid_logic.c grid_logic.c game_logic.c -o test_grid_logic

test: test_security test_game_logic test_grid_logic
	./test_security
	./test_game_logic
	./test_grid_logic

tools: $(TOOLS)

tools/%: tools/%.c security.c messages.c messages.h
	$(CC) $(CFLAGS) -I. $< security.c messages.c -o $@ \
		$(CFLAGS_DDS) $(LIBS_DDS) $(CFLAGS_CRYPTO) $(LIBS_CRYPTO)

# Join/replay/late-joiner/shutdown checks against the real server (~1 min)
integration: server tools
	bash tools/integration.sh

# Before/after benchmark: builds the pre-optimization commit and the current
# code side by side in build/, then measures both
bench:
	bash tools/build_versions.sh
	bash tools/compare.sh

clean:
	rm -f server client test_security test_game_logic test_grid_logic messages.h messages.c
	rm -f $(TOOLS)
	rm -rf build
