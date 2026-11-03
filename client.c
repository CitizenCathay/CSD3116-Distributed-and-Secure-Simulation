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
// The baseline supports at most five clients (Slide 7)
#define MAX_PLAYERS 5

// How often a held arrow key sends another Input while STEP moves a full
// grid cell at a time - too fast and the square blurs across several
// tiles per second instead of stepping cell by cell
#define MOVE_REPEAT_MSECS 150

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

// Direction - n means no direction
char ch_dir = 'n';
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
// none. Used both for real moves and for the first "hello" Input
static void publish_input(dds_entity_t a_writer, int a_int_player_id,
	char a_ch_direction)
{
	Input input;
	memset(&input, 0, sizeof(input));
	input.int_player_id = a_int_player_id;
	input.ch_direction = a_ch_direction;

	// Encrypt the payload before publishing - see security.h. len_cipher
	// stays 0 (plaintext fields are used as a fallback) if encryption is
	// ever unavailable
	unsigned char arr_plain[sizeof(int) + sizeof(char)];
	memcpy(arr_plain, &input.int_player_id, sizeof(int));
	memcpy(arr_plain + sizeof(int), &input.ch_direction, sizeof(char));
	size_t cipher_len = 0;
	if (security_encrypt(arr_plain, sizeof(arr_plain),
		input.arr_cipher, &cipher_len) &&
		cipher_len <= sizeof(input.arr_cipher))
	{
		input.len_cipher = (unsigned long)cipher_len;
		// Blank the plaintext copy, so the direction is only readable from
		// the ciphertext. int_player_id stays in the clear - it is the DDS
		// key this instance is routed/leave-detected by, not secret payload
		input.ch_direction = 'n';
	}
	else
	{
		input.len_cipher = 0;
	}

	dds_return_t rc = dds_write(a_writer, &input);
	if (rc != DDS_RETCODE_OK)
	{
		DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
	}
}

// DDS worker thread (Slide 12): sends the join request and the player's
// input, and receives the join response and the server's updates. Keeping
// this off the main thread means delayed DDS traffic never freezes the
// window's rendering or keyboard handling
void *worker(void *arg)
{
	dds_entity_t participant;
	dds_entity_t join_topic;
	dds_entity_t response_topic;
	dds_entity_t input_topic;
	dds_entity_t position_topic;
	dds_entity_t cell_topic;
	dds_entity_t state_topic;
	dds_entity_t response_reader;
	dds_entity_t position_reader;
	dds_entity_t cell_reader;
	dds_entity_t state_reader;
	dds_entity_t join_writer;
	dds_entity_t input_writer;

	//Pointer to unknown type
	void *samples_response[MAX_SAMPLES];
	dds_sample_info_t infos_response[MAX_SAMPLES];
	void *samples_position[MAX_SAMPLES];
	dds_sample_info_t infos_position[MAX_SAMPLES];
	void *samples_cell[MAX_SAMPLES];
	dds_sample_info_t infos_cell[MAX_SAMPLES];
	void *samples_state[MAX_SAMPLES];
	dds_sample_info_t infos_state[MAX_SAMPLES];
	dds_return_t rc;
    
	/* ----------------------------------------------------- */
    /* Create participant                                    */
    /* ----------------------------------------------------- */

    participant = dds_create_participant
	(
        DDS_DOMAIN_DEFAULT,
        NULL,
        NULL
    );

    if (participant < 0)
    {
        printf("Failed to create participant\n");
        //return 1;
    }

    /* ----------------------------------------------------- */
    /* Create topics                                          */
    /* ----------------------------------------------------- */

    join_topic = dds_create_topic
	(
        participant,
        &JoinRequest_desc,
        "JoinRequest",
        NULL,
        NULL
    );
    
	response_topic = dds_create_topic
	(
        participant,
        &JoinResponse_desc,
        "JoinResponse",
        NULL,
        NULL
    );
    
	input_topic = dds_create_topic
	(
        participant,
        &Input_desc,
        "Input",
        NULL,
        NULL
    );
    
	position_topic = dds_create_topic
	(
        participant,
        &Position_desc,
        "Position",
        NULL,
        NULL
    );

	cell_topic = dds_create_topic
	(
        participant,
        &CellClaimed_desc,
        "CellClaimed",
        NULL,
        NULL
    );

	state_topic = dds_create_topic
	(
        participant,
        &GameState_desc,
        "GameState",
        NULL,
        NULL
    );

	/* ----------------------------------------------------- */
    /* Create writers                                         */
    /* ----------------------------------------------------- */

    join_writer = dds_create_writer(
        participant,
        join_topic,
        NULL,
        NULL
    );
    
	input_writer = dds_create_writer(
        participant,
        input_topic,
        NULL,
        NULL
    );
    
	/* ----------------------------------------------------- */
    /* Create readers                                         */
    /* ----------------------------------------------------- */

    response_reader = dds_create_reader(
        participant,
        response_topic,
        NULL,
        NULL
    );
    
	// Must match the server's QoS for Position: reliable, so a move is
	// never silently dropped and left trailing behind the (already
	// reliable) CellClaimed for the same move
	dds_qos_t *qos_position = dds_create_qos();
	dds_qset_reliability(qos_position, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	position_reader = dds_create_reader(
        participant,
        position_topic,
        qos_position,
        NULL
    );
	dds_delete_qos(qos_position);

	// Must match the server's QoS for CellClaimed, so that the history of
	// claimed cells is delivered to a client that joins mid-round
	dds_qos_t *qos_cell = dds_create_qos();
	dds_qset_durability(qos_cell, DDS_DURABILITY_TRANSIENT_LOCAL);
	dds_qset_reliability(qos_cell, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	dds_qset_history(qos_cell, DDS_HISTORY_KEEP_LAST, 1);
	cell_reader = dds_create_reader(
        participant,
        cell_topic,
        qos_cell,
        NULL
    );
	dds_delete_qos(qos_cell);

	state_reader = dds_create_reader(
        participant,
        state_topic,
        NULL,
        NULL
    );

	JoinRequest request;
    request.b_dummy = true;
	// Tag the request, so only the response to it is accepted below. Mixing
	// in the process ID keeps clients started in the same second apart
	srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
	request.int_nonce = rand();
	// Fill the authentication credential - see security.h
	security_make_client_token(request.arr_token, sizeof(request.arr_token));
    
	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_response[0] = JoinResponse__alloc();
    
	samples_position[0] = Position__alloc();
	samples_cell[0] = CellClaimed__alloc();
	samples_state[0] = GameState__alloc();

	// Record the time now
	uint64_t last_join_req_time = dds_time();
	// Whether our first (no-direction) Input has gone out yet
	bool b_sent_hello = false;
	/* printf("last_join_req_time = %" PRIu64 "\n", last_join_req_time); */
	while (1)
	{
		// Stop once the main thread says the window has closed
		pthread_mutex_lock(&mutex);
		bool b_keep_running = b_running;
		pthread_mutex_unlock(&mutex);
		if (!b_keep_running)
			break;

		// If not in a game yet: send Join Requests until the server answers
		// with our player ID (Slide 11, Figure 4a)
		pthread_mutex_lock(&mutex);
		int int_player_id_temp = int_player_id;
		pthread_mutex_unlock(&mutex);
		if (int_player_id_temp == -1)
		{
			//Listen for server join response
			rc = dds_take(response_reader, samples_response, infos_response, MAX_SAMPLES, MAX_SAMPLES);
            
			if (rc < 0)
				DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

			/* Check if we read some data and it is valid. */
			if ((rc > 0) && (infos_response[0].valid_data))
			{
				printf("Join response received\n");
				JoinResponse *response = (JoinResponse *) samples_response[0];
				int int_resp_player_id = response->int_player_id;
				int int_resp_nonce = response->int_nonce;

				// Authenticate the response - see security.h. The server
				// already verified us via JoinRequest.arr_token; this lets
				// us verify the response really came from a holder of the
				// shared key too, rather than trusting it outright
				if (response->len_cipher > 0)
				{
					unsigned char arr_plain[sizeof(int) * 2];
					size_t plain_len = 0;
					if (!security_decrypt(response->arr_cipher, response->len_cipher,
						arr_plain, &plain_len) || plain_len != sizeof(arr_plain))
					{
						continue;
					}
					memcpy(&int_resp_player_id, arr_plain, sizeof(int));
					memcpy(&int_resp_nonce, arr_plain + sizeof(int), sizeof(int));
				}

				// Responses are broadcast: ignore the ones meant for other clients
				if (int_resp_nonce != request.int_nonce)
					continue;
				// If server responded with an invalid player ID
				if (int_resp_player_id == 0)
					{
						// Game full: spectate, and keep retrying the join
						pthread_mutex_lock(&mutex);
						bool b_first_refusal = !b_spectating;
						b_spectating = true;
						pthread_mutex_unlock(&mutex);
						if (b_first_refusal)
							printf("Game full - spectating until a spot opens\n");
					}
				else
				{
					// printf("Join response player ID = %d\n", response->int_player_id);
					pthread_mutex_lock(&mutex);
					// Update player ID
					int_player_id = int_resp_player_id;
						b_spectating = false;
						printf("Joined as player %d\n", int_resp_player_id);
					pthread_mutex_unlock(&mutex);
				}
			}
			else
			{
				/* Polling sleep. */
				dds_sleepfor (DDS_MSECS (20));
			}
            
			// Send request every 1s
			// Get the current time
			uint64_t curr_time = dds_time();
			/* printf("curr_time = %" PRIu64 "\n", curr_time); */
			// Get the time difference
			if (curr_time - last_join_req_time > DDS_SECS(1))
			{
				// Update the last req time
				last_join_req_time = curr_time;
                
				// Send join request
				rc = dds_write(join_writer, &request);
				printf("Join request sent\n");
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
			}
		}
        
		// Client is an active player: send the arrow-key direction to the
			// server as Input (Slide 11, Figure 4b)
		else
		{
			pthread_mutex_lock(&mutex);
			char ch_dir_temp = ch_dir;
			pthread_mutex_unlock(&mutex);
			// Send one no-direction Input as soon as we are in the game, so the
			// server has an Input instance for us even if we never press a key.
			// Closing the window before the first move would otherwise go
			// unnoticed by the server
			if (!b_sent_hello)
			{
				publish_input(input_writer, int_player_id_temp, 'n');
				b_sent_hello = true;
			}
			// If there is some direction input
			if(ch_dir_temp != 'n')
			{
				publish_input(input_writer, int_player_id_temp, ch_dir_temp);
				// Throttle to MOVE_REPEAT_MSECS, so holding a key steps one
				// grid cell at a time instead of blurring across several
				dds_sleepfor(DDS_MSECS(MOVE_REPEAT_MSECS));
			}
		}

		// Yield the CPU, otherwise this loop spins at 100% and starves the
		// renderer and the desktop
		dds_sleepfor(DDS_MSECS(1));

		// Listen for claimed cells. Drain them all, since a client that
		// has just joined is handed one sample per claimed cell at once
		while ((rc = dds_take(cell_reader, samples_cell, infos_cell, MAX_SAMPLES, MAX_SAMPLES)) > 0)
		{
			if (!infos_cell[0].valid_data)
				continue;

			CellClaimed *claim = (CellClaimed *) samples_cell[0];
			int int_cell_x = claim->int_cell_x;
			int int_cell_y = claim->int_cell_y;
			int int_owner_id = claim->int_owner_id;

			// Decrypt the payload if the server sent one - see security.h
			// A message that fails authentication is dropped
			if (claim->len_cipher > 0)
			{
				unsigned char arr_plain[SECURITY_CIPHERTEXT_LEN];
				size_t plain_len = 0;
				if (!security_decrypt(claim->arr_cipher, claim->len_cipher,
					arr_plain, &plain_len) || plain_len != sizeof(int) * 3)
				{
					continue;
				}
				memcpy(&int_cell_x, arr_plain, sizeof(int));
				memcpy(&int_cell_y, arr_plain + sizeof(int), sizeof(int));
				memcpy(&int_owner_id, arr_plain + sizeof(int) * 2, sizeof(int));
			}

			// Player IDs are 1..MAX_PLAYERS, 0 = unclaimed
			if (int_owner_id < 0 || int_owner_id > MAX_PLAYERS)
				continue;

			pthread_mutex_lock(&mutex);
			grid_set_owner(&grid, int_cell_x, int_cell_y, int_owner_id);
			pthread_mutex_unlock(&mutex);
		}
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Listen for the round timer / result
		while ((rc = dds_take(state_reader, samples_state, infos_state, MAX_SAMPLES, MAX_SAMPLES)) > 0)
		{
			if (!infos_state[0].valid_data)
				continue;

			GameState *state = (GameState *) samples_state[0];
			int int_state_secs = state->int_seconds_left;
			int int_state_started = state->b_started;
			int int_state_over = state->b_game_over;
			int int_state_winner = state->int_winner_id;

			// Decrypt the payload if the server sent one - see security.h
			// A message that fails authentication is dropped
			if (state->len_cipher > 0)
			{
				unsigned char arr_plain[SECURITY_CIPHERTEXT_LEN];
				size_t plain_len = 0;
				if (!security_decrypt(state->arr_cipher, state->len_cipher,
					arr_plain, &plain_len) || plain_len != sizeof(int) * 4)
				{
					continue;
				}
				memcpy(&int_state_secs, arr_plain, sizeof(int));
				memcpy(&int_state_started, arr_plain + sizeof(int), sizeof(int));
				memcpy(&int_state_over, arr_plain + sizeof(int) * 2, sizeof(int));
				memcpy(&int_state_winner, arr_plain + sizeof(int) * 3, sizeof(int));
			}

			pthread_mutex_lock(&mutex);
			int_seconds_left = int_state_secs;
			b_round_started = int_state_started != 0;
			b_game_over = int_state_over != 0;
			int_winner_id = int_state_winner;
			pthread_mutex_unlock(&mutex);
		}
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Listen for position: the server is the only source of truth, so the
		// local player array is only updated from these messages (Slide 10).
		// Drain them all, since several players' positions can be pending at
		// once (one per active player, published together each server tick)
		// - leaving any queued risks rendering a stale position after the
		// matching CellClaimed (already fully drained above) has landed
		while ((rc = dds_take(position_reader, samples_position, infos_position, MAX_SAMPLES, MAX_SAMPLES)) > 0)
		{
			if (!infos_position[0].valid_data)
			{
				// A disposed Position instance means the server dropped that
				// player (they left), so stop drawing their square
				if (infos_position[0].instance_state != DDS_ALIVE_INSTANCE_STATE)
				{
					int int_left_id = ((Position *) samples_position[0])->int_player_id;
					if (int_left_id >= 1 && int_left_id <= MAX_PLAYERS)
					{
						pthread_mutex_lock(&mutex);
						bool b_was_active = arr_Players[int_left_id - 1].b_active;
						arr_Players[int_left_id - 1].b_active = false;
						// Announce it on screen, but only once and not for ourselves
						if (b_was_active && int_left_id != int_player_id)
						{
							push_leave_notice(int_left_id);
							printf("Player %d left the game\n", int_left_id);
						}
						pthread_mutex_unlock(&mutex);
					}
				}
				continue;
			}

			// printf("Position received\n");
			Position *position = (Position *) samples_position[0];
			// Get the player ID
			int int_position_player_id = position->int_player_id;
			int int_position_x = position->int_x;
			int int_position_y = position->int_y;

			// Decrypt the payload if the server sent one - see security.h
			// Falls back to the plaintext fields above until
			// security_decrypt() is implemented (len_cipher will be 0)
			if (position->len_cipher > 0)
			{
				unsigned char arr_plain[sizeof(int) * 2];
				size_t plain_len = 0;
				if (security_decrypt(position->arr_cipher, position->len_cipher,
					arr_plain, &plain_len) && plain_len == sizeof(arr_plain))
				{
					memcpy(&int_position_x, arr_plain, sizeof(int));
					memcpy(&int_position_y, arr_plain + sizeof(int), sizeof(int));
				}
			}

			pthread_mutex_lock(&mutex);
			// Set this player to active
			arr_Players[int_position_player_id-1].b_active = true;
			// Set this player's x
			arr_Players[int_position_player_id-1].x = int_position_x;
			// Set this player's y
			arr_Players[int_position_player_id-1].y = int_position_y;
			pthread_mutex_unlock(&mutex);
		}
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
	}
	// Deleting the participant removes our writers, which is how the server
	// learns we left: our Input instance goes not-alive (see server.c)
	dds_delete(participant);
	return NULL;
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

    while (!glfwWindowShouldClose(window) && !g_quit_requested)
    {
		 /* Process keyboard/window events */
        glfwPollEvents();

		/* Non-blocking keyboard input: the arrow keys move the player's square
		once the window has been clicked (Slide 7) */
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'u';
			pthread_mutex_unlock(&mutex);
		}
        
        else if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'd';
			pthread_mutex_unlock(&mutex);
		}
        
		else if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'l';
			pthread_mutex_unlock(&mutex);
		}
        
		else if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'r';
			pthread_mutex_unlock(&mutex);
		}
        
		else
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'n';
			pthread_mutex_unlock(&mutex);
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
		// (eg software rendering under WSL), which leaves the loop spinning
		usleep(16000);
    }

    // Tell the worker thread to finish and wait for it, so DDS is shut down
	// cleanly - that is what lets the server notice we left
	pthread_mutex_lock(&mutex);
	b_running = false;
	pthread_mutex_unlock(&mutex);
	pthread_join(thread, NULL);

    glfwDestroyWindow(window);
    glfwTerminate();

    security_cleanup();
    return 0;
}
