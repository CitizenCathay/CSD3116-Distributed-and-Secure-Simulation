/*****************************************************************************
\file tools/join_test.c
\author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/04/2026 (MM/DD/YYYY)
\brief Headless DDS checks of the join and replay protection against the real
       server. Prints PASS or FAIL for each check.
*******************************************************************************/
/* Usage (server already running, see tools/integration.sh):
    join_test retry  <nonce> <wait_secs>
        1. join, then resend the same JoinRequest: must get the same slot
        2. never send Input, wait_secs (> 10) later resend it again: the
           server must have freed the slot and now refuse the nonce
    join_test replay <nonce>
        join, make one real move, resend that exact Input 3 more times and
        once under another player's ID: the square must move exactly once */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dds/dds.h"
#include "messages.h"
#include "security.h"

static dds_entity_t g_join_writer;
static dds_entity_t g_response_reader;
static dds_entity_t g_input_writer;
static dds_entity_t g_position_reader;
static SecurityReplayGuard g_response_guard = {0};
static SecurityReplayGuard g_position_guard = {0};

// Sends a_request and returns the player ID the server answers with, or -1
// if no answer for a_int_nonce arrives within a_int_secs
static int request_slot(const JoinRequest *a_request, int a_int_secs)
{
	dds_write(g_join_writer, a_request);
	void *samples[1];
	samples[0] = JoinResponse__alloc();
	dds_sample_info_t infos[1];
	dds_time_t give_up = dds_time() + DDS_SECS(a_int_secs);
	int int_id = -1;
	while (dds_time() < give_up && int_id == -1)
	{
		while (dds_take(g_response_reader, samples, infos, 1, 1) > 0)
		{
			JoinResponse *response = samples[0];
			int arr_plain[2];
			size_t plain_len = 0;
			if (infos[0].valid_data &&
				security_decrypt(SECURITY_MSG_JOIN_RESPONSE, NULL, 0, &g_response_guard,
				response->arr_cipher, response->len_cipher,
				(unsigned char *)arr_plain, sizeof(arr_plain), &plain_len) &&
				plain_len == sizeof(arr_plain) && arr_plain[1] == a_request->int_nonce)
				int_id = arr_plain[0];
		}
		dds_sleepfor(DDS_MSECS(10));
	}
	JoinResponse_free(samples[0], DDS_FREE_ALL);
	return int_id;
}

// Waits up to a_int_secs for a Position of a_int_id and stores the latest
static void read_position(int a_int_id, int a_int_secs, int *a_ptr_x, int *a_ptr_y)
{
	void *samples[1];
	samples[0] = Position__alloc();
	dds_sample_info_t infos[1];
	dds_time_t give_up = dds_time() + DDS_SECS(a_int_secs);
	while (dds_time() < give_up)
	{
		while (dds_take(g_position_reader, samples, infos, 1, 1) > 0)
		{
			Position *position = samples[0];
			int arr_bound[1] = { position->int_player_id };
			int arr_xy[2];
			size_t plain_len = 0;
			if (infos[0].valid_data && position->int_player_id == a_int_id &&
				security_decrypt(SECURITY_MSG_POSITION, arr_bound, 1, &g_position_guard,
				position->arr_cipher, position->len_cipher,
				(unsigned char *)arr_xy, sizeof(arr_xy), &plain_len))
			{
				*a_ptr_x = arr_xy[0];
				*a_ptr_y = arr_xy[1];
			}
		}
		dds_sleepfor(DDS_MSECS(20));
	}
	Position_free(samples[0], DDS_FREE_ALL);
}

// Builds an encrypted Input for a_int_id, bound like client.c's
static Input make_input(int a_int_id, int a_int_nonce, char a_ch_direction)
{
	Input input;
	memset(&input, 0, sizeof(input));
	input.int_player_id = a_int_id;
	input.ch_direction = 'n';
	int arr_bound[2] = { a_int_id, a_int_nonce };
	unsigned char arr_plain[1] = { (unsigned char)a_ch_direction };
	size_t cipher_len = 0;
	security_encrypt(SECURITY_MSG_INPUT, arr_bound, 2, arr_plain, sizeof(arr_plain),
		input.arr_cipher, &cipher_len);
	input.len_cipher = (unsigned long)cipher_len;
	return input;
}

static bool check_retry(const JoinRequest *a_request, int a_int_wait_secs)
{
	int int_first = request_slot(a_request, 3);
	int int_retry = request_slot(a_request, 3);
	printf("first join -> player %d, same request again -> player %d\n", int_first, int_retry);
	bool b_same_slot = int_first > 0 && int_retry == int_first;
	printf("%s: a repeated JoinRequest gets the same slot, not a second one\n",
		b_same_slot ? "PASS" : "FAIL");

	dds_sleepfor(DDS_SECS(a_int_wait_secs));
	int int_late = request_slot(a_request, 3);
	printf("after %d s without Input, same request again -> %d (-1 = no answer)\n",
		a_int_wait_secs, int_late);
	printf("%s: a slot that sends no Input is freed and its nonce refused\n",
		int_late == -1 ? "PASS" : "FAIL");
	return b_same_slot && int_late == -1;
}

static bool check_replay(const JoinRequest *a_request)
{
	int int_id = request_slot(a_request, 3);
	if (int_id < 1)
	{
		printf("FAIL: could not join\n");
		return false;
	}
	Input hello = make_input(int_id, a_request->int_nonce, 'n');
	dds_write(g_input_writer, &hello);

	int int_x = -1, int_y = -1;
	read_position(int_id, 2, &int_x, &int_y);

	// One real move (away from the nearer edge), then 3 copies of the same
	// sample, then the same ciphertext relabelled as another player's
	Input move = make_input(int_id, a_request->int_nonce, int_y > 300 ? 'u' : 'd');
	for (int i = 0; i < 4; i++)
	{
		dds_write(g_input_writer, &move);
		dds_sleepfor(DDS_MSECS(300));
	}
	Input relabelled = move;
	relabelled.int_player_id = int_id == 1 ? 2 : 1;
	dds_write(g_input_writer, &relabelled);

	int int_new_x = int_x, int_new_y = int_y;
	read_position(int_id, 1, &int_new_x, &int_new_y);
	int int_moved = abs(int_new_x - int_x) + abs(int_new_y - int_y);
	printf("player %d moved %d px for 1 real move + 3 replays (one step = 50)\n",
		int_id, int_moved);
	printf("%s: replayed Input is ignored\n", int_moved == 50 ? "PASS" : "FAIL");
	return int_moved == 50;
}

int main(int argc, char **argv)
{
	bool b_retry = argc == 4 && strcmp(argv[1], "retry") == 0;
	bool b_replay = argc == 3 && strcmp(argv[1], "replay") == 0;
	if (!b_retry && !b_replay)
	{
		fprintf(stderr, "usage: %s retry <nonce> <wait_secs> | replay <nonce>\n", argv[0]);
		return 2;
	}
	if (!security_init())
		return 1;

	dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
	dds_entity_t join_topic = dds_create_topic(participant, &JoinRequest_desc, "JoinRequest", NULL, NULL);
	dds_entity_t response_topic = dds_create_topic(participant, &JoinResponse_desc, "JoinResponse", NULL, NULL);
	dds_entity_t input_topic = dds_create_topic(participant, &Input_desc, "Input", NULL, NULL);
	dds_entity_t position_topic = dds_create_topic(participant, &Position_desc, "Position", NULL, NULL);
	g_join_writer = dds_create_writer(participant, join_topic, NULL, NULL);
	g_response_reader = dds_create_reader(participant, response_topic, NULL, NULL);
	dds_qos_t *qos_input = dds_create_qos();
	dds_qset_writer_data_lifecycle(qos_input, false);
	g_input_writer = dds_create_writer(participant, input_topic, qos_input, NULL);
	dds_delete_qos(qos_input);
	dds_qos_t *qos_position = dds_create_qos();
	dds_qset_reliability(qos_position, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	g_position_reader = dds_create_reader(participant, position_topic, qos_position, NULL);
	dds_delete_qos(qos_position);
	dds_sleepfor(DDS_MSECS(1500));

	JoinRequest request;
	memset(&request, 0, sizeof(request));
	request.int_nonce = atoi(argv[2]);
	security_make_client_token(request.int_nonce, request.arr_token, sizeof(request.arr_token));

	bool b_pass = b_retry ? check_retry(&request, atoi(argv[3])) : check_replay(&request);
	dds_delete(participant);
	return b_pass ? 0 : 1;
}
