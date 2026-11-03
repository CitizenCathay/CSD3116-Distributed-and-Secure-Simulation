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
// Guards the variables shared by the main thread and the DDS worker thread,
// so neither reads a value while the other is changing it. Every lock must
// be matched by an unlock (Slide 12)
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// Publishes that a cell changed owner (0 = unclaimed)
static void publish_cell_claimed(dds_entity_t a_writer, int a_int_col,
	int a_int_row, int a_int_owner_id)
{
	CellClaimed claim;
	claim.int_cell_x = a_int_col;
	claim.int_cell_y = a_int_row;
	claim.int_owner_id = a_int_owner_id;

	// Encrypt the payload before publishing - see security.h. Clients drop
	// anything without a valid cipher, so if encryption ever fails there is
	// no point sending (and leaking) the plaintext
	int arr_plain[3] = { a_int_col, a_int_row, a_int_owner_id };
	size_t cipher_len = 0;
	if (!security_encrypt((unsigned char *)arr_plain, sizeof(arr_plain),
		claim.arr_cipher, &cipher_len) ||
		cipher_len > sizeof(claim.arr_cipher))
	{
		fprintf(stderr, "CellClaimed not sent: encryption failed\n");
		return;
	}
	claim.len_cipher = (unsigned long)cipher_len;
	// Blank the plaintext owner, so it is only readable from the
	// ciphertext. int_cell_x/y stay in the clear - they are the DDS
	// key this instance is routed by, not secret payload
	claim.int_owner_id = GRID_UNCLAIMED;

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
	GameState state;
	state.int_seconds_left = a_int_seconds_left;
	state.b_started = a_b_started;
	state.b_game_over = a_b_game_over;
	state.int_winner_id = a_int_winner_id;

	// Encrypt the payload before publishing - see security.h. Clients drop
	// anything without a valid cipher, so skip sending if encryption fails
	int arr_plain[4] = { a_int_seconds_left, a_b_started, a_b_game_over,
		a_int_winner_id };
	size_t cipher_len = 0;
	if (!security_encrypt((unsigned char *)arr_plain, sizeof(arr_plain),
		state.arr_cipher, &cipher_len) ||
		cipher_len > sizeof(state.arr_cipher))
	{
		fprintf(stderr, "GameState not sent: encryption failed\n");
		return;
	}
	state.len_cipher = (unsigned long)cipher_len;
	// GameState has no DDS key, so every field here is payload -
	// blank all of them; it is only readable from the ciphertext
	state.int_seconds_left = 0;
	state.b_started = false;
	state.b_game_over = false;
	state.int_winner_id = 0;

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
    
	input_reader = dds_create_reader(
        participant,
        input_topic,
        NULL,
        NULL
    );
    
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
			// The player ID can be 0, ie invalid
			response.int_player_id = int_new_player_id_temp;
			response.int_nonce = int_new_player_nonce_temp;

			// Encrypt the payload before publishing - see security.h. Lets
			// the client verify this response really came from a holder
			// of the shared key, not just something that claims to
			int arr_plain[2] = { response.int_player_id, response.int_nonce };
			size_t cipher_len = 0;
			if (security_encrypt((unsigned char *)arr_plain, sizeof(arr_plain),
				response.arr_cipher, &cipher_len) &&
				cipher_len <= sizeof(response.arr_cipher))
			{
				response.len_cipher = (unsigned long)cipher_len;
				// Blank the plaintext copy, so the response is only
				// authentic/readable via the ciphertext
				response.int_player_id = 0;
				response.int_nonce = 0;

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
					position.int_player_id = i+1;
					position.int_x = arr_Players[i].x;
					position.int_y = arr_Players[i].y;

					// Encrypt the payload before publishing - see security.h.
					// Clients drop anything without a valid cipher, so skip
					// sending if encryption fails
					int arr_plain[2] = { position.int_x, position.int_y };
					size_t cipher_len = 0;
					if (security_encrypt((unsigned char *)arr_plain, sizeof(arr_plain),
						position.arr_cipher, &cipher_len) &&
						cipher_len <= sizeof(position.arr_cipher))
					{
						position.len_cipher = (unsigned long)cipher_len;
						// Blank the plaintext coords, so position is only
						// readable from the ciphertext. int_player_id stays
						// in the clear - it is the DDS key this instance is
						// routed/leave-detected by, not secret payload
						position.int_x = 0;
						position.int_y = 0;

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
			// Get the input player ID
			int int_input_player_id = input->int_player_id;
			// Get the input direction
			char ch_input_dir = input->ch_direction;
			/* printf("%d=%c\n", int_input_player_id, ch_input_dir); */

			// Decrypt the payload - see security.h. Input without a valid
			// cipher is dropped, never read from the plaintext fields above,
			// or anyone on the DDS domain could move any player without the key
			unsigned char arr_plain[sizeof(int) + sizeof(char)];
			size_t plain_len = 0;
			if (input->len_cipher == 0 ||
				!security_decrypt(input->arr_cipher, input->len_cipher,
				arr_plain, sizeof(arr_plain), &plain_len) ||
				plain_len != sizeof(arr_plain))
			{
				continue;
			}
			memcpy(&int_input_player_id, arr_plain, sizeof(int));
			memcpy(&ch_input_dir, arr_plain + sizeof(int), sizeof(char));
            
			// Ignore input from an unknown player ID, since it indexes arr_Players
			if (int_input_player_id < 1 || int_input_player_id > MAX_PLAYERS)
				continue;

			// Compute the move and apply it if legal - see game_logic.h
				// (a square may not leave the window or overlap another
				// player's square, Slide 7)
			pthread_mutex_lock(&mutex);
			int int_new_x, int_new_y;
			bool b_moved = arr_Players[int_input_player_id - 1].b_active &&
				game_try_move(arr_Players, MAX_PLAYERS,
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
			int int_slot = game_find_free_slot(arr_Players, MAX_PLAYERS);
			if (int_slot == -1)
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

				// Set player ID to be the index plus 1
				int_new_player_id = int_slot+1;
			}
			pthread_mutex_unlock(&mutex);
		}
	}

	security_cleanup(); // unreachable while the loop above has no exit
	return 0;
}
