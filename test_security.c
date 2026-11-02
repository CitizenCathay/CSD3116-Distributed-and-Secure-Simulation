/*****************************************************************************
\file test_security.c
\author regine wong (w.shiyuregine@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/02/2026 (MM/DD/YYYY)
\brief Dependency-free unit tests for security.c; they document the current
       stub behaviour until each TODO is implemented.
*******************************************************************************/
/* Minimal, dependency-free unit tests for security.h / security.c.
No DDS, GLFW, or external test framework needed - just a C compiler:

    gcc test_security.c security.c -o test_security
    ./test_security

Scope: only security.c is covered here, because it is the one module
with no DDS/GLFW dependency and no main() of its own. detect_collision()
in server.c is NOT covered: server.c needs messages.h (DDS-generated)
to even compile, and already defines its own main(), so it can't be
linked against this test's main() without first being pulled out into
its own DDS-free module (eg collision.h/.c) - worth doing if that
coverage is wanted later.

Each test below documents the CURRENT stub behaviour in security.c.
As each TODO in security.c gets implemented, the matching test here
has a TODO telling you what to tighten. */

#include <stdio.h>
#include <string.h>
#include "security.h"

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

static void test_init_cleanup(void)
{
	TEST_ASSERT(security_init() == true, "security_init() should succeed");
	security_cleanup(); // just needs to not crash
}

static void test_make_client_token_smoke(void)
{
	char arr_token[SECURITY_TOKEN_LEN];
	memset(arr_token, 0xAA, sizeof(arr_token));

	// Must not crash / overflow the caller's buffer
	security_make_client_token(arr_token, sizeof(arr_token));

	// TODO: once security_make_client_token() is implemented, assert
	// something about the token it produces, eg that it is no longer
	// all 0xAA, and that it is consistent with how
	// security_verify_client_token() checks it server-side.
	TEST_ASSERT(1, "smoke test only - see TODO above");
}

static void test_verify_client_token_current_stub(void)
{
	// security_verify_client_token() is currently a stub that accepts
	// every token. This test pins that behaviour down so a future
	// change to it is caught here instead of silently changing what
	// server.c admits.
	//
	// TODO: once real validation is implemented, replace this with
	// accept/reject cases, eg:
	//   TEST_ASSERT(security_verify_client_token(valid_token) == true, ...);
	//   TEST_ASSERT(security_verify_client_token("") == false, ...);
	//   TEST_ASSERT(security_verify_client_token(wrong_token) == false, ...);
	TEST_ASSERT(security_verify_client_token("any-token") == true,
		"stub currently accepts any non-empty token");
	TEST_ASSERT(security_verify_client_token("") == true,
		"stub currently accepts an empty token too");
}

static void test_encrypt_decrypt_round_trip(void)
{
	const unsigned char arr_plain[] = "hello";
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain)];
	size_t cipher_len = 0;
	size_t plain_len = 0;

	bool b_encrypted = security_encrypt(arr_plain, sizeof(arr_plain),
		arr_cipher, &cipher_len);

	// TODO: once security_encrypt()/security_decrypt() are implemented,
	// swap this block for a real round-trip assertion, eg:
	//   TEST_ASSERT(b_encrypted, "encrypt should succeed");
	//   TEST_ASSERT(security_decrypt(arr_cipher, cipher_len,
	//       arr_decrypted, &plain_len), "decrypt should succeed");
	//   TEST_ASSERT(plain_len == sizeof(arr_plain), "length mismatch");
	//   TEST_ASSERT(memcmp(arr_plain, arr_decrypted, plain_len) == 0,
	//       "round trip should recover the original plaintext");
	(void)arr_decrypted;
	(void)plain_len;
	TEST_ASSERT(b_encrypted == false,
		"stub currently always fails to encrypt - remove once implemented");
}

int main(void)
{
	test_init_cleanup();
	test_make_client_token_smoke();
	test_verify_client_token_current_stub();
	test_encrypt_decrypt_round_trip();

	printf("%d/%d tests passed\n",
		g_int_tests_run - g_int_tests_failed, g_int_tests_run);

	return g_int_tests_failed == 0 ? 0 : 1;
}
