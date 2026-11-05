/* ========================================================
 * @file        render.h
 * @author      Wong Shi Yu Regine | w.shiyuregine@digipen.edu
 * @co-author   Choi Meng Yew      | choi.m@digipen.edu
 * @date        04 NOV 2026
 *
 * @brief
 *
 * Client rendering API: draws one frame of the game (lobby, floor, players,
 * HUD, notices and end screen) with OpenGL from a snapshot of the client's
 * game state.
 *
 * Everything client.c draws lives behind this header, so client.c only deals
 * with DDS traffic, input and the main loop. The renderer never touches the
 * client's shared (mutex-guarded) state: the main thread copies it into a
 * RenderState under the lock and hands that copy to render_frame().
 *
 * Course: CSD3116, Assignment: Week 1 Team Lab
 *
 * Copyright © 2026 DigiPen, All rights reserved.
 * ========================================================
 */
#ifndef RENDER_H
#define RENDER_H

#include <stdbool.h>
#include "game_logic.h"
#include "grid_logic.h"

// "Player N left" messages shown briefly on screen
#define MAX_NOTICES 3
#define NOTICE_SECS 4.0
#define NOTICE_FADE_SECS 0.5

/**
 * @struct LeaveNotice
 * @brief One "Player N left the game" banner.
 */
typedef struct {
    int player_id;   // 0 = empty slot
    double left_at;  // glfwGetTime() when the player left
} LeaveNotice;

/**
 * @struct RenderState
 * @brief A consistent copy of everything one frame draws.
 */
typedef struct {
    Grid grid;
    Player players[MAX_PLAYERS];
    LeaveNotice notices[MAX_NOTICES];
    // Our player ID, or -1 until the server admits us
    int own_id;
    int seconds_left;
    bool round_started;
    bool spectating;
    bool game_over;
    int winner_id;
} RenderState;

/**
 * @brief Sets up OpenGL for drawing in window pixels (origin top-left) with
 *        alpha blending, and loads the PNGs from assets/. Call once, after
 *        the GLFW window's context is made current.
 */
void render_init(void);

/**
 * @brief Tells the renderer the window's current framebuffer size, so a
 *        resized, maximised or fullscreen window shows the 800x600 game
 *        scaled to fit with black bars, instead of in a corner.
 * @param fb_w Framebuffer width in pixels (from glfwGetFramebufferSize).
 * @param fb_h Framebuffer height in pixels.
 */
void render_set_viewport(int fb_w, int fb_h);

/**
 * @brief Clears the window and draws one frame.
 * @param state Snapshot of the game state to draw.
 * @param dt Seconds since the previous frame (drives the cell pop-in).
 * @param now_secs glfwGetTime(), for pulses, blinks and fading notices.
 */
void render_frame(const RenderState *state, float dt, double now_secs);

/**
 * @brief ID of the player allowed to start the round from the lobby: the
 *        lowest active ID. The server applies the same rule.
 * @param players The players array (index = player ID - 1).
 * @return The host's player ID, or 0 if nobody has joined.
 */
int render_host_id(const Player players[]);

#endif // RENDER_H
