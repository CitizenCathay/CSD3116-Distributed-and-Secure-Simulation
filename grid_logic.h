/*****************************************************************************
\file grid_logic.h
\author regine wong (w.shiyuregine@digipen.edu)
\co-author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/02/2026 (MM/DD/YYYY)
\brief Pure "color the floor" declarations: the grid of cell owners, claiming a
       cell, per-player scores, the winner and the round timer.
*******************************************************************************/
/* Pure "color the floor" rules shared by server.c and client.c, with no
DDS or GLFW dependency so it can be unit-tested on its own (see
test_grid_logic.c). The window is split into SQ_WIDTH x SQ_WIDTH cells;
a player claims the cell under the centre of their square. The server
owns the real Grid, the clients keep a copy in sync from CellClaimed
messages. */

#ifndef GRID_LOGIC_H
#define GRID_LOGIC_H

#include <stdbool.h>
#include "game_logic.h"

// Must match the cell range assumed in messages.idl's CellClaimed.
// Rows only span the playable strip (PLAY_HEIGHT), not the whole window,
// since the HUD margins at the top/bottom are never part of the grid
#define GRID_COLS (WIDTH / SQ_WIDTH)
#define GRID_ROWS (PLAY_HEIGHT / SQ_WIDTH)

// Owner value of a cell nobody has claimed (player IDs start at 1)
#define GRID_UNCLAIMED 0

// Length of a round in seconds (the server runs it, the client shows it)
#define ROUND_SECS 60

// Owner (player ID) of each cell, row-major: index = row * GRID_COLS + col
typedef struct
{
	int arr_owner[GRID_ROWS * GRID_COLS];
} Grid;

// Marks every cell as unclaimed
void grid_init(Grid *a_grid);

// Writes the column/row of the cell under the centre of a square whose
// NW corner is (a_int_x, a_int_y). Positions outside the window are
// clamped to the nearest edge cell.
void grid_cell_at(int a_int_x, int a_int_y, int *out_col, int *out_row);

// Sets the owner of one cell. Returns true if the cell is inside the
// grid and its owner actually changed; false otherwise (out of range or
// already owned by a_int_owner_id).
bool grid_set_owner(Grid *a_grid, int a_int_col, int a_int_row,
	int a_int_owner_id);

// Claims, for a_int_player_id, the cell under a square at (a_int_x,
// a_int_y), taking it from any previous owner. Writes that cell to
// *out_col/*out_row either way. Returns true if the owner changed, ie
// a CellClaimed message needs to be published.
bool grid_claim(Grid *a_grid, int a_int_player_id, int a_int_x, int a_int_y,
	int *out_col, int *out_row);

// Returns how many cells a_int_player_id currently owns
int grid_count_owned(const Grid *a_grid, int a_int_player_id);

// Returns the ID of the player (1..a_int_max_players) owning the most
// cells, or 0 if nobody owns a cell or the top score is tied (a draw).
int grid_winner(const Grid *a_grid, int a_int_max_players);

// Whole seconds left in a round of a_int_round_secs that started at
// a_ll_start_ns, rounded up and never negative. Times are in
// nanoseconds, as returned by dds_time().
int grid_seconds_left(long long a_ll_start_ns, long long a_ll_now_ns,
	int a_int_round_secs);

#endif // GRID_LOGIC_H
