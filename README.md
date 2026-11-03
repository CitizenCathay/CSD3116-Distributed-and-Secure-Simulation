# CSD3116-Distributed-and-Secure-Simulation

Multiplayer square-moving game over Cyclone DDS, GLFW and OpenGL. Built from
the Week 1 Team Lab baseline (`CSD3116 _ Week 01 _ Team Lab _ V0.2.pdf`).

## Quick start

Needs WSL (or macOS - see the spec PDF) with GLFW, OpenGL and Cyclone DDS dev
packages installed:

```bash
sudo apt install libgl1-mesa-dev libglfw3-dev cyclonedds-dev cyclonedds-tools pkg-config
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
| `client.c` | DDS client - one per player, renders the game and sends input |
| `game_logic.h`, `game_logic.c` | Pure game-rule logic (collision, spawn, movement) - no DDS/GLFW, unit-tested |
| `grid_logic.h`, `grid_logic.c` | Pure "color the floor" rules (cell claiming, scores, winner, round timer) - no DDS/GLFW, unit-tested |
| `security.h`, `security.c` | Auth + DDS-encryption hooks - wired into server/client, bodies still TODO stubs |
| `stb_easy_font.h` | Vendored third-party font renderer (draws player labels) |
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
  header comments for the API and `README`'s "Not yet implemented" below for
  what's still a stub.
- **Message flow**: client sends `JoinRequest` (with a credential) → server
  verifies it and assigns a player ID → client sends `Input` on keypress →
  server applies the move (collision-checked) and broadcasts `Position` to
  every client.
- **Threading**: server and client each run a DDS worker thread (networking)
  alongside their main thread (game-state / rendering). Details are in the
  comments at the top of each file.

## Not yet implemented

- `security.c` bodies (auth currently always accepts; encrypt/decrypt are
  stubbed out and fall back to plaintext)
- Code optimization pass
- "Should improve" items, e.g. supporting client leave/disconnect
