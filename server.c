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
/* gcc -o server server.c security.c game_logic.c grid_logic.c messages.c $(pkg-config --cflags --libs CycloneDDS) */

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
// The baseline supports at most five clients (Slide 7)
#define MAX_PLAYERS 5
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

// ### GLOBAL VARIABLES ###
int int_num_active_players = 0;

// Flag set by worker thread that a player just joined
// Must be reset to false after processing by main thread
bool b_player_join = false;

// Player ID is the array index plus 1
// -1 = No new players
// 0 = Invalid player ID (unable to admit new players)
// 1~MAX_PLAYERS = Valid player IDs
int int_new_player_id = -1;

// Nonce of the join request being processed, and of the one the pending
// response is for, so each response can be matched to the client that asked
int int_join_nonce = 0;
int int_new_player_nonce = 0;

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

	printf("Player %d left\n", a_int_player_id);
}

// DDS worker thread (Slide 12): handles all DDS traffic - join requests and
// responses, input from clients, position/cell/timer updates to clients -
// so that a delayed message never stalls the main thread's game state
// maintenance
void *worker(void *arg)
{
	dds_entity_t participant;
	dds_entity_t join_topic;
	dds_entity_t response_topic;
	dds_entity_t position_topic;
	dds_entity_t input_topic;
	dds_entity_t cell_topic;
	dds_entity_t state_topic;
	dds_entity_t join_reader;
	dds_entity_t response_writer;
	dds_entity_t position_writer;
	dds_entity_t cell_writer;
	dds_entity_t state_writer;
	dds_entity_t input_reader;
        
	//Pointer to unknown type
	void *samples_join[MAX_SAMPLES];	
	dds_sample_info_t infos_join[MAX_SAMPLES];
	void *samples_input[MAX_SAMPLES];	
	dds_sample_info_t infos_input[MAX_SAMPLES];
    
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
    
	position_topic = dds_create_topic
	(
        participant,
        &Position_desc,
        "Position",
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
    /* Create readers                                         */
    /* ----------------------------------------------------- */

    join_reader = dds_create_reader(
        participant,
        join_topic,
        NULL,
        NULL
    );
    
	// Reliable, so a client's one "hello" Input is never silently dropped -
	// a slot whose first Input never arrives is freed after
	// FIRST_INPUT_TIMEOUT_SECS, which would kick an idle but connected player
	dds_qos_t *qos_input = dds_create_qos();
	dds_qset_reliability(qos_input, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	input_reader = dds_create_reader(
        participant,
        input_topic,
        qos_input,
        NULL
    );
	dds_delete_qos(qos_input);
    
	 /* ----------------------------------------------------- */
    /* Create writers                                         */
    /* ----------------------------------------------------- */

    response_writer = dds_create_writer(
        participant,
        response_topic,
        NULL,
        NULL
    );
    
	// Reliable, so a Position update is never silently dropped and left
	// trailing behind the (already-reliable) CellClaimed for the same move.
	// No autodispose, so unregistering a departed player's instance leaves
	// it not-alive-no-writers rather than disposed - clients ignore
	// disposes, since any participant could send one (client.c)
	dds_qos_t *qos_position = dds_create_qos();
	dds_qset_reliability(qos_position, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	dds_qset_writer_data_lifecycle(qos_position, false);
	position_writer = dds_create_writer(
        participant,
        position_topic,
        qos_position,
        NULL
    );
	dds_delete_qos(qos_position);

	// CellClaimed is keyed per cell: transient-local + keep-last-1 lets a
	// client that joins mid-round receive the current owner of every cell
	dds_qos_t *qos_cell = dds_create_qos();
	dds_qset_durability(qos_cell, DDS_DURABILITY_TRANSIENT_LOCAL);
	dds_qset_reliability(qos_cell, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	dds_qset_history(qos_cell, DDS_HISTORY_KEEP_LAST, 1);
	cell_writer = dds_create_writer(
        participant,
        cell_topic,
        qos_cell,
        NULL
    );
	dds_delete_qos(qos_cell);

	state_writer = dds_create_writer(
        participant,
        state_topic,
        NULL,
        NULL
    );

	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_join[0] = JoinRequest__alloc();
	samples_input[0] = Input__alloc();
    
	// Create a Join Response
	JoinResponse response;
    
	// Create a Position
	Position position;
    
	// Ground truth of who owns which cell; only touched by this thread
	Grid grid;
	grid_init(&grid);

	// Round state: waiting for players -> running -> over -> (after
	// INTERMISSION_SECS) waiting for players again
	bool b_round_started = false;
	bool b_game_over = false;
	int int_winner_id = 0;
	dds_time_t round_start_time = 0;
	dds_time_t game_over_time = 0;
	dds_time_t last_state_time = 0;

	// Record the time now
	uint64_t last_time = dds_time();
	/* Poll until data has been read. */
	while (1)
	{
		// Process a join request (Slide 11, Figure 4a: Join Request, then Join
		// Response carrying the new player's ID)
		rc = dds_take(join_reader, samples_join, infos_join, MAX_SAMPLES, MAX_SAMPLES);
        
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check for valid join request
		if ((rc > 0) && (infos_join[0].valid_data))
		{
			JoinRequest *join_req = (JoinRequest *) samples_join[0];
			// Authenticate the client before admitting it - see security.h
				// (a required security feature, Slide 14)
			if (security_verify_client_token(join_req->int_nonce, join_req->arr_token))
			{
				printf("Join request received (authenticated)\n");
				// Signal the main thread to check if
				// number of active players reached
				pthread_mutex_lock(&mutex);
				b_player_join = true;
				int_join_nonce = join_req->int_nonce;
				pthread_mutex_unlock(&mutex);
			}
			else
			{
				printf("Join request rejected: authentication failed\n");
			}
		}
        
		// If a new player ID has been assigned by the main thread
		pthread_mutex_lock(&mutex);
		int int_new_player_id_temp = int_new_player_id;
		int int_new_player_nonce_temp = int_new_player_nonce;
		/* printf("int_new_player_id_temp=%d\n", int_new_player_id_temp); */
		pthread_mutex_unlock(&mutex);
		if (int_new_player_id_temp != -1)
		{
			pthread_mutex_lock(&mutex);
			// Reset the player ID
			int_new_player_id = -1;
			pthread_mutex_unlock(&mutex);
			// Zeroed, so the unused tail of arr_cipher never carries stack
			// memory onto the network. The plaintext fields stay zero: the
			// response is only authentic/readable via the ciphertext
			memset(&response, 0, sizeof(response));

			// Encrypt the payload before publishing - see security.h. Lets
			// the client verify this response really came from a holder
			// of the shared key, not just something that claims to.
			// The player ID can be 0, ie invalid
			int arr_plain[2] = { int_new_player_id_temp, int_new_player_nonce_temp };
			size_t cipher_len = 0;
			if (security_encrypt(SECURITY_MSG_JOIN_RESPONSE, NULL, 0,
				(unsigned char *)arr_plain, sizeof(arr_plain),
				response.arr_cipher, &cipher_len) &&
				cipher_len <= sizeof(response.arr_cipher))
			{
				response.len_cipher = (unsigned long)cipher_len;

				rc = dds_write(response_writer, &response);
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
				printf("Join response sent\n");
			}
			else
			{
				// Clients drop unencrypted responses, so don't send one
				fprintf(stderr, "Join response not sent: encryption failed\n");
			}
			/* Polling sleep. */
			dds_sleepfor (DDS_MSECS (20));
		}
        
		// Get the current time
		uint64_t curr_time = dds_time();
		// Get the time difference
		if (curr_time - last_time > DDS_SECS(0.1))
		{
			// Update the last time
			last_time = curr_time;
            
			// Loop through the players list. Positions are only sent for active
			// players (Slide 10), and every client receives them (Slide 11,
			// Figure 4b)
			for(int i = 0; i < MAX_PLAYERS; i++)
			{
				pthread_mutex_lock(&mutex);
				// If player is active
				if(arr_Players[i].b_active == true)
				{
					// Zeroed, so the unused tail of arr_cipher never
					// carries stack memory onto the network. int_x/int_y
					// stay zero: the position is only readable from the
					// ciphertext
					memset(&position, 0, sizeof(position));
					position.int_player_id = i+1;

					// Encrypt the payload before publishing - see security.h.
					// Clients drop anything without a valid cipher, so skip
					// sending if encryption fails. The cipher is bound to
					// int_player_id (the DDS key, which stays in the clear
					// for routing/leave detection), so it cannot be
					// replayed as another player's position
					int arr_bound[1] = { i+1 };
					int arr_plain[2] = { arr_Players[i].x, arr_Players[i].y };
					size_t cipher_len = 0;
					if (security_encrypt(SECURITY_MSG_POSITION, arr_bound, 1,
						(unsigned char *)arr_plain, sizeof(arr_plain),
						position.arr_cipher, &cipher_len) &&
						cipher_len <= sizeof(position.arr_cipher))
					{
						position.len_cipher = (unsigned long)cipher_len;

						rc = dds_write(position_writer, &position);
						if (rc != DDS_RETCODE_OK)
						{
							DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
						}
						// printf("Position sent\n");
					}
					else
					{
						fprintf(stderr, "Position not sent: encryption failed\n");
					}

					// Claim the cell under this player while the round runs,
					// and tell the clients if it changed hands
					if (b_round_started && !b_game_over)
					{
						int int_col, int_row;
						if (grid_claim(&grid, i+1, arr_Players[i].x,
							arr_Players[i].y, &int_col, &int_row))
						{
							publish_cell_claimed(cell_writer, int_col, int_row, i+1);
						}
					}
					/* Polling sleep. */
					dds_sleepfor (DDS_MSECS (20));
				}
				pthread_mutex_unlock(&mutex);
			}

			// Free any slot whose first Input never arrived - see
			// FIRST_INPUT_TIMEOUT_SECS. Collected under the lock, then freed
			// outside it, since handle_player_left() takes the lock itself
			bool arr_b_expired[MAX_PLAYERS] = {0};
			pthread_mutex_lock(&mutex);
			// Read under the lock (not curr_time), so it can never be
			// earlier than a join time the main thread just recorded
			dds_time_t join_check_time = dds_time();
			for (int i = 0; i < MAX_PLAYERS; i++)
			{
				arr_b_expired[i] = arr_Players[i].b_active && !arr_b_input_seen[i] &&
					join_check_time - arr_join_time[i] > DDS_SECS(FIRST_INPUT_TIMEOUT_SECS);
			}
			pthread_mutex_unlock(&mutex);
			for (int i = 0; i < MAX_PLAYERS; i++)
			{
				if (arr_b_expired[i])
				{
					printf("Player %d sent no Input within %d seconds\n", i + 1,
						FIRST_INPUT_TIMEOUT_SECS);
					handle_player_left(i + 1, &grid, cell_writer, position_writer);
				}
			}
		}

		// Yield the CPU, otherwise this loop spins at 100%
		dds_sleepfor(DDS_MSECS(1));

		// Start the round once enough players have joined
		pthread_mutex_lock(&mutex);
		int int_num_active_temp = int_num_active_players;
		pthread_mutex_unlock(&mutex);
		if (!b_round_started && int_num_active_temp >= MIN_PLAYERS)
		{
			b_round_started = true;
			round_start_time = dds_time();
			printf("Round started\n");
		}

		// End the round when the timer hits 0
		int int_seconds_left = ROUND_SECS;
		if (b_round_started)
		{
			int_seconds_left = grid_seconds_left(round_start_time, dds_time(),
				ROUND_SECS);
		}
		bool b_state_due = false;
		if (b_round_started && !b_game_over && int_seconds_left == 0)
		{
			b_game_over = true;
			game_over_time = dds_time();
			int_winner_id = grid_winner(&grid, MAX_PLAYERS);
			printf("Round over, winner = %d\n", int_winner_id);
			// Do not wait for the next tick to announce the result
			b_state_due = true;
		}

		// Automatically start a new round a few seconds after the result is
		// shown, instead of staying frozen until the server is restarted.
		// The usual "enough players have joined" check above then re-starts
		// the timer on the next iteration
		if (b_game_over && dds_time() - game_over_time > DDS_SECS(INTERMISSION_SECS))
		{
			// Tell every client (and the transient-local cache CellClaimed
			// uses for late joiners) that any claimed cell is unclaimed
			// again - clearing just our own grid here would leave every
			// client still showing the old colours, since they only ever
			// learn about the grid through CellClaimed messages
			for (int int_row = 0; int_row < GRID_ROWS; int_row++)
			{
				for (int int_col = 0; int_col < GRID_COLS; int_col++)
				{
					if (grid_set_owner(&grid, int_col, int_row, GRID_UNCLAIMED))
						publish_cell_claimed(cell_writer, int_col, int_row, GRID_UNCLAIMED);
				}
			}

			b_round_started = false;
			b_game_over = false;
			int_winner_id = 0;
			int_seconds_left = ROUND_SECS;

			// Give every active player a fresh, collision-free spawn
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

		// Publish the timer about once a second, and keep repeating the
		// result after the round ends so late clients still receive it
		dds_time_t state_now = dds_time();
		if (b_state_due || state_now - last_state_time > DDS_SECS(1))
		{
			last_state_time = state_now;
			publish_game_state(state_writer, int_seconds_left, b_round_started,
				b_game_over, int_winner_id);
		}
        
		// Listen for input (Slide 11, Figure 4b: the client sends Input, the
		// server checks collisions and updates all positions). Drain them
		// all, since several clients' inputs can be pending at once - each
		// is now its own keyed instance (messages.idl), but a single take
		// per iteration could still let a backlog build up under load
		while ((rc = dds_take(input_reader, samples_input, infos_input, MAX_SAMPLES, MAX_SAMPLES)) > 0)
		{
			if (!infos_input[0].valid_data)
			{
				// A no-writers Input instance means that client's DDS writer is
				// gone (window closed, or the process died), so free their slot.
				// A disposed instance is ignored: any DDS participant can
				// dispose another player's instance, which would let it kick them
				if (infos_input[0].instance_state == DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE)
				{
					handle_player_left(((Input *) samples_input[0])->int_player_id,
						&grid, cell_writer, position_writer);
				}
				continue;
			}

			// printf("Input received\n");
			Input *input = (Input *) samples_input[0];
			// Get the input player ID - the plaintext DDS key, authenticated
			// below as part of the cipher's associated data
			int int_input_player_id = input->int_player_id;
			/* printf("%d=%c\n", int_input_player_id, ch_input_dir); */

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

			// Compute the move and apply it if legal - see game_logic.h
				// (a square may not leave the window or overlap another
				// player's square, Slide 7)
			int int_new_x, int_new_y;
			bool b_moved = game_try_move(arr_Players, MAX_PLAYERS,
				int_input_player_id, ch_input_dir, &int_new_x, &int_new_y);
			// Players are frozen once the round is over
			if (b_moved && !b_game_over)
			{
				// Index is player ID minus 1
				arr_Players[int_input_player_id-1].x = int_new_x;
				arr_Players[int_input_player_id-1].y = int_new_y;
			}
			pthread_mutex_unlock(&mutex);
			/* printf("input->int_player_id = %d\n", input->int_player_id);
			printf("input->dir = %c\n", input->ch_direction); */
		}
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
	}
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

	// The main thread keeps the game state (join handling) while this worker
	// thread does the DDS traffic (Slide 12)
	pthread_t thread;

    if (pthread_create(&thread, NULL, worker, NULL) != 0)
    {
        perror("pthread_create");
        return 1;
    }
    
	printf("Waiting...\n");
	while (1)
	{
		// for(int i = 0; i < MAX_PLAYERS; i++)
		// {
			//printf("%d %d %d\n",i,arr_Players[i].x, arr_Players[i].y);
		// }
		//printf("============\n");
		/* dds_sleepfor (DDS_SECS (0.5)); */
                    
		// Yield the CPU, otherwise this loop spins at 100%
		dds_sleepfor(DDS_MSECS(1));

		// If a join request has been received
		pthread_mutex_lock(&mutex);
		bool b_player_join_temp = b_player_join;
		pthread_mutex_unlock(&mutex);
		if (b_player_join_temp == true)
		{
			// printf("b_player_join_temp\n");
			// Reset the flag, then find a free slot and spawn the new
			// player - see game_logic.h
			pthread_mutex_lock(&mutex);
			b_player_join = false;
			int_new_player_nonce = int_join_nonce;

			// A client resends its JoinRequest every second until answered,
			// and anyone can replay one they recorded, so the same nonce can
			// arrive many times. An active player's nonce gets its existing
			// slot again rather than a second one
			int int_existing_slot = -1;
			for (int i = 0; i < MAX_PLAYERS; i++)
			{
				if (arr_Players[i].b_active && arr_player_nonce[i] == int_join_nonce)
					int_existing_slot = i;
			}

			int int_slot = game_find_free_slot(arr_Players, MAX_PLAYERS);
			if (int_existing_slot != -1)
			{
				int_new_player_id = int_existing_slot + 1;
			}
			else if (is_nonce_retired(int_join_nonce))
			{
				// A departed player's request being replayed: no response.
				// Their real client picks a new nonce if it ever rejoins
				printf("Join request ignored: nonce already used\n");
			}
			else if (int_slot == -1)
			{
				// Set invalid player ID
				int_new_player_id = 0;
			}
			else
			{
				int int_rand_x, int_rand_y;
				game_random_spawn(arr_Players, MAX_PLAYERS, int_slot+1,
					&int_rand_x, &int_rand_y);

				// Set this index to active
				arr_Players[int_slot].b_active = true;
				// Increment the number of active players
				int_num_active_players++;
				// Set the coords
				arr_Players[int_slot].x = int_rand_x;
				arr_Players[int_slot].y = int_rand_y;

				// Start this slot's session: its Input is bound to this
				// nonce, the previous occupant's replay state is dropped,
				// and the first-Input timer starts
				arr_player_nonce[int_slot] = int_join_nonce;
				arr_input_guard[int_slot] = (SecurityReplayGuard){0};
				arr_join_time[int_slot] = dds_time();
				arr_b_input_seen[int_slot] = false;

				// Set player ID to be the index plus 1
				int_new_player_id = int_slot+1;
			}
			pthread_mutex_unlock(&mutex);
		}
	}

	security_cleanup(); // unreachable while the loop above has no exit
	return 0;
}
