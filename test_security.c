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
#include <stdlib.h>
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

	security_make_client_token(1234, arr_token, sizeof(arr_token));

	TEST_ASSERT(security_verify_client_token(1234, arr_token) == true,
		"a token produced by security_make_client_token() should be accepted");
}

static void test_verify_client_token_rejects_wrong_token(void)
{
	char arr_wrong[SECURITY_TOKEN_LEN];
	memset(arr_wrong, 0x42, sizeof(arr_wrong));
	TEST_ASSERT(security_verify_client_token(1234, arr_wrong) == false,
		"an arbitrary wrong token should be rejected");

	char arr_zero[SECURITY_TOKEN_LEN];
	memset(arr_zero, 0, sizeof(arr_zero));
	TEST_ASSERT(security_verify_client_token(1234, arr_zero) == false,
		"an all-zero token should be rejected");
}

static void test_client_token_is_bound_to_nonce(void)
{
	// The token is an HMAC of the nonce, so one sniffed from a JoinRequest
	// is only good for that nonce, and differs from request to request
	char arr_token1[SECURITY_TOKEN_LEN];
	char arr_token2[SECURITY_TOKEN_LEN];
	security_make_client_token(1, arr_token1, sizeof(arr_token1));
	security_make_client_token(2, arr_token2, sizeof(arr_token2));

	TEST_ASSERT(security_verify_client_token(2, arr_token1) == false,
		"a token made for one nonce should be rejected for another");
	TEST_ASSERT(memcmp(arr_token1, arr_token2, SECURITY_TOKEN_LEN) != 0,
		"different nonces should produce different tokens");
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
		sizeof(arr_decrypted), &plain_len), "decrypt should succeed");
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
		sizeof(arr_decrypted), &plain_len) == false,
		"tampered ciphertext should fail authentication");
}

static void test_decrypt_rejects_truncated_ciphertext(void)
{
	unsigned char arr_short[4] = { 0 };
	unsigned char arr_decrypted[16];
	size_t plain_len = 0;

	TEST_ASSERT(security_decrypt(arr_short, sizeof(arr_short), arr_decrypted,
		sizeof(arr_decrypted), &plain_len) == false,
		"ciphertext too short to hold an IV + tag should be rejected, not crash");
}

static void test_decrypt_rejects_oversized_ciphertext(void)
{
	// len_cipher comes from the network and can claim more than the fixed
	// arr_cipher size - it must be rejected before anything is read
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN + 1] = { 0 };
	unsigned char arr_decrypted[SECURITY_CIPHERTEXT_LEN];
	size_t plain_len = 0;

	TEST_ASSERT(security_decrypt(arr_cipher, sizeof(arr_cipher), arr_decrypted,
		sizeof(arr_decrypted), &plain_len) == false,
		"ciphertext longer than SECURITY_CIPHERTEXT_LEN should be rejected");
}

static void test_decrypt_forged_cipher_does_not_overflow(void)
{
	// A forged, full-size arr_cipher (no key needed) must not write past a
	// small caller buffer, even though it fails authentication - GCM emits
	// plaintext before checking the tag. Mimics server.c's 5-byte Input buffer
	struct
	{
		unsigned char arr_plain[sizeof(int) + sizeof(char)];
		unsigned char arr_guard[SECURITY_CIPHERTEXT_LEN];
	} frame;
	memset(&frame, 0xCC, sizeof(frame));
	unsigned char arr_forged[SECURITY_CIPHERTEXT_LEN];
	memset(arr_forged, 0x41, sizeof(arr_forged));
	size_t plain_len = 0;

	TEST_ASSERT(security_decrypt(arr_forged, sizeof(arr_forged), frame.arr_plain,
		sizeof(frame.arr_plain), &plain_len) == false,
		"a forged ciphertext should fail authentication");

	bool b_untouched = true;
	for (size_t i = 0; i < sizeof(frame); i++)
		if (((unsigned char *)&frame)[i] != 0xCC)
			b_untouched = false;
	TEST_ASSERT(b_untouched,
		"a forged ciphertext should not write to the output buffer or past it");
}

static void test_decrypt_rejects_output_too_small(void)
{
	// Authentic ciphertext whose plaintext does not fit out_cap must be
	// rejected rather than truncated or overflowed
	const unsigned char arr_plain[] = "hello";
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain) - 1];
	size_t cipher_len = 0;
	size_t plain_len = 0;

	security_encrypt(arr_plain, sizeof(arr_plain), arr_cipher, &cipher_len);
	TEST_ASSERT(security_decrypt(arr_cipher, cipher_len, arr_decrypted,
		sizeof(arr_decrypted), &plain_len) == false,
		"plaintext larger than out_cap should be rejected");
}

// --- Environment-variable key override -------------------------------
// These two run last: a valid override permanently replaces the active
// key for the rest of this process, which would otherwise invalidate the
// round-trip/tamper tests above that assume the built-in default key

static void test_env_var_overrides_auth_key(void)
{
	// 32 bytes ("a1" x 32 = 64 hex characters)
	const char *str_custom_hex =
		"a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1";
	TEST_ASSERT(strlen(str_custom_hex) == SECURITY_TOKEN_LEN * 2,
		"test bug: str_custom_hex must be exactly 64 hex characters");

	setenv("CSD3116_AUTH_PSK", str_custom_hex, 1);
	TEST_ASSERT(security_init() == true, "security_init() should still succeed");

	char arr_token[SECURITY_TOKEN_LEN];
	security_make_client_token(1234, arr_token, sizeof(arr_token));
	TEST_ASSERT(security_verify_client_token(1234, arr_token) == true,
		"a token made after overriding the key should still round-trip");

	char arr_other[SECURITY_TOKEN_LEN];
	memset(arr_other, 0x99, sizeof(arr_other));
	TEST_ASSERT(security_verify_client_token(1234, arr_other) == false,
		"an unrelated token should still be rejected under the overridden key");

	unsetenv("CSD3116_AUTH_PSK");
}

static void test_env_var_invalid_is_rejected(void)
{
	// Too short, and not hex - security_init() must not crash, and must
	// fail rather than quietly run with the public built-in key
	setenv("CSD3116_AUTH_PSK", "not-a-valid-key", 1);
	TEST_ASSERT(security_init() == false,
		"an invalid auth key override should make security_init() fail");
	unsetenv("CSD3116_AUTH_PSK");

	setenv("CSD3116_AES_KEY", "not-a-valid-key", 1);
	TEST_ASSERT(security_init() == false,
		"an invalid AES key override should make security_init() fail");
	unsetenv("CSD3116_AES_KEY");
}

int main(void)
{
	test_init_cleanup();
	test_make_client_token_is_accepted();
	test_verify_client_token_rejects_wrong_token();
	test_client_token_is_bound_to_nonce();
	test_encrypt_decrypt_round_trip();
	test_encrypt_is_randomized();
	test_decrypt_rejects_tampered_ciphertext();
	test_decrypt_rejects_truncated_ciphertext();
	test_decrypt_rejects_oversized_ciphertext();
	test_decrypt_forged_cipher_does_not_overflow();
	test_decrypt_rejects_output_too_small();
	test_env_var_overrides_auth_key();
	test_env_var_invalid_is_rejected();

	printf("%d/%d tests passed\n",
		g_int_tests_run - g_int_tests_failed, g_int_tests_run);

	return g_int_tests_failed == 0 ? 0 : 1;
}
