/*****************************************************************************
\file net_config.h
\author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/05/2026 (MM/DD/YYYY)
\brief Cyclone DDS network settings shared by the server and client: fixed
       ports, plus optional peer addresses for playing across machines on
       networks where DDS cannot discover peers by itself.
*******************************************************************************/
/* DDS normally finds the other programs by multicast, which works on one
machine and on most home/office LANs. Phone hotspots, some school networks
and VPNs drop multicast, so programs on different machines never meet. The
standard DDS answer is a peer list: tell a program the address of another
machine and it contacts it directly. Here that is the program's optional
command-line arguments, eg `./client 172.20.10.3`.

Only the client needs the server's address: once the server hears from a
client it answers it directly, so the server can be started with no
arguments. Both programs always use fixed ports (Cyclone's ParticipantIndex
"auto": UDP 7400-7451 for up to NET_CONFIG_MAX_PROGRAMS programs on one
machine), so a firewall rule for those ports is all another machine needs.

Settings are passed to Cyclone through the CYCLONEDDS_URI environment
variable, before any DDS call. A CYCLONEDDS_URI the user has already set is
kept, and ours is added after it. */

#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

// Most game programs (server + clients) on one machine; each one needs its
// own fixed ports
#define NET_CONFIG_MAX_PROGRAMS 20
// Most peer addresses that can be given
#define NET_CONFIG_MAX_PEERS 8
// Longest peer address (a hostname or IPv4/IPv6 address)
#define NET_CONFIG_MAX_ADDRESS_LEN 64

// True if a_str_address is safe to put in the DDS configuration: 1 to
// NET_CONFIG_MAX_ADDRESS_LEN characters, only letters, digits, '.', '-' and
// ':'. Anything else (eg quotes or '<') could change the XML it goes into
bool net_config_valid_address(const char *a_str_address);

// Builds the Cyclone configuration XML into out (out_len bytes): fixed
// ports, plus one <Peer> per address in a_arr_peers. Returns false if an
// address is invalid, there are more than NET_CONFIG_MAX_PEERS, or out is
// too small. Separate from net_config_apply() so it can be unit-tested
bool net_config_build(int a_int_peer_count, char *const a_arr_peers[],
	char *out, size_t out_len);

// Applies net_config_build()'s configuration through CYCLONEDDS_URI. Call
// once from main(), before any thread creates a DDS entity. a_arr_peers is
// typically argv + 1. Prints the problem and returns false on bad input
bool net_config_apply(int a_int_peer_count, char *const a_arr_peers[]);

#endif // NET_CONFIG_H
