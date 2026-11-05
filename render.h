/*****************************************************************************
\file render.h
\author regine wong (w.shiyuregine@digipen.edu)
\co-author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/04/2026 (MM/DD/YYYY)
\brief Client rendering API: draws one frame of the game (floor, players,
       HUD, notices and end screen) with OpenGL from a snapshot of the
       client's game state.
*******************************************************************************/
/* Everything client.c draws lives behind this header, so client.c only
deals with DDS traffic, input and the main loop. The renderer never touches
the client's shared (mutex-guarded) state: the main thread copies it into a
RenderState under the lock and hands that copy to render_frame(). */

#ifndef RENDER_H
#define RENDER_H

#include <stdbool.h>
#include "game_logic.h"
#include "grid_logic.h"

// "Player N left" messages shown briefly on screen
#define MAX_NOTICES 3
#define NOTICE_SECS 4.0
#define NOTICE_FADE_SECS 0.5

typedef struct
{
	int int_player_id;   // 0 = empty slot
	double d_time;       // glfwGetTime() when the player left
} LeaveNotice;

// A consistent copy of everything one frame draws
typedef struct
{
	Grid grid;
	Player arr_players[MAX_PLAYERS];
	LeaveNotice arr_notices[MAX_NOTICES];
	// Our player ID, or -1 until the server admits us
	int int_own_id;
	int int_seconds_left;
	bool b_round_started;
	bool b_spectating;
	bool b_game_over;
	int int_winner_id;
} RenderState;

// Sets up OpenGL for drawing in window pixels (origin top-left) with alpha
// blending. Call once, after the GLFW window's context is made current
void render_init(void);

// Clears the window and draws one frame of a_state. a_f_dt is the seconds
// since the previous frame (drives the cell pop-in animation); a_d_now is
// glfwGetTime(), for pulses, blinks and fading notices
void render_frame(const RenderState *a_state, float a_f_dt, double a_d_now);

// ID of the player allowed to start the round from the lobby (the lowest
// active ID), or 0 if nobody has joined. The server applies the same rule
int render_host_id(const Player a_arr_players[]);

#endif // RENDER_H
