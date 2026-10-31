/* game_logic.c
Implementation of game_logic.h. detect_collision() and the move/spawn
rules below are extracted verbatim (logic-wise) from server.c so they
can be built and unit-tested without pulling in DDS. */

#include "game_logic.h"
#include <stdlib.h>

// How many pixels is one step
#define STEP 1

bool detect_collision(const Player a_arr_players[], int a_int_arr_len,
	int a_int_id, int a_int_x, int a_int_y)
{
	bool b_res = false;
	// Loop through the array
	for (int i = 0; i < a_int_arr_len; i++)
	{
		// Index + 1 = Player ID
		// If a player other than the player itself is active
		if (i != a_int_id - 1 && a_arr_players[i].b_active == true)
		{
			// If they overlap horizontally
			if (a_int_x <= a_arr_players[i].x + SQ_WIDTH &&
				a_arr_players[i].x <= a_int_x + SQ_WIDTH)
			{
				// If they overlap vertically
				if (a_int_y <= a_arr_players[i].y + SQ_WIDTH &&
					a_arr_players[i].y <= a_int_y + SQ_WIDTH)
				{
					b_res = true;
					break;
				}
			}
		}
	}

	return b_res;
}

int game_find_free_slot(const Player a_arr_players[], int a_int_arr_len)
{
	for (int i = 0; i < a_int_arr_len; i++)
	{
		if (a_arr_players[i].b_active == false)
			return i;
	}
	return -1;
}

void game_random_spawn(const Player a_arr_players[], int a_int_arr_len,
	int a_int_id, int *out_x, int *out_y)
{
	int int_rand_x = 0;
	int int_rand_y = 0;
	while (1)
	{
		int_rand_x = rand() % (WIDTH - SQ_WIDTH + 1);
		int_rand_y = rand() % (HEIGHT - SQ_WIDTH + 1);
		if (!detect_collision(a_arr_players, a_int_arr_len, a_int_id, int_rand_x, int_rand_y))
			break;
	}
	*out_x = int_rand_x;
	*out_y = int_rand_y;
}

bool game_try_move(const Player a_arr_players[], int a_int_arr_len,
	int a_int_id, char a_ch_direction, int *out_x, int *out_y)
{
	int int_x = a_arr_players[a_int_id - 1].x;
	int int_y = a_arr_players[a_int_id - 1].y;

	switch (a_ch_direction)
	{
		case 'u':
			int_y = int_y - STEP;
			break;
		case 'd':
			int_y = int_y + STEP;
			break;
		case 'l':
			int_x = int_x - STEP;
			break;
		case 'r':
			int_x = int_x + STEP;
			break;

		default:
			break;
	}

	// Default to the unchanged position, in case the move turns out
	// to be illegal
	*out_x = a_arr_players[a_int_id - 1].x;
	*out_y = a_arr_players[a_int_id - 1].y;

	// Check collision with window
	if (int_x < 0 || int_x > WIDTH - SQ_WIDTH)
		return false;
	if (int_y < 0 || int_y > HEIGHT - SQ_WIDTH)
		return false;

	// Check collision with other players
	if (detect_collision(a_arr_players, a_int_arr_len, a_int_id, int_x, int_y))
		return false;

	*out_x = int_x;
	*out_y = int_y;
	return true;
}
