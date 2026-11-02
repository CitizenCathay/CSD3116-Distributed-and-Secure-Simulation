/*****************************************************************************
\file test_grid_logic.c
\author regine wong (w.shiyuregine@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/02/2026 (MM/DD/YYYY)
\brief Dependency-free unit tests for grid_logic.c: cell lookup, claiming, scoring,
       winner selection and the round timer.
*******************************************************************************/
/* Dependency-free unit tests for grid_logic.h / grid_logic.c:
grid_cell_at(), grid_set_owner(), grid_claim(), grid_count_owned(),
grid_winner() and grid_seconds_left().

    gcc test_grid_logic.c grid_logic.c -o test_grid_logic
    ./test_grid_logic

No DDS, GLFW, idlc or messages.h needed. */

#include <stdio.h>
#include "grid_logic.h"

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
#define NSECS 1000000000LL

static void test_init_clears_grid(void)
{
	Grid grid;
	grid.arr_owner[0] = 3;
	grid_init(&grid);
	TEST_ASSERT(grid_count_owned(&grid, 3) == 0, "init should clear old owners");
	TEST_ASSERT(grid.arr_owner[GRID_ROWS * GRID_COLS - 1] == GRID_UNCLAIMED,
		"init should clear the last cell");
}

static void test_cell_at_uses_square_centre(void)
{
	int int_col, int_row;

	// Square at the origin: centre is at (25,25), inside cell (0,0)
	grid_cell_at(0, 0, &int_col, &int_row);
	TEST_ASSERT(int_col == 0 && int_row == 0, "origin square is in cell 0,0");

	// Centre just short of the next cell boundary
	grid_cell_at(24, 24, &int_col, &int_row);
	TEST_ASSERT(int_col == 0 && int_row == 0, "centre at 49 is still cell 0,0");

	// Centre exactly on the boundary belongs to the next cell
	grid_cell_at(25, 25, &int_col, &int_row);
	TEST_ASSERT(int_col == 1 && int_row == 1, "centre at 50 is cell 1,1");
}

static void test_cell_at_bottom_right_corner(void)
{
	int int_col, int_row;
	grid_cell_at(WIDTH - SQ_WIDTH, HEIGHT - SQ_WIDTH, &int_col, &int_row);
	TEST_ASSERT(int_col == GRID_COLS - 1, "rightmost square is in the last column");
	TEST_ASSERT(int_row == GRID_ROWS - 1, "bottom square is in the last row");
}

static void test_cell_at_clamps_outside_window(void)
{
	int int_col, int_row;
	grid_cell_at(-500, -500, &int_col, &int_row);
	TEST_ASSERT(int_col == 0 && int_row == 0, "negative position clamps to 0,0");
	grid_cell_at(WIDTH * 2, HEIGHT * 2, &int_col, &int_row);
	TEST_ASSERT(int_col == GRID_COLS - 1 && int_row == GRID_ROWS - 1,
		"huge position clamps to the last cell");
}

static void test_set_owner_reports_change(void)
{
	Grid grid;
	grid_init(&grid);
	TEST_ASSERT(grid_set_owner(&grid, 2, 3, 1) == true, "claiming an empty cell changes it");
	TEST_ASSERT(grid.arr_owner[3 * GRID_COLS + 2] == 1, "owner is stored row-major");
	TEST_ASSERT(grid_set_owner(&grid, 2, 3, 1) == false, "same owner again is not a change");
	TEST_ASSERT(grid_set_owner(&grid, 2, 3, 2) == true, "another player can take the cell");
}

static void test_set_owner_rejects_out_of_range(void)
{
	Grid grid;
	grid_init(&grid);
	TEST_ASSERT(grid_set_owner(&grid, -1, 0, 1) == false, "negative column rejected");
	TEST_ASSERT(grid_set_owner(&grid, 0, -1, 1) == false, "negative row rejected");
	TEST_ASSERT(grid_set_owner(&grid, GRID_COLS, 0, 1) == false, "column past the end rejected");
	TEST_ASSERT(grid_set_owner(&grid, 0, GRID_ROWS, 1) == false, "row past the end rejected");
	TEST_ASSERT(grid_count_owned(&grid, 1) == 0, "rejected writes leave the grid untouched");
}

static void test_claim_takes_cell_and_reports_cell(void)
{
	Grid grid;
	int int_col, int_row;
	grid_init(&grid);

	bool b_changed = grid_claim(&grid, 1, 100, 100, &int_col, &int_row);
	TEST_ASSERT(b_changed == true, "first claim changes the cell");
	TEST_ASSERT(int_col == 2 && int_row == 2, "claimed cell is reported");
	TEST_ASSERT(grid.arr_owner[2 * GRID_COLS + 2] == 1, "player 1 owns the cell");

	b_changed = grid_claim(&grid, 1, 101, 101, &int_col, &int_row);
	TEST_ASSERT(b_changed == false, "re-claiming your own cell is not a change");
	TEST_ASSERT(int_col == 2 && int_row == 2, "cell is still reported when unchanged");
}

static void test_claim_overwrites_other_player(void)
{
	Grid grid;
	int int_col, int_row;
	grid_init(&grid);
	grid_claim(&grid, 1, 100, 100, &int_col, &int_row);
	bool b_changed = grid_claim(&grid, 2, 100, 100, &int_col, &int_row);
	TEST_ASSERT(b_changed == true, "stepping on another's cell takes it");
	TEST_ASSERT(grid_count_owned(&grid, 1) == 0, "previous owner loses the cell");
	TEST_ASSERT(grid_count_owned(&grid, 2) == 1, "new owner gains the cell");
}

static void test_winner_most_cells(void)
{
	Grid grid;
	grid_init(&grid);
	grid_set_owner(&grid, 0, 0, 1);
	grid_set_owner(&grid, 1, 0, 2);
	grid_set_owner(&grid, 2, 0, 2);
	TEST_ASSERT(grid_winner(&grid, MAX_PLAYERS) == 2, "player 2 has the most cells");
}

static void test_winner_tie_is_draw(void)
{
	Grid grid;
	grid_init(&grid);
	grid_set_owner(&grid, 0, 0, 1);
	grid_set_owner(&grid, 1, 0, 2);
	TEST_ASSERT(grid_winner(&grid, MAX_PLAYERS) == 0, "equal top scores is a draw");
}

static void test_winner_tie_below_top_is_not_draw(void)
{
	Grid grid;
	grid_init(&grid);
	grid_set_owner(&grid, 0, 0, 1);
	grid_set_owner(&grid, 1, 0, 1);
	grid_set_owner(&grid, 2, 0, 1);
	grid_set_owner(&grid, 3, 0, 2);
	grid_set_owner(&grid, 4, 0, 3);
	TEST_ASSERT(grid_winner(&grid, MAX_PLAYERS) == 1,
		"a tie for second place does not stop the leader winning");
}

static void test_winner_nobody_claimed(void)
{
	Grid grid;
	grid_init(&grid);
	TEST_ASSERT(grid_winner(&grid, MAX_PLAYERS) == 0, "empty grid has no winner");
}

static void test_seconds_left(void)
{
	TEST_ASSERT(grid_seconds_left(0, 0, 60) == 60, "full time at the start");
	TEST_ASSERT(grid_seconds_left(0, 1 * NSECS, 60) == 59, "one second elapsed");
	TEST_ASSERT(grid_seconds_left(0, NSECS / 2, 60) == 60, "half a second still rounds up to 60");
	TEST_ASSERT(grid_seconds_left(0, 59 * NSECS + NSECS / 2, 60) == 1,
		"last half second reads 1");
	TEST_ASSERT(grid_seconds_left(0, 60 * NSECS, 60) == 0, "zero when time is up");
	TEST_ASSERT(grid_seconds_left(0, 90 * NSECS, 60) == 0, "never negative");
	TEST_ASSERT(grid_seconds_left(10 * NSECS, 5 * NSECS, 60) == 60,
		"clock before the start counts as no time elapsed");
}

int main(void)
{
	test_init_clears_grid();
	test_cell_at_uses_square_centre();
	test_cell_at_bottom_right_corner();
	test_cell_at_clamps_outside_window();
	test_set_owner_reports_change();
	test_set_owner_rejects_out_of_range();
	test_claim_takes_cell_and_reports_cell();
	test_claim_overwrites_other_player();
	test_winner_most_cells();
	test_winner_tie_is_draw();
	test_winner_tie_below_top_is_not_draw();
	test_winner_nobody_claimed();
	test_seconds_left();

	printf("%d tests run, %d failed\n", g_int_tests_run, g_int_tests_failed);
	return g_int_tests_failed == 0 ? 0 : 1;
}
