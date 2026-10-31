# Thin wrapper around the build commands in README.md.
# Needs idlc, pkg-config, GLFW3 and CycloneDDS dev packages installed
# (see README.md "Setting up"). test_security needs none of those.

CC   := gcc
IDLC := idlc

CFLAGS_DDS  := $(shell pkg-config --cflags CycloneDDS)
LIBS_DDS    := $(shell pkg-config --libs CycloneDDS)
CFLAGS_GLFW := $(shell pkg-config --cflags glfw3)
LIBS_GLFW   := $(shell pkg-config --libs glfw3)

.PHONY: all test clean

all: server client

# messages.h and messages.c are generated from messages.idl - do not edit by hand
messages.h messages.c: messages.idl
	$(IDLC) -l c messages.idl

server: server.c security.c game_logic.c messages.c
	$(CC) -o server server.c security.c game_logic.c messages.c $(CFLAGS_DDS) $(LIBS_DDS)

client: client.c security.c game_logic.c messages.c
	$(CC) client.c security.c game_logic.c messages.c -o client \
		$(CFLAGS_GLFW) -lGL $(CFLAGS_DDS) $(LIBS_DDS) -pthread -lm

test_security: test_security.c security.c
	$(CC) test_security.c security.c -o test_security

test_game_logic: test_game_logic.c game_logic.c
	$(CC) test_game_logic.c game_logic.c -o test_game_logic

test: test_security test_game_logic
	./test_security
	./test_game_logic

clean:
	rm -f server client test_security test_game_logic messages.h messages.c
