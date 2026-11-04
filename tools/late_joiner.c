/*****************************************************************************
\file tools/late_joiner.c
\author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/04/2026 (MM/DD/YYYY)
\brief Headless DDS check: subscribes to Position like a client that joins
       mid-game and prints which players it is told are in the game.
*******************************************************************************/
/* Usage: late_joiner
Prints eg "late joiner sees: 1 3". Position is transient-local, so a client
that starts late must be handed every current player straight away - and
never a player who has already left. tools/integration.sh compares the
output with the players it expects. */

#include <stdio.h>
#include "dds/dds.h"
#include "messages.h"
#include "security.h"
#include "game_logic.h"

int main(void)
{
	if (!security_init())
		return 1;

	// Same QoS as client.c's Position reader
	dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
	dds_entity_t topic = dds_create_topic(participant, &Position_desc, "Position", NULL, NULL);
	dds_qos_t *qos = dds_create_qos();
	dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
	dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
	dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 1);
	dds_entity_t reader = dds_create_reader(participant, topic, qos, NULL);
	dds_delete_qos(qos);
	// Discovery plus delivery of the server's stored positions
	dds_sleepfor(DDS_SECS(3));

	bool arr_b_in_game[MAX_PLAYERS + 1] = {0};
	void *samples[1];
	samples[0] = Position__alloc();
	dds_sample_info_t infos[1];
	while (dds_take(reader, samples, infos, 1, 1) > 0)
	{
		Position *position = samples[0];
		int int_id = position->int_player_id;
		if (int_id < 1 || int_id > MAX_PLAYERS)
			continue;
		if (!infos[0].valid_data)
		{
			arr_b_in_game[int_id] = false;
			continue;
		}
		int arr_bound[1] = { int_id };
		int arr_xy[2];
		size_t plain_len = 0;
		if (security_decrypt(SECURITY_MSG_POSITION, arr_bound, 1, NULL,
			position->arr_cipher, position->len_cipher,
			(unsigned char *)arr_xy, sizeof(arr_xy), &plain_len))
			arr_b_in_game[int_id] = true;
	}

	printf("late joiner sees:");
	for (int int_id = 1; int_id <= MAX_PLAYERS; int_id++)
		if (arr_b_in_game[int_id])
			printf(" %d", int_id);
	printf("\n");
	dds_delete(participant);
	return 0;
}
