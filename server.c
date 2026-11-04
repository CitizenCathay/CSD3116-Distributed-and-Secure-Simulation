/*****************************************************************************
\file server.c
\author Dr Khoo Teck Ping
\co-author regine wong (w.shiyuregine@digipen.edu)
\co-author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/02/2026 (MM/DD/YYYY)
\brief DDS game server: authenticates joining clients, owns the game state (player
       positions and the grid of claimed cells), runs the round timer and
       publishes updates to the clients.
*******************************************************************************/
/* gcc -O2 -o server server.c security.c game_logic.c grid_logic.c messages.c $(pkg-config --cflags --libs CycloneDDS) $(pkg-config --cflags --libs libcrypto) -pthread
(or just `make server`) */

/* Dr Khoo Teck Ping
27 Aug 2026
1. DDS server
2. Supports max 5 clients
3. Start this server, then start client one by one
4. Each client can see itself and all other clients
5. Each client can move itself using the arrow keys
6. Collision detection among all players and the window is supported */

/* Lecture slide references in this file are to "CSD3116 Week 01 Team Lab"
(Dr Khoo Teck Ping): Slide 7 baseline programs, Slide 10 game state,
Slide 11 publishing and subscribing, Slide 12 threading, Slide 14 tasks. */

/* mutex lock/unlock must be matched (Slide 12)
watch out for break out of loop */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include "dds/dds.h"
#include "messages.h"
#include "security.h"
#include "game_logic.h"
#include "grid_logic.h"

// ### GLOBAL CONSTANTS ###
/* An array of one message (aka sample in dds terms) will be used. */
#define MAX_SAMPLES 1
// The round starts once this many players have joined
#define MIN_PLAYERS 2
// How long the result stays on screen before a new round starts
// automatically - no need to restart the server to play again
#define INTERMISSION_SECS 5
// A newly admitted player must send its first Input within this long, or
// its slot is freed again. Leaves are otherwise only noticed when a
// player's Input instance loses its writer, which never happens for a
// slot whose client never wrote one (eg a replayed JoinRequest)
#define FIRST_INPUT_TIMEOUT_SECS 10
// How many departed players' join nonces are remembered, so a recorded
// JoinRequest of theirs cannot be replayed to take a new slot
#define MAX_RETIRED_NONCES 64
// Longest the worker thread sleeps when no DDS traffic arrives, so the
// round timer, GameState broadcast and first-Input timeouts still advance
#define TICK_MSECS 100
// How many join requests (and responses) can wait between the two threads
// at once. A request that does not fit is dropped - the client resends it
// every second until answered
#define JOIN_QUEUE_LEN 16

// ### GLOBAL VARIABLES ###
int int_num_active_players = 0;

// Join handshake between the two threads (Slide 12, Figure 5b), guarded by
// mutex. Both directions are queues, so a second join arriving before the
// first is answered is not overwritten.
// Worker -> main: nonces of authenticated JoinRequests waiting for a slot.
// The main thread sleeps on cond_join_queued until one is queued, instead
// of waking every millisecond to check a flag
static int arr_join_queue[JOIN_QUEUE_LEN];
static int int_join_queue_len = 0;
static pthread_cond_t cond_join_queued = PTHREAD_COND_INITIALIZER;

// Main -> worker: answers waiting to be published as JoinResponses. After
// queueing some, the main thread triggers response_guard, which wakes the
// worker's waitset. Player ID 0 = game full
typedef struct
{
	int int_player_id;
	int int_nonce;
} PendingResponse;
static PendingResponse arr_response_queue[JOIN_QUEUE_LEN];
static int int_response_queue_len = 0;
// Created by the worker thread before it can queue any join request, so it
// always exists by the time the main thread triggers it. 0 until then
static dds_entity_t response_guard = 0;

// Set when Ctrl+C / SIGTERM arrives, so both threads finish and the worker
// deletes its DDS participant. That is what tells every client straight
// away that the server has gone (instead of after DDS's 10 s lease), so
// they can rejoin when it comes back. Guarded by mutex
static bool b_quit = false;

// Last position published for each slot, so a Position is only sent when
// it changes rather than for every player every tick. Only touched by the
// worker thread
static bool arr_b_position_sent[MAX_PLAYERS];
static int arr_sent_x[MAX_PLAYERS];
static int arr_sent_y[MAX_PLAYERS];

// Table to track clients
// Init all members to 0
// This is the game state, ie the ground truth: only the server updates it,
// and clients keep a copy from our Position messages. Each entry holds
// Active/X/Y, and the player ID is the array index plus 1 so a player can
// be looked up directly (Slide 10, Figure 3)
static Player arr_Players[MAX_PLAYERS] = {0};

// Per-slot session state, indexed like arr_Players. Guarded by mutex
// Nonce of the JoinRequest each slot was admitted with. Input is bound to
// it (see security.h), and a retried request with the same nonce gets the
// same slot back instead of a second one
static int arr_player_nonce[MAX_PLAYERS];
// When each slot was admitted, and whether its first Input has arrived
// yet - see FIRST_INPUT_TIMEOUT_SECS
static dds_time_t arr_join_time[MAX_PLAYERS];
static bool arr_b_input_seen[MAX_PLAYERS];
// Replay protection for each slot's Input stream - see security.h
static SecurityReplayGuard arr_input_guard[MAX_PLAYERS];

// Join nonces of players who have left: a ring buffer holding the last
// int_num_retired of them, next written at int_next_retired. Guarded by mutex
static int arr_retired_nonce[MAX_RETIRED_NONCES];
static int int_num_retired = 0;
static int int_next_retired = 0;

// Guards the variables shared by the main thread and the DDS worker thread,
// so neither reads a value while the other is changing it. Every lock must
// be matched by an unlock (Slide 12)
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// True if a_int_nonce belongs to a player who has left. The caller must
// hold mutex
static bool is_nonce_retired(int a_int_nonce)
{
	for (int i = 0; i < int_num_retired; i++)
	{
		if (arr_retired_nonce[i] == a_int_nonce)
			return true;
	}
	return false;
}

// Publishes that a cell changed owner (0 = unclaimed)
static void publish_cell_claimed(dds_entity_t a_writer, int a_int_col,
	int a_int_row, int a_int_owner_id)
{
	// Zeroed, so the unused tail of arr_cipher never carries stack memory
	// onto the network
	CellClaimed claim;
	memset(&claim, 0, sizeof(claim));
	claim.int_cell_x = a_int_col;
	claim.int_cell_y = a_int_row;

	// Encrypt the payload before publishing - see security.h. Clients drop
	// anything without a valid cipher, so if encryption ever fails there is
	// no point sending (and leaking) the plaintext. The cipher is bound to
	// the cell (the DDS key, which stays in the clear for routing), so it
	// cannot be replayed as another cell's owner
	int arr_bound[2] = { a_int_col, a_int_row };
	int arr_plain[1] = { a_int_owner_id };
	size_t cipher_len = 0;
	if (!security_encrypt(SECURITY_MSG_CELL_CLAIMED, arr_bound, 2,
		(unsigned char *)arr_plain, sizeof(arr_plain),
		claim.arr_cipher, &cipher_len) ||
		cipher_len > sizeof(claim.arr_cipher))
	{
		fprintf(stderr, "CellClaimed not sent: encryption failed\n");
		return;
	}
	claim.len_cipher = (unsigned long)cipher_len;
	// int_owner_id stays GRID_UNCLAIMED: the owner is only readable from
	// the ciphertext

	dds_return_t rc = dds_write(a_writer, &claim);
	if (rc != DDS_RETCODE_OK)
	{
		DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
	}
}

// Publishes the round timer and, once over, the result
static void publish_game_state(dds_entity_t a_writer, int a_int_seconds_left,
	bool a_b_started, bool a_b_game_over, int a_int_winner_id)
{
	// Zeroed, so the unused tail of arr_cipher never carries stack memory
	// onto the network. GameState has no DDS key, so every plaintext field
	// is payload and stays zero; it is only readable from the ciphertext
	GameState state;
	memset(&state, 0, sizeof(state));

	// Encrypt the payload before publishing - see security.h. Clients drop
	// anything without a valid cipher, so skip sending if encryption fails
	int arr_plain[4] = { a_int_seconds_left, a_b_started, a_b_game_over,
		a_int_winner_id };
	size_t cipher_len = 0;
	if (!security_encrypt(SECURITY_MSG_GAME_STATE, NULL, 0,
		(unsigned char *)arr_plain, sizeof(arr_plain),
		state.arr_cipher, &cipher_len) ||
		cipher_len > sizeof(state.arr_cipher))
	{
		fprintf(stderr, "GameState not sent: encryption failed\n");
		return;
	}
	state.len_cipher = (unsigned long)cipher_len;

	dds_return_t rc = dds_write(a_writer, &state);
	if (rc != DDS_RETCODE_OK)
	{
		DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
	}
}

// Frees a departed player's slot: marks them inactive, releases their cells
// back to unclaimed, and unregisters their Position instance so every client
// stops drawing their square. The freed slot can then be taken by a new
// player through the normal join flow
static void handle_player_left(int a_int_player_id, Grid *a_grid,
	dds_entity_t a_cell_writer, dds_entity_t a_position_writer)
{
	if (a_int_player_id < 1 || a_int_player_id > MAX_PLAYERS)
		return;

	pthread_mutex_lock(&mutex);
	bool b_was_active = arr_Players[a_int_player_id - 1].b_active;
	if (b_was_active)
	{
		arr_Players[a_int_player_id - 1].b_active = false;
		int_num_active_players--;
		// Their JoinRequest (nonce + token) was sent in the clear, so
		// refuse it from now on rather than let a recording of it take
		// this slot back without a client behind it
		arr_retired_nonce[int_next_retired] = arr_player_nonce[a_int_player_id - 1];
		int_next_retired = (int_next_retired + 1) % MAX_RETIRED_NONCES;
		if (int_num_retired < MAX_RETIRED_NONCES)
			int_num_retired++;
	}
	pthread_mutex_unlock(&mutex);

	// Already handled: DDS can report the same instance more than once
	if (!b_was_active)
		return;

	// Release their cells, so a new player who reuses this ID does not
	// inherit them (and their score)
	for (int int_row = 0; int_row < GRID_ROWS; int_row++)
	{
		for (int int_col = 0; int_col < GRID_COLS; int_col++)
		{
			if (a_grid->arr_owner[int_row * GRID_COLS + int_col] == a_int_player_id)
			{
				grid_set_owner(a_grid, int_col, int_row, GRID_UNCLAIMED);
				publish_cell_claimed(a_cell_writer, int_col, int_row, GRID_UNCLAIMED);
			}
		}
	}

	// Unregistering the keyed Position instance tells every client to drop
	// this player's square: with no writer left, it goes not-alive-no-writers.
	// Unregister rather than dispose, because any DDS participant can dispose
	// an instance, so clients only trust the no-writers state (see client.c)
	Position position;
	memset(&position, 0, sizeof(position));
	position.int_player_id = a_int_player_id;
	dds_return_t rc = dds_unregister_instance(a_position_writer, &position);
	if (rc != DDS_RETCODE_OK)
	{
		DDS_FATAL("dds_unregister_instance: %s\n", dds_strretcode(-rc));
	}
	// Whoever takes this slot next must have their first position sent,
	// even if they happen to spawn where this player last stood
	arr_b_position_sent[a_int_player_id - 1] = false;

	printf("Player %d left\n", a_int_player_id);
}

// Publishes one player's position. Returns false (and sends nothing) if
// encryption fails
static bool publish_position(dds_entity_t a_writer, int a_int_player_id,
	int a_int_x, int a_int_y)
{
	// Zeroed, so the unused tail of arr_cipher never carries stack memory
	// onto the network. int_x/int_y stay zero: the position is only
	// readable from the ciphertext
	Position position;
	memset(&position, 0, sizeof(position));
	position.int_player_id = a_int_player_id;

	// Encrypt the payload before publishing - see security.h. Clients drop
	// anything without a valid cipher, so skip sending if encryption fails.
	// The cipher is bound to int_player_id (the DDS key, which stays in the
	// clear for routing/leave detection), so it cannot be replayed as
	// another player's position
	int arr_bound[1] = { a_int_player_id };
	int arr_plain[2] = { a_int_x, a_int_y };
	size_t cipher_len = 0;
	if (!security_encrypt(SECURITY_MSG_POSITION, arr_bound, 1,
		(unsigned char *)arr_plain, sizeof(arr_plain),
		position.arr_cipher, &cipher_len) ||
		cipher_len > sizeof(position.arr_cipher))
	{
		fprintf(stderr, "Position not sent: encryption failed\n");
		return false;
	}
	position.len_cipher = (unsigned long)cipher_len;

	dds_return_t rc = dds_write(a_writer, &position);
	if (rc != DDS_RETCODE_OK)
	{
		DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
	}
	return true;
}

// Publishes the answer to one join request (player ID 0 = game full)
static void publish_join_response(dds_entity_t a_writer, int a_int_player_id,
	int a_int_nonce)
{
	// Zeroed, so the unused tail of arr_cipher never carries stack memory
	// onto the network. The plaintext fields stay zero: the response is
	// only authentic/readable via the ciphertext
	JoinResponse response;
	memset(&response, 0, sizeof(response));

	// Encrypt the payload before publishing - see security.h. Lets the
	// client verify this response really came from a holder of the shared
	// key, not just something that claims to
	int arr_plain[2] = { a_int_player_id, a_int_nonce };
	size_t cipher_len = 0;
	if (!security_encrypt(SECURITY_MSG_JOIN_RESPONSE, NULL, 0,
		(unsigned char *)arr_plain, sizeof(arr_plain),
		response.arr_cipher, &cipher_len) ||
		cipher_len > sizeof(response.arr_cipher))
	{
		// Clients drop unencrypted responses, so don't send one
		fprintf(stderr, "Join response not sent: encryption failed\n");
		return;
	}
	response.len_cipher = (unsigned long)cipher_len;

	dds_return_t rc = dds_write(a_writer, &response);
	if (rc != DDS_RETCODE_OK)
	{
		DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
	}
	printf("Join response sent\n");
}

// ### DDS WORKER THREAD ###

// The worker thread's DDS entities
typedef struct
{
	dds_entity_t participant;
	dds_entity_t join_reader;
	dds_entity_t input_reader;
	dds_entity_t response_writer;
	dds_entity_t position_writer;
	dds_entity_t cell_writer;
	dds_entity_t state_writer;
	// Wakes the worker when a JoinRequest or Input arrives, or the main
	// thread queues a JoinResponse (response_guard)
	dds_entity_t waitset;
	// Sample buffers for dds_take(), allocated once
	void *ptr_join_sample;
	void *ptr_input_sample;
} ServerDds;

// Round state, only touched by the worker thread: waiting for players ->
// running -> over -> (after INTERMISSION_SECS) waiting for players again
typedef struct
{
	// Ground truth of who owns which cell
	Grid grid;
	bool b_started;
	bool b_game_over;
	int int_winner_id;
	dds_time_t start_time;
	dds_time_t game_over_time;
	dds_time_t last_state_time;
} Round;

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
static void create_dds(ServerDds *a_dds)
{
	a_dds->participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
	if (a_dds->participant < 0)
		DDS_FATAL("dds_create_participant: %s\n", dds_strretcode(-a_dds->participant));

	dds_entity_t join_topic = create_topic(a_dds->participant, &JoinRequest_desc, "JoinRequest");
	dds_entity_t response_topic = create_topic(a_dds->participant, &JoinResponse_desc, "JoinResponse");
	dds_entity_t position_topic = create_topic(a_dds->participant, &Position_desc, "Position");
	dds_entity_t input_topic = create_topic(a_dds->participant, &Input_desc, "Input");
	dds_entity_t cell_topic = create_topic(a_dds->participant, &CellClaimed_desc, "CellClaimed");
	dds_entity_t state_topic = create_topic(a_dds->participant, &GameState_desc, "GameState");

	a_dds->join_reader = dds_create_reader(a_dds->participant, join_topic, NULL, NULL);

	// Reliable, so a client's one "hello" Input is never silently dropped -
	// a slot whose first Input never arrives is freed after
	// FIRST_INPUT_TIMEOUT_SECS, which would kick an idle but connected player
	dds_qos_t *qos_input = dds_create_qos();
	dds_qset_reliability(qos_input, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	a_dds->input_reader = dds_create_reader(a_dds->participant, input_topic, qos_input, NULL);
	dds_delete_qos(qos_input);

	a_dds->response_writer = dds_create_writer(a_dds->participant, response_topic, NULL, NULL);

	// Reliable, so a Position update is never silently dropped and left
	// trailing behind the (already-reliable) CellClaimed for the same move.
	// Positions are only published when they change, so transient-local +
	// keep-last-1 hands a client that joins later every player's current
	// position, the same way CellClaimed does for cells.
	// No autodispose, so unregistering a departed player's instance leaves
	// it not-alive-no-writers rather than disposed - clients ignore
	// disposes, since any participant could send one (client.c)
	dds_qos_t *qos_position = dds_create_qos();
	dds_qset_reliability(qos_position, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	dds_qset_durability(qos_position, DDS_DURABILITY_TRANSIENT_LOCAL);
	dds_qset_history(qos_position, DDS_HISTORY_KEEP_LAST, 1);
	dds_qset_writer_data_lifecycle(qos_position, false);
	a_dds->position_writer = dds_create_writer(a_dds->participant, position_topic, qos_position, NULL);
	dds_delete_qos(qos_position);

	// CellClaimed is keyed per cell: transient-local + keep-last-1 lets a
	// client that joins mid-round receive the current owner of every cell.
	// No autodispose, like Position: when the server stops, every cell goes
	// not-alive-no-writers, which clients trust (unlike a dispose, which
	// any participant could send) and use to clear the old server's floor
	dds_qos_t *qos_cell = dds_create_qos();
	dds_qset_durability(qos_cell, DDS_DURABILITY_TRANSIENT_LOCAL);
	dds_qset_reliability(qos_cell, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	dds_qset_history(qos_cell, DDS_HISTORY_KEEP_LAST, 1);
	dds_qset_writer_data_lifecycle(qos_cell, false);
	a_dds->cell_writer = dds_create_writer(a_dds->participant, cell_topic, qos_cell, NULL);
	dds_delete_qos(qos_cell);

	a_dds->state_writer = dds_create_writer(a_dds->participant, state_topic, NULL, NULL);

	if (a_dds->join_reader < 0 || a_dds->input_reader < 0 ||
		a_dds->response_writer < 0 || a_dds->position_writer < 0 ||
		a_dds->cell_writer < 0 || a_dds->state_writer < 0)
		DDS_FATAL("creating a DDS reader or writer failed\n");

	// Wait on "has unread samples" for both readers, plus the guard the
	// main thread triggers when it has join responses for us to send
	dds_entity_t guard = dds_create_guardcondition(a_dds->participant);
	a_dds->waitset = dds_create_waitset(a_dds->participant);
	if (guard < 0 || a_dds->waitset < 0)
		DDS_FATAL("creating the waitset failed\n");
	dds_waitset_attach(a_dds->waitset,
		dds_create_readcondition(a_dds->join_reader, DDS_ANY_STATE), 0);
	dds_waitset_attach(a_dds->waitset,
		dds_create_readcondition(a_dds->input_reader, DDS_ANY_STATE), 0);
	dds_waitset_attach(a_dds->waitset, guard, 0);

	pthread_mutex_lock(&mutex);
	response_guard = guard;
	pthread_mutex_unlock(&mutex);

	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	a_dds->ptr_join_sample = JoinRequest__alloc();
	a_dds->ptr_input_sample = Input__alloc();
}

// Takes every pending JoinRequest, authenticates it and queues it for the
// main thread (Slide 11, Figure 4a: Join Request, then Join Response
// carrying the new player's ID)
static void take_join_requests(const ServerDds *a_dds)
{
	void *samples_join[MAX_SAMPLES] = { a_dds->ptr_join_sample };
	dds_sample_info_t infos_join[MAX_SAMPLES];
	dds_return_t rc;

	while ((rc = dds_take(a_dds->join_reader, samples_join, infos_join, MAX_SAMPLES, MAX_SAMPLES)) > 0)
	{
		if (!infos_join[0].valid_data)
			continue;

		JoinRequest *join_req = (JoinRequest *) samples_join[0];
		// Authenticate the client before admitting it - see security.h
		// (a required security feature, Slide 14)
		if (!security_verify_client_token(join_req->int_nonce, join_req->arr_token))
		{
			printf("Join request rejected: authentication failed\n");
			continue;
		}
		printf("Join request received (authenticated)\n");

		// Hand it to the main thread, which owns slot allocation, and wake it
		pthread_mutex_lock(&mutex);
		if (int_join_queue_len < JOIN_QUEUE_LEN)
		{
			arr_join_queue[int_join_queue_len++] = join_req->int_nonce;
			pthread_cond_signal(&cond_join_queued);
		}
		pthread_mutex_unlock(&mutex);
	}
	if (rc < 0)
		DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
}

// Publishes every JoinResponse the main thread has queued
static void send_join_responses(const ServerDds *a_dds)
{
	// Copy the queue out under the lock, then encrypt and publish without
	// holding it, so the main thread is never kept waiting on DDS
	PendingResponse arr_pending[JOIN_QUEUE_LEN];
	pthread_mutex_lock(&mutex);
	int int_count = int_response_queue_len;
	memcpy(arr_pending, arr_response_queue, sizeof(PendingResponse) * int_count);
	int_response_queue_len = 0;
	pthread_mutex_unlock(&mutex);

	for (int i = 0; i < int_count; i++)
	{
		publish_join_response(a_dds->response_writer, arr_pending[i].int_player_id,
			arr_pending[i].int_nonce);
	}
}

// Takes every pending Input and applies the moves (Slide 11, Figure 4b:
// the client sends Input, the server checks collisions and updates all
// positions). Also frees the slot of any client whose Input writer is gone
static void take_inputs(const ServerDds *a_dds, Round *a_round)
{
	void *samples_input[MAX_SAMPLES] = { a_dds->ptr_input_sample };
	dds_sample_info_t infos_input[MAX_SAMPLES];
	dds_return_t rc;

	// Drain them all, since several clients' inputs can be pending at once
	while ((rc = dds_take(a_dds->input_reader, samples_input, infos_input, MAX_SAMPLES, MAX_SAMPLES)) > 0)
	{
		if (!infos_input[0].valid_data)
		{
			// A no-writers Input instance means that client's DDS writer is
			// gone (window closed, or the process died), so free their slot.
			// A disposed instance is ignored: any DDS participant can
			// dispose another player's instance, which would let it kick them
			int int_left_id = ((Input *) samples_input[0])->int_player_id;
			if (infos_input[0].instance_state != DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE ||
				int_left_id < 1 || int_left_id > MAX_PLAYERS)
				continue;

			// Only once this slot's player has sent its first Input: before
			// that, the event can only be stale news about an earlier writer
			// of the same player ID (eg a client rejoining after a server
			// restart deletes its old Input writer, and that can arrive after
			// its new JoinRequest has already won it the same slot). Once
			// the player's own writer has written, the instance cannot lose
			// all its writers unless that writer has gone too. A client that
			// disappears before its first Input is freed by
			// FIRST_INPUT_TIMEOUT_SECS instead
			pthread_mutex_lock(&mutex);
			bool b_heard_from = arr_Players[int_left_id - 1].b_active &&
				arr_b_input_seen[int_left_id - 1];
			pthread_mutex_unlock(&mutex);
			if (b_heard_from)
			{
				handle_player_left(int_left_id, &a_round->grid, a_dds->cell_writer,
					a_dds->position_writer);
			}
			continue;
		}

		Input *input = (Input *) samples_input[0];
		// Get the input player ID - the plaintext DDS key, authenticated
		// below as part of the cipher's associated data
		int int_input_player_id = input->int_player_id;

		// Ignore input from an unknown player ID, since it indexes arr_Players
		if (int_input_player_id < 1 || int_input_player_id > MAX_PLAYERS)
			continue;

		pthread_mutex_lock(&mutex);
		if (!arr_Players[int_input_player_id - 1].b_active)
		{
			pthread_mutex_unlock(&mutex);
			continue;
		}

		// Decrypt the payload - see security.h. Input without a valid
		// cipher is dropped, never read from the plaintext fields, or
		// anyone on the DDS domain could move any player without the key.
		// It must also be bound to this player ID and the nonce this
		// slot was admitted with, and newer than the last Input accepted
		// for the slot - so a recorded Input cannot be replayed, either
		// as this player or into a later player's session in the slot
		int arr_bound[2] = { int_input_player_id,
			arr_player_nonce[int_input_player_id - 1] };
		unsigned char arr_plain[1];
		size_t plain_len = 0;
		if (input->len_cipher == 0 ||
			!security_decrypt(SECURITY_MSG_INPUT, arr_bound, 2,
			&arr_input_guard[int_input_player_id - 1],
			input->arr_cipher, input->len_cipher,
			arr_plain, sizeof(arr_plain), &plain_len) ||
			plain_len != sizeof(arr_plain))
		{
			pthread_mutex_unlock(&mutex);
			continue;
		}
		char ch_input_dir = (char)arr_plain[0];
		// Their client is really there, so the slot is no longer at risk
		// of being freed by FIRST_INPUT_TIMEOUT_SECS
		arr_b_input_seen[int_input_player_id - 1] = true;

		// Compute the move and apply it if legal - see game_logic.h (a
		// square may not leave the window or overlap another player's
		// square, Slide 7). Players are frozen once the round is over
		int int_new_x, int_new_y;
		if (!a_round->b_game_over &&
			game_try_move(arr_Players, MAX_PLAYERS, int_input_player_id,
			ch_input_dir, &int_new_x, &int_new_y))
		{
			// Index is player ID minus 1
			arr_Players[int_input_player_id - 1].x = int_new_x;
			arr_Players[int_input_player_id - 1].y = int_new_y;
		}
		pthread_mutex_unlock(&mutex);
	}
	if (rc < 0)
		DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
}

// Frees any slot whose first Input never arrived - see
// FIRST_INPUT_TIMEOUT_SECS
static void expire_silent_players(const ServerDds *a_dds, Round *a_round)
{
	// Collected under the lock, then freed outside it, since
	// handle_player_left() takes the lock itself
	bool arr_b_expired[MAX_PLAYERS] = {0};
	pthread_mutex_lock(&mutex);
	// Read under the lock, so it can never be earlier than a join time the
	// main thread just recorded
	dds_time_t now = dds_time();
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		arr_b_expired[i] = arr_Players[i].b_active && !arr_b_input_seen[i] &&
			now - arr_join_time[i] > DDS_SECS(FIRST_INPUT_TIMEOUT_SECS);
	}
	pthread_mutex_unlock(&mutex);

	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (arr_b_expired[i])
		{
			printf("Player %d sent no Input within %d seconds\n", i + 1,
				FIRST_INPUT_TIMEOUT_SECS);
			handle_player_left(i + 1, &a_round->grid, a_dds->cell_writer,
				a_dds->position_writer);
		}
	}
}

// Starts, ends and restarts the round, and publishes the timer/result
static void update_round(const ServerDds *a_dds, Round *a_round)
{
	dds_time_t now = dds_time();

	// Start the round once enough players have joined
	pthread_mutex_lock(&mutex);
	int int_num_active_temp = int_num_active_players;
	pthread_mutex_unlock(&mutex);
	if (!a_round->b_started && int_num_active_temp >= MIN_PLAYERS)
	{
		a_round->b_started = true;
		a_round->start_time = now;
		printf("Round started\n");
	}

	// End the round when the timer hits 0
	int int_seconds_left = ROUND_SECS;
	if (a_round->b_started)
		int_seconds_left = grid_seconds_left(a_round->start_time, now, ROUND_SECS);
	bool b_state_due = false;
	if (a_round->b_started && !a_round->b_game_over && int_seconds_left == 0)
	{
		a_round->b_game_over = true;
		a_round->game_over_time = now;
		a_round->int_winner_id = grid_winner(&a_round->grid, MAX_PLAYERS);
		printf("Round over, winner = %d\n", a_round->int_winner_id);
		// Do not wait for the next second to announce the result
		b_state_due = true;
	}

	// Automatically start a new round a few seconds after the result is
	// shown, instead of staying frozen until the server is restarted. The
	// "enough players have joined" check above re-starts the timer on the
	// next call
	if (a_round->b_game_over && now - a_round->game_over_time > DDS_SECS(INTERMISSION_SECS))
	{
		// Tell every client (and the transient-local cache CellClaimed uses
		// for late joiners) that any claimed cell is unclaimed again -
		// clearing just our own grid here would leave every client still
		// showing the old colours, since they only ever learn about the
		// grid through CellClaimed messages
		for (int int_row = 0; int_row < GRID_ROWS; int_row++)
		{
			for (int int_col = 0; int_col < GRID_COLS; int_col++)
			{
				if (grid_set_owner(&a_round->grid, int_col, int_row, GRID_UNCLAIMED))
					publish_cell_claimed(a_dds->cell_writer, int_col, int_row, GRID_UNCLAIMED);
			}
		}

		a_round->b_started = false;
		a_round->b_game_over = false;
		a_round->int_winner_id = 0;
		int_seconds_left = ROUND_SECS;

		// Give every active player a fresh, collision-free spawn. The new
		// positions go out with the next publish_changes()
		pthread_mutex_lock(&mutex);
		for (int i = 0; i < MAX_PLAYERS; i++)
		{
			if (arr_Players[i].b_active)
			{
				int int_rand_x, int_rand_y;
				game_random_spawn(arr_Players, MAX_PLAYERS, i + 1,
					&int_rand_x, &int_rand_y);
				arr_Players[i].x = int_rand_x;
				arr_Players[i].y = int_rand_y;
			}
		}
		pthread_mutex_unlock(&mutex);

		printf("Starting a new round\n");
		b_state_due = true;
	}

	// Publish the timer about once a second, and keep repeating the result
	// after the round ends so late clients still receive it
	if (b_state_due || now - a_round->last_state_time > DDS_SECS(1))
	{
		a_round->last_state_time = now;
		publish_game_state(a_dds->state_writer, int_seconds_left, a_round->b_started,
			a_round->b_game_over, a_round->int_winner_id);
	}
}

// Publishes the position of every active player whose position changed
// since it was last sent, and claims the cell under each player while the
// round runs. Positions are only sent for active players (Slide 10), and
// every client receives them (Slide 11, Figure 4b)
static void publish_changes(const ServerDds *a_dds, Round *a_round)
{
	// Snapshot the players under the lock, then encrypt and publish without
	// holding it, so the main thread is never kept waiting on DDS. Only
	// this thread moves players or frees slots, so the snapshot stays valid
	Player arr_snapshot[MAX_PLAYERS];
	pthread_mutex_lock(&mutex);
	memcpy(arr_snapshot, arr_Players, sizeof(arr_snapshot));
	pthread_mutex_unlock(&mutex);

	bool b_claiming = a_round->b_started && !a_round->b_game_over;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!arr_snapshot[i].b_active)
			continue;

		int int_x = arr_snapshot[i].x;
		int int_y = arr_snapshot[i].y;
		if (!arr_b_position_sent[i] || arr_sent_x[i] != int_x || arr_sent_y[i] != int_y)
		{
			if (publish_position(a_dds->position_writer, i + 1, int_x, int_y))
			{
				arr_b_position_sent[i] = true;
				arr_sent_x[i] = int_x;
				arr_sent_y[i] = int_y;
			}
		}

		// Claim the cell under this player, and tell the clients if it
		// changed hands
		int int_col, int_row;
		if (b_claiming && grid_claim(&a_round->grid, i + 1, int_x, int_y, &int_col, &int_row))
			publish_cell_claimed(a_dds->cell_writer, int_col, int_row, i + 1);
	}
}

// DDS worker thread (Slide 12): handles all DDS traffic - join requests and
// responses, input from clients, position/cell/timer updates to clients -
// so that a delayed message never stalls the main thread's game state
// maintenance
void *worker(void *arg)
{
	(void)arg;

	ServerDds dds;
	create_dds(&dds);

	Round round;
	memset(&round, 0, sizeof(round));
	grid_init(&round.grid);

	while (1)
	{
		// Sleep until a JoinRequest or Input arrives or the main thread has
		// a JoinResponse for us, or at most TICK_MSECS so the round timer
		// and timeouts still advance. Input is handled and the new position
		// published the moment it arrives, instead of polling every
		// millisecond and publishing on a 100 ms tick
		dds_return_t rc = dds_waitset_wait(dds.waitset, NULL, 0, DDS_MSECS(TICK_MSECS));
		if (rc < 0)
			DDS_FATAL("dds_waitset_wait: %s\n", dds_strretcode(-rc));
		// Reset the guard, so it only wakes us again for new responses
		bool b_guard_triggered;
		dds_take_guardcondition(response_guard, &b_guard_triggered);

		pthread_mutex_lock(&mutex);
		bool b_quit_temp = b_quit;
		pthread_mutex_unlock(&mutex);
		if (b_quit_temp)
			break;

		take_join_requests(&dds);
		send_join_responses(&dds);
		take_inputs(&dds, &round);
		expire_silent_players(&dds, &round);
		update_round(&dds, &round);
		publish_changes(&dds, &round);
	}

	// Deleting the participant removes our writers, so every client sees
	// the server's Position/CellClaimed instances lose their writer at once
	dds_delete(dds.participant);
	return NULL;
}

// ### MAIN THREAD ###

// Queues the JoinResponse for a_int_nonce. The caller must hold mutex
static void queue_join_response(int a_int_player_id, int a_int_nonce)
{
	if (int_response_queue_len < JOIN_QUEUE_LEN)
	{
		arr_response_queue[int_response_queue_len].int_player_id = a_int_player_id;
		arr_response_queue[int_response_queue_len].int_nonce = a_int_nonce;
		int_response_queue_len++;
	}
}

// Finds a slot for the authenticated join request a_int_nonce, spawns the
// new player there - see game_logic.h - and queues the answer. The caller
// must hold mutex
static void admit_join(int a_int_nonce)
{
	// A client resends its JoinRequest every second until answered, and
	// anyone can replay one they recorded, so the same nonce can arrive many
	// times. An active player's nonce gets its existing slot again rather
	// than a second one
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (arr_Players[i].b_active && arr_player_nonce[i] == a_int_nonce)
		{
			queue_join_response(i + 1, a_int_nonce);
			return;
		}
	}

	if (is_nonce_retired(a_int_nonce))
	{
		// A departed player's request being replayed: no response. Their
		// real client picks a new nonce if it ever rejoins
		printf("Join request ignored: nonce already used\n");
		return;
	}

	int int_slot = game_find_free_slot(arr_Players, MAX_PLAYERS);
	if (int_slot == -1)
	{
		// Game full: player ID 0
		queue_join_response(0, a_int_nonce);
		return;
	}

	int int_rand_x, int_rand_y;
	game_random_spawn(arr_Players, MAX_PLAYERS, int_slot + 1, &int_rand_x, &int_rand_y);

	// Set this index to active, at the new coords
	arr_Players[int_slot].b_active = true;
	arr_Players[int_slot].x = int_rand_x;
	arr_Players[int_slot].y = int_rand_y;
	int_num_active_players++;

	// Start this slot's session: its Input is bound to this nonce, the
	// previous occupant's replay state is dropped, and the first-Input
	// timer starts
	arr_player_nonce[int_slot] = a_int_nonce;
	arr_input_guard[int_slot] = (SecurityReplayGuard){0};
	arr_join_time[int_slot] = dds_time();
	arr_b_input_seen[int_slot] = false;

	// Player ID is the index plus 1
	queue_join_response(int_slot + 1, a_int_nonce);
}

// Signal thread: waits for Ctrl+C / SIGTERM, then tells the other threads
// to finish. The signals are blocked in every other thread, so they are
// only ever delivered here, where it is safe to lock the mutex and signal
// the condition variable (a normal signal handler could do neither)
static void *wait_for_quit(void *arg)
{
	sigset_t *ptr_signals = (sigset_t *)arg;
	int int_signal;
	sigwait(ptr_signals, &int_signal);

	pthread_mutex_lock(&mutex);
	b_quit = true;
	// Wake the main thread from its condition variable...
	pthread_cond_signal(&cond_join_queued);
	dds_entity_t guard = response_guard;
	pthread_mutex_unlock(&mutex);
	// ...and the worker from its waitset
	if (guard > 0)
		dds_set_guardcondition(guard, true);
	return NULL;
}

// Entry point of the application
int main(void)
{
	// Seed rand(), so that the random numbers are different for each run
	srand((unsigned int)time(NULL));

	if (!security_init())
	{
		fprintf(stderr, "security_init failed\n");
		return 1;
	}

	// Block Ctrl+C / SIGTERM here, before any other thread exists, so every
	// thread (ours and DDS's) inherits the block and only wait_for_quit()
	// receives them
	static sigset_t quit_signals;
	sigemptyset(&quit_signals);
	sigaddset(&quit_signals, SIGINT);
	sigaddset(&quit_signals, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &quit_signals, NULL);

	// The main thread keeps the game state (join handling) while this worker
	// thread does the DDS traffic (Slide 12)
	pthread_t thread;
	pthread_t signal_thread;
	if (pthread_create(&thread, NULL, worker, NULL) != 0 ||
		pthread_create(&signal_thread, NULL, wait_for_quit, &quit_signals) != 0)
	{
		perror("pthread_create");
		return 1;
	}

	printf("Waiting...\n");
	while (1)
	{
		// Sleep until the worker queues a join request (or we are told to
		// quit), rather than waking every millisecond to check for one
		pthread_mutex_lock(&mutex);
		while (int_join_queue_len == 0 && !b_quit)
			pthread_cond_wait(&cond_join_queued, &mutex);
		if (b_quit)
		{
			pthread_mutex_unlock(&mutex);
			break;
		}

		for (int i = 0; i < int_join_queue_len; i++)
			admit_join(arr_join_queue[i]);
		int_join_queue_len = 0;
		dds_entity_t guard = response_guard;
		pthread_mutex_unlock(&mutex);

		// Wake the worker to publish the answers
		dds_set_guardcondition(guard, true);
	}

	// Wait for the worker to delete its participant, so clients learn the
	// server has gone, then shut down
	pthread_join(thread, NULL);
	pthread_join(signal_thread, NULL);
	security_cleanup();
	printf("Server stopped\n");
	return 0;
}
