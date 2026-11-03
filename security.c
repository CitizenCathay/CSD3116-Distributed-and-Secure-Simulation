/*****************************************************************************
\file security.c
\author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/04/2026 (MM/DD/YYYY)
\brief Implementation of security.h: pre-shared-key client authentication and
       AES-256-GCM encryption of DDS traffic, using OpenSSL's EVP API.
*******************************************************************************/
/* Two separate 32-byte pre-shared secrets, compiled into both client.c and
server.c (both link this same file), so they always agree without any key
exchange step:
  - g_arr_auth_psk  proves a client is one of ours before the server admits
    it (the credential in JoinRequest.arr_token)
  - g_arr_aes_key   encrypts/decrypts the Input/Position/CellClaimed/
    GameState payloads with AES-256-GCM

This is a course-project pre-shared-key scheme, not production key
management: a real deployment would provision/rotate these out of band
(per-client credentials, a KMS, mutual TLS) instead of hardcoding one shared
secret in source that every client and the server compile in identically. */

#include "security.h"
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

// 32 arbitrary bytes - only needs to match between every client and the
// server, not be memorable or secret-looking
static const unsigned char g_arr_auth_psk[SECURITY_TOKEN_LEN] =
{
	0x4a, 0x1f, 0x9c, 0x2e, 0x77, 0xb3, 0x05, 0x8d,
	0xe1, 0x62, 0xaa, 0x3b, 0x90, 0x14, 0xc7, 0x5f,
	0x28, 0xd6, 0x4e, 0x81, 0xf3, 0x0a, 0x97, 0x6c,
	0x15, 0xbe, 0x39, 0xa4, 0x72, 0xcd, 0x08, 0x5a
};

// A second, independent 32-byte secret for AES-256 - never reuse the auth
// secret as an encryption key
static const unsigned char g_arr_aes_key[32] =
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

bool security_init(void)
{
	// Nothing to allocate: both secrets above are compiled in. A real
	// implementation using provisioned/rotated keys would load them here
	return true;
}

void security_cleanup(void)
{
	// Nothing was allocated by security_init()
}

void security_make_client_token(char *out_token, size_t token_len)
{
	size_t copy_len = token_len < SECURITY_TOKEN_LEN ? token_len : SECURITY_TOKEN_LEN;
	memcpy(out_token, g_arr_auth_psk, copy_len);
}

bool security_verify_client_token(const char *token)
{
	// Constant-time compare: a length/early-exit compare (eg memcmp) would
	// let an attacker learn the secret one byte at a time by timing how
	// many leading bytes matched across many join attempts
	unsigned char b_diff = 0;
	for (size_t i = 0; i < SECURITY_TOKEN_LEN; i++)
	{
		b_diff |= (unsigned char)token[i] ^ g_arr_auth_psk[i];
	}
	return b_diff == 0;
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
                       unsigned char *out, size_t *out_len)
{
	if (in_len < GCM_OVERHEAD)
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

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
		EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL) == 1 &&
		EVP_DecryptInit_ex(ctx, NULL, NULL, g_arr_aes_key, ptr_iv) == 1 &&
		EVP_DecryptUpdate(ctx, out, &int_len, ptr_cipher, (int)cipher_len) == 1)
	{
		int_plain_len = int_len;

		if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_LEN, arr_tag) == 1 &&
			EVP_DecryptFinal_ex(ctx, out + int_plain_len, &int_len) == 1)
		{
			// EVP_DecryptFinal_ex() only succeeds if the computed tag
			// matches arr_tag, so reaching here means the ciphertext (and
			// the player/cell/position fields packed into it) is both
			// genuinely from a holder of g_arr_aes_key and unmodified
			int_plain_len += int_len;
			*out_len = (size_t)int_plain_len;
			b_ok = true;
		}
	}

	EVP_CIPHER_CTX_free(ctx);
	return b_ok;
}
