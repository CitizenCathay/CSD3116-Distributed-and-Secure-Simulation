/*****************************************************************************
\file net_config.c
\author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/05/2026 (MM/DD/YYYY)
\brief Implementation of net_config.h: builds the Cyclone DDS configuration
       (fixed ports, optional peers) and hands it to Cyclone.
*******************************************************************************/
/* Implementation of net_config.h. No DDS calls here, only the configuration
text, so it can be unit-tested without a network (test_net_config.c). */

#include "net_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Room for the fixed part of the XML plus every peer
#define CONFIG_MAX_LEN 1024

bool net_config_valid_address(const char *a_str_address)
{
	if (!a_str_address)
		return false;

	size_t len = strlen(a_str_address);
	if (len == 0 || len > NET_CONFIG_MAX_ADDRESS_LEN)
		return false;

	for (size_t i = 0; i < len; i++)
	{
		char ch = a_str_address[i];
		bool b_allowed = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == ':';
		if (!b_allowed)
			return false;
	}
	return true;
}

// Appends a_str_text to out (holding *a_ptr_used of out_len bytes).
// Returns false, leaving out unchanged, if it would not fit
static bool append(char *out, size_t out_len, size_t *a_ptr_used, const char *a_str_text)
{
	size_t text_len = strlen(a_str_text);
	if (*a_ptr_used + text_len + 1 > out_len)
		return false;
	memcpy(out + *a_ptr_used, a_str_text, text_len + 1);
	*a_ptr_used += text_len;
	return true;
}

bool net_config_build(int a_int_peer_count, char *const a_arr_peers[],
	char *out, size_t out_len)
{
	if (a_int_peer_count < 0 || a_int_peer_count > NET_CONFIG_MAX_PEERS || !out || out_len == 0)
		return false;
	for (int i = 0; i < a_int_peer_count; i++)
	{
		if (!net_config_valid_address(a_arr_peers[i]))
			return false;
	}

	// ParticipantIndex "auto" gives each program on a machine its own
	// fixed ports instead of random ones, so a peer can find it and a
	// firewall rule can let it through
	char str_fixed[160];
	snprintf(str_fixed, sizeof(str_fixed),
		"<CycloneDDS><Domain id=\"any\"><Discovery>"
		"<ParticipantIndex>auto</ParticipantIndex>"
		"<MaxAutoParticipantIndex>%d</MaxAutoParticipantIndex>",
		NET_CONFIG_MAX_PROGRAMS);

	size_t used = 0;
	out[0] = '\0';
	bool b_ok = append(out, out_len, &used, str_fixed);
	if (a_int_peer_count > 0)
	{
		b_ok = b_ok && append(out, out_len, &used, "<Peers>");
		for (int i = 0; i < a_int_peer_count; i++)
		{
			b_ok = b_ok && append(out, out_len, &used, "<Peer address=\"") &&
				append(out, out_len, &used, a_arr_peers[i]) &&
				append(out, out_len, &used, "\"/>");
		}
		b_ok = b_ok && append(out, out_len, &used, "</Peers>");
	}
	b_ok = b_ok && append(out, out_len, &used, "</Discovery></Domain></CycloneDDS>");
	return b_ok;
}

bool net_config_apply(int a_int_peer_count, char *const a_arr_peers[])
{
	for (int i = 0; i < a_int_peer_count; i++)
	{
		if (!net_config_valid_address(a_arr_peers[i]))
		{
			fprintf(stderr, "Not a valid peer address: %s\n", a_arr_peers[i]);
			return false;
		}
	}

	char str_config[CONFIG_MAX_LEN];
	if (!net_config_build(a_int_peer_count, a_arr_peers, str_config, sizeof(str_config)))
	{
		fprintf(stderr, "Too many peer addresses (at most %d)\n", NET_CONFIG_MAX_PEERS);
		return false;
	}

	// Keep any configuration the user has set, and add ours after it -
	// Cyclone reads a comma-separated list of configurations in order
	const char *str_existing = getenv("CYCLONEDDS_URI");
	char str_uri[CONFIG_MAX_LEN * 2];
	int int_len;
	if (str_existing && str_existing[0] != '\0')
		int_len = snprintf(str_uri, sizeof(str_uri), "%s,%s", str_existing, str_config);
	else
		int_len = snprintf(str_uri, sizeof(str_uri), "%s", str_config);
	if (int_len < 0 || (size_t)int_len >= sizeof(str_uri))
	{
		fprintf(stderr, "CYCLONEDDS_URI is too long to add to\n");
		return false;
	}

	if (setenv("CYCLONEDDS_URI", str_uri, 1) != 0)
	{
		perror("setenv");
		return false;
	}

	for (int i = 0; i < a_int_peer_count; i++)
		printf("Also looking for peers at %s\n", a_arr_peers[i]);
	return true;
}
