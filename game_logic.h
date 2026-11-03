/*****************************************************************************
\file game_logic.h
\author Dr Khoo Teck Ping
\co-author regine wong (w.shiyuregine@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/02/2026 (MM/DD/YYYY)
\brief Pure game-rule declarations shared by the server and client: the player
       array layout, window/square sizes, collision, spawning and movement.
*******************************************************************************/
/* Pure game-rule logic shared by server.c and client.c, with no DDS or
GLFW dependency so it can be unit-tested on its own (see
test_game_logic.c). This is the single source of truth for the Player
layout and the window/square dimensions - server.c and client.c both
include this instead of each keeping their own copy. */

#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include <stdbool.h>

// The window is 800x600 and each player is a 50x50 square (Slide 7)
#define WIDTH    800
#define HEIGHT   600
#define SQ_WIDTH 50

// How many pixels a player moves per accepted input - a full cell, so
// positions (and the floor cell they claim) always stay grid-snapped
#define STEP     SQ_WIDTH

// Strip reserved for the HUD at the top and bottom of the window, so the
// grid and players never render underneath draw_hud()'s overlay bars.
// Both are exactly one cell tall, which keeps the playable area (below)
// a whole number of rows
#define HUD_MARGIN_TOP    SQ_WIDTH
#define HUD_MARGIN_BOTTOM SQ_WIDTH

// The vertical span players and the grid may actually use - y stays
// within [PLAY_TOP, PLAY_TOP + PLAY_HEIGHT - SQ_WIDTH]
#define PLAY_TOP    HUD_MARGIN_TOP
#define PLAY_HEIGHT (HEIGHT - HUD_MARGIN_TOP - HUD_MARGIN_BOTTOM)

// One row of the game state array: Active, X, Y (Slide 10, Figure 3).
// The player ID is the array index plus 1.
// Player coord defined as NW corner of square
typedef struct
{
	bool b_active;
	int x;
	int y;
} Player;

// Returns true if a SQ_WIDTH x SQ_WIDTH square at (a_int_x, a_int_y)
// would overlap any OTHER active player in a_arr_players[0..a_int_arr_len).
// a_int_id is the player ID (array index + 1) of the square being
// checked, so that player's own current entry is excluded from the
// check. Merely touching edges is NOT a collision (uses <, not <=), so
// two players may legally occupy adjacent grid cells.
bool detect_collision(const Player a_arr_players[], int a_int_arr_len,
	int a_int_id, int a_int_x, int a_int_y);

// Returns the index (0-based) of the first inactive entry in
// a_arr_players[0..a_int_arr_len), or -1 if all of them are active
// (ie the game is full).
int game_find_free_slot(const Player a_arr_players[], int a_int_arr_len);

// A new player's square appears at a random position (Slide 7).
// Picks a random position inside the window that does not collide
// with any other active player (per detect_collision()'s rules for
// a_int_id), and writes it to *out_x/*out_y. a_arr_players is not
// modified by this call. a_int_id should be the ID (array index + 1)
// the new player is about to take; that slot must still be inactive
// when this is called, since detect_collision() only skips a_int_id's
// own index - it does not know the slot is about to become active.
void game_random_spawn(const Player a_arr_players[], int a_int_arr_len,
	int a_int_id, int *out_x, int *out_y);

// A square may not intersect the window edges or another player's square
// (Slide 7).
// Computes a_int_id's position after moving one step in a_ch_direction
// ('u'/'d'/'l'/'r'; any other character is a no-op), checks it against
// the window edges and detect_collision(), and writes the result to
// *out_x/*out_y. a_arr_players is NOT modified - the caller applies
// the move.
// Returns true if the move is legal (*out_x/*out_y hold the new
// position); returns false if it is not (*out_x/*out_y are set to
// a_int_id's current, unchanged position).
bool game_try_move(const Player a_arr_players[], int a_int_arr_len,
	int a_int_id, char a_ch_direction, int *out_x, int *out_y);

#endif // GAME_LOGIC_H
