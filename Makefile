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

.PHONY: all test clean tools integration bench sanitize

all: server client

# messages.h and messages.c are generated from messages.idl - do not edit by hand
messages.h messages.c: messages.idl
	$(IDLC) -l c messages.idl

server: server.c security.c game_logic.c grid_logic.c net_config.c messages.c
	$(CC) $(CFLAGS) -o server server.c security.c game_logic.c grid_logic.c net_config.c messages.c \
		$(CFLAGS_DDS) $(LIBS_DDS) $(CFLAGS_CRYPTO) $(LIBS_CRYPTO) -pthread

client: client.c render.c render.h stb_image.h security.c game_logic.c grid_logic.c net_config.c messages.c
	$(CC) $(CFLAGS) client.c render.c security.c game_logic.c grid_logic.c net_config.c messages.c -o client \
		$(CFLAGS_GLFW) -lGL $(LIBS_GLFW) $(CFLAGS_DDS) $(LIBS_DDS) \
		$(CFLAGS_CRYPTO) $(LIBS_CRYPTO) -pthread -lm

test_security: test_security.c security.c
	$(CC) $(CFLAGS) test_security.c security.c -o test_security $(CFLAGS_CRYPTO) $(LIBS_CRYPTO) -pthread

test_game_logic: test_game_logic.c game_logic.c
	$(CC) $(CFLAGS) test_game_logic.c game_logic.c -o test_game_logic

test_grid_logic: test_grid_logic.c grid_logic.c game_logic.c
	$(CC) $(CFLAGS) test_grid_logic.c grid_logic.c game_logic.c -o test_grid_logic

test_net_config: test_net_config.c net_config.c net_config.h
	$(CC) $(CFLAGS) test_net_config.c net_config.c -o test_net_config

test: test_security test_game_logic test_grid_logic test_net_config
	./test_security
	./test_game_logic
	./test_grid_logic
	./test_net_config

tools: $(TOOLS)

tools/%: tools/%.c security.c messages.c messages.h
	$(CC) $(CFLAGS) -I. $< security.c messages.c -o $@ \
		$(CFLAGS_DDS) $(LIBS_DDS) $(CFLAGS_CRYPTO) $(LIBS_CRYPTO) -pthread

# Join/replay/late-joiner/shutdown checks against the real server (~1 min)
integration: server tools
	bash tools/integration.sh

# Before/after benchmark: builds the pre-optimization commit and the current
# code side by side in build/, then measures both
bench:
	bash tools/build_versions.sh
	bash tools/compare.sh

# Rebuilds everything with ASan + UBSan and runs the unit and integration tests 
# Every sanitized program writes its report to sanitizer-logs/, including the server 
# and tools whose output tools/integration.sh hides, and the total is printed at the end. 
# The sanitized builds are removed afterwards, so a normal `make` starts clean
SAN_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_LOG    := sanitizer-logs/report

sanitize:
	$(MAKE) clean
	rm -rf sanitizer-logs && mkdir sanitizer-logs
	ASAN_OPTIONS=log_path=$(SAN_LOG) UBSAN_OPTIONS=log_path=$(SAN_LOG):print_stacktrace=1 \
		$(MAKE) CFLAGS="$(SAN_CFLAGS)" test integration; \
	status=$$?; \
	$(MAKE) clean > /dev/null; \
	n=$$(cat sanitizer-logs/* 2> /dev/null | grep -c -E 'ERROR: (Address|Leak)Sanitizer|runtime error'); \
	echo "Sanitizer reports: $$n (details in sanitizer-logs/)"; \
	[ $$status = 0 ] && [ $$n = 0 ]

clean:
	rm -f server client test_security test_game_logic test_grid_logic test_net_config messages.h messages.c
	rm -f $(TOOLS)
	rm -rf build
