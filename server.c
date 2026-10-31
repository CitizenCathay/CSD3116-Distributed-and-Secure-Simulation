/* gcc -o server server.c security.c game_logic.c messages.c $(pkg-config --cflags --libs CycloneDDS) */

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

// ### GLOBAL CONSTANTS ###
/* An array of one message (aka sample in dds terms) will be used. */
#define MAX_SAMPLES 1
#define MAX_PLAYERS 5

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

// Table to track clients
// Init all members to 0
static Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void *worker(void *arg)
{
	dds_entity_t participant;
	dds_entity_t join_topic;
	dds_entity_t response_topic;
	dds_entity_t position_topic;
	dds_entity_t input_topic;
	dds_entity_t join_reader;
	dds_entity_t response_writer;
	dds_entity_t position_writer;
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
	
	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_join[0] = JoinRequest__alloc();
	samples_input[0] = Input__alloc();
	
	// Create a Join Response
	JoinResponse response;
	
	// Create a Position
	Position position;
	
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
					printf("Position sent\n");
					/* Polling sleep. */
					dds_sleepfor (DDS_MSECS (20));
				}
				pthread_mutex_unlock(&mutex);
			}
		}
		
		// Listen for input
		rc = dds_take(input_reader, samples_input, infos_input, MAX_SAMPLES, MAX_SAMPLES);
		
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check if we read some data and it is valid.
		if ((rc > 0) && (infos_input[0].valid_data))
		{
			printf("Input received\n");
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
			if (b_moved)
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
