/*****************************************************************************
\file render.c
\author regine wong (w.shiyuregine@digipen.edu)
\co-author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/04/2026 (MM/DD/YYYY)
\brief Implementation of render.h: draws the floor, players, HUD, notices and
       end screen with OpenGL, and text with stb_easy_font. All art is
       procedural (shapes, gradients, particles) - no image assets.
*******************************************************************************/
/* Implementation of render.h. Only the client's main (rendering) thread calls
into this file (Slide 12), always with a RenderState snapshot, so nothing
here needs the client's mutex. The visual polish (smooth gliding squares with
faces, bevelled tiles, particles, confetti, rounded HUD) is client-side only
and never touches the network or game rules. */

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <math.h>
#include "render.h"
#include <stdlib.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include "stb_easy_font.h"

// Older GL headers (GLFW pulls in GL 1.1) lack this
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// ### CONSTANTS AND SMALL STATE ###

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
static const float arr_gold[3]  = { 1.0f, 0.82f, 0.25f };
static const float arr_mint[3]  = { 0.45f, 1.00f, 0.65f };

#define PI_F 3.14159265f

// How long a newly claimed cell takes to pop in, in seconds
#define CLAIM_ANIM_SECS 0.25f

// How quickly displayed squares / counts chase the real values (per second)
#define GLIDE_RATE 24.0f
#define COUNT_RATE 8.0f

// Per-cell animation state: the owner last drawn, and seconds since that
// owner changed. Only used by the main (rendering) thread
static int arr_shown_owner[GRID_ROWS * GRID_COLS];
static float arr_claim_age[GRID_ROWS * GRID_COLS];

// Per-player display state: where the square is drawn (it glides toward the
// real position), where it last was, which way it faces, a hop timer for
// squash-and-stretch, and the smoothed cell count shown in the HUD
static bool arr_was_active[MAX_PLAYERS];
static float arr_disp_x[MAX_PLAYERS];
static float arr_disp_y[MAX_PLAYERS];
static int arr_last_x[MAX_PLAYERS];
static int arr_last_y[MAX_PLAYERS];
static float arr_face_x[MAX_PLAYERS];
static float arr_face_y[MAX_PLAYERS];
static float arr_hop[MAX_PLAYERS];
static float arr_disp_count[MAX_PLAYERS];

// Banner / transition timers
static bool b_prev_started = false;
static bool b_prev_over = false;
static float f_go_timer = 0.0f;
static float f_end_timer = 0.0f;
static int int_prev_secs = -1;
static float f_tick_age = 1.0f;
static float f_confetti_accum = 0.0f;

// ### PARTICLES ###

typedef enum { PART_SPARK, PART_PUFF, PART_CONFETTI } PartKind;

typedef struct
{
	float x, y, vx, vy;
	float life, max_life;
	float size, gravity, rot, vrot;
	float r, g, b;
	PartKind kind;
} Particle;

#define MAX_PARTICLES 512
static Particle arr_particles[MAX_PARTICLES];

// Tiny xorshift generator - visual randomness only, so quality is irrelevant
static unsigned int uint_seed = 2463534242u;

static float rnd01(void)
{
	uint_seed ^= uint_seed << 13;
	uint_seed ^= uint_seed >> 17;
	uint_seed ^= uint_seed << 5;
	return (float)(uint_seed & 0xFFFFFF) / (float)0x1000000;
}

static float rnd_range(float a_f_lo, float a_f_hi)
{
	return a_f_lo + (a_f_hi - a_f_lo) * rnd01();
}

static void spawn_particle(PartKind a_kind, float a_f_x, float a_f_y,
	float a_f_vx, float a_f_vy, float a_f_life, float a_f_size,
	float a_f_gravity, const float *a_arr_rgb)
{
	for (int i = 0; i < MAX_PARTICLES; i++)
	{
		Particle *p = &arr_particles[i];
		if (p->life > 0.0f)
			continue;
		p->x = a_f_x;
		p->y = a_f_y;
		p->vx = a_f_vx;
		p->vy = a_f_vy;
		p->life = p->max_life = a_f_life;
		p->size = a_f_size;
		p->gravity = a_f_gravity;
		p->rot = rnd_range(0.0f, 360.0f);
		p->vrot = rnd_range(-360.0f, 360.0f);
		p->r = a_arr_rgb[0];
		p->g = a_arr_rgb[1];
		p->b = a_arr_rgb[2];
		p->kind = a_kind;
		return;
	}
	// Pool full: drop the particle, nobody will miss one
}

static void update_particles(float a_f_dt)
{
	for (int i = 0; i < MAX_PARTICLES; i++)
	{
		Particle *p = &arr_particles[i];
		if (p->life <= 0.0f)
			continue;
		p->life -= a_f_dt;
		p->vy += p->gravity * a_f_dt;
		p->x += p->vx * a_f_dt;
		p->y += p->vy * a_f_dt;
		p->rot += p->vrot * a_f_dt;
		if (p->kind != PART_CONFETTI)
		{
			// Sparks and puffs slow down; confetti flutters on
			p->vx *= (1.0f - 2.5f * a_f_dt > 0.0f) ? (1.0f - 2.5f * a_f_dt) : 0.0f;
			p->vy *= (1.0f - 2.5f * a_f_dt > 0.0f) ? (1.0f - 2.5f * a_f_dt) : 0.0f;
		}
	}
}

// ### DRAWING HELPERS ###

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

// ### TEXTURES ###
// The art lives in the assets folder as PNGs (see README "Art assets"),
// not in this file. Most are white or grey on purpose: every draw call below
// sets the colour first (glColor), and OpenGL multiplies it into the image,
// so one picture serves all five player colours and any alpha fade.

typedef enum
{
	TEX_CIRCLE, TEX_SPARK, TEX_SOFT, TEX_PANEL, TEX_RING, TEX_TILE,
	TEX_PLAYER, TEX_EYE, TEX_PUPIL, TEX_CONFETTI, TEX_FLOOR, TEX_CROWN,
	TEX_BACKGROUND, TEX_VIGNETTE, TEX_HUD_BAR, TEX_COUNT
} TexId;

// File names, indexed by TexId
static const char *arr_tex_files[TEX_COUNT] =
{
	"circle.png", "spark.png", "soft.png", "panel.png", "ring.png", "tile.png",
	"player.png", "eye.png", "pupil.png", "confetti.png", "floor.png", "crown.png",
	"background.png", "vignette.png", "hud_bar.png"
};

static GLuint arr_tex[TEX_COUNT];

// Loads one PNG into an OpenGL texture. A missing or unreadable file is
// reported and replaced by a plain white pixel, so the game still runs
static GLuint load_texture(const char *a_str_dir, const char *a_str_file)
{
	char str_path[512];
	snprintf(str_path, sizeof(str_path), "%s/%s", a_str_dir, a_str_file);

	int int_w = 0, int_h = 0;
	int int_comp = 0;
	unsigned char *ptr_px = stbi_load(str_path, &int_w, &int_h, &int_comp, 4);
	static unsigned char arr_white_px[4] = { 255, 255, 255, 255 };
	if (!ptr_px)
	{
		fprintf(stderr, "render: cannot load %s (run from the repository root, or "
			"set CTF_ASSET_DIR) - using a blank placeholder\n", str_path);
		ptr_px = arr_white_px;
		int_w = int_h = 1;
	}

	GLuint id = 0;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, int_w, int_h, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, ptr_px);
	if (ptr_px != arr_white_px)
		stbi_image_free(ptr_px);
	return id;
}

// Draws a texture over a rectangle, tinted by the current colour; the
// u/v arguments pick the part of the image (swap them to flip it)
static void draw_image_uv(TexId a_tex, float a_f_x, float a_f_y, float a_f_w,
	float a_f_h, float a_f_u0, float a_f_v0, float a_f_u1, float a_f_v1)
{
	glBindTexture(GL_TEXTURE_2D, arr_tex[a_tex]);
	glEnable(GL_TEXTURE_2D);
	glBegin(GL_QUADS);
		glTexCoord2f(a_f_u0, a_f_v0); glVertex2f(a_f_x, a_f_y);
		glTexCoord2f(a_f_u0, a_f_v1); glVertex2f(a_f_x, a_f_y + a_f_h);
		glTexCoord2f(a_f_u1, a_f_v1); glVertex2f(a_f_x + a_f_w, a_f_y + a_f_h);
		glTexCoord2f(a_f_u1, a_f_v0); glVertex2f(a_f_x + a_f_w, a_f_y);
	glEnd();
	glDisable(GL_TEXTURE_2D);
}

static void draw_image(TexId a_tex, float a_f_x, float a_f_y, float a_f_w, float a_f_h)
{
	draw_image_uv(a_tex, a_f_x, a_f_y, a_f_w, a_f_h, 0.0f, 0.0f, 1.0f, 1.0f);
}

// Stretches a 64x64 image that has 16 px rounded corners over any size
// without distorting the corners (a "nine-slice"): the corners are drawn at
// a_f_corner pixels, the edges and middle stretch
static void draw_nine(TexId a_tex, float a_f_x, float a_f_y, float a_f_w,
	float a_f_h, float a_f_corner)
{
	float f_max = (a_f_w < a_f_h ? a_f_w : a_f_h) / 2.0f;
	float f_c = a_f_corner < f_max ? a_f_corner : f_max;
	float arr_x[4] = { a_f_x, a_f_x + f_c, a_f_x + a_f_w - f_c, a_f_x + a_f_w };
	float arr_y[4] = { a_f_y, a_f_y + f_c, a_f_y + a_f_h - f_c, a_f_y + a_f_h };
	static const float arr_uv[4] = { 0.0f, 0.25f, 0.75f, 1.0f };

	glBindTexture(GL_TEXTURE_2D, arr_tex[a_tex]);
	glEnable(GL_TEXTURE_2D);
	glBegin(GL_QUADS);
	for (int r = 0; r < 3; r++)
	{
		for (int c = 0; c < 3; c++)
		{
			glTexCoord2f(arr_uv[c], arr_uv[r]);         glVertex2f(arr_x[c], arr_y[r]);
			glTexCoord2f(arr_uv[c], arr_uv[r + 1]);     glVertex2f(arr_x[c], arr_y[r + 1]);
			glTexCoord2f(arr_uv[c + 1], arr_uv[r + 1]); glVertex2f(arr_x[c + 1], arr_y[r + 1]);
			glTexCoord2f(arr_uv[c + 1], arr_uv[r]);     glVertex2f(arr_x[c + 1], arr_y[r]);
		}
	}
	glEnd();
	glDisable(GL_TEXTURE_2D);
}

// ### SHAPES (all built from the textures above) ###

// Rectangle with a different RGBA colour at each corner (top-left,
// top-right, bottom-right, bottom-left) - used for the animated time bar
static void draw_quad_grad(float a_f_x, float a_f_y, float a_f_w, float a_f_h,
	const float *a_tl, const float *a_tr, const float *a_br, const float *a_bl)
{
	glBegin(GL_QUADS);
		glColor4fv(a_tl); glVertex2f(a_f_x, a_f_y);
		glColor4fv(a_bl); glVertex2f(a_f_x, a_f_y + a_f_h);
		glColor4fv(a_br); glVertex2f(a_f_x + a_f_w, a_f_y + a_f_h);
		glColor4fv(a_tr); glVertex2f(a_f_x + a_f_w, a_f_y);
	glEnd();
}

static void draw_hgrad(float a_f_x, float a_f_y, float a_f_w, float a_f_h,
	const float *a_left, const float *a_right)
{
	draw_quad_grad(a_f_x, a_f_y, a_f_w, a_f_h, a_left, a_right, a_right, a_left);
}

// Filled circle in the current colour. The image's disc has a 30/64 radius
static void draw_circle(float a_f_cx, float a_f_cy, float a_f_r)
{
	float f_size = a_f_r * 2.0f * 64.0f / 60.0f;
	draw_image(TEX_CIRCLE, a_f_cx - f_size / 2.0f, a_f_cy - f_size / 2.0f, f_size, f_size);
}

// Rounded rectangle in the current colour, corner radius a_f_r
static void draw_rrect(float a_f_x, float a_f_y, float a_f_w, float a_f_h, float a_f_r)
{
	draw_nine(TEX_PANEL, a_f_x, a_f_y, a_f_w, a_f_h, a_f_r);
}

// Its outline, about 2 px thick. The ring's corners never get tighter than
// 10 px, or the line would thin out
static void draw_rrect_outline(float a_f_x, float a_f_y, float a_f_w, float a_f_h, float a_f_r)
{
	float f_c = a_f_r < 10.0f ? 10.0f : (a_f_r > 22.0f ? 22.0f : a_f_r);
	draw_nine(TEX_RING, a_f_x, a_f_y, a_f_w, a_f_h, f_c);
}

// Blurred rounded blob around the given box (the image's solid part is
// 40/64 of its width): tinted black it is a shadow, white a glow
static void draw_soft(float a_f_x, float a_f_y, float a_f_w, float a_f_h)
{
	float f_k = 64.0f / 40.0f;
	float f_w = a_f_w * f_k;
	float f_h = a_f_h * f_k;
	draw_image(TEX_SOFT, a_f_x + a_f_w / 2.0f - f_w / 2.0f,
		a_f_y + a_f_h / 2.0f - f_h / 2.0f, f_w, f_h);
}

// Gold crown, a_f_x/a_f_y = top-left, with a faint shadow so it reads on any
// colour. Not tinted - the image already has its colours
static void draw_crown(float a_f_x, float a_f_y, float a_f_w, float a_f_h, float a_f_alpha)
{
	glColor4f(0.0f, 0.0f, 0.0f, 0.35f * a_f_alpha);
	draw_image(TEX_CROWN, a_f_x + 1.0f, a_f_y + 1.0f, a_f_w, a_f_h);
	glColor4f(1.0f, 1.0f, 1.0f, a_f_alpha);
	draw_image(TEX_CROWN, a_f_x, a_f_y, a_f_w, a_f_h);
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

// Text whose glyph middle sits on a_f_cy (a glyph is about 7 units tall)
static void draw_label_middle(float a_f_cx, float a_f_cy, float a_f_scale,
	const float *a_arr_rgb, float a_f_alpha, const char *a_str_text)
{
	draw_label_centered(a_f_cx, a_f_cy - 3.5f * a_f_scale, a_f_scale,
		a_arr_rgb, a_f_alpha, a_str_text);
}

// "COLOR THE FLOOR" with every letter in the next player colour, bobbing
// in a wave
static void draw_rainbow_title(float a_f_cx, float a_f_y, float a_f_scale,
	float a_f_alpha, double a_d_time)
{
	const char *str_title = "COLOR THE FLOOR";
	float f_total = stb_easy_font_width((char *)str_title) * a_f_scale;
	float f_x = a_f_cx - f_total / 2.0f;
	int int_color = 0;

	for (int i = 0; str_title[i] != '\0'; i++)
	{
		char str_prefix[32];
		char str_one[2] = { str_title[i], '\0' };
		snprintf(str_prefix, sizeof(str_prefix), "%.*s", i, str_title);
		float f_off = stb_easy_font_width(str_prefix) * a_f_scale;
		if (str_title[i] == ' ')
			continue;
		float f_bob = sinf((float)a_d_time * 3.0f + i * 0.5f) * 4.0f;
		draw_label(f_x + f_off, a_f_y + f_bob, a_f_scale,
			arr_colors[int_color % MAX_PLAYERS], a_f_alpha, str_one);
		int_color++;
	}
}

// ### SCENE ###

// Dark gradient backdrop; the floor tiles are drawn on top of it
static void draw_background(void)
{
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	draw_image(TEX_BACKGROUND, 0, 0, WIDTH, HEIGHT);
}

// Checkerboard floor, grid lines, every claimed cell as a bevelled tile in
// its owner's colour, and a vignette to frame it. A tile that has just
// changed hands grows in from its centre with a brief white flash and
// throws off sparks
static void draw_cells(const Grid *a_grid, float a_f_dt, double a_d_time)
{
	// Unclaimed floor: faint checkerboard with a slow shimmer
	for (int int_row = 0; int_row < GRID_ROWS; int_row++)
	{
		for (int int_col = 0; int_col < GRID_COLS; int_col++)
		{
			float f_shimmer = 0.012f * sinf((float)a_d_time * 1.2f + (int_col + int_row) * 0.6f);
			float f_a = ((int_col + int_row) & 1) ? 0.055f : 0.020f;
			glColor4f(0.75f, 0.85f, 1.0f, f_a + f_shimmer);
			draw_image(TEX_FLOOR, int_col * SQ_WIDTH, PLAY_TOP + int_row * SQ_WIDTH,
				SQ_WIDTH, SQ_WIDTH);
		}
	}

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

	int int_bursts = 0;
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

				// Sparks from the new tile's centre. Capped per frame so a
				// late joiner receiving the whole grid does not flood the pool
				if (int_owner >= 1 && int_owner <= MAX_PLAYERS && int_bursts < 10)
				{
					int_bursts++;
					float f_cx = int_col * SQ_WIDTH + SQ_WIDTH / 2.0f;
					float f_cy = PLAY_TOP + int_row * SQ_WIDTH + SQ_WIDTH / 2.0f;
					for (int k = 0; k < 6; k++)
					{
						float f_ang = rnd_range(0.0f, 2.0f * PI_F);
						float f_spd = rnd_range(60.0f, 170.0f);
						spawn_particle(PART_SPARK, f_cx, f_cy,
							cosf(f_ang) * f_spd, sinf(f_ang) * f_spd,
							rnd_range(0.35f, 0.7f), rnd_range(2.0f, 3.5f), 0.0f,
							k & 1 ? arr_white : arr_colors[int_owner - 1]);
					}
				}
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
			float f_x = int_col * SQ_WIDTH + f_inset;
			float f_y = PLAY_TOP + int_row * SQ_WIDTH + f_inset;
			float f_s = SQ_WIDTH - 2.0f * f_inset;

			// The tile image already has its bevel, highlight and gloss baked
			// in (assets/tile.png); here it is just tinted with the owner's
			// colour, brightened by the claim flash
			glColor4f(arr_c[0] + f_flash * (1.0f - arr_c[0]),
				arr_c[1] + f_flash * (1.0f - arr_c[1]),
				arr_c[2] + f_flash * (1.0f - arr_c[2]), 1.0f);
			draw_image(TEX_TILE, f_x, f_y, f_s, f_s);
		}
	}

	// Vignette: darkens the edges so the eye stays on the action
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	draw_image_uv(TEX_VIGNETTE, 0, PLAY_TOP, 90, PLAY_HEIGHT, 0.0f, 0.0f, 1.0f, 1.0f);
	draw_image_uv(TEX_VIGNETTE, WIDTH - 90, PLAY_TOP, 90, PLAY_HEIGHT, 1.0f, 0.0f, 0.0f, 1.0f);
}

// Moves each player's drawn position toward the real one, tracking facing
// direction, hop timers and dust puffs for the movement
static void update_players(const Player a_arr_players[], float a_f_dt)
{
	float f_follow = 1.0f - expf(-GLIDE_RATE * a_f_dt);

	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!a_arr_players[i].b_active)
		{
			arr_was_active[i] = false;
			continue;
		}

		float f_tx = (float)a_arr_players[i].x;
		float f_ty = (float)a_arr_players[i].y;

		if (!arr_was_active[i])
		{
			// New (or re-spawned) player: appear in place, facing down
			arr_was_active[i] = true;
			arr_disp_x[i] = f_tx;
			arr_disp_y[i] = f_ty;
			arr_last_x[i] = a_arr_players[i].x;
			arr_last_y[i] = a_arr_players[i].y;
			arr_face_x[i] = 0.0f;
			arr_face_y[i] = 1.0f;
			arr_hop[i] = 1.0f;
			// Spawn flourish
			for (int k = 0; k < 12; k++)
			{
				float f_ang = rnd_range(0.0f, 2.0f * PI_F);
				spawn_particle(PART_SPARK, f_tx + SQ_WIDTH / 2.0f, f_ty + SQ_WIDTH / 2.0f,
					cosf(f_ang) * 120.0f, sinf(f_ang) * 120.0f, 0.6f, 3.0f, 0.0f,
					arr_colors[i]);
			}
			continue;
		}

		if (a_arr_players[i].x != arr_last_x[i] || a_arr_players[i].y != arr_last_y[i])
		{
			float f_dx = (float)(a_arr_players[i].x - arr_last_x[i]);
			float f_dy = (float)(a_arr_players[i].y - arr_last_y[i]);
			float f_len = sqrtf(f_dx * f_dx + f_dy * f_dy);
			if (f_len > 0.0f)
			{
				arr_face_x[i] = f_dx / f_len;
				arr_face_y[i] = f_dy / f_len;
			}
			arr_hop[i] = 1.0f;

			// Dust puffs where the square just was
			for (int k = 0; k < 3; k++)
			{
				spawn_particle(PART_PUFF,
					arr_disp_x[i] + SQ_WIDTH / 2.0f + rnd_range(-10.0f, 10.0f),
					arr_disp_y[i] + SQ_WIDTH - 6.0f + rnd_range(-4.0f, 4.0f),
					rnd_range(-25.0f, 25.0f), rnd_range(-25.0f, 5.0f),
					rnd_range(0.3f, 0.5f), rnd_range(4.0f, 7.0f), 0.0f,
					arr_colors[i]);
			}
			arr_last_x[i] = a_arr_players[i].x;
			arr_last_y[i] = a_arr_players[i].y;
		}

		arr_disp_x[i] += (f_tx - arr_disp_x[i]) * f_follow;
		arr_disp_y[i] += (f_ty - arr_disp_y[i]) * f_follow;
		if (arr_hop[i] > 0.0f)
		{
			arr_hop[i] -= a_f_dt * 5.0f;
			if (arr_hop[i] < 0.0f)
				arr_hop[i] = 0.0f;
		}
	}
}

// Each active player's square: soft shadow, rounded gradient body, eyes
// that look where it is heading (and blink), ID badge and outline. The local
// player gets a pulsing glow ring and a YOU marker; whoever leads gets a crown
static void draw_players(const Grid *a_grid, const Player a_arr_players[],
	int a_int_own_id, bool a_b_started, double a_d_time)
{
	float f_pulse = 0.5f + 0.5f * sinf((float)a_d_time * 6.0f);
	int int_leader = a_b_started ? grid_winner(a_grid, MAX_PLAYERS) : 0;

	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!a_arr_players[i].b_active)
			continue;

		bool b_own = (i + 1 == a_int_own_id);
		const float *arr_c = arr_colors[i];
		float f_hop = arr_hop[i];

		// Squash on landing: wider and shorter, anchored to the floor
		float f_w = SQ_WIDTH * (1.0f + 0.14f * f_hop);
		float f_h = SQ_WIDTH * (1.0f - 0.14f * f_hop);
		float f_bob = sinf((float)a_d_time * 3.0f + i * 1.7f) * 1.5f;
		float f_x = arr_disp_x[i] + (SQ_WIDTH - f_w) / 2.0f;
		float f_y = arr_disp_y[i] + (SQ_WIDTH - f_h) + f_bob - 2.0f;

		// Soft shadow on the floor
		glColor4f(0.0f, 0.0f, 0.0f, 0.45f);
		draw_soft(arr_disp_x[i] + 3.0f, arr_disp_y[i] + 6.0f, SQ_WIDTH, SQ_WIDTH - 2.0f);

		// Glow for the local player
		if (b_own)
		{
			glColor4f(1.0f, 1.0f, 1.0f, 0.30f + 0.30f * f_pulse);
			draw_soft(f_x - 1.0f, f_y - 1.0f, f_w + 2.0f, f_h + 2.0f);
		}

		// Body: assets/player.png (gradient, gloss and darker lip), tinted
		float f_k = 1.0f / 0.9f;
		glColor4f(arr_c[0] * f_k > 1.0f ? 1.0f : arr_c[0] * f_k,
			arr_c[1] * f_k > 1.0f ? 1.0f : arr_c[1] * f_k,
			arr_c[2] * f_k > 1.0f ? 1.0f : arr_c[2] * f_k, 1.0f);
		draw_image(TEX_PLAYER, f_x, f_y, f_w, f_h);

		if (b_own)
		{
			glLineWidth(3.0f);
			glColor4f(1.0f, 1.0f, 1.0f, 0.75f + 0.25f * f_pulse);
		}
		else
		{
			glLineWidth(2.0f);
			glColor4f(1.0f, 1.0f, 1.0f, 0.45f);
		}
		draw_rrect_outline(f_x, f_y, f_w, f_h - 3.0f, 10.0f);
		glLineWidth(1.0f);

		// Eyes: whites, pupils shifted toward the facing direction, and a
		// quick blink every few seconds (offset per player)
		float f_ey = f_y + (f_h - 3.0f) * 0.38f;
		bool b_blink = fmodf((float)a_d_time + i * 1.3f, 3.6f) < 0.12f;
		for (int e = 0; e < 2; e++)
		{
			float f_ex = f_x + f_w * (e == 0 ? 0.32f : 0.68f);
			if (b_blink)
			{
				glColor4f(0.08f, 0.08f, 0.12f, 0.9f);
				draw_rect(f_ex - 5.0f, f_ey - 1.0f, 10.0f, 2.0f);
				continue;
			}
			// assets/eye.png (white) and assets/pupil.png (dark, with its
			// highlight painted in), the pupil shifted toward the facing
			glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
			draw_image(TEX_EYE, f_ex - 7.2f, f_ey - 7.2f, 14.4f, 14.4f);
			draw_image(TEX_PUPIL, f_ex + arr_face_x[i] * 2.4f - 4.4f,
				f_ey + arr_face_y[i] * 2.4f - 4.4f, 8.8f, 8.8f);
		}

		// ID badge under the eyes
		char str_id[2];
		snprintf(str_id, sizeof(str_id), "%d", i + 1);
		draw_label_centered(f_x + f_w / 2.0f, f_y + f_h - 17.0f, 1.4f, arr_white, 0.95f, str_id);

		float f_top = f_y;
		if (int_leader == i + 1)
		{
			draw_crown(f_x + f_w / 2.0f - 11.0f, f_y - 13.0f + 2.0f * f_pulse, 22.0f, 15.0f, 1.0f);
			f_top -= 14.0f;
		}

		if (b_own)
		{
			// Above the square, or below it when there is not enough room
			// above to clear the top HUD margin (PLAY_TOP)
			bool b_room_above = (f_top - PLAY_TOP) > 18.0f;
			float f_marker_y = b_room_above ? f_top - 17.0f : f_y + SQ_WIDTH + 5.0f;
			draw_label_centered(f_x + f_w / 2.0f, f_marker_y, 1.5f, arr_white, 1.0f, "YOU");
		}
	}
}

// Sparks, dust puffs (a_b_confetti false) or confetti (true)
static void draw_particles(bool a_b_confetti)
{
	for (int i = 0; i < MAX_PARTICLES; i++)
	{
		const Particle *p = &arr_particles[i];
		if (p->life <= 0.0f || (p->kind == PART_CONFETTI) != a_b_confetti)
			continue;

		float f_k = p->life / p->max_life;
		if (p->kind == PART_CONFETTI)
		{
			// Flutter: the sheet's apparent width oscillates as it spins
			float f_flip = fabsf(cosf(p->rot * PI_F / 180.0f));
			float f_a = f_k < 0.2f ? f_k / 0.2f : 1.0f;
			glColor4f(p->r, p->g, p->b, f_a);
			glPushMatrix();
			glTranslatef(p->x, p->y, 0.0f);
			glRotatef(p->rot * 0.5f, 0.0f, 0.0f, 1.0f);
			draw_image(TEX_CONFETTI, -p->size * f_flip, -p->size * 0.5f,
				p->size * 2.0f * f_flip, p->size);
			glPopMatrix();
		}
		else if (p->kind == PART_PUFF)
		{
			float f_r = p->size * (1.4f - 0.6f * f_k);
			glColor4f(p->r, p->g, p->b, 0.35f * f_k);
			draw_image(TEX_SPARK, p->x - f_r * 1.6f, p->y - f_r * 1.6f, f_r * 3.2f, f_r * 3.2f);
		}
		else
		{
			float f_r = p->size * (0.4f + 0.6f * f_k);
			glColor4f(p->r, p->g, p->b, f_k);
			draw_image(TEX_SPARK, p->x - f_r * 1.6f, p->y - f_r * 1.6f, f_r * 3.2f, f_r * 3.2f);
		}
	}
}

// Top strip (who you are, countdown, time bar) and bottom strip
// (territory bar, per-player cell counts)
static void draw_hud(const Grid *a_grid, const Player a_arr_players[],
	int a_int_own_id, int a_int_secs, bool a_b_started, double a_d_time,
	bool a_b_spectating, float a_f_dt)
{
	char str_text[32];
	bool b_low = a_b_started && a_int_secs <= 10;
	float f_blink = 0.65f + 0.35f * sinf((float)a_d_time * 8.0f);

	// Top strip - exactly HUD_MARGIN_TOP tall (solid bar + the thin time
	// bar below it), so it can never bleed into the playable grid
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	draw_image(TEX_HUD_BAR, 0, 0, WIDTH, HUD_MARGIN_TOP - 4);

	if (a_int_own_id >= 1 && a_int_own_id <= MAX_PLAYERS)
	{
		const float *arr_c = arr_colors[a_int_own_id - 1];
		glColor4f(0.0f, 0.0f, 0.0f, 0.35f);
		draw_rrect(8, 7, 142, 32, 16.0f);
		glColor4f(arr_c[0], arr_c[1], arr_c[2], 1.0f);
		draw_circle(25, 23, 10.0f);
		glColor4f(1.0f, 1.0f, 1.0f, 0.35f);
		draw_circle(22, 20, 3.5f);
		snprintf(str_text, sizeof(str_text), "Player %d", a_int_own_id);
		draw_label(42, 15, 2.0f, arr_white, 1.0f, str_text);
	}
	else if (a_b_spectating)
	{
		draw_label(12, 14, 2.5f, arr_white, 0.9f, "Spectator");
	}
	else
	{
		draw_label(12, 14, 2.5f, arr_white, 0.7f, "Connecting...");
	}

	// Timer in a pill
	snprintf(str_text, sizeof(str_text), "%d:%02d", a_int_secs / 60, a_int_secs % 60);
	glColor4f(0.0f, 0.0f, 0.0f, 0.4f);
	draw_rrect(WIDTH / 2.0f - 52, 5, 104, 36, 18.0f);
	if (b_low)
		glColor4f(arr_alarm[0], arr_alarm[1], arr_alarm[2], 0.5f * f_blink);
	else
		glColor4f(1.0f, 1.0f, 1.0f, 0.25f);
	glLineWidth(2.0f);
	draw_rrect_outline(WIDTH / 2.0f - 52, 5, 104, 36, 18.0f);
	glLineWidth(1.0f);
	draw_label_middle(WIDTH / 2.0f, 23, 3.4f, b_low ? arr_alarm : arr_white,
		b_low ? f_blink : 1.0f, str_text);

	float f_title_w = stb_easy_font_width((char *)"COLOR THE FLOOR") * 1.5f;
	draw_label(WIDTH - 14 - f_title_w, 18, 1.5f, arr_white, 0.45f, "COLOR THE FLOOR");

	// Time bar: drains as the round runs out
	float f_frac = a_b_started ? (float)a_int_secs / ROUND_SECS : 1.0f;
	glColor4f(1.0f, 1.0f, 1.0f, 0.1f);
	draw_rect(0, HUD_MARGIN_TOP - 4, WIDTH, 4);
	if (b_low)
	{
		glColor4f(arr_alarm[0], arr_alarm[1], arr_alarm[2], f_blink);
		draw_rect(0, HUD_MARGIN_TOP - 4, WIDTH * f_frac, 4);
	}
	else
	{
		// Mint-to-white sweep
		const float arr_l[4] = { arr_mint[0], arr_mint[1], arr_mint[2], 0.9f };
		const float arr_r[4] = { 1.0f, 1.0f, 1.0f, 0.9f };
		draw_hgrad(0, HUD_MARGIN_TOP - 4, WIDTH * f_frac, 4, arr_l, arr_r);
	}

	// Count each player's cells once, for both the bar and the labels, and
	// ease the displayed value toward it so bars slide instead of jumping
	int arr_counts[MAX_PLAYERS];
	float f_count_follow = 1.0f - expf(-COUNT_RATE * a_f_dt);
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		arr_counts[i] = grid_count_owned(a_grid, i + 1);
		arr_disp_count[i] += (arr_counts[i] - arr_disp_count[i]) * f_count_follow;
	}

	// Bottom strip - exactly HUD_MARGIN_BOTTOM tall, for the same reason
	// as the top strip above
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	draw_image_uv(TEX_HUD_BAR, 0, HEIGHT - HUD_MARGIN_BOTTOM, WIDTH, HUD_MARGIN_BOTTOM,
		0.0f, 1.0f, 1.0f, 0.0f);

	// Territory bar: each player's share of the whole floor
	float f_bar_x = 10.0f;
	float f_bar_w = WIDTH - 20.0f;
	float f_bar_y = HEIGHT - HUD_MARGIN_BOTTOM + 5.0f;
	glColor4f(1.0f, 1.0f, 1.0f, 0.10f);
	draw_rrect(f_bar_x, f_bar_y, f_bar_w, 10, 5.0f);
	float f_cursor = f_bar_x;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		float f_seg = f_bar_w * arr_disp_count[i] / (GRID_COLS * GRID_ROWS);
		if (f_seg < 0.5f)
			continue;
		glColor3fv(arr_colors[i]);
		draw_rect(f_cursor, f_bar_y, f_seg, 10);
		glColor4f(1.0f, 1.0f, 1.0f, 0.25f);
		draw_rect(f_cursor, f_bar_y, f_seg, 3);
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
		bool b_own = (i + 1 == a_int_own_id);
		if (b_own)
		{
			glColor4f(1.0f, 1.0f, 1.0f, 0.12f);
			draw_rrect(f_x - 4, f_y - 3, 130, 20, 8.0f);
		}
		glColor3fv(arr_colors[i]);
		draw_circle(f_x + 7, f_y + 6, 6.5f);
		glColor4f(1.0f, 1.0f, 1.0f, 0.35f);
		draw_circle(f_x + 5, f_y + 4, 2.0f);
		snprintf(str_text, sizeof(str_text), "P%d  %d", i + 1, arr_counts[i]);
		draw_label(f_x + 20.0f, f_y, 1.7f, arr_white, 1.0f, str_text);
		int_slot++;
	}
}

// Big faded number that pulses on each of the last five seconds, and a
// "GO!" that zooms out when a round starts
static void draw_countdown_fx(int a_int_secs, bool a_b_started, bool a_b_over)
{
	if (a_b_started && !a_b_over && a_int_secs >= 1 && a_int_secs <= 5)
	{
		char str_n[2];
		snprintf(str_n, sizeof(str_n), "%d", a_int_secs);
		float f_k = f_tick_age < 1.0f ? f_tick_age : 1.0f;
		float f_scale = 16.0f + 8.0f * (1.0f - f_k);
		draw_label_middle(WIDTH / 2.0f, HEIGHT / 2.0f, f_scale, arr_alarm,
			0.30f * (1.0f - f_k), str_n);
	}

	if (f_go_timer > 0.0f)
	{
		float f_k = f_go_timer / 1.2f;
		float f_scale = 8.0f + 6.0f * (1.0f - f_k);
		draw_label_middle(WIDTH / 2.0f, HEIGHT / 2.0f - 20.0f, f_scale, arr_mint,
			f_k > 1.0f ? 1.0f : f_k, "GO!");
	}
}

// ID of the player who may start the round: the lowest active ID, or 0 if
// nobody has joined. Declared in render.h so client.c and this file agree
// with the server, which applies the same rule
int render_host_id(const Player a_arr_players[])
{
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (a_arr_players[i].b_active)
			return i + 1;
	}
	return 0;
}

// Lobby shown until the host starts the round: the title, one card per
// slot (joined players in their colour, empty slots dashed), and a prompt
// that depends on who you are
static void draw_lobby(const Player a_arr_players[], int a_int_own_id, double a_d_time)
{
	int int_host = render_host_id(a_arr_players);
	int int_joined = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
		int_joined += a_arr_players[i].b_active ? 1 : 0;

	const float f_panel_w = 560.0f;
	const float f_panel_h = 300.0f;
	float f_px = (WIDTH - f_panel_w) / 2.0f;
	float f_py = 135.0f;

	glColor4f(0.0f, 0.0f, 0.0f, 0.68f);
	draw_rrect(f_px, f_py, f_panel_w, f_panel_h, 22.0f);
	glColor4f(1.0f, 1.0f, 1.0f, 0.15f);
	glLineWidth(2.0f);
	draw_rrect_outline(f_px, f_py, f_panel_w, f_panel_h, 22.0f);
	glLineWidth(1.0f);

	draw_rainbow_title(WIDTH / 2.0f, f_py + 22.0f, 4.5f, 1.0f, a_d_time);
	draw_label_centered(WIDTH / 2.0f, f_py + 70.0f, 2.0f, arr_white, 0.6f, "LOBBY");

	// Slot cards
	const float f_card = 80.0f;
	const float f_gap = 14.0f;
	float f_row_w = MAX_PLAYERS * f_card + (MAX_PLAYERS - 1) * f_gap;
	float f_x0 = (WIDTH - f_row_w) / 2.0f;
	float f_y0 = f_py + 105.0f;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		float f_x = f_x0 + i * (f_card + f_gap);
		char str_text[16];
		if (a_arr_players[i].b_active)
		{
			const float *arr_c = arr_colors[i];
			float f_hop = sinf((float)a_d_time * 3.0f + i) * 2.0f;
			glColor4f(0.0f, 0.0f, 0.0f, 0.3f);
			draw_rrect(f_x + 2.0f, f_y0 + 4.0f, f_card, f_card - 14.0f, 12.0f);
			glColor4f(arr_c[0] * 0.65f, arr_c[1] * 0.65f, arr_c[2] * 0.65f, 1.0f);
			draw_rrect(f_x, f_y0 + f_hop + 2.0f, f_card, f_card - 14.0f, 12.0f);
			glColor4f(arr_c[0], arr_c[1], arr_c[2], 1.0f);
			draw_rrect(f_x, f_y0 + f_hop, f_card, f_card - 17.0f, 12.0f);
			glColor4f(1.0f, 1.0f, 1.0f, 0.2f);
			draw_rrect(f_x + 5.0f, f_y0 + f_hop + 4.0f, f_card - 10.0f, 18.0f, 8.0f);
			if (i + 1 == a_int_own_id)
			{
				glColor4f(1.0f, 1.0f, 1.0f, 0.8f);
				glLineWidth(3.0f);
				draw_rrect_outline(f_x, f_y0 + f_hop, f_card, f_card - 17.0f, 12.0f);
				glLineWidth(1.0f);
			}
			// Eyes
			for (int e = 0; e < 2; e++)
			{
				float f_ex = f_x + f_card * (e == 0 ? 0.3f : 0.7f);
				glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
				draw_circle(f_ex, f_y0 + f_hop + 24.0f, 7.0f);
				glColor4f(0.08f, 0.08f, 0.12f, 1.0f);
				draw_circle(f_ex, f_y0 + f_hop + 26.0f, 3.4f);
			}
			snprintf(str_text, sizeof(str_text), "P%d", i + 1);
			draw_label_centered(f_x + f_card / 2.0f, f_y0 + f_hop + 40.0f, 2.0f,
				arr_white, 1.0f, str_text);
			if (i + 1 == int_host)
				draw_crown(f_x + f_card / 2.0f - 12.0f, f_y0 - 16.0f + f_hop, 24.0f, 16.0f, 1.0f);
			if (i + 1 == a_int_own_id)
				draw_label_centered(f_x + f_card / 2.0f, f_y0 + f_card - 10.0f, 1.5f,
					arr_white, 1.0f, "YOU");
			else if (i + 1 == int_host)
				draw_label_centered(f_x + f_card / 2.0f, f_y0 + f_card - 10.0f, 1.5f,
					arr_gold, 1.0f, "HOST");
		}
		else
		{
			glColor4f(1.0f, 1.0f, 1.0f, 0.07f);
			draw_rrect(f_x, f_y0, f_card, f_card - 17.0f, 12.0f);
			glColor4f(1.0f, 1.0f, 1.0f, 0.22f);
			draw_rrect_outline(f_x, f_y0, f_card, f_card - 17.0f, 12.0f);
			draw_label_middle(f_x + f_card / 2.0f, f_y0 + (f_card - 17.0f) / 2.0f, 1.5f,
				arr_white, 0.3f, "open");
		}
	}

	// Prompt
	float f_pulse = 0.5f + 0.5f * sinf((float)a_d_time * 4.0f);
	float f_by = f_py + f_panel_h - 62.0f;
	bool b_host = (a_int_own_id >= 1 && a_int_own_id == int_host);
	if (b_host && int_joined >= 2)
	{
		glColor4f(arr_mint[0], arr_mint[1], arr_mint[2], 0.18f + 0.18f * f_pulse);
		draw_rrect(WIDTH / 2.0f - 190.0f, f_by, 380.0f, 40.0f, 20.0f);
		glColor4f(arr_mint[0], arr_mint[1], arr_mint[2], 0.9f);
		glLineWidth(2.0f);
		draw_rrect_outline(WIDTH / 2.0f - 190.0f, f_by, 380.0f, 40.0f, 20.0f);
		glLineWidth(1.0f);
		draw_label_middle(WIDTH / 2.0f, f_by + 20.0f, 2.2f, arr_mint, 1.0f,
			"Press ENTER to start");
	}
	else if (b_host)
	{
		draw_label_middle(WIDTH / 2.0f, f_by + 20.0f, 2.0f, arr_white,
			0.5f + 0.4f * f_pulse, "Waiting for another player...");
	}
	else if (a_int_own_id >= 1)
	{
		char str_wait[48];
		snprintf(str_wait, sizeof(str_wait), "Waiting for Player %d to start...", int_host);
		draw_label_middle(WIDTH / 2.0f, f_by + 20.0f, 2.0f, arr_white,
			0.5f + 0.4f * f_pulse, str_wait);
	}
	else
	{
		draw_label_middle(WIDTH / 2.0f, f_by + 20.0f, 2.0f, arr_white,
			0.5f + 0.4f * f_pulse, "Connecting...");
	}
	draw_label_centered(WIDTH / 2.0f, f_py + f_panel_h - 16.0f, 1.2f, arr_white, 0.35f,
		"Arrow keys to move - warm up while you wait");
}

// Dims the floor and shows the result and final cell counts on a panel,
// sliding in; the winner is crowned and each score has a proportional bar
static void draw_end_screen(const Grid *a_grid, const Player a_arr_players[],
	int a_int_own_id, int a_int_winner_id, double a_d_time)
{
	float f_k = f_end_timer * 3.0f;
	if (f_k > 1.0f)
		f_k = 1.0f;
	float f_ease = 1.0f - (1.0f - f_k) * (1.0f - f_k);

	glColor4f(0.0f, 0.0f, 0.0f, 0.70f * f_ease);
	draw_rect(0, 0, WIDTH, HEIGHT);

	const float f_panel_w = 520.0f;
	const float f_panel_h = 310.0f;
	float f_panel_x = (WIDTH - f_panel_w) / 2.0f;
	float f_panel_y = 145.0f + (1.0f - f_ease) * 50.0f;
	bool b_has_winner = (a_int_winner_id >= 1 && a_int_winner_id <= MAX_PLAYERS);
	const float *arr_accent = b_has_winner ? arr_colors[a_int_winner_id - 1] : arr_white;

	// Panel with a coloured glow behind it
	glColor4f(arr_accent[0], arr_accent[1], arr_accent[2], 0.16f * f_ease);
	draw_rrect(f_panel_x - 8, f_panel_y - 8, f_panel_w + 16, f_panel_h + 16, 26.0f);
	glColor4f(0.10f, 0.11f, 0.17f, 0.97f * f_ease);
	draw_rrect(f_panel_x, f_panel_y, f_panel_w, f_panel_h, 20.0f);
	glColor4f(arr_accent[0], arr_accent[1], arr_accent[2], f_ease);
	draw_rrect(f_panel_x, f_panel_y, f_panel_w, 12.0f, 6.0f);
	glColor4f(arr_accent[0], arr_accent[1], arr_accent[2], 0.6f * f_ease);
	glLineWidth(2.0f);
	draw_rrect_outline(f_panel_x, f_panel_y, f_panel_w, f_panel_h, 20.0f);
	glLineWidth(1.0f);

	if (b_has_winner)
		draw_crown(WIDTH / 2.0f - 20.0f, f_panel_y + 24.0f + 3.0f * sinf((float)a_d_time * 4.0f),
			40.0f, 28.0f, f_ease);

	char str_text[64];
	if (!b_has_winner)
		snprintf(str_text, sizeof(str_text), "DRAW!");
	else if (a_int_winner_id == a_int_own_id)
		snprintf(str_text, sizeof(str_text), "YOU WIN!");
	else
		snprintf(str_text, sizeof(str_text), "PLAYER %d WINS!", a_int_winner_id);
	float f_pop = 1.0f + 0.04f * sinf((float)a_d_time * 5.0f);
	draw_label_centered(WIDTH / 2.0f, f_panel_y + 60.0f, 5.0f * f_pop, arr_accent,
		f_ease, str_text);

	int int_line = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!a_arr_players[i].b_active)
			continue;

		int int_cells = grid_count_owned(a_grid, i + 1);
		float f_y = f_panel_y + 125.0f + int_line * 30.0f;
		float f_frac = (float)int_cells / (GRID_COLS * GRID_ROWS);
		float f_bar_x = f_panel_x + 140.0f;
		float f_bar_w = 250.0f;

		glColor4f(arr_colors[i][0], arr_colors[i][1], arr_colors[i][2], f_ease);
		draw_circle(f_panel_x + 50.0f, f_y + 9.0f, 9.0f);
		glColor4f(1.0f, 1.0f, 1.0f, 0.3f * f_ease);
		draw_circle(f_panel_x + 47.0f, f_y + 6.0f, 3.0f);

		snprintf(str_text, sizeof(str_text), "Player %d", i + 1);
		draw_label(f_panel_x + 68.0f, f_y + 2.0f, 1.8f, arr_white, f_ease, str_text);

		glColor4f(1.0f, 1.0f, 1.0f, 0.10f * f_ease);
		draw_rrect(f_bar_x, f_y + 3.0f, f_bar_w, 12.0f, 6.0f);
		if (f_frac > 0.0f)
		{
			float f_fill = f_bar_w * f_frac * f_ease;
			if (f_fill < 12.0f)
				f_fill = 12.0f;
			glColor4f(arr_colors[i][0], arr_colors[i][1], arr_colors[i][2], f_ease);
			draw_rrect(f_bar_x, f_y + 3.0f, f_fill, 12.0f, 6.0f);
			glColor4f(1.0f, 1.0f, 1.0f, 0.25f * f_ease);
			draw_rrect(f_bar_x + 2.0f, f_y + 4.0f, f_fill - 4.0f, 4.0f, 2.0f);
		}

		snprintf(str_text, sizeof(str_text), "%d", int_cells);
		draw_label(f_bar_x + f_bar_w + 14.0f, f_y + 2.0f, 1.8f, arr_white, f_ease, str_text);

		if (i + 1 == a_int_winner_id)
			draw_crown(f_panel_x + 14.0f, f_y + 1.0f, 20.0f, 14.0f, f_ease);
		int_line++;
	}

	draw_label_centered(WIDTH / 2.0f, f_panel_y + f_panel_h - 22.0f, 1.4f, arr_white,
		0.45f * f_ease, "Returning to the lobby...");
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
		// Slide down into place over the first moments
		float f_slide = d_age < 0.2 ? (float)(1.0 - d_age / 0.2) * -10.0f : 0.0f;

		char str_text[32];
		snprintf(str_text, sizeof(str_text), "Player %d left the game", int_id);
		float f_text_w = stb_easy_font_width((char *)str_text) * 2.0f;
		float f_w = f_text_w + 44.0f;
		float f_x = (WIDTH - f_w) / 2.0f;
		float f_y = 58.0f + int_row * 32.0f + f_slide;

		glColor4f(0.0f, 0.0f, 0.0f, 0.72f * f_alpha);
		draw_rrect(f_x, f_y, f_w, 26.0f, 13.0f);
		glColor4f(arr_colors[int_id - 1][0], arr_colors[int_id - 1][1],
			arr_colors[int_id - 1][2], f_alpha);
		draw_circle(f_x + 17.0f, f_y + 13.0f, 6.5f);
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
	draw_rrect((WIDTH - f_w) / 2.0f, f_y, f_w, 62.0f, 14.0f);
	glColor4f(arr_amber[0], arr_amber[1], arr_amber[2], 0.5f * f_alpha);
	glLineWidth(2.0f);
	draw_rrect_outline((WIDTH - f_w) / 2.0f, f_y, f_w, 62.0f, 14.0f);
	glLineWidth(1.0f);
	draw_label_centered(WIDTH / 2.0f, f_y + 8.0f, 3.0f, arr_amber, f_alpha, str_title);
	draw_label_centered(WIDTH / 2.0f, f_y + 40.0f, 1.5f, arr_white, 0.9f, str_sub);
}

// Advances the transition timers and rains confetti while the game is over
static void update_effects(const RenderState *a_state, float a_f_dt)
{
	// "GO!" on a fresh round: either it just started, or it restarted after
	// a game over
	if ((a_state->b_round_started && !b_prev_started) ||
		(!a_state->b_game_over && b_prev_over && a_state->b_round_started))
		f_go_timer = 1.2f;
	else if (f_go_timer > 0.0f)
		f_go_timer -= a_f_dt;

	if (a_state->int_seconds_left != int_prev_secs)
	{
		int_prev_secs = a_state->int_seconds_left;
		f_tick_age = 0.0f;
	}
	else
	{
		f_tick_age += a_f_dt;
	}

	if (a_state->b_game_over)
	{
		if (!b_prev_over)
		{
			f_end_timer = 0.0f;
			f_confetti_accum = 0.0f;
		}
		f_end_timer += a_f_dt;

		// Confetti in the winner's colour plus gold and white (or every
		// player colour for a draw)
		f_confetti_accum += a_f_dt * 70.0f;
		bool b_has_winner = (a_state->int_winner_id >= 1 &&
			a_state->int_winner_id <= MAX_PLAYERS);
		while (f_confetti_accum >= 1.0f)
		{
			f_confetti_accum -= 1.0f;
			float f_pick = rnd01();
			const float *arr_col;
			if (b_has_winner && f_pick < 0.6f)
				arr_col = arr_colors[a_state->int_winner_id - 1];
			else if (f_pick < 0.8f && b_has_winner)
				arr_col = arr_gold;
			else if (!b_has_winner)
				arr_col = arr_colors[(int)(rnd01() * MAX_PLAYERS) % MAX_PLAYERS];
			else
				arr_col = arr_white;
			spawn_particle(PART_CONFETTI, rnd_range(0.0f, WIDTH), -10.0f,
				rnd_range(-40.0f, 40.0f), rnd_range(60.0f, 160.0f),
				rnd_range(4.0f, 6.0f), rnd_range(4.0f, 7.0f), 45.0f, arr_col);
		}
	}

	b_prev_started = a_state->b_round_started;
	b_prev_over = a_state->b_game_over;
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

	// Load every PNG from the assets folder: next to where the game is run
	// from, or wherever CTF_ASSET_DIR points
	const char *str_dir = getenv("CTF_ASSET_DIR");
	if (!str_dir || str_dir[0] == '\0')
		str_dir = "assets";
	for (int i = 0; i < TEX_COUNT; i++)
		arr_tex[i] = load_texture(str_dir, arr_tex_files[i]);
}

// Letterboxed viewport: the whole game scaled uniformly to fit, centred
static int int_vp_x = 0, int_vp_y = 0, int_vp_w = WIDTH, int_vp_h = HEIGHT;

void render_set_viewport(int a_int_fb_w, int a_int_fb_h)
{
	if (a_int_fb_w <= 0 || a_int_fb_h <= 0)
		return;
	float f_scale = (float)a_int_fb_w / WIDTH;
	if ((float)a_int_fb_h / HEIGHT < f_scale)
		f_scale = (float)a_int_fb_h / HEIGHT;
	int_vp_w = (int)(WIDTH * f_scale);
	int_vp_h = (int)(HEIGHT * f_scale);
	int_vp_x = (a_int_fb_w - int_vp_w) / 2;
	int_vp_y = (a_int_fb_h - int_vp_h) / 2;
}

void render_frame(const RenderState *a_state, float a_f_dt, double a_d_now)
{
	// Bars first (the clear covers the whole window), then the game inside
	// its letterboxed viewport
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glViewport(int_vp_x, int_vp_y, int_vp_w, int_vp_h);
	glEnable(GL_SCISSOR_TEST);
	glScissor(int_vp_x, int_vp_y, int_vp_w, int_vp_h);

	// A stalled frame (window drag, breakpoint) must not fling the
	// animations forward
	if (a_f_dt > 0.1f)
		a_f_dt = 0.1f;

	glClearColor(0.07f, 0.08f, 0.12f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	update_effects(a_state, a_f_dt);
	update_players(a_state->arr_players, a_f_dt);
	update_particles(a_f_dt);

	// Back to front: backdrop, floor, squares, particles, HUD, then the
	// overlays and confetti
	draw_background();
	draw_cells(&a_state->grid, a_f_dt, a_d_now);
	draw_players(&a_state->grid, a_state->arr_players, a_state->int_own_id,
		a_state->b_round_started, a_d_now);
	draw_particles(false);
	draw_countdown_fx(a_state->int_seconds_left, a_state->b_round_started,
		a_state->b_game_over);
	draw_hud(&a_state->grid, a_state->arr_players, a_state->int_own_id,
		a_state->int_seconds_left, a_state->b_round_started, a_d_now,
		a_state->b_spectating, a_f_dt);
	draw_notices(a_state->arr_notices, a_d_now);
	if (a_state->b_spectating)
		draw_spectator_banner(a_d_now);
	if (!a_state->b_round_started)
		draw_lobby(a_state->arr_players, a_state->int_own_id, a_d_now);
	if (a_state->b_game_over)
		draw_end_screen(&a_state->grid, a_state->arr_players, a_state->int_own_id,
			a_state->int_winner_id, a_d_now);
	draw_particles(true);
}
