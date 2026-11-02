/*****************************************************************************
\file grid_logic.c
\author regine wong (w.shiyuregine@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/02/2026 (MM/DD/YYYY)
\brief Implementation of grid_logic.h: cell lookup, claiming, scoring, winner and
       seconds-left calculations, with no DDS or GLFW dependency.
*******************************************************************************/
/* Implementation of grid_logic.h. */

#include "grid_logic.h"

#define NSECS_PER_SEC 1000000000LL

void grid_init(Grid *a_grid)
{
	for (int i = 0; i < GRID_ROWS * GRID_COLS; i++)
		a_grid->arr_owner[i] = GRID_UNCLAIMED;
}

void grid_cell_at(int a_int_x, int a_int_y, int *out_col, int *out_row)
{
	// Use the centre of the square, so exactly one cell is claimed
	int int_col = (a_int_x + SQ_WIDTH / 2) / SQ_WIDTH;
	int int_row = (a_int_y + SQ_WIDTH / 2) / SQ_WIDTH;

	if (int_col < 0)
		int_col = 0;
	if (int_col > GRID_COLS - 1)
		int_col = GRID_COLS - 1;
	if (int_row < 0)
		int_row = 0;
	if (int_row > GRID_ROWS - 1)
		int_row = GRID_ROWS - 1;

	*out_col = int_col;
	*out_row = int_row;
}

bool grid_set_owner(Grid *a_grid, int a_int_col, int a_int_row,
	int a_int_owner_id)
{
	if (a_int_col < 0 || a_int_col >= GRID_COLS ||
		a_int_row < 0 || a_int_row >= GRID_ROWS)
		return false;

	int *ptr_owner = &a_grid->arr_owner[a_int_row * GRID_COLS + a_int_col];
	if (*ptr_owner == a_int_owner_id)
		return false;

	*ptr_owner = a_int_owner_id;
	return true;
}

bool grid_claim(Grid *a_grid, int a_int_player_id, int a_int_x, int a_int_y,
	int *out_col, int *out_row)
{
	grid_cell_at(a_int_x, a_int_y, out_col, out_row);
	return grid_set_owner(a_grid, *out_col, *out_row, a_int_player_id);
}

int grid_count_owned(const Grid *a_grid, int a_int_player_id)
{
	int int_count = 0;
	for (int i = 0; i < GRID_ROWS * GRID_COLS; i++)
	{
		if (a_grid->arr_owner[i] == a_int_player_id)
			int_count++;
	}
	return int_count;
}

int grid_winner(const Grid *a_grid, int a_int_max_players)
{
	int int_best_id = 0;
	int int_best_count = 0;
	bool b_tied = false;

	for (int id = 1; id <= a_int_max_players; id++)
	{
		int int_count = grid_count_owned(a_grid, id);
		if (int_count > int_best_count)
		{
			int_best_id = id;
			int_best_count = int_count;
			b_tied = false;
		}
		else if (int_count == int_best_count && int_count > 0)
		{
			b_tied = true;
		}
	}

	if (b_tied)
		return 0;
	return int_best_id;
}

int grid_seconds_left(long long a_ll_start_ns, long long a_ll_now_ns,
	int a_int_round_secs)
{
	long long ll_elapsed = a_ll_now_ns - a_ll_start_ns;
	if (ll_elapsed < 0)
		ll_elapsed = 0;

	long long ll_left = (long long)a_int_round_secs * NSECS_PER_SEC - ll_elapsed;
	if (ll_left <= 0)
		return 0;

	// Round up, so the display reads 1 until the very last moment
	return (int)((ll_left + NSECS_PER_SEC - 1) / NSECS_PER_SEC);
}
