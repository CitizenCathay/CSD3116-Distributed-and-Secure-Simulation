/* gcc -o server server.c security.c game_logic.c grid_logic.c messages.c $(pkg-config --cflags --libs CycloneDDS) */

/* Dr Khoo Teck Ping
27 Aug 2026
1. DDS server
2. Supports max 5 clients
3. Start this server, then start client one by one
4. Each client can see itself and all other clients
5. Each client can move itself using the arrow keys
6. Collision detection among all players and the window is supported */

/* mutex lock/unlock must be matched
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
#define MAX_PLAYERS 5
// The round starts once this many players have joined
#define MIN_PLAYERS 2
// Length of a round in seconds
#define ROUND_SECS 60

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
static Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// Publishes that a cell changed owner (0 = unclaimed)
static void publish_cell_claimed(dds_entity_t a_writer, int a_int_col,
	int a_int_row, int a_int_owner_id)
{
	CellClaimed claim;
	claim.int_cell_x = a_int_col;
	claim.int_cell_y = a_int_row;
	claim.int_owner_id = a_int_owner_id;

	// Encrypt the payload before publishing - see security.h
	// len_cipher stays 0 (plaintext fields above are used instead) if
	// encryption is not available
	int arr_plain[3] = { a_int_col, a_int_row, a_int_owner_id };
	size_t cipher_len = 0;
	if (security_encrypt((unsigned char *)arr_plain, sizeof(arr_plain),
		claim.arr_cipher, &cipher_len) &&
		cipher_len <= sizeof(claim.arr_cipher))
	{
		claim.len_cipher = (unsigned long)cipher_len;
	}
	else
	{
		claim.len_cipher = 0;
	}

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

	// Encrypt the payload before publishing - see security.h
	int arr_plain[4] = { a_int_seconds_left, a_b_started, a_b_game_over,
		a_int_winner_id };
	size_t cipher_len = 0;
	if (security_encrypt((unsigned char *)arr_plain, sizeof(arr_plain),
		state.arr_cipher, &cipher_len) &&
		cipher_len <= sizeof(state.arr_cipher))
	{
		state.len_cipher = (unsigned long)cipher_len;
	}
	else
	{
		state.len_cipher = 0;
	}

	dds_return_t rc = dds_write(a_writer, &state);
	if (rc != DDS_RETCODE_OK)
	{
		DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
	}
}

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
	
	position_writer = dds_create_writer(
        participant,
        position_topic,
        NULL,
        NULL
    );

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

	// Round state: waiting for players -> running -> over
	bool b_round_started = false;
	bool b_game_over = false;
	int int_winner_id = 0;
	dds_time_t round_start_time = 0;
	dds_time_t last_state_time = 0;

	// Record the time now
	uint64_t last_time = dds_time();
	/* Poll until data has been read. */
	while (1)
	{
		// Process a join request
		rc = dds_take(join_reader, samples_join, infos_join, MAX_SAMPLES, MAX_SAMPLES);
		
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check for valid join request
		if ((rc > 0) && (infos_join[0].valid_data))
		{
			JoinRequest *join_req = (JoinRequest *) samples_join[0];
			// Authenticate the client before admitting it - see security.h
			if (security_verify_client_token(join_req->arr_token))
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
			
			rc = dds_write(response_writer, &response);
			if (rc != DDS_RETCODE_OK)
			{
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
			}
			printf("Join response sent\n");
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
			
			// Loop through the players list
			for(int i = 0; i < MAX_PLAYERS; i++)
			{
				pthread_mutex_lock(&mutex);
				// If player is active
				if(arr_Players[i].b_active == true)
				{
					position.int_player_id = i+1;
					position.int_x = arr_Players[i].x;
					position.int_y = arr_Players[i].y;

					// Encrypt the payload before publishing - see security.h
					// len_cipher stays 0 (plaintext fields above are used
					// instead) until security_encrypt() is implemented
					int arr_plain[2] = { position.int_x, position.int_y };
					size_t cipher_len = 0;
					if (security_encrypt((unsigned char *)arr_plain, sizeof(arr_plain),
						position.arr_cipher, &cipher_len) &&
						cipher_len <= sizeof(position.arr_cipher))
					{
						position.len_cipher = (unsigned long)cipher_len;
					}
					else
					{
						position.len_cipher = 0;
					}

					rc = dds_write(position_writer, &position);
					if (rc != DDS_RETCODE_OK)
					{
						DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
					}
					// printf("Position sent\n");

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
			int_winner_id = grid_winner(&grid, MAX_PLAYERS);
			printf("Round over, winner = %d\n", int_winner_id);
			// Do not wait for the next tick to announce the result
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
		
		// Listen for input
		rc = dds_take(input_reader, samples_input, infos_input, MAX_SAMPLES, MAX_SAMPLES);
		
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check if we read some data and it is valid.
		if ((rc > 0) && (infos_input[0].valid_data))
		{
			// printf("Input received\n");
			Input *input = (Input *) samples_input[0];
			// Get the input player ID
			int int_input_player_id = input->int_player_id;
			// Get the input direction
			char ch_input_dir = input->ch_direction;
			/* printf("%d=%c\n", int_input_player_id, ch_input_dir); */

			// Decrypt the payload if the client sent one - see security.h
			// Falls back to the plaintext fields above until
			// security_decrypt() is implemented (len_cipher will be 0)
			if (input->len_cipher > 0)
			{
				unsigned char arr_plain[sizeof(int) + sizeof(char)];
				size_t plain_len = 0;
				if (security_decrypt(input->arr_cipher, input->len_cipher,
					arr_plain, &plain_len) && plain_len == sizeof(arr_plain))
				{
					memcpy(&int_input_player_id, arr_plain, sizeof(int));
					memcpy(&ch_input_dir, arr_plain + sizeof(int), sizeof(char));
				}
			}
			
			// Compute the move and apply it if legal - see game_logic.h
			pthread_mutex_lock(&mutex);
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
	}
}

int main(void)
{
	// Seed rand(), so that the random numbers are different for each run
	srand((unsigned int)time(NULL));

	if (!security_init())
	{
		fprintf(stderr, "security_init failed\n");
		return 1;
	}

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
