/*****************************************************************************
\file tools/bench.c
\author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/04/2026 (MM/DD/YYYY)
\brief Headless benchmark client: joins the real server over DDS the same
       way client.c does, then either just stays in the game (to add
       players without opening windows) or times key press -> new position.
*******************************************************************************/
/* Usage (server already running):
    bench idle  <nonce> <secs>    join, send the "hello" Input, stay for secs
    bench moves <nonce> <count>   join, then send count moves and print the
                                  Input -> Position latency (mean, median,
                                  p95, max)
<nonce> just has to be different for every bench process.

Built by `make tools` against the current messages.h, or by
tools/build_versions.sh against an older commit's, so the same tool can
benchmark both versions. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dds/dds.h"
#include "messages.h"
#include "security.h"

// Give up on a move after this long (blocked by an edge or another square)
#define MOVE_TIMEOUT_SECS 1
// Pause between moves, so each position update is measured on its own
#define MOVE_GAP_MSECS 50

static dds_entity_t g_position_reader;
static SecurityReplayGuard g_position_guard = {0};
static int g_int_player_id = -1;

// Takes every pending Position and updates *a_ptr_x/*a_ptr_y if one of
// them is our own
static void drain_positions(int *a_ptr_x, int *a_ptr_y)
{
	void *samples[1];
	samples[0] = Position__alloc();
	dds_sample_info_t infos[1];
	while (dds_take(g_position_reader, samples, infos, 1, 1) > 0)
	{
		Position *position = samples[0];
		int arr_bound[1] = { position->int_player_id };
		int arr_xy[2];
		size_t plain_len = 0;
		if (infos[0].valid_data && position->int_player_id == g_int_player_id &&
			security_decrypt(SECURITY_MSG_POSITION, arr_bound, 1, &g_position_guard,
			position->arr_cipher, position->len_cipher,
			(unsigned char *)arr_xy, sizeof(arr_xy), &plain_len) &&
			plain_len == sizeof(arr_xy))
		{
			*a_ptr_x = arr_xy[0];
			*a_ptr_y = arr_xy[1];
		}
	}
	Position_free(samples[0], DDS_FREE_ALL);
}

// Publishes one Input for us, encrypted and bound like client.c's
static void send_input(dds_entity_t a_writer, int a_int_nonce, char a_ch_direction)
{
	Input input;
	memset(&input, 0, sizeof(input));
	input.int_player_id = g_int_player_id;
	input.ch_direction = 'n';
	int arr_bound[2] = { g_int_player_id, a_int_nonce };
	unsigned char arr_plain[1] = { (unsigned char)a_ch_direction };
	size_t cipher_len = 0;
	security_encrypt(SECURITY_MSG_INPUT, arr_bound, 2, arr_plain, sizeof(arr_plain),
		input.arr_cipher, &cipher_len);
	input.len_cipher = (unsigned long)cipher_len;
	dds_write(a_writer, &input);
}

// Sends JoinRequests until the server admits us; returns our player ID, or
// -1 if it never does
static int join(dds_entity_t a_join_writer, dds_entity_t a_response_reader, int a_int_nonce)
{
	JoinRequest request;
	memset(&request, 0, sizeof(request));
	request.int_nonce = a_int_nonce;
	security_make_client_token(a_int_nonce, request.arr_token, sizeof(request.arr_token));

	SecurityReplayGuard response_guard = {0};
	void *samples[1];
	samples[0] = JoinResponse__alloc();
	dds_sample_info_t infos[1];
	for (int int_try = 0; int_try < 10; int_try++)
	{
		dds_write(a_join_writer, &request);
		dds_time_t give_up = dds_time() + DDS_SECS(1);
		while (dds_time() < give_up)
		{
			while (dds_take(a_response_reader, samples, infos, 1, 1) > 0)
			{
				JoinResponse *response = samples[0];
				int arr_plain[2];
				size_t plain_len = 0;
				if (infos[0].valid_data &&
					security_decrypt(SECURITY_MSG_JOIN_RESPONSE, NULL, 0, &response_guard,
					response->arr_cipher, response->len_cipher,
					(unsigned char *)arr_plain, sizeof(arr_plain), &plain_len) &&
					plain_len == sizeof(arr_plain) && arr_plain[1] == a_int_nonce &&
					arr_plain[0] > 0)
				{
					// must be set freeee
					JoinResponse_free(samples[0], DDS_FREE_ALL);
					return arr_plain[0];
				}
			}
			dds_sleepfor(DDS_MSECS(5));
		}
	}

	// this too set them free.
	JoinResponse_free(samples[0], DDS_FREE_ALL);
	return -1;
}

static int compare_doubles(const void *a_ptr_a, const void *a_ptr_b)
{
	double d_diff = *(const double *)a_ptr_a - *(const double *)a_ptr_b;
	return (d_diff > 0) - (d_diff < 0);
}

int main(int argc, char **argv)
{
	if (argc != 4 || (strcmp(argv[1], "idle") != 0 && strcmp(argv[1], "moves") != 0))
	{
		fprintf(stderr, "usage: %s idle <nonce> <secs> | moves <nonce> <count>\n", argv[0]);
		return 2;
	}
	bool b_moves = strcmp(argv[1], "moves") == 0;
	int int_nonce = atoi(argv[2]);
	int int_arg = atoi(argv[3]);
	if (!security_init())
		return 1;

	// Same topics and QoS as client.c
	dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
	dds_entity_t join_topic = dds_create_topic(participant, &JoinRequest_desc, "JoinRequest", NULL, NULL);
	dds_entity_t response_topic = dds_create_topic(participant, &JoinResponse_desc, "JoinResponse", NULL, NULL);
	dds_entity_t input_topic = dds_create_topic(participant, &Input_desc, "Input", NULL, NULL);
	dds_entity_t position_topic = dds_create_topic(participant, &Position_desc, "Position", NULL, NULL);
	dds_entity_t join_writer = dds_create_writer(participant, join_topic, NULL, NULL);
	dds_entity_t response_reader = dds_create_reader(participant, response_topic, NULL, NULL);
	dds_qos_t *qos_input = dds_create_qos();
	dds_qset_writer_data_lifecycle(qos_input, false);
	dds_entity_t input_writer = dds_create_writer(participant, input_topic, qos_input, NULL);
	dds_delete_qos(qos_input);
	dds_qos_t *qos_position = dds_create_qos();
	dds_qset_reliability(qos_position, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	g_position_reader = dds_create_reader(participant, position_topic, qos_position, NULL);
	dds_delete_qos(qos_position);
	// Let DDS discovery find the server first
	dds_sleepfor(DDS_MSECS(1500));

	g_int_player_id = join(join_writer, response_reader, int_nonce);
	if (g_int_player_id < 1)
	{
		fprintf(stderr, "bench %d: join failed\n", int_nonce);
		dds_delete(participant);
		return 1;
	}
	send_input(input_writer, int_nonce, 'n');

	if (!b_moves)
	{
		dds_sleepfor(DDS_SECS(int_arg));
		// Leave cleanly, like client.c, so the server frees the slot at once
		dds_delete(participant);
		return 0;
	}

	// Wait for our starting position
	int int_x = -1, int_y = -1;
	for (int i = 0; i < 200 && int_x < 0; i++)
	{
		drain_positions(&int_x, &int_y);
		dds_sleepfor(DDS_MSECS(10));
	}

	// Alternate up/down (or left/right after being blocked), so the square
	// stays near its spawn and every move should succeed
	double *arr_latency_ms = calloc((size_t)int_arg, sizeof(double));
	int int_ok = 0, int_blocked = 0, int_axis = 0;
	const char *arr_pairs[2] = { "ud", "lr" };
	for (int k = 0; k < int_arg; k++)
	{
		int int_old_x = int_x, int_old_y = int_y;
		dds_time_t start = dds_time();
		send_input(input_writer, int_nonce, arr_pairs[int_axis][k % 2]);
		while (dds_time() - start < DDS_SECS(MOVE_TIMEOUT_SECS) &&
			int_x == int_old_x && int_y == int_old_y)
		{
			drain_positions(&int_x, &int_y);
			dds_sleepfor(DDS_USECS(200));
		}
		if (int_x != int_old_x || int_y != int_old_y)
		{
			arr_latency_ms[int_ok++] = (double)(dds_time() - start) / 1e6;
		}
		else
		{
			int_blocked++;
			int_axis ^= 1;
		}
		dds_sleepfor(DDS_MSECS(MOVE_GAP_MSECS));
		drain_positions(&int_x, &int_y);
	}

	if (int_ok > 0)
	{
		qsort(arr_latency_ms, (size_t)int_ok, sizeof(double), compare_doubles);
		double d_sum = 0;
		for (int i = 0; i < int_ok; i++)
			d_sum += arr_latency_ms[i];
		printf("moves=%d blocked=%d  latency ms: mean=%.1f median=%.1f p95=%.1f max=%.1f\n",
			int_ok, int_blocked, d_sum / int_ok, arr_latency_ms[int_ok / 2],
			arr_latency_ms[(int)(int_ok * 0.95)], arr_latency_ms[int_ok - 1]);
	}
	else
	{
		printf("no successful moves (%d blocked)\n", int_blocked);
	}
	free(arr_latency_ms);
	dds_delete(participant);
	return 0;
}
