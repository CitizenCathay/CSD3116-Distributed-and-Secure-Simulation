# CSD3116-Distributed-and-Secure-Simulation

Multiplayer square-moving game built on Cyclone DDS, GLFW and OpenGL.
Based on the Week 1 Team Lab baseline (`CSD3116 _ Week 01 _ Team Lab _ V0.2.pdf`).

## Project structure

```
messages.idl       DDS message/topic definitions (source of truth)
messages.h         Generated from messages.idl by idlc - do not edit by hand
messages.c         Generated from messages.idl by idlc - do not edit by hand
server.c           DDS server: owns the authoritative game state
client.c           DDS client: renders the game, reads player input
security.h         Shared auth + encryption API used by server.c and client.c
security.c         Implementation of security.h (wired in; bodies are still stubs)
game_logic.h       Shared Player struct + pure game-rule logic (no DDS/GLFW)
game_logic.c       Implementation of game_logic.h (detect_collision())
stb_easy_font.h    Third-party single-header font renderer (player labels)
test_security.c    Dependency-free unit tests for security.h / security.c
test_game_logic.c  Dependency-free unit tests for game_logic.h / game_logic.c
Makefile           Thin wrapper around the build/test commands below
```

### File responsibilities

- **messages.idl** - Defines the four DDS message types exchanged between
  server and client: `JoinRequest`, `JoinResponse`, `Input`, `Position`.
  Regenerate `messages.h`/`messages.c` with `idlc -l c messages.idl`
  whenever this file changes.

- **server.c** - The single source of truth for game state.
  - Maintains `arr_Players[MAX_PLAYERS]`, the authoritative array of
    `{b_active, x, y}` per player (array index + 1 = player ID).
  - DDS worker thread: authenticates and flags `JoinRequest` samples
    for the main thread to admit; applies `Input` samples directly via
    `game_try_move()` from `game_logic.h` (window-edge and collision
    checks included); periodically publishes a `Position` sample for
    each active player; and sends the `JoinResponse` once the main
    thread has assigned a new player ID.
  - Main thread: on a flagged join, finds a free slot
    (`game_find_free_slot()`) and picks a collision-free spawn
    (`game_random_spawn()`), both from `game_logic.h`.
  - A `mutex` guards `arr_Players` and the join-request flag/id, since
    they are written by the worker thread and read/written by main.
  - Security hooks: the worker thread verifies the client's credential
    (`security_verify_client_token()`) before flagging a `JoinRequest`
    for admission, decrypts `Input` payloads (`security_decrypt()`,
    falling back to the plaintext fields while unimplemented), and
    encrypts outgoing `Position` payloads (`security_encrypt()`).

- **client.c** - One instance per player.
  - DDS worker thread: receives `JoinResponse` (assigns this client's
    player ID) and `Position` samples (updates the local copy of
    `arr_Players` used for rendering).
  - Main thread: opens an 800x600 GLFW/OpenGL window, captures arrow-key
    input while the window has focus/was clicked, publishes `Input`
    samples, and renders all active players' squares + labels each
    frame via `stb_easy_font.h`.
  - Same `mutex` pattern as server.c protects the shared player array
    between the two threads.
  - Security hooks: fills a credential into `JoinRequest.arr_token`
    (`security_make_client_token()`), encrypts outgoing `Input`
    payloads (`security_encrypt()`), and decrypts incoming `Position`
    payloads (`security_decrypt()`, falling back to the plaintext
    fields while unimplemented).

- **security.h / security.c** - Wired into server.c/client.c, but the
  function bodies are still stubs (auth always succeeds, encrypt/decrypt
  always fail) - see "Not yet implemented" below.
  - `security_init()` / `security_cleanup()` - crypto/credential setup
    and teardown, called once at startup (`main()`) and shutdown.
  - `security_make_client_token()` - client fills `JoinRequest.arr_token`
    with this before publishing it.
  - `security_verify_client_token()` - server checks
    `JoinRequest.arr_token` with this before admitting the client
    (authentication requirement).
  - `security_encrypt()` / `security_decrypt()` - client encrypts each
    `Input` payload into `Input.arr_cipher`/`len_cipher` before
    publishing it, and server does the same for `Position`. The
    receiving side uses the cipher fields when `len_cipher > 0` and
    otherwise falls back to the existing plaintext fields (encryption
    requirement; the fallback is what currently always runs, since the
    stub bodies return false).

- **game_logic.h / game_logic.c** - Pure game-rule logic with no DDS or
  GLFW dependency, extracted out of `server.c` so it can be unit-tested
  on its own. Both `server.c` and `client.c` include this instead of
  each keeping their own copy of `Player`/`WIDTH`/`HEIGHT`/`SQ_WIDTH`.
  Holds:
  - `Player`, `WIDTH`, `HEIGHT`, `SQ_WIDTH` - the shared layout.
  - `detect_collision()` - true if a `SQ_WIDTH`-square at a given
    position would overlap any other *active* player; touching edges
    count as a collision.
  - `game_find_free_slot()` - first inactive index, or -1 if full.
  - `game_random_spawn()` - a random collision-free position for a
    new player.
  - `game_try_move()` - one step in a direction, checked against the
    window edges and `detect_collision()`; reports whether the move
    is legal without mutating the array (the caller applies it).

- **stb_easy_font.h** - Draws the "Player N" text label above each
  square; vendored, unmodified third-party code.

- **test_security.c** - Unit tests for `security.h`/`security.c` only.
  No DDS/GLFW dependency and no shared `main()` conflict with
  `server.c`/`client.c`, so it builds and runs with nothing but a C
  compiler. Currently pins down the *stub* behavior (auth always
  accepts, encrypt/decrypt always fail); each test has a TODO marking
  what to tighten once the corresponding `security.c` body is
  implemented.

- **test_game_logic.c** - Unit tests for `game_logic.h`/`game_logic.c`.
  - `detect_collision()`: no collision when far apart, collision when
    fully overlapping, the touching-edges boundary case (and one
    pixel clear of it), inactive players being ignored, a player's
    own entry not colliding with itself, and detecting a collision
    against any other active player (not just the first one checked).
  - `game_find_free_slot()`: first slot on an empty array, skipping
    already-active entries, and -1 when full.
  - `game_random_spawn()`: result stays within the window and does
    not collide with existing active players (seeded `srand()` for a
    reproducible run).
  - `game_try_move()`: correct delta for each of `u`/`d`/`l`/`r`, an
    unrecognized direction being a no-op, rejecting a move past the
    window edge, and rejecting a move into another active player -
    each rejected case also checks the reported position is left
    unchanged.

## Program flow

### Build

```
make
```

This is a thin wrapper (see `Makefile`) around the equivalent manual
commands:

```
idlc -l c messages.idl
gcc -o server server.c security.c game_logic.c messages.c $(pkg-config --cflags --libs CycloneDDS)
gcc client.c security.c game_logic.c messages.c -o client $(pkg-config --cflags --libs glfw3) -lGL $(pkg-config --cflags --libs CycloneDDS) -pthread -lm
```

### Run

1. Start the server: `./server`
2. Start up to five clients one by one: `./client`

### Test

```
make test
```

Equivalent to:

```
gcc test_security.c security.c -o test_security
./test_security

gcc test_game_logic.c game_logic.c -o test_game_logic
./test_game_logic
```

No `idlc`, DDS or GLFW needed for either one - they only exercise
`security.h`/`security.c` and `game_logic.h`/`game_logic.c` respectively.

### Sequence

1. **Startup** - Server creates its DDS participant, topics, readers and
   writers, then spawns its DDS worker thread and enters its main loop.
   Each client does the same, then opens its GLFW window.
2. **Join** - Client fills a credential into `JoinRequest.arr_token` and
   publishes it. Server's worker thread receives it, verifies the
   credential, and only then flags `b_player_join`; server's main
   thread finds a free slot and picks a collision-free random spawn
   (`game_find_free_slot()`/`game_random_spawn()`); the worker thread
   then publishes `JoinResponse` with the assigned player ID back to
   that client.
3. **Input loop** - On keypress, client encrypts and publishes an
   `Input` sample (player ID + direction). Server's worker thread
   receives it, decrypts it if a cipher was sent, and applies the move
   directly via `game_try_move()` if it does not collide with the
   window edge or another player's square, then periodically encrypts
   and publishes a `Position` sample for each active player.
4. **Render loop** - Every client's worker thread receives `Position`
   samples for all active players and updates its local
   `arr_Players` copy under the mutex; the main thread redraws all
   active squares + labels each GLFW frame.

## Not yet implemented

- `security.c` bodies (authentication + DDS encryption)
- Code optimization pass (see assignment slide 14)
- Any "should improve" items, e.g. supporting client leave/disconnect
