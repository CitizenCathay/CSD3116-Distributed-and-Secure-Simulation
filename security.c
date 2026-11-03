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

This is still a course-project pre-shared-key scheme, not production key
management: a real deployment would provision/rotate per-client credentials
(eg via a KMS or mutual TLS) rather than one shared secret every client and
the server compile in or export identically. */

#include "security.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

// Wire format for security_encrypt()'s out buffer: [IV][ciphertext][tag]
#define GCM_OVERHEAD (GCM_IV_LEN + GCM_TAG_LEN)

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

	return true;
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

bool security_encrypt(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t *out_len)
{
	// out is always a fixed SECURITY_CIPHERTEXT_LEN-byte buffer (the
	// arr_cipher field of whichever message is being sent) - refuse rather
	// than overflow it if a future, larger message type doesn't fit
	if (in_len + GCM_OVERHEAD > SECURITY_CIPHERTEXT_LEN)
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
	unsigned char *ptr_cipher = out + GCM_IV_LEN;

	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
		EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL) == 1 &&
		EVP_EncryptInit_ex(ctx, NULL, NULL, g_arr_aes_key, arr_iv) == 1 &&
		EVP_EncryptUpdate(ctx, ptr_cipher, &int_len, in, (int)in_len) == 1)
	{
		int_cipher_len = int_len;

		if (EVP_EncryptFinal_ex(ctx, ptr_cipher + int_cipher_len, &int_len) == 1 &&
			EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_LEN, arr_tag) == 1)
		{
			int_cipher_len += int_len;
			memcpy(out, arr_iv, GCM_IV_LEN);
			memcpy(ptr_cipher + int_cipher_len, arr_tag, GCM_TAG_LEN);
			*out_len = (size_t)(GCM_IV_LEN + int_cipher_len + GCM_TAG_LEN);
			b_ok = true;
		}
	}

	EVP_CIPHER_CTX_free(ctx);
	return b_ok;
}

bool security_decrypt(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t out_cap, size_t *out_len)
{
	// in_len comes straight from the network (len_cipher), so bound it by
	// the fixed arr_cipher size before reading the tag from its end
	if (in_len < GCM_OVERHEAD || in_len > SECURITY_CIPHERTEXT_LEN)
		return false;

	size_t cipher_len = in_len - GCM_OVERHEAD;
	const unsigned char *ptr_iv = in;
	const unsigned char *ptr_cipher = in + GCM_IV_LEN;
	// GCM tag is a copy, not const: EVP_CIPHER_CTX_ctrl's void* parameter
	// isn't const-qualified even though SET_TAG only reads it
	unsigned char arr_tag[GCM_TAG_LEN];
	memcpy(arr_tag, in + GCM_IV_LEN + cipher_len, GCM_TAG_LEN);

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
		EVP_DecryptUpdate(ctx, arr_scratch, &int_len, ptr_cipher, (int)cipher_len) == 1)
	{
		int_plain_len = int_len;

		if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_LEN, arr_tag) == 1 &&
			EVP_DecryptFinal_ex(ctx, arr_scratch + int_plain_len, &int_len) == 1)
		{
			// EVP_DecryptFinal_ex() only succeeds if the computed tag
			// matches arr_tag, so reaching here means the ciphertext (and
			// the player/cell/position fields packed into it) is both
			// genuinely from a holder of g_arr_aes_key and unmodified
			int_plain_len += int_len;
			if ((size_t)int_plain_len <= out_cap)
			{
				memcpy(out, arr_scratch, (size_t)int_plain_len);
				*out_len = (size_t)int_plain_len;
				b_ok = true;
			}
		}
	}

	OPENSSL_cleanse(arr_scratch, sizeof(arr_scratch));
	EVP_CIPHER_CTX_free(ctx);
	return b_ok;
}
