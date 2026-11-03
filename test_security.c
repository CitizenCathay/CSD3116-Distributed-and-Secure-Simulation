/*****************************************************************************
\file test_security.c
\author regine wong (w.shiyuregine@digipen.edu)
\co-author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/04/2026 (MM/DD/YYYY)
\brief Dependency-free unit tests for security.c: pre-shared-key
       authentication and AES-256-GCM encrypt/decrypt.
*******************************************************************************/
/* Dependency-free unit tests for security.h / security.c, except for
OpenSSL's libcrypto (security.c's actual crypto implementation):

    gcc test_security.c security.c -o test_security $(pkg-config --cflags --libs libcrypto)
    ./test_security

(or just `make test_security` / `make test`, which already link libcrypto)

Scope: only security.c is covered here, because it is the one module with
no DDS/GLFW dependency and no main() of its own. detect_collision() in
server.c is NOT covered: server.c needs messages.h (DDS-generated) to even
compile, and already defines its own main(), so it can't be linked against
this test's main() without first being pulled out into its own DDS-free
module (eg collision.h/.c) - worth doing if that coverage is wanted later. */

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

static void test_make_client_token_is_accepted(void)
{
	// Black-box, not tied to the pre-shared key's actual bytes: whatever
	// the client produces, the server must accept - this is the real
	// end-to-end contract between security_make_client_token() and
	// security_verify_client_token()
	char arr_token[SECURITY_TOKEN_LEN];
	memset(arr_token, 0xAA, sizeof(arr_token));

	security_make_client_token(arr_token, sizeof(arr_token));

	TEST_ASSERT(security_verify_client_token(arr_token) == true,
		"a token produced by security_make_client_token() should be accepted");
}

static void test_verify_client_token_rejects_wrong_token(void)
{
	char arr_wrong[SECURITY_TOKEN_LEN];
	memset(arr_wrong, 0x42, sizeof(arr_wrong));
	TEST_ASSERT(security_verify_client_token(arr_wrong) == false,
		"an arbitrary wrong token should be rejected");

	char arr_zero[SECURITY_TOKEN_LEN];
	memset(arr_zero, 0, sizeof(arr_zero));
	TEST_ASSERT(security_verify_client_token(arr_zero) == false,
		"an all-zero token should be rejected");
}

static void test_encrypt_decrypt_round_trip(void)
{
	const unsigned char arr_plain[] = "hello";
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain)];
	size_t cipher_len = 0;
	size_t plain_len = 0;

	TEST_ASSERT(security_encrypt(arr_plain, sizeof(arr_plain), arr_cipher,
		&cipher_len), "encrypt should succeed");
	TEST_ASSERT(security_decrypt(arr_cipher, cipher_len, arr_decrypted,
		&plain_len), "decrypt should succeed");
	TEST_ASSERT(plain_len == sizeof(arr_plain), "decrypted length should match");
	TEST_ASSERT(memcmp(arr_plain, arr_decrypted, plain_len) == 0,
		"round trip should recover the original plaintext");
}

static void test_encrypt_is_randomized(void)
{
	// Catches a reused-IV bug: encrypting the same plaintext twice must
	// not produce the same ciphertext, or an eavesdropper could tell two
	// messages carried the same payload without even decrypting either
	const unsigned char arr_plain[] = "hello";
	unsigned char arr_cipher1[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_cipher2[SECURITY_CIPHERTEXT_LEN];
	size_t len1 = 0, len2 = 0;

	security_encrypt(arr_plain, sizeof(arr_plain), arr_cipher1, &len1);
	security_encrypt(arr_plain, sizeof(arr_plain), arr_cipher2, &len2);

	TEST_ASSERT(len1 == len2,
		"ciphertext length should be deterministic for the same plaintext length");
	TEST_ASSERT(memcmp(arr_cipher1, arr_cipher2, len1) != 0,
		"encrypting the same plaintext twice should not produce identical ciphertext");
}

static void test_decrypt_rejects_tampered_ciphertext(void)
{
	const unsigned char arr_plain[] = "hello";
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain)];
	size_t cipher_len = 0;
	size_t plain_len = 0;

	security_encrypt(arr_plain, sizeof(arr_plain), arr_cipher, &cipher_len);
	arr_cipher[cipher_len - 1] ^= 0xFF; // flip a bit in the GCM auth tag

	TEST_ASSERT(security_decrypt(arr_cipher, cipher_len, arr_decrypted,
		&plain_len) == false, "tampered ciphertext should fail authentication");
}

static void test_decrypt_rejects_truncated_ciphertext(void)
{
	unsigned char arr_short[4] = { 0 };
	unsigned char arr_decrypted[16];
	size_t plain_len = 0;

	TEST_ASSERT(security_decrypt(arr_short, sizeof(arr_short), arr_decrypted,
		&plain_len) == false,
		"ciphertext too short to hold an IV + tag should be rejected, not crash");
}

int main(void)
{
	test_init_cleanup();
	test_make_client_token_is_accepted();
	test_verify_client_token_rejects_wrong_token();
	test_encrypt_decrypt_round_trip();
	test_encrypt_is_randomized();
	test_decrypt_rejects_tampered_ciphertext();
	test_decrypt_rejects_truncated_ciphertext();

	printf("%d/%d tests passed\n",
		g_int_tests_run - g_int_tests_failed, g_int_tests_run);

	return g_int_tests_failed == 0 ? 0 : 1;
}
