/*****************************************************************************
\file test_net_config.c
\author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/05/2026 (MM/DD/YYYY)
\brief Dependency-free unit tests for net_config.c: peer address validation
       and the Cyclone DDS configuration it builds.
*******************************************************************************/
/* Build and run with `make test` (or `make test_net_config`). No DDS, network
or other library needed: net_config.c only builds configuration text. */

#include <stdio.h>
#include <string.h>
#include "net_config.h"

static int g_int_tests_run = 0;
static int g_int_tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
	do { \
		g_int_tests_run++; \
		if (!(cond)) { \
			g_int_tests_failed++; \
			printf("FAIL %s:%d - %s\n", __FILE__, __LINE__, msg); \
		} \
	} while (0)

static void test_accepts_normal_addresses(void)
{
	TEST_ASSERT(net_config_valid_address("172.20.10.3"), "IPv4 address accepted");
	TEST_ASSERT(net_config_valid_address("fe80::1"), "IPv6 address accepted");
	TEST_ASSERT(net_config_valid_address("lab-pc-07.local"), "hostname accepted");
}

static void test_rejects_unsafe_addresses(void)
{
	// The address goes inside an XML attribute: anything that could close
	// the attribute or start a new element must be refused
	TEST_ASSERT(!net_config_valid_address("1.2.3.4\"/><Peer address=\"6.6.6.6"),
		"quote and angle brackets rejected (XML injection)");
	TEST_ASSERT(!net_config_valid_address("1.2.3.4 5.6.7.8"), "space rejected");
	TEST_ASSERT(!net_config_valid_address(""), "empty address rejected");
	TEST_ASSERT(!net_config_valid_address(NULL), "NULL rejected");

	char str_long[NET_CONFIG_MAX_ADDRESS_LEN + 2];
	memset(str_long, 'a', sizeof(str_long) - 1);
	str_long[sizeof(str_long) - 1] = '\0';
	TEST_ASSERT(!net_config_valid_address(str_long), "over-long address rejected");
}

static void test_no_peers_still_fixes_ports(void)
{
	char str_config[512];
	TEST_ASSERT(net_config_build(0, NULL, str_config, sizeof(str_config)),
		"config builds with no peers");
	TEST_ASSERT(strstr(str_config, "<ParticipantIndex>auto</ParticipantIndex>") != NULL,
		"fixed ports are always set");
	TEST_ASSERT(strstr(str_config, "<Peers>") == NULL, "no peer list without peers");
}

static void test_peers_are_listed(void)
{
	char *arr_peers[2] = { "172.20.10.2", "172.20.10.3" };
	char str_config[512];
	TEST_ASSERT(net_config_build(2, arr_peers, str_config, sizeof(str_config)),
		"config builds with two peers");
	TEST_ASSERT(strstr(str_config, "<Peer address=\"172.20.10.2\"/>") != NULL &&
		strstr(str_config, "<Peer address=\"172.20.10.3\"/>") != NULL,
		"every peer address appears in the config");
}

static void test_build_refuses_bad_input(void)
{
	char str_config[512];
	char *arr_bad[1] = { "1.2.3.4\"/><x" };
	TEST_ASSERT(!net_config_build(1, arr_bad, str_config, sizeof(str_config)),
		"an unsafe address makes the whole build fail");

	char *arr_many[NET_CONFIG_MAX_PEERS + 1];
	for (int i = 0; i <= NET_CONFIG_MAX_PEERS; i++)
		arr_many[i] = "10.0.0.1";
	TEST_ASSERT(!net_config_build(NET_CONFIG_MAX_PEERS + 1, arr_many, str_config,
		sizeof(str_config)), "more than NET_CONFIG_MAX_PEERS peers refused");

	char str_tiny[16];
	TEST_ASSERT(!net_config_build(0, NULL, str_tiny, sizeof(str_tiny)),
		"an output buffer that is too small is refused, not overflowed");
}

int main(void)
{
	test_accepts_normal_addresses();
	test_rejects_unsafe_addresses();
	test_no_peers_still_fixes_ports();
	test_peers_are_listed();
	test_build_refuses_bad_input();

	printf("%d/%d tests passed\n", g_int_tests_run - g_int_tests_failed, g_int_tests_run);
	return g_int_tests_failed == 0 ? 0 : 1;
}
