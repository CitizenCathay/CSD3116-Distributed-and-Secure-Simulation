/*****************************************************************************
\file test_game_logic.c
\author regine wong (w.shiyuregine@digipen.edu)
\co-author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/02/2026 (MM/DD/YYYY)
\brief Dependency-free unit tests for game_logic.c: collision, free slot, spawn
       and movement rules.
*******************************************************************************/
/* Dependency-free unit tests for game_logic.h / game_logic.c:
detect_collision(), game_find_free_slot(), game_random_spawn() and
game_try_move().

    gcc test_game_logic.c game_logic.c -o test_game_logic
    ./test_game_logic

No DDS, GLFW, idlc or messages.h needed - game_logic.h only depends on
stdbool.h. */

#include <stdio.h>
#include <stdlib.h>
#include "game_logic.h"

static int g_int_tests_run = 0;
static int g_int_tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
	do { \
		g_int_tests_run++; \
		if (!(cond)) { \
			g_int_tests_failed++; \
			printf("FAIL %s:%d - %s\n", __FILE__, __LINE__, msg); \
		} \
	} while (0)

#define MAX_PLAYERS 5

static void reset_players(Player arr_players[MAX_PLAYERS])
{
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		arr_players[i].b_active = false;
		arr_players[i].x = 0;
		arr_players[i].y = 0;
	}
}

static void test_no_collision_when_far_apart(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	// Player 1 (index 0) at (0,0)
	arr_players[0].b_active = true;
	arr_players[0].x = 0;
	arr_players[0].y = 0;

	// Player 2 trying to move far away from player 1
	bool b_res = detect_collision(arr_players, MAX_PLAYERS, 2,
		500, 500);

	TEST_ASSERT(b_res == false, "squares far apart should not collide");
}

static void test_collision_when_fully_overlapping(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	arr_players[0].b_active = true;
	arr_players[0].x = 100;
	arr_players[0].y = 100;

	// Player 2 trying to move onto the exact same square
	bool b_res = detect_collision(arr_players, MAX_PLAYERS, 2,
		100, 100);

	TEST_ASSERT(b_res == true, "identical squares should collide");
}

static void test_no_collision_when_edges_touch(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	arr_players[0].b_active = true;
	arr_players[0].x = 0;
	arr_players[0].y = 0;

	// Player 2's square placed exactly SQ_WIDTH to the right - the
	// right edge of player 1 and the left edge of player 2 touch, ie
	// adjacent grid cells. detect_collision() uses < (not <=), so this
	// is legal: two players may stand on neighbouring cells
	bool b_res = detect_collision(arr_players, MAX_PLAYERS, 2,
		SQ_WIDTH, 0);

	TEST_ASSERT(b_res == false, "merely touching edges (adjacent cells) should not collide");
}

static void test_collision_when_overlapping_by_one_pixel(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	arr_players[0].b_active = true;
	arr_players[0].x = 0;
	arr_players[0].y = 0;

	// One pixel closer than the touching case above: a genuine overlap
	bool b_res = detect_collision(arr_players, MAX_PLAYERS, 2,
		SQ_WIDTH - 1, 0);

	TEST_ASSERT(b_res == true, "squares overlapping by even one pixel should collide");
}

static void test_ignores_inactive_players(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	// Player 1 occupies (100,100) but is NOT active (eg left the game)
	arr_players[0].b_active = false;
	arr_players[0].x = 100;
	arr_players[0].y = 100;

	bool b_res = detect_collision(arr_players, MAX_PLAYERS, 2,
		100, 100);

	TEST_ASSERT(b_res == false, "an inactive player's square should be ignored");
}

static void test_ignores_own_entry(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	// Player 1 (index 0) checking a move for itself - a_int_id == 1
	// means index 0 must be excluded from the check, even though it
	// is active and at the same spot (its own current position).
	arr_players[0].b_active = true;
	arr_players[0].x = 100;
	arr_players[0].y = 100;

	bool b_res = detect_collision(arr_players, MAX_PLAYERS, 1,
		100, 100);

	TEST_ASSERT(b_res == false, "a player's own entry should not collide with itself");
}

static void test_collision_against_any_other_active_player(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	arr_players[0].b_active = true;
	arr_players[0].x = 0;
	arr_players[0].y = 0;
	arr_players[2].b_active = true; // player 3, index 2
	arr_players[2].x = 300;
	arr_players[2].y = 300;

	// Player 4 tries to move onto player 3's square - should collide
	// even though player 1 is unrelated
	bool b_res = detect_collision(arr_players, MAX_PLAYERS, 4,
		300, 300);

	TEST_ASSERT(b_res == true, "should detect collision against any other active player");
}

static void test_find_free_slot_on_empty_array(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);

	int int_slot = game_find_free_slot(arr_players, MAX_PLAYERS);

	TEST_ASSERT(int_slot == 0, "first slot in an all-inactive array should be 0");
}

static void test_find_free_slot_skips_active(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	arr_players[0].b_active = true;
	arr_players[1].b_active = true;

	int int_slot = game_find_free_slot(arr_players, MAX_PLAYERS);

	TEST_ASSERT(int_slot == 2, "should return the first INACTIVE index");
}

static void test_find_free_slot_when_full(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	for (int i = 0; i < MAX_PLAYERS; i++)
		arr_players[i].b_active = true;

	int int_slot = game_find_free_slot(arr_players, MAX_PLAYERS);

	TEST_ASSERT(int_slot == -1, "a fully active array should report no free slot");
}

static void test_random_spawn_is_in_bounds_and_collision_free(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	// Pack most of the top play-area row with active players, leaving the
	// new player (slot index 4, ID 5) as the only free one
	arr_players[0] = (Player){ true, 0, PLAY_TOP };
	arr_players[1] = (Player){ true, 200, PLAY_TOP };
	arr_players[2] = (Player){ true, 400, PLAY_TOP };
	arr_players[3] = (Player){ true, 600, PLAY_TOP };

	srand(42); // deterministic for this test run
	int int_x = -1, int_y = -1;
	game_random_spawn(arr_players, MAX_PLAYERS, 5, &int_x, &int_y);

	TEST_ASSERT(int_x >= 0 && int_x <= WIDTH - SQ_WIDTH, "spawn x should be within the window");
	TEST_ASSERT(int_y >= PLAY_TOP && int_y <= PLAY_TOP + PLAY_HEIGHT - SQ_WIDTH,
		"spawn y should be within the playable strip, clear of the HUD margins");
	TEST_ASSERT(int_x % SQ_WIDTH == 0 && (int_y - PLAY_TOP) % SQ_WIDTH == 0,
		"spawn position should snap to the grid");
	TEST_ASSERT(detect_collision(arr_players, MAX_PLAYERS, 5, int_x, int_y) == false,
		"spawn position should not collide with any existing active player");
}

static void test_try_move_each_direction(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	arr_players[0] = (Player){ true, 400, 300 };
	int int_x, int_y;

	TEST_ASSERT(game_try_move(arr_players, MAX_PLAYERS, 1, 'u', &int_x, &int_y)
		&& int_x == 400 && int_y == 300 - STEP, "'u' should decrease y by STEP");
	TEST_ASSERT(game_try_move(arr_players, MAX_PLAYERS, 1, 'd', &int_x, &int_y)
		&& int_x == 400 && int_y == 300 + STEP, "'d' should increase y by STEP");
	TEST_ASSERT(game_try_move(arr_players, MAX_PLAYERS, 1, 'l', &int_x, &int_y)
		&& int_x == 400 - STEP && int_y == 300, "'l' should decrease x by STEP");
	TEST_ASSERT(game_try_move(arr_players, MAX_PLAYERS, 1, 'r', &int_x, &int_y)
		&& int_x == 400 + STEP && int_y == 300, "'r' should increase x by STEP");
}

static void test_moves_stay_grid_aligned(void)
{
	// STEP == SQ_WIDTH and the starting position is grid-aligned, so every
	// legal move should land on another multiple of SQ_WIDTH (grid snapping)
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	arr_players[0] = (Player){ true, 400, 300 };
	int int_x, int_y;

	game_try_move(arr_players, MAX_PLAYERS, 1, 'r', &int_x, &int_y);
	TEST_ASSERT(int_x % SQ_WIDTH == 0, "x should stay a multiple of SQ_WIDTH");
	TEST_ASSERT((int_y - PLAY_TOP) % SQ_WIDTH == 0,
		"y should stay a multiple of SQ_WIDTH above PLAY_TOP");
}

static void test_try_move_unrecognized_direction_is_noop(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	arr_players[0] = (Player){ true, 400, 300 };
	int int_x, int_y;

	bool b_moved = game_try_move(arr_players, MAX_PLAYERS, 1, 'n', &int_x, &int_y);

	TEST_ASSERT(b_moved == true, "an unrecognized direction has zero delta, so it is legal");
	TEST_ASSERT(int_x == 400 && int_y == 300, "position should be unchanged");
}

static void test_try_move_rejects_window_edge(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	arr_players[0] = (Player){ true, 0, PLAY_TOP };
	int int_x = -999, int_y = -999;

	bool b_moved = game_try_move(arr_players, MAX_PLAYERS, 1, 'l', &int_x, &int_y);

	TEST_ASSERT(b_moved == false, "moving past the left window edge should be rejected");
	TEST_ASSERT(int_x == 0 && int_y == PLAY_TOP, "rejected move should report the unchanged position");
}

static void test_try_move_rejects_top_hud_margin(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	// Already in the topmost play-area row
	arr_players[0] = (Player){ true, 0, PLAY_TOP };
	int int_x, int_y;

	bool b_moved = game_try_move(arr_players, MAX_PLAYERS, 1, 'u', &int_x, &int_y);

	TEST_ASSERT(b_moved == false, "moving above PLAY_TOP, into the HUD margin, should be rejected");
	TEST_ASSERT(int_x == 0 && int_y == PLAY_TOP, "rejected move should report the unchanged position");
}

static void test_try_move_rejects_bottom_hud_margin(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	// Already in the bottommost play-area row
	int int_bottom_y = PLAY_TOP + PLAY_HEIGHT - SQ_WIDTH;
	arr_players[0] = (Player){ true, 0, int_bottom_y };
	int int_x, int_y;

	bool b_moved = game_try_move(arr_players, MAX_PLAYERS, 1, 'd', &int_x, &int_y);

	TEST_ASSERT(b_moved == false, "moving below the play area, into the HUD margin, should be rejected");
	TEST_ASSERT(int_x == 0 && int_y == int_bottom_y, "rejected move should report the unchanged position");
}

static void test_try_move_rejects_collision_with_other_player(void)
{
	Player arr_players[MAX_PLAYERS];
	reset_players(arr_players);
	arr_players[0] = (Player){ true, 100, 100 };      // player 1
	arr_players[1] = (Player){ true, 100 + SQ_WIDTH, 100 }; // player 2, right next to player 1
	int int_x, int_y;

	// Player 2 tries to step left, straight into player 1's square
	bool b_moved = game_try_move(arr_players, MAX_PLAYERS, 2, 'l', &int_x, &int_y);

	TEST_ASSERT(b_moved == false, "moving into another active player should be rejected");
	TEST_ASSERT(int_x == arr_players[1].x && int_y == arr_players[1].y,
		"rejected move should report the unchanged position");
}

int main(void)
{
	test_no_collision_when_far_apart();
	test_collision_when_fully_overlapping();
	test_no_collision_when_edges_touch();
	test_collision_when_overlapping_by_one_pixel();
	test_ignores_inactive_players();
	test_ignores_own_entry();
	test_collision_against_any_other_active_player();

	test_find_free_slot_on_empty_array();
	test_find_free_slot_skips_active();
	test_find_free_slot_when_full();

	test_random_spawn_is_in_bounds_and_collision_free();

	test_try_move_each_direction();
	test_moves_stay_grid_aligned();
	test_try_move_unrecognized_direction_is_noop();
	test_try_move_rejects_window_edge();
	test_try_move_rejects_top_hud_margin();
	test_try_move_rejects_bottom_hud_margin();
	test_try_move_rejects_collision_with_other_player();

	printf("%d/%d tests passed\n",
		g_int_tests_run - g_int_tests_failed, g_int_tests_run);

	return g_int_tests_failed == 0 ? 0 : 1;
}
