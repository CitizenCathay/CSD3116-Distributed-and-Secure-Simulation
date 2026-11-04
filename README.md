# CSD3116-Distributed-and-Secure-Simulation

Multiplayer square-moving game over Cyclone DDS, GLFW and OpenGL. Built from
the Week 1 Team Lab baseline (`CSD3116 _ Week 01 _ Team Lab _ V0.2.pdf`).

## Quick start

Needs WSL (or macOS - see the spec PDF) with GLFW, OpenGL, Cyclone DDS and
OpenSSL dev packages installed:

```bash
sudo apt install libgl1-mesa-dev libglfw3-dev cyclonedds-dev cyclonedds-tools libssl-dev pkg-config
```

Then:

```bash
make            # builds server + client (runs idlc for you)
./server        # terminal 1
./client        # terminal 2 - repeat in more terminals for up to 5 players
```

Click inside a client window, then use the arrow keys to move your square.

```bash
make test       # unit tests - no DDS/GLFW/idlc needed for these
make clean      # remove built binaries + idlc-generated files
```

(See `Makefile` for the exact `gcc`/`idlc` commands if you need to run a step
by hand.)

## Project structure

| File | What it is |
|---|---|
| `messages.idl` | DDS message/topic definitions |
| `messages.h`, `messages.c` | Generated from `messages.idl` by `idlc` - not committed (`.gitignore`), rebuilt by `make` |
| `server.c` | DDS server - owns the authoritative game state |
| `client.c` | DDS client - one per player: joins, sends input, mirrors the game state and runs the window |
| `render.h`, `render.c` | Client rendering - draws a frame (floor, players, HUD, notices, end screen) with OpenGL from a snapshot of the client's state |
| `game_logic.h`, `game_logic.c` | Pure game-rule logic (collision, spawn, movement) - no DDS/GLFW, unit-tested |
| `grid_logic.h`, `grid_logic.c` | Pure "color the floor" rules (cell claiming, scores, winner, round timer) - no DDS/GLFW, unit-tested |
| `security.h`, `security.c` | Pre-shared-key client auth + AES-256-GCM DDS-payload encryption (OpenSSL EVP), wired into server/client |
| `stb_easy_font.h` | Vendored third-party font renderer (used by `render.c` for all text) |
| `test_security.c`, `test_game_logic.c`, `test_grid_logic.c` | Unit tests - build with plain `gcc`, no other deps |
| `Makefile` | `make`, `make test`, `make clean` |

## Architecture

- **`server.c`** is the single source of truth: `arr_Players[MAX_PLAYERS]`,
  guarded by a mutex, written only by the server.
- **`game_logic.h`** is shared by both `server.c` and `client.c` instead of
  each keeping its own copy - `detect_collision()`, `game_try_move()`,
  `game_find_free_slot()`, `game_random_spawn()`. See the header comments
  there for exactly what each function does.
- **`security.h`** is the shared auth/encryption API, same deal - see its
  header comments for the API, and `security.c`'s header comment for the
  pre-shared-key scheme (two 32-byte secrets compiled into both client and
  server: one for `JoinRequest` authentication, one for AES-256-GCM). The
  join credential is an HMAC of the request's nonce, so the auth key itself
  is never sent. Once a message is encrypted, its plaintext fields are
  blanked (except the DDS key field, eg `Position.int_player_id`, which has
  to stay in the clear for routing) so the real data is only readable from
  the ciphertext. Those key fields are authenticated anyway: each
  ciphertext is bound (GCM associated data) to its message type and DDS key
  fields, so it cannot be moved into another message type or instance (eg
  player 2's `Position` republished as player 3's). `Input` is also bound to
  the join nonce its slot was admitted with. Each ciphertext carries a
  sequence number, and receivers track the last one per stream (per player,
  per cell, ...) and drop anything not newer - so recorded traffic cannot be
  replayed. Receivers drop any message that is unencrypted, fails to
  decrypt, or is a replay - there is no plaintext fallback.
- **Keys**: `CSD3116_AUTH_PSK` and `CSD3116_AES_KEY` (64 hex characters
  each) override the built-in keys; server and every client must use the
  same values. A malformed value makes the program exit instead of falling
  back to the built-in key. Generate one with `openssl rand -hex 32`.
- **Message flow**: client sends `JoinRequest` (with a credential) → server
  verifies it and assigns a player ID → client sends `Input` on keypress →
  server applies the move (collision-checked) and immediately broadcasts the
  new `Position` to every client. A `Position` is only published when it
  changes; it is transient-local, like `CellClaimed`, so a client that joins
  later still receives every player's current position. A repeated `JoinRequest` nonce (the client's own retries,
  or a replay) gets the same slot back rather than a new one, and the nonce
  of a player who has left is refused. A player leaves when their `Input`
  instance has no writers left (their client shut down DDS); a slot whose
  first `Input` never arrives within 10 seconds is freed too, since there
  is then no instance to lose its writers. The server then unregisters
  their `Position` instance. Disposed instances are ignored, since any DDS
  participant could dispose another player's instance to kick them.
- **Threading**: server and client each run a DDS worker thread (networking)
  alongside their main thread (game-state / rendering). Neither thread
  polls: each worker sleeps on a DDS waitset until a reader has data or the
  other thread triggers its guard condition, and the server's main thread
  sleeps on a condition variable until a join request is queued for it.
  Joins pass between the server's threads through small queues guarded by
  the mutex. Details are in the comments at the top of each file.

## Optimizations

Measured on WSL2, both versions built with `-O2`. The old build is the
security-hardened code before this pass. Latency was measured with a
headless DDS benchmark client; CPU was sampled from `/proc` per thread over
10 s; ThreadSanitizer was run on the server.

| Metric | Before | After |
|---|---|---|
| Input → Position latency, 2 players (mean) | 51.2 ms | 1.6 ms |
| Input → Position latency, 5 players (mean) | 156.4 ms | 1.5 ms |
| Server CPU, 5 idle players | 3.3% | 0.3% |
| Client DDS worker thread CPU, idle | 2.1% | 0.2% |
| Client frame rate | ~53 FPS | ~59.5 FPS |
| Client time from Ctrl+C to exit | - | 59 ms |

What changed:

- **No sleeping while holding the mutex.** The server slept 20 ms per
  active player inside the Position loop with the mutex locked, so latency
  grew with the number of players and the main thread was blocked for up to
  100 ms of every 100 ms tick. It also slept 20 ms after every JoinResponse.
- **Event-driven threads instead of 1 ms polling.** Both worker threads wait
  on a DDS waitset (read conditions + a guard condition); the server main
  thread waits on a `pthread_cond_t`; the client main thread only wakes the
  worker when the arrow keys actually change.
- **Publish on change.** The server sent every player's `Position` every
  100 ms, encrypting each one, whether or not anyone moved. Now a position
  is sent once, the moment it changes.
- **Non-blocking move throttle.** The client's worker slept 150 ms after
  every move while a key was held, so server updates queued up behind it.
  It now throttles by timestamp and keeps receiving in between.
- **No repeated computation in the render loop.** Per-player cell counts
  (drawn every frame) were a full scan of the grid per player per frame;
  `Grid` now keeps running counts, so `grid_count_owned()` is O(1).
- **Frame pacing.** The client slept a flat 16 ms after each frame on top of
  the time spent drawing; it now sleeps only for the rest of the 1/60 s
  frame budget.
- **Less time holding the lock.** The server copies shared state under the
  mutex and encrypts/publishes after releasing it.
- **`-O2`** for every build in the `Makefile`.

The higher frame rate is why the client's total CPU use (mostly Mesa's
software-rendering threads under WSL) went up, from 32% to 42%: it now
draws about 12% more frames.

## Known security limitations

- **One key shared by everyone.** Every client and the server hold the same
  AES key, so any client can decrypt all traffic and forge any message -
  server updates (`Position`, `CellClaimed`, `GameState`, `JoinResponse`) or
  another player's `Input` (the join nonces `Input` is bound to travel in
  the clear) - and it passes authentication and replay checks. The AAD
  binding and replay protection only stop parties *without* the key. Fixing
  this needs per-participant keys and signatures (eg DDS Security, or a
  per-client key agreed at join plus a server signing key).
- **The built-in keys are public.** They're in source control and kept on
  purpose so the game runs out of the box for grading. Unless both
  `CSD3116_AUTH_PSK` and `CSD3116_AES_KEY` are overridden, anyone with the
  repository has the keys.
- **Replay protection edges.** Sequence numbers are seeded from the
  sender's clock, so they assume a restarted process's clock hasn't gone
  backwards. A receiver accepts the first message it sees on each stream,
  so a freshly started client can briefly show a replayed (stale but
  genuine) server message until the next real one. DDS itself is
  unauthenticated, so an outsider can still push an unread sample out of a
  keep-last-1 reader cache: the replay is dropped, but the real sample is
  lost until that instance is next written. A `JoinRequest` recorded during
  a previous server run is not recognised by a restarted server; it can take
  a slot for up to 10 seconds before the no-`Input` timeout frees it.

See the header comment in `security.c` for details.

## Not yet implemented

- Real key provisioning/rotation for `security.c`'s pre-shared secrets
  (see Known security limitations above)
