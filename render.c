/* ========================================================
 * @file        render.c
 * @author      Wong Shi Yu Regine | w.shiyuregine@digipen.edu
 * @co-author   Choi Meng Yew      | choi.m@digipen.edu
 * @date        04 NOV 2026
 *
 * @brief
 *
 * Implementation of render.h: draws the lobby, floor, players, HUD, notices
 * and end screen with OpenGL. Sprites and backdrops are PNGs loaded from
 * assets/ with stb_image and tinted in code, so one image serves every
 * player colour; particles and animation are code; text uses stb_easy_font.
 *
 * Course: CSD3116, Assignment: Week 1 Team Lab
 *
 * Copyright © 2026 DigiPen, All rights reserved.
 * ========================================================
 */
/* Implementation of render.h. Only the client's main (rendering) thread calls
into this file (Slide 12), always with a RenderState snapshot, so nothing
here needs the client's mutex. The visual polish (smooth gliding squares with
faces, bevelled tiles, particles, confetti, rounded HUD) is client-side only
and never touches the network or game rules. */

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "render.h"
#include "stb_easy_font.h"

// stb_image: only the PNG decoder is needed, so only that is compiled in
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

// Older GL headers (GLFW pulls in GL 1.1) lack this
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

//============ CONSTANTS AND SMALL STATE ===============

// One RGB colour per player, indexed by player ID minus 1
static const float colors[MAX_PLAYERS][3] = {
    { 0.90f, 0.20f, 0.20f },
    { 0.20f, 0.45f, 0.90f },
    { 0.20f, 0.70f, 0.30f },
    { 0.95f, 0.75f, 0.10f },
    { 0.65f, 0.30f, 0.80f }
};

// All drawing is in window pixels, origin top-left, and blending is on, so
// every colour here takes an alpha - see render_init()

static const float white[3] = { 1.0f, 1.0f, 1.0f };
static const float alarm[3] = { 1.0f, 0.35f, 0.35f };
static const float gold[3]  = { 1.0f, 0.82f, 0.25f };
static const float mint[3]  = { 0.45f, 1.00f, 0.65f };

#define PI_F 3.14159265f

// How long a newly claimed cell takes to pop in, in seconds
#define CLAIM_ANIM_SECS 0.25f

// Geometry baked into the PNGs in assets/ (see README "Art assets"): the
// disc in circle.png is 60 px across in a 64 px image; the solid part of
// soft.png is 40 px wide in a 64 px image; panel.png / ring.png have 16 px
// corners, ie a quarter of the image at each corner
#define IMG_SIZE         64.0f
#define CIRCLE_IMG_DIAM  60.0f
#define SOFT_IMG_SOLID   40.0f
#define NINE_CORNER_UV   0.25f

// How quickly displayed squares / counts chase the real values (per second)
#define GLIDE_RATE 24.0f
#define COUNT_RATE 8.0f

// Per-cell animation state: the owner last drawn, and seconds since that
// owner changed. Only used by the main (rendering) thread
static int shown_owner[GRID_ROWS * GRID_COLS];
static float claim_age[GRID_ROWS * GRID_COLS];

// Per-player display state: where the square is drawn (it glides toward the
// real position), where it last was, which way it faces, a hop timer for
// squash-and-stretch, and the smoothed cell count shown in the HUD
static bool was_active[MAX_PLAYERS];
static float disp_x[MAX_PLAYERS];
static float disp_y[MAX_PLAYERS];
static int last_x[MAX_PLAYERS];
static int last_y[MAX_PLAYERS];
static float face_x[MAX_PLAYERS];
static float face_y[MAX_PLAYERS];
static float hop_t[MAX_PLAYERS];
static float disp_count[MAX_PLAYERS];

// Banner / transition timers
static bool prev_started = false;
static bool prev_over = false;
static float go_timer = 0.0f;
static float end_timer = 0.0f;
static int prev_secs = -1;
static float tick_age = 1.0f;
static float confetti_accum = 0.0f;

//============ PARTICLES ===============

typedef enum { PART_SPARK, PART_PUFF, PART_CONFETTI } PartKind;

typedef struct {
    float x, y, vx, vy;
    float life, max_life;
    float size, gravity, rot, vrot;
    float r, g, b;
    PartKind kind;
} Particle;

#define MAX_PARTICLES 512
static Particle particles[MAX_PARTICLES];

// Random float in [0, 1) - visual effects only, so plain rand() is enough
static float rnd01(void) {
    return rand() / ((float)RAND_MAX + 1.0f);
}

static float rnd_range(float lo, float hi) {
    return lo + (hi - lo) * rnd01();
}

static void spawn_particle(PartKind kind, float x, float y,
    float vx, float vy, float life, float size,
    float gravity, const float *rgb) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        if (p->life > 0.0f)
            continue;
        p->x = x;
        p->y = y;
        p->vx = vx;
        p->vy = vy;
        p->life = p->max_life = life;
        p->size = size;
        p->gravity = gravity;
        p->rot = rnd_range(0.0f, 360.0f);
        p->vrot = rnd_range(-360.0f, 360.0f);
        p->r = rgb[0];
        p->g = rgb[1];
        p->b = rgb[2];
        p->kind = kind;
        return;
    }
    // Pool full: drop the particle, nobody will miss one
}

static void update_particles(float dt) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        if (p->life <= 0.0f)
            continue;
        p->life -= dt;
        p->vy += p->gravity * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->rot += p->vrot * dt;
        if (p->kind != PART_CONFETTI) {
            // Sparks and puffs slow down; confetti flutters on
            p->vx *= (1.0f - 2.5f * dt > 0.0f) ? (1.0f - 2.5f * dt) : 0.0f;
            p->vy *= (1.0f - 2.5f * dt > 0.0f) ? (1.0f - 2.5f * dt) : 0.0f;
        }
    }
}

//============ DRAWING HELPERS ===============

/**
 * @brief Draws text with stb_easy_font at (x, y), scaled by scale, in the
 *        current colour
 */
static void draw_text(float x, float y, float scale, const char *text) {
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

/**
 * @brief Fills a rectangle with the current colour
 */
static void draw_rect(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x, y + h);
        glVertex2f(x + w, y + h);
        glVertex2f(x + w, y);
    glEnd();
}

//============ TEXTURES ===============
// The art lives in the assets folder as PNGs (see README "Art assets"),
// not in this file. Most are white or grey on purpose: every draw call below
// sets the colour first (glColor), and OpenGL multiplies it into the image,
// so one picture serves all five player colours and any alpha fade.

typedef enum {
    TEX_CIRCLE, TEX_SPARK, TEX_SOFT, TEX_PANEL, TEX_RING, TEX_TILE,
    TEX_PLAYER, TEX_EYE, TEX_PUPIL, TEX_CONFETTI, TEX_FLOOR, TEX_CROWN,
    TEX_BACKGROUND, TEX_VIGNETTE, TEX_HUD_BAR, TEX_COUNT
} TexId;

// File names, indexed by TexId
static const char *texture_files[TEX_COUNT] = {
    "circle.png", "spark.png", "soft.png", "panel.png", "ring.png", "tile.png",
    "player.png", "eye.png", "pupil.png", "confetti.png", "floor.png", "crown.png",
    "background.png", "vignette.png", "hud_bar.png"
};

static GLuint textures[TEX_COUNT];

/**
 * @brief Loads one PNG into an OpenGL texture. A missing or unreadable file is
 *        reported and replaced by a plain white pixel, so the game still runs
 */
static GLuint load_texture(const char *dir, const char *file) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);

    int w = 0, h = 0;
    int comp = 0;
    unsigned char *px = stbi_load(path, &w, &h, &comp, 4);
    static unsigned char white_px[4] = { 255, 255, 255, 255 };
    if (!px) {
        fprintf(stderr, "render: cannot load %s (run from the repository root, or "
            "set CTF_ASSET_DIR) - using a blank placeholder\n", path);
        px = white_px;
        w = h = 1;
    }

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
        GL_UNSIGNED_BYTE, px);
    if (px != white_px)
        stbi_image_free(px);
    return id;
}

/**
 * @brief Draws a texture over a rectangle, tinted by the current colour; the
 *        u/v arguments pick the part of the image (swap them to flip it)
 */
static void draw_image_uv(TexId tex, float x, float y, float w,
    float h, float u0, float v0, float u1, float v1) {
    glBindTexture(GL_TEXTURE_2D, textures[tex]);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex2f(x, y);
        glTexCoord2f(u0, v1); glVertex2f(x, y + h);
        glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
        glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

static void draw_image(TexId tex, float x, float y, float w, float h) {
    draw_image_uv(tex, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f);
}

/**
 * @brief Stretches a 64x64 image that has 16 px rounded corners over any size
 *        without distorting the corners (a "nine-slice"): the corners are drawn at
 *        corner pixels, the edges and middle stretch
 */
static void draw_nine(TexId tex, float x, float y, float w,
    float h, float corner) {
    float max = (w < h ? w : h) / 2.0f;
    float corner_px = corner < max ? corner : max;
    float xs[4] = { x, x + corner_px, x + w - corner_px, x + w };
    float ys[4] = { y, y + corner_px, y + h - corner_px, y + h };
    static const float uv[4] = { 0.0f, NINE_CORNER_UV, 1.0f - NINE_CORNER_UV, 1.0f };

    glBindTexture(GL_TEXTURE_2D, textures[tex]);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            glTexCoord2f(uv[c], uv[r]);         glVertex2f(xs[c], ys[r]);
            glTexCoord2f(uv[c], uv[r + 1]);     glVertex2f(xs[c], ys[r + 1]);
            glTexCoord2f(uv[c + 1], uv[r + 1]); glVertex2f(xs[c + 1], ys[r + 1]);
            glTexCoord2f(uv[c + 1], uv[r]);     glVertex2f(xs[c + 1], ys[r]);
        }
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

//============ SHAPES (all built from the textures above) ===============

/**
 * @brief Rectangle with a different RGBA colour at each corner (top-left,
 *        top-right, bottom-right, bottom-left) - used for the animated time bar
 */
static void draw_quad_grad(float x, float y, float w, float h,
    const float *tl, const float *tr, const float *br, const float *bl) {
    glBegin(GL_QUADS);
        glColor4fv(tl); glVertex2f(x, y);
        glColor4fv(bl); glVertex2f(x, y + h);
        glColor4fv(br); glVertex2f(x + w, y + h);
        glColor4fv(tr); glVertex2f(x + w, y);
    glEnd();
}

static void draw_hgrad(float x, float y, float w, float h,
    const float *left, const float *right) {
    draw_quad_grad(x, y, w, h, left, right, right, left);
}

/**
 * @brief Filled circle in the current colour. The image's disc has a 30/64 radius
 */
static void draw_circle(float cx, float cy, float r) {
    float size = r * 2.0f * IMG_SIZE / CIRCLE_IMG_DIAM;
    draw_image(TEX_CIRCLE, cx - size / 2.0f, cy - size / 2.0f, size, size);
}

/**
 * @brief Rounded rectangle in the current colour, corner radius r
 */
static void draw_rrect(float x, float y, float w, float h, float r) {
    draw_nine(TEX_PANEL, x, y, w, h, r);
}

/**
 * @brief Its outline, about 2 px thick. The ring's corners never get tighter than
 *        10 px, or the line would thin out
 */
static void draw_rrect_outline(float x, float y, float w, float h, float r) {
    float corner_px = r < 10.0f ? 10.0f : (r > 22.0f ? 22.0f : r);
    draw_nine(TEX_RING, x, y, w, h, corner_px);
}

/**
 * @brief Blurred rounded blob around the given box (the image's solid part is
 *        40/64 of its width): tinted black it is a shadow, white a glow
 */
static void draw_soft(float x, float y, float w, float h) {
    float k = IMG_SIZE / SOFT_IMG_SOLID;
    float soft_w = w * k;
    float soft_h = h * k;
    draw_image(TEX_SOFT, x + w / 2.0f - soft_w / 2.0f,
        y + h / 2.0f - soft_h / 2.0f, soft_w, soft_h);
}

/**
 * @brief Gold crown, x/y = top-left, with a faint shadow so it reads on any
 *        colour. Not tinted - the image already has its colours
 */
static void draw_crown(float x, float y, float w, float h, float alpha) {
    glColor4f(0.0f, 0.0f, 0.0f, 0.35f * alpha);
    draw_image(TEX_CROWN, x + 1.0f, y + 1.0f, w, h);
    glColor4f(1.0f, 1.0f, 1.0f, alpha);
    draw_image(TEX_CROWN, x, y, w, h);
}


/**
 * @brief Text with a soft drop shadow, so it reads on any background
 */
static void draw_label(float x, float y, float scale,
    const float *rgb, float alpha, const char *text) {
    glColor4f(0.0f, 0.0f, 0.0f, alpha * 0.6f);
    draw_text(x + scale, y + scale, scale, text);
    glColor4f(rgb[0], rgb[1], rgb[2], alpha);
    draw_text(x, y, scale, text);
}

/**
 * @brief Same as draw_label(), but centred horizontally on cx
 */
static void draw_label_centered(float cx, float y, float scale,
    const float *rgb, float alpha, const char *text) {
    float width = stb_easy_font_width((char *)text) * scale;
    draw_label(cx - width / 2.0f, y, scale, rgb,
        alpha, text);
}

/**
 * @brief Text whose glyph middle sits on cy (a glyph is about 7 units tall)
 */
static void draw_label_middle(float cx, float cy, float scale,
    const float *rgb, float alpha, const char *text) {
    draw_label_centered(cx, cy - 3.5f * scale, scale,
        rgb, alpha, text);
}

/**
 * @brief "COLOR THE FLOOR" with every letter in the next player colour, bobbing
 *        in a wave
 */
static void draw_rainbow_title(float cx, float y, float scale,
    float alpha, double elapsed) {
    const char *title = "COLOR THE FLOOR";
    float total = stb_easy_font_width((char *)title) * scale;
    float x = cx - total / 2.0f;
    int color = 0;

    for (int i = 0; title[i] != '\0'; i++) {
        char prefix[32];
        char one[2] = { title[i], '\0' };
        snprintf(prefix, sizeof(prefix), "%.*s", i, title);
        float off = stb_easy_font_width(prefix) * scale;
        if (title[i] == ' ')
            continue;
        float bob = sinf((float)elapsed * 3.0f + i * 0.5f) * 4.0f;
        draw_label(x + off, y + bob, scale,
            colors[color % MAX_PLAYERS], alpha, one);
        color++;
    }
}

//============ SCENE ===============

/**
 * @brief Dark gradient backdrop; the floor tiles are drawn on top of it
 */
static void draw_background(void) {
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    draw_image(TEX_BACKGROUND, 0, 0, WIDTH, HEIGHT);
}

/**
 * @brief Checkerboard floor, grid lines, every claimed cell as a bevelled tile in
 *        its owner's colour, and a vignette to frame it. A tile that has just
 *        changed hands grows in from its centre with a brief white flash and
 *        throws off sparks
 */
static void draw_cells(const Grid *grid, float dt, double elapsed) {
    // Unclaimed floor: faint checkerboard with a slow shimmer
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            float shimmer = 0.012f * sinf((float)elapsed * 1.2f + (col + row) * 0.6f);
            float a = ((col + row) & 1) ? 0.055f : 0.020f;
            glColor4f(0.75f, 0.85f, 1.0f, a + shimmer);
            draw_image(TEX_FLOOR, col * SQ_WIDTH, PLAY_TOP + row * SQ_WIDTH,
                SQ_WIDTH, SQ_WIDTH);
        }
    }

    glColor4f(1.0f, 1.0f, 1.0f, 0.06f);
    glBegin(GL_LINES);
    for (int col = 0; col <= GRID_COLS; col++) {
        glVertex2f(col * SQ_WIDTH, PLAY_TOP);
        glVertex2f(col * SQ_WIDTH, PLAY_TOP + PLAY_HEIGHT);
    }
    for (int row = 0; row <= GRID_ROWS; row++) {
        glVertex2f(0, PLAY_TOP + row * SQ_WIDTH);
        glVertex2f(WIDTH, PLAY_TOP + row * SQ_WIDTH);
    }
    glEnd();

    int bursts = 0;
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            int idx = row * GRID_COLS + col;
            int owner = grid->arr_owner[idx];

            // Restart the pop-in whenever the owner changes
            if (owner != shown_owner[idx]) {
                shown_owner[idx] = owner;
                claim_age[idx] = 0.0f;

                // Sparks from the new tile's centre. Capped per frame so a
                // late joiner receiving the whole grid does not flood the pool
                if (owner >= 1 && owner <= MAX_PLAYERS && bursts < 10) {
                    bursts++;
                    float cx = col * SQ_WIDTH + SQ_WIDTH / 2.0f;
                    float cy = PLAY_TOP + row * SQ_WIDTH + SQ_WIDTH / 2.0f;
                    for (int k = 0; k < 6; k++) {
                        float ang = rnd_range(0.0f, 2.0f * PI_F);
                        float spd = rnd_range(60.0f, 170.0f);
                        spawn_particle(PART_SPARK, cx, cy,
                            cosf(ang) * spd, sinf(ang) * spd,
                            rnd_range(0.35f, 0.7f), rnd_range(2.0f, 3.5f), 0.0f,
                            k & 1 ? white : colors[owner - 1]);
                    }
                }
            } else if (claim_age[idx] < CLAIM_ANIM_SECS) {
                claim_age[idx] += dt;
            }

            if (owner < 1 || owner > MAX_PLAYERS)
                continue;

            float t = claim_age[idx] / CLAIM_ANIM_SECS;
            if (t > 1.0f)
                t = 1.0f;
            // Ease out: fast at first, settling at the end
            float ease = 1.0f - (1.0f - t) * (1.0f - t);
            float inset = 2.0f + (1.0f - ease) * (SQ_WIDTH / 2 - 2);
            float flash = (1.0f - ease) * 0.6f;

            const float *tint = colors[owner - 1];
            float x = col * SQ_WIDTH + inset;
            float y = PLAY_TOP + row * SQ_WIDTH + inset;
            float s = SQ_WIDTH - 2.0f * inset;

            // The tile image already has its bevel, highlight and gloss baked
            // in (assets/tile.png); here it is just tinted with the owner's
            // colour, brightened by the claim flash
            glColor4f(tint[0] + flash * (1.0f - tint[0]),
                tint[1] + flash * (1.0f - tint[1]),
                tint[2] + flash * (1.0f - tint[2]), 1.0f);
            draw_image(TEX_TILE, x, y, s, s);
        }
    }

    // Vignette: darkens the edges so the eye stays on the action
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    draw_image_uv(TEX_VIGNETTE, 0, PLAY_TOP, 90, PLAY_HEIGHT, 0.0f, 0.0f, 1.0f, 1.0f);
    draw_image_uv(TEX_VIGNETTE, WIDTH - 90, PLAY_TOP, 90, PLAY_HEIGHT, 1.0f, 0.0f, 0.0f, 1.0f);
}

/**
 * @brief Moves each player's drawn position toward the real one, tracking facing
 *        direction, hop timers and dust puffs for the movement
 */
static void update_players(const Player players[], float dt) {
    float follow = 1.0f - expf(-GLIDE_RATE * dt);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].b_active) {
            was_active[i] = false;
            continue;
        }

        float tx = (float)players[i].x;
        float ty = (float)players[i].y;

        if (!was_active[i]) {
            // New (or re-spawned) player: appear in place, facing down
            was_active[i] = true;
            disp_x[i] = tx;
            disp_y[i] = ty;
            last_x[i] = players[i].x;
            last_y[i] = players[i].y;
            face_x[i] = 0.0f;
            face_y[i] = 1.0f;
            hop_t[i] = 1.0f;
            // Spawn flourish
            for (int k = 0; k < 12; k++) {
                float ang = rnd_range(0.0f, 2.0f * PI_F);
                spawn_particle(PART_SPARK, tx + SQ_WIDTH / 2.0f, ty + SQ_WIDTH / 2.0f,
                    cosf(ang) * 120.0f, sinf(ang) * 120.0f, 0.6f, 3.0f, 0.0f,
                    colors[i]);
            }
            continue;
        }

        if (players[i].x != last_x[i] || players[i].y != last_y[i]) {
            float dx = (float)(players[i].x - last_x[i]);
            float dy = (float)(players[i].y - last_y[i]);
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.0f) {
                face_x[i] = dx / len;
                face_y[i] = dy / len;
            }
            hop_t[i] = 1.0f;

            // Dust puffs where the square just was
            for (int k = 0; k < 3; k++) {
                spawn_particle(PART_PUFF,
                    disp_x[i] + SQ_WIDTH / 2.0f + rnd_range(-10.0f, 10.0f),
                    disp_y[i] + SQ_WIDTH - 6.0f + rnd_range(-4.0f, 4.0f),
                    rnd_range(-25.0f, 25.0f), rnd_range(-25.0f, 5.0f),
                    rnd_range(0.3f, 0.5f), rnd_range(4.0f, 7.0f), 0.0f,
                    colors[i]);
            }
            last_x[i] = players[i].x;
            last_y[i] = players[i].y;
        }

        disp_x[i] += (tx - disp_x[i]) * follow;
        disp_y[i] += (ty - disp_y[i]) * follow;
        if (hop_t[i] > 0.0f) {
            hop_t[i] -= dt * 5.0f;
            if (hop_t[i] < 0.0f)
                hop_t[i] = 0.0f;
        }
    }
}

/**
 * @brief Each active player's square: soft shadow, rounded gradient body, eyes
 *        that look where it is heading (and blink), ID badge and outline. The local
 *        player gets a pulsing glow ring and a YOU marker; whoever leads gets a crown
 */
static void draw_players(const Grid *grid, const Player players[],
    int own_id, bool started, double elapsed) {
    float pulse = 0.5f + 0.5f * sinf((float)elapsed * 6.0f);
    int leader = started ? grid_winner(grid, MAX_PLAYERS) : 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].b_active)
            continue;

        bool own = (i + 1 == own_id);
        const float *tint = colors[i];
        float hop = hop_t[i];

        // Squash on landing: wider and shorter, anchored to the floor
        float w = SQ_WIDTH * (1.0f + 0.14f * hop);
        float h = SQ_WIDTH * (1.0f - 0.14f * hop);
        float bob = sinf((float)elapsed * 3.0f + i * 1.7f) * 1.5f;
        float x = disp_x[i] + (SQ_WIDTH - w) / 2.0f;
        float y = disp_y[i] + (SQ_WIDTH - h) + bob - 2.0f;

        // Soft shadow on the floor
        glColor4f(0.0f, 0.0f, 0.0f, 0.45f);
        draw_soft(disp_x[i] + 3.0f, disp_y[i] + 6.0f, SQ_WIDTH, SQ_WIDTH - 2.0f);

        // Glow for the local player
        if (own) {
            glColor4f(1.0f, 1.0f, 1.0f, 0.30f + 0.30f * pulse);
            draw_soft(x - 1.0f, y - 1.0f, w + 2.0f, h + 2.0f);
        }

        // Body: assets/player.png (gradient, gloss and darker lip), tinted
        float k = 1.0f / 0.9f;
        glColor4f(tint[0] * k > 1.0f ? 1.0f : tint[0] * k,
            tint[1] * k > 1.0f ? 1.0f : tint[1] * k,
            tint[2] * k > 1.0f ? 1.0f : tint[2] * k, 1.0f);
        draw_image(TEX_PLAYER, x, y, w, h);

        if (own) {
            glColor4f(1.0f, 1.0f, 1.0f, 0.75f + 0.25f * pulse);
        } else {
            glColor4f(1.0f, 1.0f, 1.0f, 0.45f);
        }
        draw_rrect_outline(x, y, w, h - 3.0f, 10.0f);

        // Eyes: whites, pupils shifted toward the facing direction, and a
        // quick blink every few seconds (offset per player)
        float ey = y + (h - 3.0f) * 0.38f;
        bool blink = fmodf((float)elapsed + i * 1.3f, 3.6f) < 0.12f;
        for (int e = 0; e < 2; e++) {
            float ex = x + w * (e == 0 ? 0.32f : 0.68f);
            if (blink) {
                glColor4f(0.08f, 0.08f, 0.12f, 0.9f);
                draw_rect(ex - 5.0f, ey - 1.0f, 10.0f, 2.0f);
                continue;
            }
            // assets/eye.png (white) and assets/pupil.png (dark, with its
            // highlight painted in), the pupil shifted toward the facing
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            draw_image(TEX_EYE, ex - 7.2f, ey - 7.2f, 14.4f, 14.4f);
            draw_image(TEX_PUPIL, ex + face_x[i] * 2.4f - 4.4f,
                ey + face_y[i] * 2.4f - 4.4f, 8.8f, 8.8f);
        }

        // ID badge under the eyes
        char id[2];
        snprintf(id, sizeof(id), "%d", i + 1);
        draw_label_centered(x + w / 2.0f, y + h - 17.0f, 1.4f, white, 0.95f, id);

        float top = y;
        if (leader == i + 1) {
            draw_crown(x + w / 2.0f - 11.0f, y - 13.0f + 2.0f * pulse, 22.0f, 15.0f, 1.0f);
            top -= 14.0f;
        }

        if (own) {
            // Above the square, or below it when there is not enough room
            // above to clear the top HUD margin (PLAY_TOP)
            bool room_above = (top - PLAY_TOP) > 18.0f;
            float marker_y = room_above ? top - 17.0f : y + SQ_WIDTH + 5.0f;
            draw_label_centered(x + w / 2.0f, marker_y, 1.5f, white, 1.0f, "YOU");
        }
    }
}

/**
 * @brief Sparks, dust puffs (confetti false) or confetti (true)
 */
static void draw_particles(bool confetti) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const Particle *p = &particles[i];
        if (p->life <= 0.0f || (p->kind == PART_CONFETTI) != confetti)
            continue;

        float k = p->life / p->max_life;
        if (p->kind == PART_CONFETTI) {
            // Flutter: the sheet's apparent width oscillates as it spins
            float flip = fabsf(cosf(p->rot * PI_F / 180.0f));
            float a = k < 0.2f ? k / 0.2f : 1.0f;
            glColor4f(p->r, p->g, p->b, a);
            glPushMatrix();
            glTranslatef(p->x, p->y, 0.0f);
            glRotatef(p->rot * 0.5f, 0.0f, 0.0f, 1.0f);
            draw_image(TEX_CONFETTI, -p->size * flip, -p->size * 0.5f,
                p->size * 2.0f * flip, p->size);
            glPopMatrix();
        } else if (p->kind == PART_PUFF) {
            float r = p->size * (1.4f - 0.6f * k);
            glColor4f(p->r, p->g, p->b, 0.35f * k);
            draw_image(TEX_SPARK, p->x - r * 1.6f, p->y - r * 1.6f, r * 3.2f, r * 3.2f);
        } else {
            float r = p->size * (0.4f + 0.6f * k);
            glColor4f(p->r, p->g, p->b, k);
            draw_image(TEX_SPARK, p->x - r * 1.6f, p->y - r * 1.6f, r * 3.2f, r * 3.2f);
        }
    }
}

/**
 * @brief Top strip (who you are, countdown, time bar) and bottom strip
 *        (territory bar, per-player cell counts)
 */
static void draw_hud(const Grid *grid, const Player players[],
    int own_id, int secs, bool started, double elapsed,
    bool spectating, float dt) {
    char text[32];
    bool low = started && secs <= 10;
    float blink = 0.65f + 0.35f * sinf((float)elapsed * 8.0f);

    // Top strip - exactly HUD_MARGIN_TOP tall (solid bar + the thin time
    // bar below it), so it can never bleed into the playable grid
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    draw_image(TEX_HUD_BAR, 0, 0, WIDTH, HUD_MARGIN_TOP - 4);

    if (own_id >= 1 && own_id <= MAX_PLAYERS) {
        const float *tint = colors[own_id - 1];
        glColor4f(0.0f, 0.0f, 0.0f, 0.35f);
        draw_rrect(8, 7, 142, 32, 16.0f);
        glColor4f(tint[0], tint[1], tint[2], 1.0f);
        draw_circle(25, 23, 10.0f);
        glColor4f(1.0f, 1.0f, 1.0f, 0.35f);
        draw_circle(22, 20, 3.5f);
        snprintf(text, sizeof(text), "Player %d", own_id);
        draw_label(42, 15, 2.0f, white, 1.0f, text);
    } else if (spectating) {
        draw_label(12, 14, 2.5f, white, 0.9f, "Spectator");
    } else {
        draw_label(12, 14, 2.5f, white, 0.7f, "Connecting...");
    }

    // Timer in a pill
    snprintf(text, sizeof(text), "%d:%02d", secs / 60, secs % 60);
    glColor4f(0.0f, 0.0f, 0.0f, 0.4f);
    draw_rrect(WIDTH / 2.0f - 52, 5, 104, 36, 18.0f);
    if (low)
        glColor4f(alarm[0], alarm[1], alarm[2], 0.5f * blink);
    else
        glColor4f(1.0f, 1.0f, 1.0f, 0.25f);
    draw_rrect_outline(WIDTH / 2.0f - 52, 5, 104, 36, 18.0f);
    draw_label_middle(WIDTH / 2.0f, 23, 3.4f, low ? alarm : white,
        low ? blink : 1.0f, text);

    float title_w = stb_easy_font_width((char *)"COLOR THE FLOOR") * 1.5f;
    draw_label(WIDTH - 14 - title_w, 18, 1.5f, white, 0.45f, "COLOR THE FLOOR");

    // Time bar: drains as the round runs out
    float frac = started ? (float)secs / ROUND_SECS : 1.0f;
    glColor4f(1.0f, 1.0f, 1.0f, 0.1f);
    draw_rect(0, HUD_MARGIN_TOP - 4, WIDTH, 4);
    if (low) {
        glColor4f(alarm[0], alarm[1], alarm[2], blink);
        draw_rect(0, HUD_MARGIN_TOP - 4, WIDTH * frac, 4);
    } else {
        // Mint-to-white sweep
        const float l[4] = { mint[0], mint[1], mint[2], 0.9f };
        const float r[4] = { 1.0f, 1.0f, 1.0f, 0.9f };
        draw_hgrad(0, HUD_MARGIN_TOP - 4, WIDTH * frac, 4, l, r);
    }

    // Count each player's cells once, for both the bar and the labels, and
    // ease the displayed value toward it so bars slide instead of jumping
    int counts[MAX_PLAYERS];
    float count_follow = 1.0f - expf(-COUNT_RATE * dt);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        counts[i] = grid_count_owned(grid, i + 1);
        disp_count[i] += (counts[i] - disp_count[i]) * count_follow;
    }

    // Bottom strip - exactly HUD_MARGIN_BOTTOM tall, for the same reason
    // as the top strip above
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    draw_image_uv(TEX_HUD_BAR, 0, HEIGHT - HUD_MARGIN_BOTTOM, WIDTH, HUD_MARGIN_BOTTOM,
        0.0f, 1.0f, 1.0f, 0.0f);

    // Territory bar: each player's share of the whole floor
    float bar_x = 10.0f;
    float bar_w = WIDTH - 20.0f;
    float bar_y = HEIGHT - HUD_MARGIN_BOTTOM + 5.0f;
    glColor4f(1.0f, 1.0f, 1.0f, 0.10f);
    draw_rrect(bar_x, bar_y, bar_w, 10, 5.0f);
    float cursor = bar_x;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        float seg = bar_w * disp_count[i] / (GRID_COLS * GRID_ROWS);
        if (seg < 0.5f)
            continue;
        glColor3fv(colors[i]);
        draw_rect(cursor, bar_y, seg, 10);
        glColor4f(1.0f, 1.0f, 1.0f, 0.25f);
        draw_rect(cursor, bar_y, seg, 3);
        cursor += seg;
    }

    // Per-player label: colour swatch + cells owned
    int slot = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].b_active)
            continue;

        float x = 10.0f + slot * 150.0f;
        float y = HEIGHT - HUD_MARGIN_BOTTOM + 22.0f;
        bool own = (i + 1 == own_id);
        if (own) {
            glColor4f(1.0f, 1.0f, 1.0f, 0.12f);
            draw_rrect(x - 4, y - 3, 130, 20, 8.0f);
        }
        glColor3fv(colors[i]);
        draw_circle(x + 7, y + 6, 6.5f);
        glColor4f(1.0f, 1.0f, 1.0f, 0.35f);
        draw_circle(x + 5, y + 4, 2.0f);
        snprintf(text, sizeof(text), "P%d  %d", i + 1, counts[i]);
        draw_label(x + 20.0f, y, 1.7f, white, 1.0f, text);
        slot++;
    }
}

/**
 * @brief Big faded number that pulses on each of the last five seconds, and a
 *        "GO!" that zooms out when a round starts
 */
static void draw_countdown_fx(int secs, bool started, bool over) {
    if (started && !over && secs >= 1 && secs <= 5) {
        char n[2];
        snprintf(n, sizeof(n), "%d", secs);
        float k = tick_age < 1.0f ? tick_age : 1.0f;
        float scale = 16.0f + 8.0f * (1.0f - k);
        draw_label_middle(WIDTH / 2.0f, HEIGHT / 2.0f, scale, alarm,
            0.30f * (1.0f - k), n);
    }

    if (go_timer > 0.0f) {
        float k = go_timer / 1.2f;
        float scale = 8.0f + 6.0f * (1.0f - k);
        draw_label_middle(WIDTH / 2.0f, HEIGHT / 2.0f - 20.0f, scale, mint,
            k > 1.0f ? 1.0f : k, "GO!");
    }
}

/**
 * @brief ID of the player who may start the round: the lowest active ID, or 0 if
 *        nobody has joined. Declared in render.h so client.c and this file agree
 *        with the server, which applies the same rule
 */
int render_host_id(const Player players[]) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].b_active)
            return i + 1;
    }
    return 0;
}

/**
 * @brief Lobby shown until the host starts the round: the title, one card per
 *        slot (joined players in their colour, empty slots dashed), and a prompt
 *        that depends on who you are
 */
static void draw_lobby(const Player players[], int own_id, double elapsed) {
    int host = render_host_id(players);
    int joined = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        joined += players[i].b_active ? 1 : 0;

    const float panel_w = 560.0f;
    const float panel_h = 300.0f;
    float px = (WIDTH - panel_w) / 2.0f;
    float py = 135.0f;

    glColor4f(0.0f, 0.0f, 0.0f, 0.68f);
    draw_rrect(px, py, panel_w, panel_h, 22.0f);
    glColor4f(1.0f, 1.0f, 1.0f, 0.15f);
    draw_rrect_outline(px, py, panel_w, panel_h, 22.0f);

    draw_rainbow_title(WIDTH / 2.0f, py + 22.0f, 4.5f, 1.0f, elapsed);
    draw_label_centered(WIDTH / 2.0f, py + 70.0f, 2.0f, white, 0.6f, "LOBBY");

    // Slot cards
    const float card = 80.0f;
    const float gap = 14.0f;
    float row_w = MAX_PLAYERS * card + (MAX_PLAYERS - 1) * gap;
    float left_x = (WIDTH - row_w) / 2.0f;
    float top_y = py + 105.0f;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        float x = left_x + i * (card + gap);
        char text[16];
        if (players[i].b_active) {
            const float *tint = colors[i];
            float hop = sinf((float)elapsed * 3.0f + i) * 2.0f;
            glColor4f(0.0f, 0.0f, 0.0f, 0.3f);
            draw_rrect(x + 2.0f, top_y + 4.0f, card, card - 14.0f, 12.0f);
            glColor4f(tint[0] * 0.65f, tint[1] * 0.65f, tint[2] * 0.65f, 1.0f);
            draw_rrect(x, top_y + hop + 2.0f, card, card - 14.0f, 12.0f);
            glColor4f(tint[0], tint[1], tint[2], 1.0f);
            draw_rrect(x, top_y + hop, card, card - 17.0f, 12.0f);
            glColor4f(1.0f, 1.0f, 1.0f, 0.2f);
            draw_rrect(x + 5.0f, top_y + hop + 4.0f, card - 10.0f, 18.0f, 8.0f);
            if (i + 1 == own_id) {
                glColor4f(1.0f, 1.0f, 1.0f, 0.8f);
                draw_rrect_outline(x, top_y + hop, card, card - 17.0f, 12.0f);
            }
            // Eyes
            for (int e = 0; e < 2; e++) {
                float ex = x + card * (e == 0 ? 0.3f : 0.7f);
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                draw_circle(ex, top_y + hop + 24.0f, 7.0f);
                glColor4f(0.08f, 0.08f, 0.12f, 1.0f);
                draw_circle(ex, top_y + hop + 26.0f, 3.4f);
            }
            snprintf(text, sizeof(text), "P%d", i + 1);
            draw_label_centered(x + card / 2.0f, top_y + hop + 40.0f, 2.0f,
                white, 1.0f, text);
            if (i + 1 == host)
                draw_crown(x + card / 2.0f - 12.0f, top_y - 16.0f + hop, 24.0f, 16.0f, 1.0f);
            if (i + 1 == own_id)
                draw_label_centered(x + card / 2.0f, top_y + card - 10.0f, 1.5f,
                    white, 1.0f, "YOU");
            else if (i + 1 == host)
                draw_label_centered(x + card / 2.0f, top_y + card - 10.0f, 1.5f,
                    gold, 1.0f, "HOST");
        } else {
            glColor4f(1.0f, 1.0f, 1.0f, 0.07f);
            draw_rrect(x, top_y, card, card - 17.0f, 12.0f);
            glColor4f(1.0f, 1.0f, 1.0f, 0.22f);
            draw_rrect_outline(x, top_y, card, card - 17.0f, 12.0f);
            draw_label_middle(x + card / 2.0f, top_y + (card - 17.0f) / 2.0f, 1.5f,
                white, 0.3f, "open");
        }
    }

    // Prompt
    float pulse = 0.5f + 0.5f * sinf((float)elapsed * 4.0f);
    float by = py + panel_h - 62.0f;
    bool is_host = (own_id >= 1 && own_id == host);
    if (is_host && joined >= 2) {
        glColor4f(mint[0], mint[1], mint[2], 0.18f + 0.18f * pulse);
        draw_rrect(WIDTH / 2.0f - 190.0f, by, 380.0f, 40.0f, 20.0f);
        glColor4f(mint[0], mint[1], mint[2], 0.9f);
        draw_rrect_outline(WIDTH / 2.0f - 190.0f, by, 380.0f, 40.0f, 20.0f);
        draw_label_middle(WIDTH / 2.0f, by + 20.0f, 2.2f, mint, 1.0f,
            "Press ENTER to start");
    } else if (is_host) {
        draw_label_middle(WIDTH / 2.0f, by + 20.0f, 2.0f, white,
            0.5f + 0.4f * pulse, "Waiting for another player...");
    } else if (own_id >= 1) {
        char wait[48];
        snprintf(wait, sizeof(wait), "Waiting for Player %d to start...", host);
        draw_label_middle(WIDTH / 2.0f, by + 20.0f, 2.0f, white,
            0.5f + 0.4f * pulse, wait);
    } else {
        draw_label_middle(WIDTH / 2.0f, by + 20.0f, 2.0f, white,
            0.5f + 0.4f * pulse, "Connecting...");
    }
    draw_label_centered(WIDTH / 2.0f, py + panel_h - 16.0f, 1.2f, white, 0.35f,
        "Arrow keys to move - warm up while you wait");
}

/**
 * @brief Dims the floor and shows the result and final cell counts on a panel,
 *        sliding in; the winner is crowned and each score has a proportional bar
 */
static void draw_end_screen(const Grid *grid, const Player players[],
    int own_id, int winner_id, double elapsed) {
    float k = end_timer * 3.0f;
    if (k > 1.0f)
        k = 1.0f;
    float ease = 1.0f - (1.0f - k) * (1.0f - k);

    glColor4f(0.0f, 0.0f, 0.0f, 0.70f * ease);
    draw_rect(0, 0, WIDTH, HEIGHT);

    const float panel_w = 520.0f;
    const float panel_h = 310.0f;
    float panel_x = (WIDTH - panel_w) / 2.0f;
    float panel_y = 145.0f + (1.0f - ease) * 50.0f;
    bool has_winner = (winner_id >= 1 && winner_id <= MAX_PLAYERS);
    const float *accent = has_winner ? colors[winner_id - 1] : white;

    // Panel with a coloured glow behind it
    glColor4f(accent[0], accent[1], accent[2], 0.16f * ease);
    draw_rrect(panel_x - 8, panel_y - 8, panel_w + 16, panel_h + 16, 26.0f);
    glColor4f(0.10f, 0.11f, 0.17f, 0.97f * ease);
    draw_rrect(panel_x, panel_y, panel_w, panel_h, 20.0f);
    glColor4f(accent[0], accent[1], accent[2], ease);
    draw_rrect(panel_x, panel_y, panel_w, 12.0f, 6.0f);
    glColor4f(accent[0], accent[1], accent[2], 0.6f * ease);
    draw_rrect_outline(panel_x, panel_y, panel_w, panel_h, 20.0f);

    if (has_winner)
        draw_crown(WIDTH / 2.0f - 20.0f, panel_y + 24.0f + 3.0f * sinf((float)elapsed * 4.0f),
            40.0f, 28.0f, ease);

    char text[64];
    if (!has_winner)
        snprintf(text, sizeof(text), "DRAW!");
    else if (winner_id == own_id)
        snprintf(text, sizeof(text), "YOU WIN!");
    else
        snprintf(text, sizeof(text), "PLAYER %d WINS!", winner_id);
    float pop = 1.0f + 0.04f * sinf((float)elapsed * 5.0f);
    draw_label_centered(WIDTH / 2.0f, panel_y + 60.0f, 5.0f * pop, accent,
        ease, text);

    int line = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].b_active)
            continue;

        int cells = grid_count_owned(grid, i + 1);
        float y = panel_y + 125.0f + line * 30.0f;
        float frac = (float)cells / (GRID_COLS * GRID_ROWS);
        float bar_x = panel_x + 140.0f;
        float bar_w = 250.0f;

        glColor4f(colors[i][0], colors[i][1], colors[i][2], ease);
        draw_circle(panel_x + 50.0f, y + 9.0f, 9.0f);
        glColor4f(1.0f, 1.0f, 1.0f, 0.3f * ease);
        draw_circle(panel_x + 47.0f, y + 6.0f, 3.0f);

        snprintf(text, sizeof(text), "Player %d", i + 1);
        draw_label(panel_x + 68.0f, y + 2.0f, 1.8f, white, ease, text);

        glColor4f(1.0f, 1.0f, 1.0f, 0.10f * ease);
        draw_rrect(bar_x, y + 3.0f, bar_w, 12.0f, 6.0f);
        if (frac > 0.0f) {
            float fill = bar_w * frac * ease;
            if (fill < 12.0f)
                fill = 12.0f;
            glColor4f(colors[i][0], colors[i][1], colors[i][2], ease);
            draw_rrect(bar_x, y + 3.0f, fill, 12.0f, 6.0f);
            glColor4f(1.0f, 1.0f, 1.0f, 0.25f * ease);
            draw_rrect(bar_x + 2.0f, y + 4.0f, fill - 4.0f, 4.0f, 2.0f);
        }

        snprintf(text, sizeof(text), "%d", cells);
        draw_label(bar_x + bar_w + 14.0f, y + 2.0f, 1.8f, white, ease, text);

        if (i + 1 == winner_id)
            draw_crown(panel_x + 14.0f, y + 1.0f, 20.0f, 14.0f, ease);
        line++;
    }

    draw_label_centered(WIDTH / 2.0f, panel_y + panel_h - 22.0f, 1.4f, white,
        0.45f * ease, "Returning to the lobby...");
}

/**
 * @brief Small banners under the top bar announcing who left, fading out after
 *        NOTICE_SECS. Newer banners sit lower in the stack
 */
static void draw_notices(const LeaveNotice notices[], double now_secs) {
    int row = 0;
    for (int i = 0; i < MAX_NOTICES; i++) {
        int id = notices[i].player_id;
        double age = now_secs - notices[i].left_at;
        if (id < 1 || id > MAX_PLAYERS || age < 0.0 || age >= NOTICE_SECS)
            continue;

        float alpha = 1.0f;
        if (age > NOTICE_SECS - NOTICE_FADE_SECS)
            alpha = (float)((NOTICE_SECS - age) / NOTICE_FADE_SECS);
        // Slide down into place over the first moments
        float slide = age < 0.2 ? (float)(1.0 - age / 0.2) * -10.0f : 0.0f;

        char text[32];
        snprintf(text, sizeof(text), "Player %d left the game", id);
        float text_w = stb_easy_font_width((char *)text) * 2.0f;
        float w = text_w + 44.0f;
        float x = (WIDTH - w) / 2.0f;
        float y = 58.0f + row * 32.0f + slide;

        glColor4f(0.0f, 0.0f, 0.0f, 0.72f * alpha);
        draw_rrect(x, y, w, 26.0f, 13.0f);
        glColor4f(colors[id - 1][0], colors[id - 1][1],
            colors[id - 1][2], alpha);
        draw_circle(x + 17.0f, y + 13.0f, 6.5f);
        draw_label(x + 30.0f, y + 6.0f, 2.0f, white, alpha, text);
        row++;
    }
}

/**
 * @brief Banner for a client refused a slot because the game is full: it can still
 *        watch, and joins automatically when a player leaves
 */
static void draw_spectator_banner(double elapsed) {
    static const float amber[3] = { 1.0f, 0.8f, 0.3f };
    const char *title = "GAME FULL - SPECTATING";
    const char *sub = "You will join automatically when a spot opens";

    float alpha = 0.75f + 0.25f * sinf((float)elapsed * 3.0f);
    float title_w = stb_easy_font_width((char *)title) * 3.0f;
    float sub_w = stb_easy_font_width((char *)sub) * 1.5f;
    float w = (title_w > sub_w ? title_w : sub_w) + 40.0f;
    // Sits just above the bottom strip
    float y = HEIGHT - 56.0f - 62.0f - 8.0f;

    glColor4f(0.0f, 0.0f, 0.0f, 0.75f);
    draw_rrect((WIDTH - w) / 2.0f, y, w, 62.0f, 14.0f);
    glColor4f(amber[0], amber[1], amber[2], 0.5f * alpha);
    draw_rrect_outline((WIDTH - w) / 2.0f, y, w, 62.0f, 14.0f);
    draw_label_centered(WIDTH / 2.0f, y + 8.0f, 3.0f, amber, alpha, title);
    draw_label_centered(WIDTH / 2.0f, y + 40.0f, 1.5f, white, 0.9f, sub);
}

/**
 * @brief Advances the transition timers and rains confetti while the game is over
 */
static void update_effects(const RenderState *state, float dt) {
    // "GO!" on a fresh round: either it just started, or it restarted after
    // a game over
    if ((state->round_started && !prev_started) ||
        (!state->game_over && prev_over && state->round_started))
        go_timer = 1.2f;
    else if (go_timer > 0.0f)
        go_timer -= dt;

    if (state->seconds_left != prev_secs) {
        prev_secs = state->seconds_left;
        tick_age = 0.0f;
    } else {
        tick_age += dt;
    }

    if (state->game_over) {
        if (!prev_over) {
            end_timer = 0.0f;
            confetti_accum = 0.0f;
        }
        end_timer += dt;

        // Confetti in the winner's colour plus gold and white (or every
        // player colour for a draw)
        confetti_accum += dt * 70.0f;
        bool has_winner = (state->winner_id >= 1 &&
            state->winner_id <= MAX_PLAYERS);
        while (confetti_accum >= 1.0f) {
            confetti_accum -= 1.0f;
            float pick = rnd01();
            const float *col;
            if (has_winner && pick < 0.6f)
                col = colors[state->winner_id - 1];
            else if (pick < 0.8f && has_winner)
                col = gold;
            else if (!has_winner)
                col = colors[(int)(rnd01() * MAX_PLAYERS) % MAX_PLAYERS];
            else
                col = white;
            spawn_particle(PART_CONFETTI, rnd_range(0.0f, WIDTH), -10.0f,
                rnd_range(-40.0f, 40.0f), rnd_range(60.0f, 160.0f),
                rnd_range(4.0f, 6.0f), rnd_range(4.0f, 7.0f), 45.0f, col);
        }
    }

    prev_started = state->round_started;
    prev_over = state->game_over;
}

//============ PUBLIC API ===============

void render_init(void) {
    // Seed the effects' randomness so sparks and confetti differ every run
    srand((unsigned int)time(NULL));

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
    const char *dir = getenv("CTF_ASSET_DIR");
    if (!dir || dir[0] == '\0')
        dir = "assets";
    for (int i = 0; i < TEX_COUNT; i++)
        textures[i] = load_texture(dir, texture_files[i]);
}

// Letterboxed viewport: the whole game scaled uniformly to fit, centred
static int vp_x = 0, vp_y = 0, vp_w = WIDTH, vp_h = HEIGHT;

void render_set_viewport(int fb_w, int fb_h) {
    if (fb_w <= 0 || fb_h <= 0)
        return;
    float scale = (float)fb_w / WIDTH;
    if ((float)fb_h / HEIGHT < scale)
        scale = (float)fb_h / HEIGHT;
    vp_w = (int)(WIDTH * scale);
    vp_h = (int)(HEIGHT * scale);
    vp_x = (fb_w - vp_w) / 2;
    vp_y = (fb_h - vp_h) / 2;
}

void render_frame(const RenderState *state, float dt, double now_secs) {
    // Bars first (the clear covers the whole window), then the game inside
    // its letterboxed viewport
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(vp_x, vp_y, vp_w, vp_h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vp_x, vp_y, vp_w, vp_h);

    // A stalled frame (window drag, breakpoint) must not fling the
    // animations forward
    if (dt > 0.1f)
        dt = 0.1f;

    glClearColor(0.07f, 0.08f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    update_effects(state, dt);
    update_players(state->players, dt);
    update_particles(dt);

    // Back to front: backdrop, floor, squares, particles, HUD, then the
    // overlays and confetti
    draw_background();
    draw_cells(&state->grid, dt, now_secs);
    draw_players(&state->grid, state->players, state->own_id,
        state->round_started, now_secs);
    draw_particles(false);
    draw_countdown_fx(state->seconds_left, state->round_started,
        state->game_over);
    draw_hud(&state->grid, state->players, state->own_id,
        state->seconds_left, state->round_started, now_secs,
        state->spectating, dt);
    draw_notices(state->notices, now_secs);
    if (state->spectating)
        draw_spectator_banner(now_secs);
    if (!state->round_started)
        draw_lobby(state->players, state->own_id, now_secs);
    if (state->game_over)
        draw_end_screen(&state->grid, state->players, state->own_id,
            state->winner_id, now_secs);
    draw_particles(true);
}
