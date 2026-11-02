/* gcc client.c security.c game_logic.c grid_logic.c messages.c -o client $(pkg-config --cflags --libs glfw3) -lGL $(pkg-config --cflags --libs CycloneDDS) -pthread -lm*/

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
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include "dds/dds.h"
#include "messages.h"
#include "security.h"
#include "game_logic.h"
#include "grid_logic.h"
#include "stb_easy_font.h"

#define STB_EASY_FONT_IMPLEMENTATION

#define MAX_SAMPLES 1
#define MAX_PLAYERS 5

// One RGB colour per player, indexed by player ID minus 1
static const float arr_colors[MAX_PLAYERS][3] =
{
	{ 0.90f, 0.20f, 0.20f },
	{ 0.20f, 0.45f, 0.90f },
	{ 0.20f, 0.70f, 0.30f },
	{ 0.95f, 0.75f, 0.10f },
	{ 0.65f, 0.30f, 0.80f }
};

// ### GLOBAL VARIABLES ###
int int_player_id = -1;

// Table to track clients
// Init all members to 0
static Player arr_Players[MAX_PLAYERS] = {0};

// Local copy of who owns which cell, kept in sync from the server's
// CellClaimed messages. Shared with the main thread, so guarded by mutex
static Grid grid;

// Latest round state from the server's GameState messages. Guarded by mutex
static int int_seconds_left = 0;
static bool b_round_started = false;
static bool b_game_over = false;
static int int_winner_id = 0;

// Direction - n means no direction
char ch_dir = 'n';
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void draw_text(float x, float y, float scale, const char *text)
{
    char buffer[4096];

    int num_quads = stb_easy_font_print(
        0, 0,
        (char *)text,
        NULL,
        buffer,
        sizeof(buffer)
    );

    glPushMatrix();

    glTranslatef(x, y, 0.0f);
    glScalef(scale, scale, 1.0f);

    glEnableClientState(GL_VERTEX_ARRAY);

    glVertexPointer(2, GL_FLOAT, 16, buffer);
    glDrawArrays(GL_QUADS, 0, num_quads * 4);

    glDisableClientState(GL_VERTEX_ARRAY);

    glPopMatrix();
}

void *worker(void *arg)
{
	dds_entity_t participant;
	dds_entity_t join_topic;
	dds_entity_t response_topic;
	dds_entity_t input_topic;
	dds_entity_t position_topic;
	dds_entity_t cell_topic;
	dds_entity_t state_topic;
	dds_entity_t response_reader;
	dds_entity_t position_reader;
	dds_entity_t cell_reader;
	dds_entity_t state_reader;
	dds_entity_t join_writer;
	dds_entity_t input_writer;

	//Pointer to unknown type
	void *samples_response[MAX_SAMPLES];
	dds_sample_info_t infos_response[MAX_SAMPLES];
	void *samples_position[MAX_SAMPLES];
	dds_sample_info_t infos_position[MAX_SAMPLES];
	void *samples_cell[MAX_SAMPLES];
	dds_sample_info_t infos_cell[MAX_SAMPLES];
	void *samples_state[MAX_SAMPLES];
	dds_sample_info_t infos_state[MAX_SAMPLES];
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
	
	input_topic = dds_create_topic
	(
        participant,
        &Input_desc,
        "Input",
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
    /* Create writers                                         */
    /* ----------------------------------------------------- */

    join_writer = dds_create_writer(
        participant,
        join_topic,
        NULL,
        NULL
    );
	
	input_writer = dds_create_writer(
        participant,
        input_topic,
        NULL,
        NULL
    );
	
	/* ----------------------------------------------------- */
    /* Create readers                                         */
    /* ----------------------------------------------------- */

    response_reader = dds_create_reader(
        participant,
        response_topic,
        NULL,
        NULL
    );
	
	position_reader = dds_create_reader(
        participant,
        position_topic,
        NULL,
        NULL
    );

	// Must match the server's QoS for CellClaimed, so that the history of
	// claimed cells is delivered to a client that joins mid-round
	dds_qos_t *qos_cell = dds_create_qos();
	dds_qset_durability(qos_cell, DDS_DURABILITY_TRANSIENT_LOCAL);
	dds_qset_reliability(qos_cell, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	dds_qset_history(qos_cell, DDS_HISTORY_KEEP_LAST, 1);
	cell_reader = dds_create_reader(
        participant,
        cell_topic,
        qos_cell,
        NULL
    );
	dds_delete_qos(qos_cell);

	state_reader = dds_create_reader(
        participant,
        state_topic,
        NULL,
        NULL
    );

	JoinRequest request;
    request.b_dummy = true;
	// Tag the request, so only the response to it is accepted below. Mixing
	// in the process ID keeps clients started in the same second apart
	srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
	request.int_nonce = rand();
	// Fill the authentication credential - see security.h
	security_make_client_token(request.arr_token, sizeof(request.arr_token));
	
	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_response[0] = JoinResponse__alloc();
	
	samples_position[0] = Position__alloc();
	samples_cell[0] = CellClaimed__alloc();
	samples_state[0] = GameState__alloc();

	// Record the time now
	uint64_t last_join_req_time = dds_time();
	/* printf("last_join_req_time = %" PRIu64 "\n", last_join_req_time); */
	while (1)
	{
		// If not in a game yet
		pthread_mutex_lock(&mutex);
		int int_player_id_temp = int_player_id;
		pthread_mutex_unlock(&mutex);
		if (int_player_id_temp == -1)
		{
			//Listen for server join response
			rc = dds_take(response_reader, samples_response, infos_response, MAX_SAMPLES, MAX_SAMPLES);
			
			if (rc < 0)
				DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

			/* Check if we read some data and it is valid. */
			if ((rc > 0) && (infos_response[0].valid_data))
			{
				printf("Join response received\n");
				JoinResponse *response = (JoinResponse *) samples_response[0];
				// Responses are broadcast: ignore the ones meant for other clients
					if (response->int_nonce != request.int_nonce)
						continue;
					int int_resp_player_id = response->int_player_id;
				// If server responded with an invalid player ID
				if (int_resp_player_id == 0)
					printf("Unable to join\n");
				else
				{
					// printf("Join response player ID = %d\n", response->int_player_id);
					pthread_mutex_lock(&mutex);
					// Update player ID
					int_player_id = int_resp_player_id;
						printf("Joined as player %d\n", int_resp_player_id);
					pthread_mutex_unlock(&mutex);
				}
			}
			else
			{
				/* Polling sleep. */
				dds_sleepfor (DDS_MSECS (20));
			}
			
			// Send request every 1s
			// Get the current time
			uint64_t curr_time = dds_time();
			/* printf("curr_time = %" PRIu64 "\n", curr_time); */
			// Get the time difference
			if (curr_time - last_join_req_time > DDS_SECS(1))
			{
				// Update the last req time
				last_join_req_time = curr_time;
				
				// Send join request
				rc = dds_write(join_writer, &request);
				printf("Join request sent\n");
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
			}
		}
		
		// Client is an active player
		else
		{
			pthread_mutex_lock(&mutex);
			char ch_dir_temp = ch_dir;
			pthread_mutex_unlock(&mutex);
			// If there is some direction input
			if(ch_dir_temp != 'n')
			{
				// Create a fresh input
				Input input;
				input.int_player_id = int_player_id;

				// Set the direction in the message
				input.ch_direction = ch_dir_temp;

				// Encrypt the payload before publishing - see security.h
				// len_cipher stays 0 (plaintext fields above are used
				// instead) until security_encrypt() is implemented
				unsigned char arr_plain[sizeof(int) + sizeof(char)];
				memcpy(arr_plain, &input.int_player_id, sizeof(int));
				memcpy(arr_plain + sizeof(int), &input.ch_direction, sizeof(char));
				size_t cipher_len = 0;
				if (security_encrypt(arr_plain, sizeof(arr_plain),
					input.arr_cipher, &cipher_len) &&
					cipher_len <= sizeof(input.arr_cipher))
				{
					input.len_cipher = (unsigned long)cipher_len;
				}
				else
				{
					input.len_cipher = 0;
				}

				// Send input
				rc = dds_write(input_writer, &input);
				// printf("curr_time = %" PRIu64 "\n", dds_time());
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
				// printf("Input sent\n");
				// Need to slow down how often input gets sent, otherwise, 
				//the player will move to the window limits immediately
				dds_sleepfor(DDS_MSECS(10));
			}
		}

		// Yield the CPU, otherwise this loop spins at 100% and starves the
		// renderer and the desktop
		dds_sleepfor(DDS_MSECS(1));

		// Listen for claimed cells. Drain them all, since a client that
		// has just joined is handed one sample per claimed cell at once
		while ((rc = dds_take(cell_reader, samples_cell, infos_cell, MAX_SAMPLES, MAX_SAMPLES)) > 0)
		{
			if (!infos_cell[0].valid_data)
				continue;

			CellClaimed *claim = (CellClaimed *) samples_cell[0];
			int int_cell_x = claim->int_cell_x;
			int int_cell_y = claim->int_cell_y;
			int int_owner_id = claim->int_owner_id;

			// Decrypt the payload if the server sent one - see security.h
			// A message that fails authentication is dropped
			if (claim->len_cipher > 0)
			{
				unsigned char arr_plain[SECURITY_CIPHERTEXT_LEN];
				size_t plain_len = 0;
				if (!security_decrypt(claim->arr_cipher, claim->len_cipher,
					arr_plain, &plain_len) || plain_len != sizeof(int) * 3)
				{
					continue;
				}
				memcpy(&int_cell_x, arr_plain, sizeof(int));
				memcpy(&int_cell_y, arr_plain + sizeof(int), sizeof(int));
				memcpy(&int_owner_id, arr_plain + sizeof(int) * 2, sizeof(int));
			}

			// Player IDs are 1..MAX_PLAYERS, 0 = unclaimed
			if (int_owner_id < 0 || int_owner_id > MAX_PLAYERS)
				continue;

			pthread_mutex_lock(&mutex);
			grid_set_owner(&grid, int_cell_x, int_cell_y, int_owner_id);
			pthread_mutex_unlock(&mutex);
		}
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Listen for the round timer / result
		while ((rc = dds_take(state_reader, samples_state, infos_state, MAX_SAMPLES, MAX_SAMPLES)) > 0)
		{
			if (!infos_state[0].valid_data)
				continue;

			GameState *state = (GameState *) samples_state[0];
			int int_state_secs = state->int_seconds_left;
			int int_state_started = state->b_started;
			int int_state_over = state->b_game_over;
			int int_state_winner = state->int_winner_id;

			// Decrypt the payload if the server sent one - see security.h
			// A message that fails authentication is dropped
			if (state->len_cipher > 0)
			{
				unsigned char arr_plain[SECURITY_CIPHERTEXT_LEN];
				size_t plain_len = 0;
				if (!security_decrypt(state->arr_cipher, state->len_cipher,
					arr_plain, &plain_len) || plain_len != sizeof(int) * 4)
				{
					continue;
				}
				memcpy(&int_state_secs, arr_plain, sizeof(int));
				memcpy(&int_state_started, arr_plain + sizeof(int), sizeof(int));
				memcpy(&int_state_over, arr_plain + sizeof(int) * 2, sizeof(int));
				memcpy(&int_state_winner, arr_plain + sizeof(int) * 3, sizeof(int));
			}

			pthread_mutex_lock(&mutex);
			int_seconds_left = int_state_secs;
			b_round_started = int_state_started != 0;
			b_game_over = int_state_over != 0;
			int_winner_id = int_state_winner;
			pthread_mutex_unlock(&mutex);
		}
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Listen for position
		rc = dds_take(position_reader, samples_position, infos_position, MAX_SAMPLES, MAX_SAMPLES);
		
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check if we read some data and it is valid.
		if ((rc > 0) && (infos_position[0].valid_data))
		{
			// printf("Position received\n");
			Position *position = (Position *) samples_position[0];
			// Get the player ID
			int int_position_player_id = position->int_player_id;
			int int_position_x = position->int_x;
			int int_position_y = position->int_y;

			// Decrypt the payload if the server sent one - see security.h
			// Falls back to the plaintext fields above until
			// security_decrypt() is implemented (len_cipher will be 0)
			if (position->len_cipher > 0)
			{
				unsigned char arr_plain[sizeof(int) * 2];
				size_t plain_len = 0;
				if (security_decrypt(position->arr_cipher, position->len_cipher,
					arr_plain, &plain_len) && plain_len == sizeof(arr_plain))
				{
					memcpy(&int_position_x, arr_plain, sizeof(int));
					memcpy(&int_position_y, arr_plain + sizeof(int), sizeof(int));
				}
			}

			pthread_mutex_lock(&mutex);
			// Set this player to active
			arr_Players[int_position_player_id-1].b_active = true;
			// Set this player's x
			arr_Players[int_position_player_id-1].x = int_position_x;
			// Set this player's y
			arr_Players[int_position_player_id-1].y = int_position_y;
			pthread_mutex_unlock(&mutex);
		}
	}
}

int main(void)
{
	if (!security_init())
	{
		fprintf(stderr, "security_init failed\n");
		return 1;
	}

	grid_init(&grid);

	pthread_t thread;

    if (pthread_create(&thread, NULL, worker, NULL) != 0)
    {
        perror("pthread_create");
        return 1;
    }
	
	
    if (!glfwInit())
        return 1;

    GLFWwindow *window =
        glfwCreateWindow(WIDTH, HEIGHT, "DDS Client (Click within the window to control)", NULL, NULL);

    if (!window)
    {
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
	// Wait for the display refresh instead of redrawing as fast as possible
	glfwSwapInterval(1);

    /* Use pixel coordinates */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* glOrtho(0, WIDTH, 0, HEIGHT, -1, 1); */
	glOrtho(0, WIDTH, HEIGHT, 0, -1, 1);

  /*   glMatrixMode(GL_MODELVIEW);
    glLoadIdentity(); */

    while (!glfwWindowShouldClose(window))
    {
		 /* Process keyboard/window events */
        glfwPollEvents();

        /* Non-blocking keyboard input */
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'u';
			pthread_mutex_unlock(&mutex);
		}
		
        else if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'd';
			pthread_mutex_unlock(&mutex);
		}
		
		else if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'l';
			pthread_mutex_unlock(&mutex);
		}
		
		else if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'r';
			pthread_mutex_unlock(&mutex);
		}
		
		else
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'n';
			pthread_mutex_unlock(&mutex);
		}
		
		glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
		// Take a consistent copy of the shared state under one lock, then
			// draw from it without holding the lock
			pthread_mutex_lock(&mutex);
			Grid grid_snap = grid;
			int int_secs_snap = int_seconds_left;
			bool b_started_snap = b_round_started;
			bool b_over_snap = b_game_over;
			int int_winner_snap = int_winner_id;
			bool arr_active_snap[MAX_PLAYERS];
			for (int i = 0; i < MAX_PLAYERS; i++)
				arr_active_snap[i] = arr_Players[i].b_active;
			pthread_mutex_unlock(&mutex);

			// Tint each claimed cell with its owner's colour, lightened so
			// the squares and text on top stay readable
			glBegin(GL_QUADS);
			for (int int_row = 0; int_row < GRID_ROWS; int_row++)
			{
				for (int int_col = 0; int_col < GRID_COLS; int_col++)
				{
					int int_owner = grid_snap.arr_owner[int_row * GRID_COLS + int_col];
					if (int_owner < 1 || int_owner > MAX_PLAYERS)
						continue;

					const float *arr_c = arr_colors[int_owner - 1];
					glColor3f(0.4f + 0.6f * arr_c[0], 0.4f + 0.6f * arr_c[1],
						0.4f + 0.6f * arr_c[2]);
					glVertex2f(int_col * SQ_WIDTH, int_row * SQ_WIDTH);
					glVertex2f(int_col * SQ_WIDTH, (int_row + 1) * SQ_WIDTH);
					glVertex2f((int_col + 1) * SQ_WIDTH, (int_row + 1) * SQ_WIDTH);
					glVertex2f((int_col + 1) * SQ_WIDTH, int_row * SQ_WIDTH);
				}
			}
			glEnd();

			glColor3f(0.0f, 0.0f, 0.0f);
		
		
		char str_banner[100] = "Waiting for server ...";
		if (int_player_id != -1)
		{
			snprintf(str_banner, sizeof(str_banner), "Player %d", int_player_id);
		}
		draw_text(320, 20, 3.0f, str_banner);

			// Countdown
			if (b_started_snap)
			{
				char str_timer[32];
				snprintf(str_timer, sizeof(str_timer), "Time: %d", int_secs_snap);
				draw_text(10, 20, 3.0f, str_timer);
			}

			// Score of each active player: colour swatch + cells owned
			int int_slot = 0;
			for (int i = 0; i < MAX_PLAYERS; i++)
			{
				if (!arr_active_snap[i])
					continue;

				float f_x = 10.0f + int_slot * 150.0f;
				float f_y = HEIGHT - 30.0f;
				glColor3fv(arr_colors[i]);
				glBegin(GL_QUADS);
					glVertex2f(f_x, f_y);
					glVertex2f(f_x, f_y + 16.0f);
					glVertex2f(f_x + 16.0f, f_y + 16.0f);
					glVertex2f(f_x + 16.0f, f_y);
				glEnd();

				char str_score[32];
				snprintf(str_score, sizeof(str_score), "P%d: %d", i + 1,
					grid_count_owned(&grid_snap, i + 1));
				glColor3f(0.0f, 0.0f, 0.0f);
				draw_text(f_x + 22.0f, f_y + 1.0f, 2.0f, str_score);
				int_slot++;
			}
		
		for(int i = 0; i < MAX_PLAYERS; i++)
		{
			pthread_mutex_lock(&mutex);
			if(arr_Players[i].b_active == true)
			{
				// Label the player ID
				char str_player[2] = "";
				snprintf(str_player, sizeof(str_player), "%d", i+1);
				/* printf("Coords = %d, %d\n",arr_Players[i].x, arr_Players[i].y);				 */
				/* draw_text(100, 400, 3.0f, str_banner); */
				
				// Fill the square with the player's colour, then label it
					glColor3fv(arr_colors[i]);
					glBegin(GL_QUADS);
						glVertex2f(arr_Players[i].x, arr_Players[i].y);
						glVertex2f(arr_Players[i].x, arr_Players[i].y + SQ_WIDTH);
						glVertex2f(arr_Players[i].x + SQ_WIDTH, arr_Players[i].y + SQ_WIDTH);
						glVertex2f(arr_Players[i].x + SQ_WIDTH, arr_Players[i].y);
					glEnd();
					glColor3f(0.0f, 0.0f, 0.0f);
					draw_text(arr_Players[i].x, arr_Players[i].y, 3.0f, str_player);				
				
				glBegin(GL_LINE_LOOP);
					glVertex2f(arr_Players[i].x, arr_Players[i].y);
					glVertex2f(arr_Players[i].x, arr_Players[i].y + SQ_WIDTH);
					glVertex2f(arr_Players[i].x + SQ_WIDTH, arr_Players[i].y + SQ_WIDTH);
					glVertex2f(arr_Players[i].x + SQ_WIDTH, arr_Players[i].y);
				glEnd();
			}
			pthread_mutex_unlock(&mutex);			
		}

			if (!b_started_snap)
			{
				glColor3f(0.0f, 0.0f, 0.0f);
				draw_text(150, 280, 3.0f, "Waiting for another player...");
			}

			// End-of-round screen: dim the floor and announce the winner
			if (b_over_snap)
			{
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glColor4f(0.0f, 0.0f, 0.0f, 0.65f);
				glBegin(GL_QUADS);
					glVertex2f(0, 0);
					glVertex2f(0, HEIGHT);
					glVertex2f(WIDTH, HEIGHT);
					glVertex2f(WIDTH, 0);
				glEnd();
				glDisable(GL_BLEND);

				char str_result[64];
				if (int_winner_snap >= 1 && int_winner_snap <= MAX_PLAYERS)
				{
					snprintf(str_result, sizeof(str_result), "Player %d wins!", int_winner_snap);
					glColor3fv(arr_colors[int_winner_snap - 1]);
				}
				else
				{
					snprintf(str_result, sizeof(str_result), "Draw!");
					glColor3f(1.0f, 1.0f, 1.0f);
				}
				draw_text(150, 260, 6.0f, str_result);
			}
        glfwSwapBuffers(window);
		// Cap at about 60 frames per second; vsync is not always honoured
		// (eg software rendering under WSL), which leaves the loop spinning
		usleep(16000);
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    security_cleanup();
    return 0;
}
