/*****************************************************************************
\file client.c
\author Dr Khoo Teck Ping
\co-author regine wong (w.shiyuregine@digipen.edu)
\co-author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/02/2026 (MM/DD/YYYY)
\brief DDS game client: joins the server, sends the arrow-key input, mirrors the
       server's game state, and renders the grid, players and HUD with
       OpenGL/GLFW.
*******************************************************************************/
/* gcc client.c security.c game_logic.c grid_logic.c messages.c -o client $(pkg-config --cflags --libs glfw3) -lGL $(pkg-config --cflags --libs CycloneDDS) -pthread -lm*/

/* Lecture slide references in this file are to "CSD3116 Week 01 Team Lab"
(Dr Khoo Teck Ping): Slide 7 baseline programs, Slide 8 setting up (GLFW,
OpenGL), Slide 10 game state, Slide 11 publishing and subscribing,
Slide 12 threading, Slide 14 tasks. */

/* Dr Khoo Teck Ping
27 Aug 2026
1. DDS client
2. Supports max 5 clients
3. Start server, then start client one by one
4. Each client can see itself and all other clients
5. Each client can move itself using the arrow keys
6. Collision detection among all players and the window is supported */

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "dds/dds.h"
#include "messages.h"
#include "security.h"
#include "game_logic.h"
#include "grid_logic.h"
#include "stb_easy_font.h"

#define STB_EASY_FONT_IMPLEMENTATION

#define MAX_SAMPLES 1

// How often a held arrow key sends another Input while STEP moves a full
// grid cell at a time - too fast and the square blurs across several
// tiles per second instead of stepping cell by cell
#define MOVE_REPEAT_MSECS 150

// Target time per rendered frame (60 frames per second)
#define FRAME_SECS (1.0 / 60.0)

// One RGB colour per player, indexed by player ID minus 1
static const float arr_colors[MAX_PLAYERS][3] =
{
	{ 0.90f, 0.20f, 0.20f },
	{ 0.20f, 0.45f, 0.90f },
	{ 0.20f, 0.70f, 0.30f },
	{ 0.95f, 0.75f, 0.10f },
	{ 0.65f, 0.30f, 0.80f }
};

// ### GLOBAL VARIABLES ###
int int_player_id = -1;

// Table to track clients
// Init all members to 0
// The client's copy of the server's game state array. It is only changed
// by Position messages from the server, and only active players are drawn
// (Slide 10). Player ID is the array index plus 1
static Player arr_Players[MAX_PLAYERS] = {0};

// Local copy of who owns which cell, kept in sync from the server's
// CellClaimed messages. Shared with the main thread, so guarded by mutex
static Grid grid;

// Latest round state from the server's GameState messages. Guarded by mutex
static int int_seconds_left = 0;
static bool b_round_started = false;
static bool b_game_over = false;
static int int_winner_id = 0;

// Cleared by the main thread when the window closes, so the worker thread
// leaves its loop and shuts DDS down. Guarded by mutex
static bool b_running = true;

// True while the server has refused us a slot because the game is full. We
// then just watch (the state messages arrive regardless) and keep retrying
// the join, so we get in automatically if a player leaves. Guarded by mutex
static bool b_spectating = false;

// Set by Ctrl+C / SIGTERM in the terminal, so the client leaves the game the
// same clean way as closing the window does
static volatile sig_atomic_t g_quit_requested = 0;

static void on_quit_signal(int a_int_signal)
{
	(void)a_int_signal;
	g_quit_requested = 1;
}

// "Player N left" messages shown briefly on screen
#define MAX_NOTICES 3
#define NOTICE_SECS 4.0
#define NOTICE_FADE_SECS 0.5

typedef struct
{
	int int_player_id;   // 0 = empty slot
	double d_time;       // glfwGetTime() when the player left
} LeaveNotice;

// Written by the worker thread, drawn by the main thread. Guarded by mutex
static LeaveNotice arr_notices[MAX_NOTICES];

// Records that a_int_player_id just left, reusing an empty slot or else
// replacing the oldest notice. The caller must hold mutex
static void push_leave_notice(int a_int_player_id)
{
	int int_slot = 0;
	for (int i = 0; i < MAX_NOTICES; i++)
	{
		if (arr_notices[i].int_player_id == 0)
		{
			int_slot = i;
			break;
		}
		if (arr_notices[i].d_time < arr_notices[int_slot].d_time)
			int_slot = i;
	}
	arr_notices[int_slot].int_player_id = a_int_player_id;
	arr_notices[int_slot].d_time = glfwGetTime();
}

// Direction - n means no direction. Guarded by mutex
char ch_dir = 'n';
// Guard condition in the worker thread's waitset. The main thread triggers
// it (wake_worker()) when ch_dir or b_running changes, so the worker sleeps
// until there is something to do instead of polling. 0 until the worker
// has created it. Guarded by mutex
static dds_entity_t wake_guard = 0;
// Guards the variables shared by the main (rendering) thread and the DDS
// worker thread. Every lock must be matched by an unlock (Slide 12)
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void draw_text(float x, float y, float scale, const char *text)
{
    // stb_easy_font silently stops drawing once this buffer is full, and each
    // character needs a few hundred bytes - 4096 cut off strings of about 22
    // characters (eg "Player 2 left the game"), so leave plenty of room
    char buffer[16384];

    int num_quads = stb_easy_font_print(
        0, 0,
        (char *)text,
        NULL,
        buffer,
        sizeof(buffer)
    );

    glPushMatrix();

    glTranslatef(x, y, 0.0f);
    glScalef(scale, scale, 1.0f);

    glEnableClientState(GL_VERTEX_ARRAY);

    glVertexPointer(2, GL_FLOAT, 16, buffer);
    glDrawArrays(GL_QUADS, 0, num_quads * 4);

    glDisableClientState(GL_VERTEX_ARRAY);

    glPopMatrix();
}

// ### DRAWING HELPERS ###
// All drawing is in window pixels, origin top-left (see glOrtho in main).
// Blending is enabled once at start-up, so every colour here takes an alpha.

static const float arr_white[3] = { 1.0f, 1.0f, 1.0f };
static const float arr_alarm[3] = { 1.0f, 0.35f, 0.35f };

// How long a newly claimed cell takes to pop in, in seconds
#define CLAIM_ANIM_SECS 0.25f

// Per-cell animation state: the owner last drawn, and seconds since that
// owner changed. Only used by the main (rendering) thread
static int arr_shown_owner[GRID_ROWS * GRID_COLS];
static float arr_claim_age[GRID_ROWS * GRID_COLS];

// Fills a rectangle with the current colour
static void draw_rect(float a_f_x, float a_f_y, float a_f_w, float a_f_h)
{
	glBegin(GL_QUADS);
		glVertex2f(a_f_x, a_f_y);
		glVertex2f(a_f_x, a_f_y + a_f_h);
		glVertex2f(a_f_x + a_f_w, a_f_y + a_f_h);
		glVertex2f(a_f_x + a_f_w, a_f_y);
	glEnd();
}

// Outlines a rectangle with the current colour and line width
static void draw_rect_outline(float a_f_x, float a_f_y, float a_f_w, float a_f_h)
{
	glBegin(GL_LINE_LOOP);
		glVertex2f(a_f_x, a_f_y);
		glVertex2f(a_f_x, a_f_y + a_f_h);
		glVertex2f(a_f_x + a_f_w, a_f_y + a_f_h);
		glVertex2f(a_f_x + a_f_w, a_f_y);
	glEnd();
}

// Text with a soft drop shadow, so it reads on any background
static void draw_label(float a_f_x, float a_f_y, float a_f_scale,
	const float *a_arr_rgb, float a_f_alpha, const char *a_str_text)
{
	glColor4f(0.0f, 0.0f, 0.0f, a_f_alpha * 0.6f);
	draw_text(a_f_x + a_f_scale, a_f_y + a_f_scale, a_f_scale, a_str_text);
	glColor4f(a_arr_rgb[0], a_arr_rgb[1], a_arr_rgb[2], a_f_alpha);
	draw_text(a_f_x, a_f_y, a_f_scale, a_str_text);
}

// Same as draw_label(), but centred horizontally on a_f_cx
static void draw_label_centered(float a_f_cx, float a_f_y, float a_f_scale,
	const float *a_arr_rgb, float a_f_alpha, const char *a_str_text)
{
	float f_width = stb_easy_font_width((char *)a_str_text) * a_f_scale;
	draw_label(a_f_cx - f_width / 2.0f, a_f_y, a_f_scale, a_arr_rgb,
		a_f_alpha, a_str_text);
}

// Faint grid lines, plus every claimed cell as a tile in its owner's colour.
// A tile that has just changed hands grows in from its centre with a
// brief white flash
static void draw_cells(const Grid *a_grid, float a_f_dt)
{
	glColor4f(1.0f, 1.0f, 1.0f, 0.06f);
	glBegin(GL_LINES);
	for (int int_col = 0; int_col <= GRID_COLS; int_col++)
	{
		glVertex2f(int_col * SQ_WIDTH, PLAY_TOP);
		glVertex2f(int_col * SQ_WIDTH, PLAY_TOP + PLAY_HEIGHT);
	}
	for (int int_row = 0; int_row <= GRID_ROWS; int_row++)
	{
		glVertex2f(0, PLAY_TOP + int_row * SQ_WIDTH);
		glVertex2f(WIDTH, PLAY_TOP + int_row * SQ_WIDTH);
	}
	glEnd();

	for (int int_row = 0; int_row < GRID_ROWS; int_row++)
	{
		for (int int_col = 0; int_col < GRID_COLS; int_col++)
		{
			int int_i = int_row * GRID_COLS + int_col;
			int int_owner = a_grid->arr_owner[int_i];

			// Restart the pop-in whenever the owner changes
			if (int_owner != arr_shown_owner[int_i])
			{
				arr_shown_owner[int_i] = int_owner;
				arr_claim_age[int_i] = 0.0f;
			}
			else if (arr_claim_age[int_i] < CLAIM_ANIM_SECS)
			{
				arr_claim_age[int_i] += a_f_dt;
			}

			if (int_owner < 1 || int_owner > MAX_PLAYERS)
				continue;

			float f_t = arr_claim_age[int_i] / CLAIM_ANIM_SECS;
			if (f_t > 1.0f)
				f_t = 1.0f;
			// Ease out: fast at first, settling at the end
			float f_ease = 1.0f - (1.0f - f_t) * (1.0f - f_t);
			float f_inset = 2.0f + (1.0f - f_ease) * (SQ_WIDTH / 2 - 2);
			float f_flash = (1.0f - f_ease) * 0.6f;

			const float *arr_c = arr_colors[int_owner - 1];
			glColor4f(0.8f * arr_c[0] + f_flash * (1.0f - 0.8f * arr_c[0]),
				0.8f * arr_c[1] + f_flash * (1.0f - 0.8f * arr_c[1]),
				0.8f * arr_c[2] + f_flash * (1.0f - 0.8f * arr_c[2]), 1.0f);
			draw_rect(int_col * SQ_WIDTH + f_inset, PLAY_TOP + int_row * SQ_WIDTH + f_inset,
				SQ_WIDTH - 2.0f * f_inset, SQ_WIDTH - 2.0f * f_inset);
		}
	}
}

// Each active player's square: shadow, fill, highlight, outline and ID.
// The local player gets a pulsing outline and a YOU marker
static void draw_players(const Player a_arr_players[], int a_int_own_id,
	double a_d_time)
{
	float f_pulse = 0.5f + 0.5f * sinf((float)a_d_time * 6.0f);

	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!a_arr_players[i].b_active)
			continue;

		float f_x = (float)a_arr_players[i].x;
		float f_y = (float)a_arr_players[i].y;
		bool b_own = (i + 1 == a_int_own_id);

		glColor4f(0.0f, 0.0f, 0.0f, 0.45f);
		draw_rect(f_x + 4.0f, f_y + 4.0f, SQ_WIDTH, SQ_WIDTH);

		glColor3fv(arr_colors[i]);
		draw_rect(f_x, f_y, SQ_WIDTH, SQ_WIDTH);

		glColor4f(1.0f, 1.0f, 1.0f, 0.25f);
		draw_rect(f_x, f_y, SQ_WIDTH, 6.0f);

		if (b_own)
		{
			glLineWidth(3.0f);
			glColor4f(1.0f, 1.0f, 1.0f, 0.7f + 0.3f * f_pulse);
		}
		else
		{
			glLineWidth(2.0f);
			glColor4f(1.0f, 1.0f, 1.0f, 0.45f);
		}
		draw_rect_outline(f_x, f_y, SQ_WIDTH, SQ_WIDTH);
		glLineWidth(1.0f);

		char str_id[2];
		snprintf(str_id, sizeof(str_id), "%d", i + 1);
		draw_label_centered(f_x + SQ_WIDTH / 2.0f, f_y + SQ_WIDTH / 2.0f - 10.0f,
			3.0f, arr_white, 1.0f, str_id);

		if (b_own)
		{
			// Above the square, or below it when there is not enough room
			// above to clear the top HUD margin (PLAY_TOP)
			bool b_room_above = (f_y - PLAY_TOP) > 16.0f;
			float f_marker_y = b_room_above ? f_y - 16.0f : f_y + SQ_WIDTH + 4.0f;
			draw_label_centered(f_x + SQ_WIDTH / 2.0f, f_marker_y, 1.5f,
				arr_white, 1.0f, "YOU");
		}
	}
}

// Top strip (who you are, countdown, time bar) and bottom strip
// (territory bar, per-player cell counts)
static void draw_hud(const Grid *a_grid, const Player a_arr_players[],
	int a_int_own_id, int a_int_secs, bool a_b_started, double a_d_time,
	bool a_b_spectating)
{
	char str_text[32];
	bool b_low = a_b_started && a_int_secs <= 10;
	float f_blink = 0.65f + 0.35f * sinf((float)a_d_time * 8.0f);

	// Top strip - exactly HUD_MARGIN_TOP tall (solid bar + the thin time
	// bar below it), so it can never bleed into the playable grid
	glColor4f(0.0f, 0.0f, 0.0f, 0.55f);
	draw_rect(0, 0, WIDTH, HUD_MARGIN_TOP - 4);

	if (a_int_own_id >= 1 && a_int_own_id <= MAX_PLAYERS)
	{
		snprintf(str_text, sizeof(str_text), "Player %d", a_int_own_id);
		draw_label(12, 14, 2.5f, arr_colors[a_int_own_id - 1], 1.0f, str_text);
	}
	else if (a_b_spectating)
	{
		draw_label(12, 14, 2.5f, arr_white, 0.9f, "Spectator");
	}
	else
	{
		draw_label(12, 14, 2.5f, arr_white, 0.7f, "Connecting...");
	}

	snprintf(str_text, sizeof(str_text), "%d:%02d", a_int_secs / 60, a_int_secs % 60);
	draw_label_centered(WIDTH / 2.0f, 8, 4.0f, b_low ? arr_alarm : arr_white,
		b_low ? f_blink : 1.0f, str_text);

	float f_title_w = stb_easy_font_width((char *)"COLOR THE FLOOR") * 1.5f;
	draw_label(WIDTH - 12 - f_title_w, 18, 1.5f, arr_white, 0.5f, "COLOR THE FLOOR");

	// Time bar: drains as the round runs out
	float f_frac = a_b_started ? (float)a_int_secs / ROUND_SECS : 1.0f;
	glColor4f(1.0f, 1.0f, 1.0f, 0.1f);
	draw_rect(0, HUD_MARGIN_TOP - 4, WIDTH, 4);
	if (b_low)
		glColor4f(arr_alarm[0], arr_alarm[1], arr_alarm[2], f_blink);
	else
		glColor4f(1.0f, 1.0f, 1.0f, 0.8f);
	draw_rect(0, HUD_MARGIN_TOP - 4, WIDTH * f_frac, 4);

	// Count each player's cells once, for both the bar and the labels
	int arr_counts[MAX_PLAYERS];
	for (int i = 0; i < MAX_PLAYERS; i++)
		arr_counts[i] = grid_count_owned(a_grid, i + 1);

	// Bottom strip - exactly HUD_MARGIN_BOTTOM tall, for the same reason
	// as the top strip above
	glColor4f(0.0f, 0.0f, 0.0f, 0.55f);
	draw_rect(0, HEIGHT - HUD_MARGIN_BOTTOM, WIDTH, HUD_MARGIN_BOTTOM);

	// Territory bar: each player's share of the whole floor
	float f_bar_x = 10.0f;
	float f_bar_w = WIDTH - 20.0f;
	float f_bar_y = HEIGHT - HUD_MARGIN_BOTTOM + 4.0f;
	glColor4f(1.0f, 1.0f, 1.0f, 0.1f);
	draw_rect(f_bar_x, f_bar_y, f_bar_w, 10);
	float f_cursor = f_bar_x;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		float f_seg = f_bar_w * arr_counts[i] / (GRID_COLS * GRID_ROWS);
		glColor3fv(arr_colors[i]);
		draw_rect(f_cursor, f_bar_y, f_seg, 10);
		f_cursor += f_seg;
	}

	// Per-player label: colour swatch + cells owned
	int int_slot = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!a_arr_players[i].b_active)
			continue;

		float f_x = 10.0f + int_slot * 150.0f;
		float f_y = HEIGHT - HUD_MARGIN_BOTTOM + 22.0f;
		glColor3fv(arr_colors[i]);
		draw_rect(f_x, f_y, 12, 12);
		snprintf(str_text, sizeof(str_text), "P%d  %d", i + 1, arr_counts[i]);
		draw_label(f_x + 18.0f, f_y, 1.7f, arr_white, 1.0f, str_text);
		int_slot++;
	}
}

// Pulsing message shown until enough players have joined
static void draw_waiting(double a_d_time)
{
	float f_alpha = 0.55f + 0.45f * sinf((float)a_d_time * 3.0f);
	draw_label_centered(WIDTH / 2.0f, 280, 3.0f, arr_white, f_alpha,
		"Waiting for another player...");
}

// Dims the floor and shows the result and final cell counts on a panel
static void draw_end_screen(const Grid *a_grid, const Player a_arr_players[],
	int a_int_own_id, int a_int_winner_id)
{
	glColor4f(0.0f, 0.0f, 0.0f, 0.7f);
	draw_rect(0, 0, WIDTH, HEIGHT);

	float f_panel_x = (WIDTH - 500) / 2.0f;
	float f_panel_y = 170.0f;
	bool b_has_winner = (a_int_winner_id >= 1 && a_int_winner_id <= MAX_PLAYERS);
	const float *arr_accent = b_has_winner ? arr_colors[a_int_winner_id - 1] : arr_white;

	glColor4f(0.10f, 0.11f, 0.16f, 0.96f);
	draw_rect(f_panel_x, f_panel_y, 500, 260);
	glColor3fv(arr_accent);
	draw_rect(f_panel_x, f_panel_y, 500, 8);

	char str_text[64];
	if (!b_has_winner)
		snprintf(str_text, sizeof(str_text), "DRAW!");
	else if (a_int_winner_id == a_int_own_id)
		snprintf(str_text, sizeof(str_text), "YOU WIN!");
	else
		snprintf(str_text, sizeof(str_text), "PLAYER %d WINS!", a_int_winner_id);
	draw_label_centered(WIDTH / 2.0f, f_panel_y + 30.0f, 5.0f, arr_accent, 1.0f, str_text);

	int int_line = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!a_arr_players[i].b_active)
			continue;

		float f_y = f_panel_y + 105.0f + int_line * 28.0f;
		glColor3fv(arr_colors[i]);
		draw_rect(f_panel_x + 120.0f, f_y, 16, 16);
		snprintf(str_text, sizeof(str_text), "Player %d   %d cells", i + 1,
			grid_count_owned(a_grid, i + 1));
		draw_label(f_panel_x + 150.0f, f_y + 1.0f, 2.5f, arr_white, 1.0f, str_text);
		int_line++;
	}
}

// Small banners under the top bar announcing who left, fading out after
// NOTICE_SECS. Newer banners sit lower in the stack
static void draw_notices(const LeaveNotice a_arr_notices[], double a_d_now)
{
	int int_row = 0;
	for (int i = 0; i < MAX_NOTICES; i++)
	{
		int int_id = a_arr_notices[i].int_player_id;
		double d_age = a_d_now - a_arr_notices[i].d_time;
		if (int_id < 1 || int_id > MAX_PLAYERS || d_age < 0.0 || d_age >= NOTICE_SECS)
			continue;

		float f_alpha = 1.0f;
		if (d_age > NOTICE_SECS - NOTICE_FADE_SECS)
			f_alpha = (float)((NOTICE_SECS - d_age) / NOTICE_FADE_SECS);

		char str_text[32];
		snprintf(str_text, sizeof(str_text), "Player %d left the game", int_id);
		float f_text_w = stb_easy_font_width((char *)str_text) * 2.0f;
		float f_w = f_text_w + 44.0f;
		float f_x = (WIDTH - f_w) / 2.0f;
		float f_y = 58.0f + int_row * 32.0f;

		glColor4f(0.0f, 0.0f, 0.0f, 0.7f * f_alpha);
		draw_rect(f_x, f_y, f_w, 26.0f);
		glColor4f(arr_colors[int_id - 1][0], arr_colors[int_id - 1][1],
			arr_colors[int_id - 1][2], f_alpha);
		draw_rect(f_x + 8.0f, f_y + 6.0f, 14.0f, 14.0f);
		draw_label(f_x + 30.0f, f_y + 6.0f, 2.0f, arr_white, f_alpha, str_text);
		int_row++;
	}
}

// Banner for a client refused a slot because the game is full: it can still
// watch, and joins automatically when a player leaves
static void draw_spectator_banner(double a_d_time)
{
	static const float arr_amber[3] = { 1.0f, 0.8f, 0.3f };
	const char *str_title = "GAME FULL - SPECTATING";
	const char *str_sub = "You will join automatically when a spot opens";

	float f_alpha = 0.75f + 0.25f * sinf((float)a_d_time * 3.0f);
	float f_title_w = stb_easy_font_width((char *)str_title) * 3.0f;
	float f_sub_w = stb_easy_font_width((char *)str_sub) * 1.5f;
	float f_w = (f_title_w > f_sub_w ? f_title_w : f_sub_w) + 40.0f;
	// Sits just above the bottom strip
	float f_y = HEIGHT - 56.0f - 62.0f - 8.0f;

	glColor4f(0.0f, 0.0f, 0.0f, 0.75f);
	draw_rect((WIDTH - f_w) / 2.0f, f_y, f_w, 62.0f);
	draw_label_centered(WIDTH / 2.0f, f_y + 8.0f, 3.0f, arr_amber, f_alpha, str_title);
	draw_label_centered(WIDTH / 2.0f, f_y + 40.0f, 1.5f, arr_white, 0.9f, str_sub);
}

// Sends one Input for a_int_player_id: the arrow-key direction, or 'n' for
// none. Used both for real moves and for the first "hello" Input.
// a_int_join_nonce is the nonce of the JoinRequest that won us this player
// ID, which the server binds our Input to
static void publish_input(dds_entity_t a_writer, int a_int_player_id,
	int a_int_join_nonce, char a_ch_direction)
{
	Input input;
	memset(&input, 0, sizeof(input));
	input.int_player_id = a_int_player_id;

	// Encrypt the payload before publishing - see security.h. The server
	// drops anything without a valid cipher, so skip sending if
	// encryption fails. The cipher is bound to our player ID (the DDS key,
	// which stays in the clear for routing/leave detection) and join
	// nonce, so it cannot be replayed as another player's Input, or into
	// a later player's session with the same ID
	int arr_bound[2] = { a_int_player_id, a_int_join_nonce };
	unsigned char arr_plain[1] = { (unsigned char)a_ch_direction };
	size_t cipher_len = 0;
	if (!security_encrypt(SECURITY_MSG_INPUT, arr_bound, 2, arr_plain,
		sizeof(arr_plain), input.arr_cipher, &cipher_len) ||
		cipher_len > sizeof(input.arr_cipher))
	{
		fprintf(stderr, "Input not sent: encryption failed\n");
		return;
	}
	input.len_cipher = (unsigned long)cipher_len;
	// ch_direction stays 'n': the direction is only readable from the
	// ciphertext
	input.ch_direction = 'n';

	dds_return_t rc = dds_write(a_writer, &input);
	if (rc != DDS_RETCODE_OK)
	{
		DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
	}
}

// ### DDS WORKER THREAD ###

// The worker thread's DDS entities, sample buffers and replay state. Only
// the worker thread touches it
typedef struct
{
	dds_entity_t participant;
	dds_entity_t response_reader;
	dds_entity_t position_reader;
	dds_entity_t cell_reader;
	dds_entity_t state_reader;
	dds_entity_t join_writer;
	dds_entity_t input_writer;
	// Wakes the worker when any reader has data, or the main thread
	// triggers wake_guard
	dds_entity_t waitset;

	// Sample buffers for dds_take(), allocated once
	void *ptr_response_sample;
	void *ptr_position_sample;
	void *ptr_cell_sample;
	void *ptr_state_sample;

	// Replay protection - see security.h. One guard per stream the server
	// sends us: each keyed instance (player, cell) is its own stream
	SecurityReplayGuard response_guard;
	SecurityReplayGuard state_guard;
	SecurityReplayGuard arr_position_guard[MAX_PLAYERS];
	SecurityReplayGuard arr_cell_guard[GRID_ROWS * GRID_COLS];
} ClientDds;

// Creates a topic, exiting if that fails
static dds_entity_t create_topic(dds_entity_t a_participant,
	const dds_topic_descriptor_t *a_desc, const char *a_str_name)
{
	dds_entity_t topic = dds_create_topic(a_participant, a_desc, a_str_name, NULL, NULL);
	if (topic < 0)
		DDS_FATAL("dds_create_topic(%s): %s\n", a_str_name, dds_strretcode(-topic));
	return topic;
}

// Creates the participant, topics, readers, writers and waitset
static void create_dds(ClientDds *a_dds)
{
	memset(a_dds, 0, sizeof(*a_dds));

	a_dds->participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
	if (a_dds->participant < 0)
		DDS_FATAL("dds_create_participant: %s\n", dds_strretcode(-a_dds->participant));

	dds_entity_t join_topic = create_topic(a_dds->participant, &JoinRequest_desc, "JoinRequest");
	dds_entity_t response_topic = create_topic(a_dds->participant, &JoinResponse_desc, "JoinResponse");
	dds_entity_t input_topic = create_topic(a_dds->participant, &Input_desc, "Input");
	dds_entity_t position_topic = create_topic(a_dds->participant, &Position_desc, "Position");
	dds_entity_t cell_topic = create_topic(a_dds->participant, &CellClaimed_desc, "CellClaimed");
	dds_entity_t state_topic = create_topic(a_dds->participant, &GameState_desc, "GameState");

	a_dds->join_writer = dds_create_writer(a_dds->participant, join_topic, NULL, NULL);

	// No autodispose, so deleting this writer when we leave unregisters our
	// Input instance (not-alive-no-writers) instead of disposing it - the
	// server ignores disposes, since any participant could send one (server.c)
	dds_qos_t *qos_input = dds_create_qos();
	dds_qset_writer_data_lifecycle(qos_input, false);
	a_dds->input_writer = dds_create_writer(a_dds->participant, input_topic, qos_input, NULL);
	dds_delete_qos(qos_input);

	a_dds->response_reader = dds_create_reader(a_dds->participant, response_topic, NULL, NULL);

	// Must match the server's QoS for Position: reliable, so a move is
	// never silently dropped and left trailing behind the (already
	// reliable) CellClaimed for the same move; transient-local, because the
	// server only publishes a position when it changes, so a client that
	// joins later must be handed every player's current one
	dds_qos_t *qos_position = dds_create_qos();
	dds_qset_reliability(qos_position, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	dds_qset_durability(qos_position, DDS_DURABILITY_TRANSIENT_LOCAL);
	dds_qset_history(qos_position, DDS_HISTORY_KEEP_LAST, 1);
	a_dds->position_reader = dds_create_reader(a_dds->participant, position_topic, qos_position, NULL);
	dds_delete_qos(qos_position);

	// Must match the server's QoS for CellClaimed, so that the history of
	// claimed cells is delivered to a client that joins mid-round
	dds_qos_t *qos_cell = dds_create_qos();
	dds_qset_durability(qos_cell, DDS_DURABILITY_TRANSIENT_LOCAL);
	dds_qset_reliability(qos_cell, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	dds_qset_history(qos_cell, DDS_HISTORY_KEEP_LAST, 1);
	a_dds->cell_reader = dds_create_reader(a_dds->participant, cell_topic, qos_cell, NULL);
	dds_delete_qos(qos_cell);

	a_dds->state_reader = dds_create_reader(a_dds->participant, state_topic, NULL, NULL);

	if (a_dds->join_writer < 0 || a_dds->input_writer < 0 ||
		a_dds->response_reader < 0 || a_dds->position_reader < 0 ||
		a_dds->cell_reader < 0 || a_dds->state_reader < 0)
		DDS_FATAL("creating a DDS reader or writer failed\n");

	// Wait on "has unread samples" for every reader, plus the guard the
	// main thread triggers when the arrow keys change or the window closes
	dds_entity_t guard = dds_create_guardcondition(a_dds->participant);
	a_dds->waitset = dds_create_waitset(a_dds->participant);
	if (guard < 0 || a_dds->waitset < 0)
		DDS_FATAL("creating the waitset failed\n");
	dds_entity_t arr_readers[4] = { a_dds->response_reader, a_dds->position_reader,
		a_dds->cell_reader, a_dds->state_reader };
	for (int i = 0; i < 4; i++)
		dds_waitset_attach(a_dds->waitset, dds_create_readcondition(arr_readers[i], DDS_ANY_STATE), 0);
	dds_waitset_attach(a_dds->waitset, guard, 0);

	pthread_mutex_lock(&mutex);
	wake_guard = guard;
	pthread_mutex_unlock(&mutex);

	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	a_dds->ptr_response_sample = JoinResponse__alloc();
	a_dds->ptr_position_sample = Position__alloc();
	a_dds->ptr_cell_sample = CellClaimed__alloc();
	a_dds->ptr_state_sample = GameState__alloc();
}

// Takes every pending JoinResponse. Until we have a player ID, one meant for
// a_int_nonce (our JoinRequest) either admits us or tells us the game is
// full. Responses keep arriving for other clients after we join, so they
// are always drained - otherwise the waitset would keep waking us for them
static void take_join_responses(ClientDds *a_dds, int a_int_nonce)
{
	void *samples_response[MAX_SAMPLES] = { a_dds->ptr_response_sample };
	dds_sample_info_t infos_response[MAX_SAMPLES];
	dds_return_t rc;

	while ((rc = dds_take(a_dds->response_reader, samples_response, infos_response,
		MAX_SAMPLES, MAX_SAMPLES)) > 0)
	{
		pthread_mutex_lock(&mutex);
		bool b_joined = int_player_id != -1;
		pthread_mutex_unlock(&mutex);
		if (b_joined || !infos_response[0].valid_data)
			continue;

		JoinResponse *response = (JoinResponse *) samples_response[0];

		// Authenticate the response - see security.h. The server already
		// verified us via JoinRequest.arr_token; this lets us verify the
		// response really came from a holder of the shared key too, rather
		// than trusting it outright. An unencrypted response is dropped
		unsigned char arr_plain[sizeof(int) * 2];
		size_t plain_len = 0;
		if (response->len_cipher == 0 ||
			!security_decrypt(SECURITY_MSG_JOIN_RESPONSE, NULL, 0,
			&a_dds->response_guard, response->arr_cipher, response->len_cipher,
			arr_plain, sizeof(arr_plain), &plain_len) ||
			plain_len != sizeof(arr_plain))
		{
			continue;
		}
		int int_resp_player_id, int_resp_nonce;
		memcpy(&int_resp_player_id, arr_plain, sizeof(int));
		memcpy(&int_resp_nonce, arr_plain + sizeof(int), sizeof(int));

		// Responses are broadcast: ignore the ones meant for other clients
		if (int_resp_nonce != a_int_nonce)
			continue;
		printf("Join response received\n");

		pthread_mutex_lock(&mutex);
		if (int_resp_player_id == 0)
		{
			// Game full: spectate, and keep retrying the join
			if (!b_spectating)
				printf("Game full - spectating until a spot opens\n");
			b_spectating = true;
		}
		else if (int_resp_player_id >= 1 && int_resp_player_id <= MAX_PLAYERS)
		{
			int_player_id = int_resp_player_id;
			b_spectating = false;
			printf("Joined as player %d\n", int_resp_player_id);
		}
		pthread_mutex_unlock(&mutex);
	}
	if (rc < 0)
		DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
}

// Takes every pending CellClaimed into the local grid. A client that has
// just joined is handed one sample per claimed cell at once
static void take_cells(ClientDds *a_dds)
{
	void *samples_cell[MAX_SAMPLES] = { a_dds->ptr_cell_sample };
	dds_sample_info_t infos_cell[MAX_SAMPLES];
	dds_return_t rc;

	while ((rc = dds_take(a_dds->cell_reader, samples_cell, infos_cell, MAX_SAMPLES, MAX_SAMPLES)) > 0)
	{
		if (!infos_cell[0].valid_data)
			continue;

		CellClaimed *claim = (CellClaimed *) samples_cell[0];
		int int_cell_x = claim->int_cell_x;
		int int_cell_y = claim->int_cell_y;

		// The cell is the plaintext DDS key and indexes arr_cell_guard,
		// so reject anything off the grid
		if (int_cell_x < 0 || int_cell_x >= GRID_COLS ||
			int_cell_y < 0 || int_cell_y >= GRID_ROWS)
			continue;

		// Decrypt the payload - see security.h. A message that is
		// unencrypted, fails authentication, was made for another cell
		// or is a replay is dropped
		int arr_bound[2] = { int_cell_x, int_cell_y };
		unsigned char arr_plain[sizeof(int)];
		size_t plain_len = 0;
		if (claim->len_cipher == 0 ||
			!security_decrypt(SECURITY_MSG_CELL_CLAIMED, arr_bound, 2,
			&a_dds->arr_cell_guard[int_cell_y * GRID_COLS + int_cell_x],
			claim->arr_cipher, claim->len_cipher,
			arr_plain, sizeof(arr_plain), &plain_len) ||
			plain_len != sizeof(arr_plain))
		{
			continue;
		}
		int int_owner_id;
		memcpy(&int_owner_id, arr_plain, sizeof(int));

		// grid_set_owner() ignores owners outside 0..MAX_PLAYERS
		pthread_mutex_lock(&mutex);
		grid_set_owner(&grid, int_cell_x, int_cell_y, int_owner_id);
		pthread_mutex_unlock(&mutex);
	}
	if (rc < 0)
		DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
}

// Takes every pending GameState (round timer / result)
static void take_states(ClientDds *a_dds)
{
	void *samples_state[MAX_SAMPLES] = { a_dds->ptr_state_sample };
	dds_sample_info_t infos_state[MAX_SAMPLES];
	dds_return_t rc;

	while ((rc = dds_take(a_dds->state_reader, samples_state, infos_state, MAX_SAMPLES, MAX_SAMPLES)) > 0)
	{
		if (!infos_state[0].valid_data)
			continue;

		GameState *state = (GameState *) samples_state[0];

		// Decrypt the payload - see security.h. A message that is
		// unencrypted or fails authentication is dropped
		unsigned char arr_plain[sizeof(int) * 4];
		size_t plain_len = 0;
		if (state->len_cipher == 0 ||
			!security_decrypt(SECURITY_MSG_GAME_STATE, NULL, 0, &a_dds->state_guard,
			state->arr_cipher, state->len_cipher,
			arr_plain, sizeof(arr_plain), &plain_len) ||
			plain_len != sizeof(arr_plain))
		{
			continue;
		}
		int int_state_secs, int_state_started, int_state_over, int_state_winner;
		memcpy(&int_state_secs, arr_plain, sizeof(int));
		memcpy(&int_state_started, arr_plain + sizeof(int), sizeof(int));
		memcpy(&int_state_over, arr_plain + sizeof(int) * 2, sizeof(int));
		memcpy(&int_state_winner, arr_plain + sizeof(int) * 3, sizeof(int));

		pthread_mutex_lock(&mutex);
		int_seconds_left = int_state_secs;
		b_round_started = int_state_started != 0;
		b_game_over = int_state_over != 0;
		int_winner_id = int_state_winner;
		pthread_mutex_unlock(&mutex);
	}
	if (rc < 0)
		DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
}

// Takes every pending Position: the server is the only source of truth, so
// the local player array is only updated from these messages (Slide 10).
// Several players' positions can be pending at once
static void take_positions(ClientDds *a_dds)
{
	void *samples_position[MAX_SAMPLES] = { a_dds->ptr_position_sample };
	dds_sample_info_t infos_position[MAX_SAMPLES];
	dds_return_t rc;

	while ((rc = dds_take(a_dds->position_reader, samples_position, infos_position,
		MAX_SAMPLES, MAX_SAMPLES)) > 0)
	{
		Position *position = (Position *) samples_position[0];
		int int_position_player_id = position->int_player_id;

		// The player ID is the plaintext DDS key and indexes arr_Players
		// and arr_position_guard, so reject anything out of range
		if (int_position_player_id < 1 || int_position_player_id > MAX_PLAYERS)
			continue;

		if (!infos_position[0].valid_data)
		{
			// A no-writers Position instance means the server unregistered
			// that player (they left), so stop drawing their square. A
			// disposed instance is ignored: any DDS participant can
			// dispose one, which would let it hide another player
			if (infos_position[0].instance_state == DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE)
			{
				pthread_mutex_lock(&mutex);
				bool b_was_active = arr_Players[int_position_player_id - 1].b_active;
				arr_Players[int_position_player_id - 1].b_active = false;
				// Announce it on screen, but only once and not for ourselves
				if (b_was_active && int_position_player_id != int_player_id)
				{
					push_leave_notice(int_position_player_id);
					printf("Player %d left the game\n", int_position_player_id);
				}
				pthread_mutex_unlock(&mutex);
			}
			continue;
		}

		// Decrypt the payload - see security.h. A message that is
		// unencrypted, fails authentication, was made for another player
		// or is a replay is dropped, never read from the plaintext fields
		int arr_bound[1] = { int_position_player_id };
		unsigned char arr_plain[sizeof(int) * 2];
		size_t plain_len = 0;
		if (position->len_cipher == 0 ||
			!security_decrypt(SECURITY_MSG_POSITION, arr_bound, 1,
			&a_dds->arr_position_guard[int_position_player_id - 1],
			position->arr_cipher, position->len_cipher,
			arr_plain, sizeof(arr_plain), &plain_len) ||
			plain_len != sizeof(arr_plain))
		{
			continue;
		}
		int int_position_x, int_position_y;
		memcpy(&int_position_x, arr_plain, sizeof(int));
		memcpy(&int_position_y, arr_plain + sizeof(int), sizeof(int));

		pthread_mutex_lock(&mutex);
		arr_Players[int_position_player_id - 1].b_active = true;
		arr_Players[int_position_player_id - 1].x = int_position_x;
		arr_Players[int_position_player_id - 1].y = int_position_y;
		pthread_mutex_unlock(&mutex);
	}
	if (rc < 0)
		DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
}

// Returns a_value clamped to at least 0
static dds_duration_t at_least_zero(dds_duration_t a_value)
{
	return a_value > 0 ? a_value : 0;
}

// DDS worker thread (Slide 12): sends the join request and the player's
// input, and receives the join response and the server's updates. Keeping
// this off the main thread means delayed DDS traffic never freezes the
// window's rendering or keyboard handling
void *worker(void *arg)
{
	(void)arg;

	ClientDds dds;
	create_dds(&dds);

	JoinRequest request;
	memset(&request, 0, sizeof(request));
	request.b_dummy = true;
	// Tag the request, so only the response to it is accepted, and so the
	// server can bind our Input to it - see security.h
	request.int_nonce = security_make_nonce();
	// Fill the authentication credential, bound to this request's nonce
	security_make_client_token(request.int_nonce, request.arr_token,
		sizeof(request.arr_token));

	// When the next JoinRequest is due (sent every second until answered),
	// and the earliest a held arrow key may send its next move
	dds_time_t next_join_time = dds_time();
	dds_time_t next_move_time = 0;
	// Whether our first (no-direction) Input has gone out yet
	bool b_sent_hello = false;

	while (1)
	{
		pthread_mutex_lock(&mutex);
		bool b_keep_running = b_running;
		int int_player_id_temp = int_player_id;
		char ch_dir_temp = ch_dir;
		pthread_mutex_unlock(&mutex);
		// Stop once the main thread says the window has closed
		if (!b_keep_running)
			break;

		dds_time_t now = dds_time();

		// If not in a game yet: send Join Requests until the server answers
		// with our player ID (Slide 11, Figure 4a)
		if (int_player_id_temp == -1 && now >= next_join_time)
		{
			next_join_time = now + DDS_SECS(1);
			dds_return_t rc = dds_write(dds.join_writer, &request);
			if (rc != DDS_RETCODE_OK)
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
			printf("Join request sent\n");
		}

		// Client is an active player: send the arrow-key direction to the
		// server as Input (Slide 11, Figure 4b)
		if (int_player_id_temp != -1)
		{
			// Send one no-direction Input as soon as we are in the game, so
			// the server has an Input instance for us even if we never press
			// a key. Closing the window before the first move would
			// otherwise go unnoticed by the server
			if (!b_sent_hello)
			{
				publish_input(dds.input_writer, int_player_id_temp, request.int_nonce, 'n');
				b_sent_hello = true;
			}
			// Throttle a held key to one move per MOVE_REPEAT_MSECS, so the
			// square steps one grid cell at a time instead of blurring
			// across several. Done by timestamp, not by sleeping, so
			// server updates keep being received in between
			if (ch_dir_temp != 'n' && now >= next_move_time)
			{
				publish_input(dds.input_writer, int_player_id_temp, request.int_nonce,
					ch_dir_temp);
				next_move_time = now + DDS_MSECS(MOVE_REPEAT_MSECS);
			}
		}

		// Sleep until the server sends something, the main thread wakes us
		// (arrow keys changed, or the window closed), or the next join
		// request / held-key move is due - instead of polling every
		// millisecond and sleeping through MOVE_REPEAT_MSECS after each move
		dds_duration_t timeout = DDS_SECS(1);
		if (int_player_id_temp == -1 && next_join_time - now < timeout)
			timeout = at_least_zero(next_join_time - now);
		if (int_player_id_temp != -1 && ch_dir_temp != 'n' && next_move_time - now < timeout)
			timeout = at_least_zero(next_move_time - now);
		dds_return_t rc = dds_waitset_wait(dds.waitset, NULL, 0, timeout);
		if (rc < 0)
			DDS_FATAL("dds_waitset_wait: %s\n", dds_strretcode(-rc));
		// Reset the guard, so it only wakes us again for a new change
		bool b_guard_triggered;
		dds_take_guardcondition(wake_guard, &b_guard_triggered);

		take_join_responses(&dds, request.int_nonce);
		// Drain everything that arrived
		take_cells(&dds);
		take_states(&dds);
		take_positions(&dds);
	}

	// Deleting the participant removes our writers, which is how the server
	// learns we left: our Input instance goes not-alive (see server.c)
	dds_delete(dds.participant);
	return NULL;
}

// Tells the worker thread something changed (arrow keys, or the window
// closed), so it wakes from its waitset straight away
static void wake_worker(void)
{
	pthread_mutex_lock(&mutex);
	dds_entity_t guard = wake_guard;
	pthread_mutex_unlock(&mutex);
	// Not created yet: the worker reads ch_dir/b_running when it starts
	if (guard > 0)
		dds_set_guardcondition(guard, true);
}

// Entry point of the application
int main(void)
{
	if (!security_init())
	{
		fprintf(stderr, "security_init failed\n");
		return 1;
	}

	signal(SIGINT, on_quit_signal);
	signal(SIGTERM, on_quit_signal);

	grid_init(&grid);

	// The main thread does OpenGL rendering and GLFW input handling, while
	// this worker thread does the DDS traffic (Slide 12)
	pthread_t thread;

    if (pthread_create(&thread, NULL, worker, NULL) != 0)
    {
        perror("pthread_create");
        return 1;
    }
    
    
	// GLFW opens the 800x600 window and reads the keyboard, and OpenGL draws
	// into it (Slides 7 and 8)
    if (!glfwInit())
        return 1;

    GLFWwindow *window =
        glfwCreateWindow(WIDTH, HEIGHT, "DDS Client (Click within the window to control)", NULL, NULL);

    if (!window)
    {
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
	// Wait for the display refresh instead of redrawing as fast as possible
	glfwSwapInterval(1);
	// Alpha blending for the translucent overlays and fades
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Use pixel coordinates */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* glOrtho(0, WIDTH, 0, HEIGHT, -1, 1); */
	glOrtho(0, WIDTH, HEIGHT, 0, -1, 1);

  /*   glMatrixMode(GL_MODELVIEW);
    glLoadIdentity(); */

	double d_last_frame = glfwGetTime();
	// Direction last handed to the worker thread, so the shared ch_dir is
	// only locked and the worker only woken when a key actually changes,
	// not on every frame
	char ch_last_dir = 'n';

    while (!glfwWindowShouldClose(window) && !g_quit_requested)
    {
		 /* Process keyboard/window events */
        glfwPollEvents();

		/* Non-blocking keyboard input: the arrow keys move the player's square
		once the window has been clicked (Slide 7) */
		char ch_new_dir = 'n';
		if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
			ch_new_dir = 'u';
		else if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
			ch_new_dir = 'd';
		else if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
			ch_new_dir = 'l';
		else if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
			ch_new_dir = 'r';

		if (ch_new_dir != ch_last_dir)
		{
			ch_last_dir = ch_new_dir;
			pthread_mutex_lock(&mutex);
			ch_dir = ch_new_dir;
			pthread_mutex_unlock(&mutex);
			// A new key press goes out now, not on the worker's next wake-up
			wake_worker();
		}

		double d_now = glfwGetTime();
		float f_dt = (float)(d_now - d_last_frame);
		d_last_frame = d_now;

		glClearColor(0.07f, 0.08f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

		// Take a consistent copy of the shared state under one lock, then
		// draw from it without holding the lock
		pthread_mutex_lock(&mutex);
		Grid grid_snap = grid;
		Player arr_players_snap[MAX_PLAYERS];
		memcpy(arr_players_snap, arr_Players, sizeof(arr_players_snap));
		LeaveNotice arr_notices_snap[MAX_NOTICES];
		memcpy(arr_notices_snap, arr_notices, sizeof(arr_notices_snap));
		int int_own_snap = int_player_id;
		int int_secs_snap = int_seconds_left;
		bool b_started_snap = b_round_started;
		bool b_spectating_snap = b_spectating;
		bool b_over_snap = b_game_over;
		int int_winner_snap = int_winner_id;
		pthread_mutex_unlock(&mutex);

		draw_cells(&grid_snap, f_dt);
		draw_players(arr_players_snap, int_own_snap, d_now);
		draw_hud(&grid_snap, arr_players_snap, int_own_snap, int_secs_snap,
			b_started_snap, d_now, b_spectating_snap);
		draw_notices(arr_notices_snap, d_now);
		if (b_spectating_snap)
			draw_spectator_banner(d_now);
		if (!b_started_snap)
			draw_waiting(d_now);
		if (b_over_snap)
			draw_end_screen(&grid_snap, arr_players_snap, int_own_snap, int_winner_snap);
        glfwSwapBuffers(window);
		// Cap at about 60 frames per second; vsync is not always honoured
		// (eg software rendering under WSL), which leaves the loop spinning.
		// Only sleep for what is left of this frame's budget - a flat 16 ms
		// on top of the time spent drawing (and any vsync wait) would drop
		// the frame rate well below 60
		double d_frame_left = FRAME_SECS - (glfwGetTime() - d_now);
		if (d_frame_left > 0.0)
			usleep((useconds_t)(d_frame_left * 1e6));
    }

    // Tell the worker thread to finish and wait for it, so DDS is shut down
	// cleanly - that is what lets the server notice we left
	pthread_mutex_lock(&mutex);
	b_running = false;
	pthread_mutex_unlock(&mutex);
	wake_worker();
	pthread_join(thread, NULL);

    glfwDestroyWindow(window);
    glfwTerminate();

    security_cleanup();
    return 0;
}
