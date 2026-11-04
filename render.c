/*****************************************************************************
\file render.c
\author regine wong (w.shiyuregine@digipen.edu)
\co-author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/04/2026 (MM/DD/YYYY)
\brief Implementation of render.h: draws the floor, players, HUD, notices and
       end screen with OpenGL, and text with stb_easy_font.
*******************************************************************************/
/* Implementation of render.h. Only the client's main (rendering) thread calls
into this file (Slide 12), always with a RenderState snapshot, so nothing
here needs the client's mutex. */

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <math.h>
#include "render.h"
#include "stb_easy_font.h"

// ### DRAWING HELPERS ###

// One RGB colour per player, indexed by player ID minus 1
static const float arr_colors[MAX_PLAYERS][3] =
{
	{ 0.90f, 0.20f, 0.20f },
	{ 0.20f, 0.45f, 0.90f },
	{ 0.20f, 0.70f, 0.30f },
	{ 0.95f, 0.75f, 0.10f },
	{ 0.65f, 0.30f, 0.80f }
};

// All drawing is in window pixels, origin top-left, and blending is on, so
// every colour here takes an alpha - see render_init()

static const float arr_white[3] = { 1.0f, 1.0f, 1.0f };
static const float arr_alarm[3] = { 1.0f, 0.35f, 0.35f };

// How long a newly claimed cell takes to pop in, in seconds
#define CLAIM_ANIM_SECS 0.25f

// Draws a_text with stb_easy_font at (x, y), scaled by scale, in the
// current colour
static void draw_text(float x, float y, float scale, const char *text)
{
	// stb_easy_font silently stops drawing once this buffer is full, and each
	// character needs a few hundred bytes - 4096 cut off strings of about 22
	// characters (eg "Player 2 left the game"), so leave plenty of room
	char buffer[16384];
	int num_quads = stb_easy_font_print(0, 0, (char *)text, NULL, buffer, sizeof(buffer));

	glPushMatrix();
	glTranslatef(x, y, 0.0f);
	glScalef(scale, scale, 1.0f);

	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 16, buffer);
	glDrawArrays(GL_QUADS, 0, num_quads * 4);
	glDisableClientState(GL_VERTEX_ARRAY);

	glPopMatrix();
}

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

// ### PUBLIC API ###

void render_init(void)
{
	// Alpha blending for the translucent overlays and fades
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// Window pixels, origin top-left, so y grows downwards like the game's
	// coordinates
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, WIDTH, HEIGHT, 0, -1, 1);
}

void render_frame(const RenderState *a_state, float a_f_dt, double a_d_now)
{
	glClearColor(0.07f, 0.08f, 0.12f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	// Back to front: floor, squares, HUD, then the overlays
	draw_cells(&a_state->grid, a_f_dt);
	draw_players(a_state->arr_players, a_state->int_own_id, a_d_now);
	draw_hud(&a_state->grid, a_state->arr_players, a_state->int_own_id,
		a_state->int_seconds_left, a_state->b_round_started, a_d_now,
		a_state->b_spectating);
	draw_notices(a_state->arr_notices, a_d_now);
	if (a_state->b_spectating)
		draw_spectator_banner(a_d_now);
	if (!a_state->b_round_started)
		draw_waiting(a_d_now);
	if (a_state->b_game_over)
		draw_end_screen(&a_state->grid, a_state->arr_players, a_state->int_own_id,
			a_state->int_winner_id);
}
