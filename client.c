/*****************************************************************************
\file client.c
\author Dr Khoo Teck Ping
\co-author regine wong (w.shiyuregine@digipen.edu)
\co-author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/02/2026 (MM/DD/YYYY)
\brief DDS game client: joins the server, sends the arrow-key input, mirrors the
       server's game state, and runs the GLFW window, handing each frame
       to render.c to draw with OpenGL.
*******************************************************************************/
/* gcc -O2 client.c render.c security.c game_logic.c grid_logic.c net_config.c messages.c -o client $(pkg-config --cflags --libs glfw3) -lGL $(pkg-config --cflags --libs CycloneDDS) $(pkg-config --cflags --libs libcrypto) -pthread -lm
(or just `make client`) */

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
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "dds/dds.h"
#include "messages.h"
#include "security.h"
#include "net_config.h"
#include "game_logic.h"
#include "grid_logic.h"
#include "render.h"

#define MAX_SAMPLES 1

// How often a held arrow key sends another Input while STEP moves a full
// grid cell at a time - too fast and the square blurs across several
// tiles per second instead of stepping cell by cell
#define MOVE_REPEAT_MSECS 150

// Target time per rendered frame (60 frames per second)
#define FRAME_SECS (1.0 / 60.0)

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

// "Player N left" messages (LeaveNotice, see render.h). Written by the worker thread, drawn by the main thread. Guarded by mutex
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
	dds_entity_t input_topic;
	dds_entity_t input_writer;
	// Wakes the worker when any reader has data, or the main thread
	// triggers wake_guard
	dds_entity_t waitset;

	// Set by take_positions() when our own Position loses its writer: the
	// server has stopped, restarted, or freed our slot. The worker then
	// rejoins - see rejoin()
	bool b_dropped_by_server;

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

// Creates the Input writer. No autodispose, so deleting it (when we leave or
// rejoin) unregisters our Input instance (not-alive-no-writers) instead of
// disposing it - the server ignores disposes, since any participant could
// send one (server.c)
static dds_entity_t create_input_writer(const ClientDds *a_dds)
{
	dds_qos_t *qos_input = dds_create_qos();
	dds_qset_writer_data_lifecycle(qos_input, false);
	dds_entity_t writer = dds_create_writer(a_dds->participant, a_dds->input_topic, qos_input, NULL);
	dds_delete_qos(qos_input);
	if (writer < 0)
		DDS_FATAL("dds_create_writer(Input): %s\n", dds_strretcode(-writer));
	return writer;
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
	a_dds->input_topic = create_topic(a_dds->participant, &Input_desc, "Input");
	dds_entity_t position_topic = create_topic(a_dds->participant, &Position_desc, "Position");
	dds_entity_t cell_topic = create_topic(a_dds->participant, &CellClaimed_desc, "CellClaimed");
	dds_entity_t state_topic = create_topic(a_dds->participant, &GameState_desc, "GameState");

	a_dds->join_writer = dds_create_writer(a_dds->participant, join_topic, NULL, NULL);
	a_dds->input_writer = create_input_writer(a_dds);

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
		CellClaimed *claim = (CellClaimed *) samples_cell[0];
		int int_cell_x = claim->int_cell_x;
		int int_cell_y = claim->int_cell_y;

		// The cell is the plaintext DDS key and indexes arr_cell_guard,
		// so reject anything off the grid
		if (int_cell_x < 0 || int_cell_x >= GRID_COLS ||
			int_cell_y < 0 || int_cell_y >= GRID_ROWS)
			continue;

		if (!infos_cell[0].valid_data)
		{
			// Only the server writes CellClaimed, so a cell losing its
			// writer means the server has gone. Forget that server's
			// claims: a restarted server starts with an empty floor and
			// only publishes cells as they are claimed again
			if (infos_cell[0].instance_state == DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE)
			{
				pthread_mutex_lock(&mutex);
				grid_set_owner(&grid, int_cell_x, int_cell_y, GRID_UNCLAIMED);
				pthread_mutex_unlock(&mutex);
			}
			continue;
		}

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
			// that player (they left), or the server itself has gone, so
			// stop drawing their square. A disposed instance is ignored:
			// any DDS participant can dispose one, which would let it hide
			// another player
			if (infos_position[0].instance_state == DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE)
			{
				pthread_mutex_lock(&mutex);
				bool b_was_active = arr_Players[int_position_player_id - 1].b_active;
				arr_Players[int_position_player_id - 1].b_active = false;
				bool b_ours = int_position_player_id == int_player_id;
				// Announce it on screen, but only once and not for ourselves
				if (b_was_active && !b_ours)
				{
					push_leave_notice(int_position_player_id);
					printf("Player %d left the game\n", int_position_player_id);
				}
				pthread_mutex_unlock(&mutex);
				// Our own square: the server no longer has us in the game
				if (b_ours)
					a_dds->b_dropped_by_server = true;
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

// Fills a_request with a fresh nonce and the authentication credential
// bound to it - see security.h. The nonce tags the request, so only the
// response to it is accepted, and the server binds our Input to it
static void make_join_request(JoinRequest *a_request)
{
	memset(a_request, 0, sizeof(*a_request));
	a_request->int_nonce = security_make_nonce();
	security_make_client_token(a_request->int_nonce, a_request->arr_token,
		sizeof(a_request->arr_token));
}

// The server has dropped us (it stopped, restarted, or freed our slot), so
// start over as a new client and join again. Spectating clients never get
// here: they have no Position of their own and keep retrying anyway
static void rejoin(ClientDds *a_dds, JoinRequest *a_request)
{
	printf("Lost our place in the game - rejoining\n");

	pthread_mutex_lock(&mutex);
	int_player_id = -1;
	b_spectating = false;
	pthread_mutex_unlock(&mutex);

	// A new nonce: the server refuses a departed player's old one
	make_join_request(a_request);

	// A new Input writer: deleting the old one unregisters the Input
	// instance of our old player ID, so it is not left registered by us
	// after that ID goes to someone else
	dds_delete(a_dds->input_writer);
	a_dds->input_writer = create_input_writer(a_dds);

	// A restarted server is a new sender, so start its streams afresh
	a_dds->response_guard = (SecurityReplayGuard){0};
	a_dds->state_guard = (SecurityReplayGuard){0};
	memset(a_dds->arr_position_guard, 0, sizeof(a_dds->arr_position_guard));
	memset(a_dds->arr_cell_guard, 0, sizeof(a_dds->arr_cell_guard));

	a_dds->b_dropped_by_server = false;
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
	make_join_request(&request);

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

		if (dds.b_dropped_by_server)
		{
			rejoin(&dds, &request);
			b_sent_hello = false;
			// Ask straight away; repeats every second until a server answers
			next_join_time = dds_time();
		}
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

// ### MAIN THREAD ###

// Reads the arrow keys: the direction of the first one held, or 'n' for none.
// The keys only reach the window once it has been clicked (Slide 7)
static char read_direction(GLFWwindow *a_window)
{
	if (glfwGetKey(a_window, GLFW_KEY_UP) == GLFW_PRESS)
		return 'u';
	if (glfwGetKey(a_window, GLFW_KEY_DOWN) == GLFW_PRESS)
		return 'd';
	if (glfwGetKey(a_window, GLFW_KEY_LEFT) == GLFW_PRESS)
		return 'l';
	if (glfwGetKey(a_window, GLFW_KEY_RIGHT) == GLFW_PRESS)
		return 'r';
	return 'n';
}

// Copies everything one frame draws out of the shared state, under one
// lock, so the frame is drawn without holding it
static void snapshot_state(RenderState *out_state)
{
	pthread_mutex_lock(&mutex);
	out_state->grid = grid;
	memcpy(out_state->arr_players, arr_Players, sizeof(out_state->arr_players));
	memcpy(out_state->arr_notices, arr_notices, sizeof(out_state->arr_notices));
	out_state->int_own_id = int_player_id;
	out_state->int_seconds_left = int_seconds_left;
	out_state->b_round_started = b_round_started;
	out_state->b_spectating = b_spectating;
	out_state->b_game_over = b_game_over;
	out_state->int_winner_id = int_winner_id;
	pthread_mutex_unlock(&mutex);
}

// Entry point of the application
// Usage: ./client [server address ...] - the address is only needed on
// networks that block DDS's own discovery (see net_config.h)
int main(int argc, char **argv)
{
	if (!security_init())
	{
		fprintf(stderr, "security_init failed\n");
		return 1;
	}

	// Fixed ports and any peer addresses, for playing across machines -
	// see net_config.h. Must run before the worker thread creates its DDS
	// participant
	if (!net_config_apply(argc - 1, argv + 1))
	{
		fprintf(stderr, "usage: %s [server address ...]\n", argv[0]);
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

	GLFWwindow *window = glfwCreateWindow(WIDTH, HEIGHT,
		"DDS Client (Click within the window to control)", NULL, NULL);
	if (!window)
	{
		glfwTerminate();
		return 1;
	}

	glfwMakeContextCurrent(window);
	// Wait for the display refresh instead of redrawing as fast as possible
	glfwSwapInterval(1);
	render_init();

	double d_last_frame = glfwGetTime();
	// Direction last handed to the worker thread, so the shared ch_dir is
	// only locked and the worker only woken when a key actually changes,
	// not on every frame
	char ch_last_dir = 'n';
	RenderState state;

	while (!glfwWindowShouldClose(window) && !g_quit_requested)
	{
		// Process keyboard/window events
		glfwPollEvents();

		// Non-blocking keyboard input: the arrow keys move the player's square
		char ch_new_dir = read_direction(window);
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

		snapshot_state(&state);
		render_frame(&state, f_dt, d_now);
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
