/*****************************************************************************
\file security.c
\author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/04/2026 (MM/DD/YYYY)
\brief Implementation of security.h: pre-shared-key client authentication and
       AES-256-GCM encryption of DDS traffic, using OpenSSL's EVP API.
*******************************************************************************/
/* Two separate 32-byte pre-shared secrets, shared by every client and the
server so they always agree without any key exchange step:
  - g_arr_auth_psk  proves a client is one of ours before the server admits
    it. The credential in JoinRequest.arr_token is an HMAC-SHA256 of the
    request's nonce under this key, so the key itself never goes over the
    network where anyone on the DDS domain could read it
  - g_arr_aes_key   encrypts/decrypts the Input/Position/CellClaimed/
    GameState/JoinResponse payloads with AES-256-GCM

Each can be overridden at startup via an environment variable (64 hex
characters = 32 bytes), so a real run does not have to use the value sitting
in this source file - see security_init(). The hardcoded fallbacks below
exist purely so the game still runs out of the box for grading/demos
without extra setup.

Every ciphertext is bound to its message type and DDS key fields through
GCM's associated data (AAD), and carries a sequence number receivers track
per stream (SecurityReplayGuard), so an outsider who records traffic can
neither move a valid ciphertext under another key (eg replay player 2's
Position as player 3's) nor resend an old one later.

KNOWN LIMITATION - ONE SHARED KEY. Both secrets are the same for every
client and the server, so every client is, cryptographically, the server:
  - any client can decrypt all traffic, including other players' Input
  - any client can forge any message - Position/CellClaimed/GameState "from
    the server", or Input for another player (the join nonces Input is
    bound to are sent in the clear in JoinRequest) - and it will pass
    authentication and replay checks
  - the built-in keys below are in source control, so unless both are
    overridden from the environment, anyone with the repository is an
    insider too
AAD binding and replay protection only defend against parties WITHOUT the
key. Defending against a malicious client needs per-participant keys and
signatures, so a receiver can tell the server's messages from a client's
and one client's from another's - eg DDS Security (per-participant
certificates, access-control permissions and per-writer keys), or a
per-client key agreed at join time plus a server signing key. The built-in
keys are kept deliberately so the game still runs out of the box for
grading/demos; this is a course-project scheme, not production key
management (no provisioning, rotation or revocation).

Replay protection limitations: sequence numbers are seeded from the
sender's wall clock so they keep increasing across restarts, which assumes
a restarted sender's clock has not gone backwards. A receiver accepts the
first message it sees on a stream, so a freshly started client can briefly
be shown a replayed (stale but genuine) server message until the server's
next real one replaces it. And because DDS itself is unauthenticated, an
outsider can still displace an unread sample from a keep-last-1 reader
cache - the displacing replay is then dropped, but the real sample is lost
until that instance is next written. JoinRequest is not encrypted and has
no sequence number: server.c dedupes it by nonce and refuses departed
players' nonces, but a restarted server has forgotten those, so a request
recorded during an earlier run can hold a slot until server.c's
FIRST_INPUT_TIMEOUT_SECS frees it. */

#include "security.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

// Environment variables security_init() checks for a hex-encoded override
#define AUTH_PSK_ENV_VAR "CSD3116_AUTH_PSK"
#define AES_KEY_ENV_VAR  "CSD3116_AES_KEY"

// 32 arbitrary bytes - only needs to match between every client and the
// server, not be memorable or secret-looking. Overridden by
// CSD3116_AUTH_PSK if that is set to 64 valid hex characters
static unsigned char g_arr_auth_psk[SECURITY_TOKEN_LEN] =
{
	0x4a, 0x1f, 0x9c, 0x2e, 0x77, 0xb3, 0x05, 0x8d,
	0xe1, 0x62, 0xaa, 0x3b, 0x90, 0x14, 0xc7, 0x5f,
	0x28, 0xd6, 0x4e, 0x81, 0xf3, 0x0a, 0x97, 0x6c,
	0x15, 0xbe, 0x39, 0xa4, 0x72, 0xcd, 0x08, 0x5a
};

// A second, independent 32-byte secret for AES-256 - never reuse the auth
// secret as an encryption key. Overridden by CSD3116_AES_KEY if that is
// set to 64 valid hex characters
static unsigned char g_arr_aes_key[32] =
{
	0x91, 0x3d, 0x6f, 0xc8, 0x22, 0xa0, 0x5b, 0xe7,
	0x4c, 0x19, 0x8a, 0xf6, 0x37, 0xd1, 0x0e, 0x95,
	0x6b, 0x2a, 0xc4, 0x58, 0xfd, 0x71, 0x0d, 0xb9,
	0x44, 0xe2, 0x1a, 0x7c, 0x93, 0x60, 0xaf, 0x35
};

// AES-GCM: a 12-byte nonce is the size CycloneDDS's default algorithm
// expects and avoids the internal counter construction smaller nonces need
#define GCM_IV_LEN  12
// A 16-byte authentication tag: tampering with the ciphertext, the IV, or
// using the wrong key all make the tag fail to verify on decrypt
#define GCM_TAG_LEN 16

// Big-endian sequence number sent ahead of the IV - in the clear, but
// authenticated as part of the AAD
#define SEQ_LEN 8

// Wire format for security_encrypt()'s out buffer: [seq][IV][ciphertext][tag]
#define GCM_OVERHEAD (SEQ_LEN + GCM_IV_LEN + GCM_TAG_LEN)
_Static_assert(GCM_OVERHEAD == SECURITY_MAX_CIPHER_OVERHEAD,
	"SECURITY_MAX_CIPHER_OVERHEAD must match the wire format");

// AAD: [message type, 1 byte][seq][each bound value, 4 bytes big-endian]
#define AAD_MAX_LEN (1 + SEQ_LEN + 4 * SECURITY_MAX_BOUND)

// Next sequence number security_encrypt() hands out. Atomic, since the
// client and server may both encrypt from more than one thread
static _Atomic uint64_t g_uint_next_seq = 1;

// Prefixed to the nonce before it is HMAC'd, so a join token can never be
// mistaken for an HMAC computed for some other purpose under the same key
#define JOIN_TOKEN_LABEL "CSD3116-join"

// Parses a_str_hex as exactly out_len bytes of hex (eg "4a1f9c...",
// out_len * 2 characters) into out. Leaves out untouched and returns false
// if a_str_hex is NULL, the wrong length, or contains non-hex characters
static bool parse_hex_key(const char *a_str_hex, unsigned char *out, size_t out_len)
{
	if (!a_str_hex || strlen(a_str_hex) != out_len * 2)
		return false;

	for (size_t i = 0; i < out_len; i++)
	{
		unsigned int int_byte;
		if (sscanf(a_str_hex + i * 2, "%2x", &int_byte) != 1)
			return false;
		out[i] = (unsigned char)int_byte;
	}
	return true;
}

bool security_init(void)
{
	// Prefer keys from the environment over the hardcoded defaults above,
	// so a real run is not stuck with a secret that is sitting in source
	// control - see this file's header comment. A variable that is set but
	// malformed is an error, not a reason to fall back to the public default
	// key, which would silently leave the game open to anyone with the source
	const char *str_auth_env = getenv(AUTH_PSK_ENV_VAR);
	if (str_auth_env && !parse_hex_key(str_auth_env, g_arr_auth_psk, sizeof(g_arr_auth_psk)))
	{
		fprintf(stderr, AUTH_PSK_ENV_VAR " must be exactly %zu hex characters\n",
			sizeof(g_arr_auth_psk) * 2);
		return false;
	}

	const char *str_aes_env = getenv(AES_KEY_ENV_VAR);
	if (str_aes_env && !parse_hex_key(str_aes_env, g_arr_aes_key, sizeof(g_arr_aes_key)))
	{
		fprintf(stderr, AES_KEY_ENV_VAR " must be exactly %zu hex characters\n",
			sizeof(g_arr_aes_key) * 2);
		return false;
	}

	// Start sequence numbers at the current time in nanoseconds, so a
	// restarted server/client carries on above the numbers its previous
	// run sent instead of being rejected as a replay by receivers that
	// still remember them. Never moves the counter backwards
	struct timespec ts_now;
	if (clock_gettime(CLOCK_REALTIME, &ts_now) == 0)
	{
		uint64_t uint_now = (uint64_t)ts_now.tv_sec * 1000000000u + (uint64_t)ts_now.tv_nsec;
		if (uint_now > atomic_load(&g_uint_next_seq))
			atomic_store(&g_uint_next_seq, uint_now);
	}

	return true;
}

// Writes a_uint_value to out as a_int_bytes big-endian bytes
static void put_be(unsigned char *out, uint64_t a_uint_value, int a_int_bytes)
{
	for (int i = 0; i < a_int_bytes; i++)
		out[i] = (unsigned char)(a_uint_value >> (8 * (a_int_bytes - 1 - i)));
}

// Builds the GCM associated data for one message into out (AAD_MAX_LEN
// bytes) and returns its length, or 0 if there are too many bound values
static size_t make_aad(SecurityMsgType a_type, uint64_t a_uint_seq,
	const int *a_arr_bound, size_t a_num_bound, unsigned char *out)
{
	if (a_num_bound > SECURITY_MAX_BOUND || (a_num_bound > 0 && !a_arr_bound))
		return 0;

	size_t aad_len = 0;
	out[aad_len++] = (unsigned char)a_type;
	put_be(out + aad_len, a_uint_seq, SEQ_LEN);
	aad_len += SEQ_LEN;
	for (size_t i = 0; i < a_num_bound; i++)
	{
		put_be(out + aad_len, (uint32_t)a_arr_bound[i], 4);
		aad_len += 4;
	}
	return aad_len;
}

// Computes HMAC-SHA256(g_arr_auth_psk, JOIN_TOKEN_LABEL || nonce) into
// out_mac (SECURITY_TOKEN_LEN bytes). The nonce is encoded big-endian so
// client and server agree regardless of byte order
static bool make_join_mac(int a_int_nonce, unsigned char *out_mac)
{
	unsigned char arr_msg[sizeof(JOIN_TOKEN_LABEL) - 1 + 4];
	unsigned int uint_nonce = (unsigned int)a_int_nonce;
	memcpy(arr_msg, JOIN_TOKEN_LABEL, sizeof(JOIN_TOKEN_LABEL) - 1);
	for (int i = 0; i < 4; i++)
		arr_msg[sizeof(JOIN_TOKEN_LABEL) - 1 + i] = (unsigned char)(uint_nonce >> (24 - 8 * i));

	unsigned int uint_mac_len = 0;
	return HMAC(EVP_sha256(), g_arr_auth_psk, sizeof(g_arr_auth_psk),
		arr_msg, sizeof(arr_msg), out_mac, &uint_mac_len) != NULL &&
		uint_mac_len == SECURITY_TOKEN_LEN;
}

void security_cleanup(void)
{
	// Nothing was allocated by security_init()
}

void security_make_client_token(int a_int_nonce, char *out_token, size_t token_len)
{
	unsigned char arr_mac[SECURITY_TOKEN_LEN];
	// An all-zero token is always rejected, so a failed HMAC just means the
	// join is refused rather than sending whatever was in out_token
	if (!make_join_mac(a_int_nonce, arr_mac))
		memset(arr_mac, 0, sizeof(arr_mac));

	size_t copy_len = token_len < SECURITY_TOKEN_LEN ? token_len : SECURITY_TOKEN_LEN;
	memcpy(out_token, arr_mac, copy_len);
}

int security_make_nonce(void)
{
	int int_nonce = 0;
	// Practically never fails; fall back to rand() rather than a fixed value
	// that every client would then share
	if (RAND_bytes((unsigned char *)&int_nonce, sizeof(int_nonce)) != 1)
		int_nonce = rand();
	return int_nonce;
}

bool security_verify_client_token(int a_int_nonce, const char *token)
{
	unsigned char arr_expected[SECURITY_TOKEN_LEN];
	if (!make_join_mac(a_int_nonce, arr_expected))
		return false;

	// Constant-time compare: a length/early-exit compare (eg memcmp) would
	// let an attacker learn the expected MAC one byte at a time by timing
	// how many leading bytes matched across many join attempts
	return CRYPTO_memcmp(arr_expected, token, SECURITY_TOKEN_LEN) == 0;
}

bool security_encrypt(SecurityMsgType a_type, const int *a_arr_bound,
                       size_t a_num_bound, const unsigned char *in,
                       size_t in_len, unsigned char *out, size_t *out_len)
{
	// out is always a fixed SECURITY_CIPHERTEXT_LEN-byte buffer (the
	// arr_cipher field of whichever message is being sent) - refuse rather
	// than overflow it if a future, larger message type doesn't fit
	if (in_len + GCM_OVERHEAD > SECURITY_CIPHERTEXT_LEN)
		return false;

	uint64_t uint_seq = atomic_fetch_add(&g_uint_next_seq, 1);
	unsigned char arr_aad[AAD_MAX_LEN];
	size_t aad_len = make_aad(a_type, uint_seq, a_arr_bound, a_num_bound, arr_aad);
	if (aad_len == 0)
		return false;

	unsigned char arr_iv[GCM_IV_LEN];
	if (RAND_bytes(arr_iv, GCM_IV_LEN) != 1)
		return false;

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return false;

	bool b_ok = false;
	int int_len = 0;
	int int_cipher_len = 0;
	unsigned char arr_tag[GCM_TAG_LEN];
	unsigned char *ptr_cipher = out + SEQ_LEN + GCM_IV_LEN;

	// The AAD goes in as an EncryptUpdate with a NULL output buffer: it is
	// covered by the tag but not encrypted or written to out
	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
		EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL) == 1 &&
		EVP_EncryptInit_ex(ctx, NULL, NULL, g_arr_aes_key, arr_iv) == 1 &&
		EVP_EncryptUpdate(ctx, NULL, &int_len, arr_aad, (int)aad_len) == 1 &&
		EVP_EncryptUpdate(ctx, ptr_cipher, &int_len, in, (int)in_len) == 1)
	{
		int_cipher_len = int_len;

		if (EVP_EncryptFinal_ex(ctx, ptr_cipher + int_cipher_len, &int_len) == 1 &&
			EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_LEN, arr_tag) == 1)
		{
			int_cipher_len += int_len;
			put_be(out, uint_seq, SEQ_LEN);
			memcpy(out + SEQ_LEN, arr_iv, GCM_IV_LEN);
			memcpy(ptr_cipher + int_cipher_len, arr_tag, GCM_TAG_LEN);
			*out_len = (size_t)(GCM_OVERHEAD + int_cipher_len);
			b_ok = true;
		}
	}

	EVP_CIPHER_CTX_free(ctx);
	return b_ok;
}

bool security_decrypt(SecurityMsgType a_type, const int *a_arr_bound,
                       size_t a_num_bound, SecurityReplayGuard *a_guard,
                       const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t out_cap, size_t *out_len)
{
	// in_len comes straight from the network (len_cipher), so bound it by
	// the fixed arr_cipher size before reading the tag from its end
	if (in_len < GCM_OVERHEAD || in_len > SECURITY_CIPHERTEXT_LEN)
		return false;

	uint64_t uint_seq = 0;
	for (int i = 0; i < SEQ_LEN; i++)
		uint_seq = (uint_seq << 8) | in[i];

	// Cheap early-out for an obvious replay; the guard is only advanced
	// below, once the sequence number has been authenticated
	if (a_guard && a_guard->b_seen && uint_seq <= a_guard->uint_last_seq)
		return false;

	// Rebuilt from what the receiver expects, not read from the message:
	// if the sender bound it to another type/key/session, the tag fails
	unsigned char arr_aad[AAD_MAX_LEN];
	size_t aad_len = make_aad(a_type, uint_seq, a_arr_bound, a_num_bound, arr_aad);
	if (aad_len == 0)
		return false;

	size_t cipher_len = in_len - GCM_OVERHEAD;
	const unsigned char *ptr_iv = in + SEQ_LEN;
	const unsigned char *ptr_cipher = ptr_iv + GCM_IV_LEN;
	// GCM tag is a copy, not const: EVP_CIPHER_CTX_ctrl's void* parameter
	// isn't const-qualified even though SET_TAG only reads it
	unsigned char arr_tag[GCM_TAG_LEN];
	memcpy(arr_tag, ptr_cipher + cipher_len, GCM_TAG_LEN);

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return false;

	bool b_ok = false;
	int int_len = 0;
	int int_plain_len = 0;
	// GCM's DecryptUpdate emits plaintext before the tag is checked, so
	// decrypt into a scratch buffer sized for the largest possible
	// plaintext and only copy it to out once it has been authenticated -
	// otherwise a forged message could write past a small caller buffer
	unsigned char arr_scratch[SECURITY_CIPHERTEXT_LEN];

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
		EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL) == 1 &&
		EVP_DecryptInit_ex(ctx, NULL, NULL, g_arr_aes_key, ptr_iv) == 1 &&
		EVP_DecryptUpdate(ctx, NULL, &int_len, arr_aad, (int)aad_len) == 1 &&
		EVP_DecryptUpdate(ctx, arr_scratch, &int_len, ptr_cipher, (int)cipher_len) == 1)
	{
		int_plain_len = int_len;

		if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_LEN, arr_tag) == 1 &&
			EVP_DecryptFinal_ex(ctx, arr_scratch + int_plain_len, &int_len) == 1)
		{
			// EVP_DecryptFinal_ex() only succeeds if the computed tag
			// matches arr_tag, so reaching here means the ciphertext, its
			// sequence number and the type/key fields it is bound to are
			// all genuinely from a holder of g_arr_aes_key and unmodified
			int_plain_len += int_len;
			if ((size_t)int_plain_len <= out_cap)
			{
				memcpy(out, arr_scratch, (size_t)int_plain_len);
				*out_len = (size_t)int_plain_len;
				if (a_guard)
				{
					a_guard->b_seen = true;
					a_guard->uint_last_seq = uint_seq;
				}
				b_ok = true;
			}
		}
	}

	OPENSSL_cleanse(arr_scratch, sizeof(arr_scratch));
	EVP_CIPHER_CTX_free(ctx);
	return b_ok;
}
