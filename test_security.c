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

Scope: only security.c is covered here. The game rules (collision,
spawning, movement) and the grid rules (claiming, scores, winner, timer)
have their own tests in test_game_logic.c and test_grid_logic.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "security.h"

static int g_int_tests_run = 0;
static int g_int_tests_failed = 0;

// Message type for the tests that are not about type binding
#define TEST_TYPE SECURITY_MSG_GAME_STATE

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

static void test_nonces_are_random(void)
{
	// Comes from the CSPRNG, so a run of nonces should not repeat (a clash
	// among 8 random 32-bit values is a ~1 in 600 million chance)
	int arr_nonce[8];
	for (int i = 0; i < 8; i++)
		arr_nonce[i] = security_make_nonce();

	bool b_all_different = true;
	for (int i = 0; i < 8; i++)
		for (int j = i + 1; j < 8; j++)
			if (arr_nonce[i] == arr_nonce[j])
				b_all_different = false;
	TEST_ASSERT(b_all_different, "security_make_nonce() should not repeat values");
}

static void test_encrypt_decrypt_round_trip(void)
{
	const unsigned char arr_plain[] = "hello";
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain)];
	size_t cipher_len = 0;
	size_t plain_len = 0;

	TEST_ASSERT(security_encrypt(TEST_TYPE, NULL, 0, arr_plain, sizeof(arr_plain), arr_cipher,
		&cipher_len), "encrypt should succeed");
	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, NULL, arr_cipher, cipher_len, arr_decrypted,
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

	security_encrypt(TEST_TYPE, NULL, 0, arr_plain, sizeof(arr_plain), arr_cipher1, &len1);
	security_encrypt(TEST_TYPE, NULL, 0, arr_plain, sizeof(arr_plain), arr_cipher2, &len2);

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

	security_encrypt(TEST_TYPE, NULL, 0, arr_plain, sizeof(arr_plain), arr_cipher, &cipher_len);
	arr_cipher[cipher_len - 1] ^= 0xFF; // flip a bit in the GCM auth tag

	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, NULL, arr_cipher, cipher_len, arr_decrypted,
		sizeof(arr_decrypted), &plain_len) == false,
		"tampered ciphertext should fail authentication");
}

static void test_decrypt_rejects_truncated_ciphertext(void)
{
	unsigned char arr_short[4] = { 0 };
	unsigned char arr_decrypted[16];
	size_t plain_len = 0;

	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, NULL, arr_short, sizeof(arr_short), arr_decrypted,
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

	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, NULL, arr_cipher, sizeof(arr_cipher), arr_decrypted,
		sizeof(arr_decrypted), &plain_len) == false,
		"ciphertext longer than SECURITY_CIPHERTEXT_LEN should be rejected");
}

static void test_decrypt_forged_cipher_does_not_overflow(void)
{
	// A forged, full-size arr_cipher (no key needed) must not write past a
	// small caller buffer, even though it fails authentication - GCM emits
	// plaintext before checking the tag. Mimics server.c's 1-byte Input buffer
	struct
	{
		unsigned char arr_plain[1];
		unsigned char arr_guard[SECURITY_CIPHERTEXT_LEN];
	} frame;
	memset(&frame, 0xCC, sizeof(frame));
	unsigned char arr_forged[SECURITY_CIPHERTEXT_LEN];
	memset(arr_forged, 0x41, sizeof(arr_forged));
	size_t plain_len = 0;

	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, NULL, arr_forged, sizeof(arr_forged), frame.arr_plain,
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

	security_encrypt(TEST_TYPE, NULL, 0, arr_plain, sizeof(arr_plain), arr_cipher, &cipher_len);
	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, NULL, arr_cipher, cipher_len, arr_decrypted,
		sizeof(arr_decrypted), &plain_len) == false,
		"plaintext larger than out_cap should be rejected");
}

// --- AAD binding (message type + DDS key fields) ----------------------

static void test_decrypt_with_matching_binding(void)
{
	const unsigned char arr_plain[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	int arr_bound[1] = { 3 };
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain)];
	size_t cipher_len = 0;
	size_t plain_len = 0;

	TEST_ASSERT(security_encrypt(SECURITY_MSG_POSITION, arr_bound, 1, arr_plain,
		sizeof(arr_plain), arr_cipher, &cipher_len), "encrypt should succeed");
	TEST_ASSERT(cipher_len == sizeof(arr_plain) + SECURITY_MAX_CIPHER_OVERHEAD,
		"ciphertext should be plaintext + SECURITY_MAX_CIPHER_OVERHEAD bytes");
	TEST_ASSERT(security_decrypt(SECURITY_MSG_POSITION, arr_bound, 1, NULL,
		arr_cipher, cipher_len, arr_decrypted, sizeof(arr_decrypted), &plain_len) &&
		plain_len == sizeof(arr_plain) &&
		memcmp(arr_plain, arr_decrypted, plain_len) == 0,
		"decrypting with the same type and bound values should recover the plaintext");
}

static void test_decrypt_rejects_wrong_type(void)
{
	// eg a recorded Position ciphertext pasted into a CellClaimed sample
	const unsigned char arr_plain[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	int arr_bound[1] = { 3 };
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain)];
	size_t cipher_len = 0;
	size_t plain_len = 0;

	security_encrypt(SECURITY_MSG_POSITION, arr_bound, 1, arr_plain,
		sizeof(arr_plain), arr_cipher, &cipher_len);
	TEST_ASSERT(security_decrypt(SECURITY_MSG_CELL_CLAIMED, arr_bound, 1, NULL,
		arr_cipher, cipher_len, arr_decrypted, sizeof(arr_decrypted), &plain_len) == false,
		"a ciphertext made for one message type should not decrypt as another");
}

static void test_decrypt_rejects_wrong_key_field(void)
{
	// eg player 2's Position ciphertext republished under player 3's key
	const unsigned char arr_plain[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	int arr_bound2[1] = { 2 };
	int arr_bound3[1] = { 3 };
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain)];
	size_t cipher_len = 0;
	size_t plain_len = 0;

	security_encrypt(SECURITY_MSG_POSITION, arr_bound2, 1, arr_plain,
		sizeof(arr_plain), arr_cipher, &cipher_len);
	TEST_ASSERT(security_decrypt(SECURITY_MSG_POSITION, arr_bound3, 1, NULL,
		arr_cipher, cipher_len, arr_decrypted, sizeof(arr_decrypted), &plain_len) == false,
		"a ciphertext bound to one DDS key should not decrypt under another");
	TEST_ASSERT(security_decrypt(SECURITY_MSG_POSITION, NULL, 0, NULL,
		arr_cipher, cipher_len, arr_decrypted, sizeof(arr_decrypted), &plain_len) == false,
		"a ciphertext bound to a DDS key should not decrypt with no bound values");

	// Input is bound to (player ID, join nonce): same player, earlier session
	int arr_session_old[2] = { 2, 1111 };
	int arr_session_new[2] = { 2, 2222 };
	security_encrypt(SECURITY_MSG_INPUT, arr_session_old, 2, arr_plain, 1,
		arr_cipher, &cipher_len);
	TEST_ASSERT(security_decrypt(SECURITY_MSG_INPUT, arr_session_new, 2, NULL,
		arr_cipher, cipher_len, arr_decrypted, sizeof(arr_decrypted), &plain_len) == false,
		"an Input from an earlier session in the same slot should be rejected");
}

static void test_encrypt_rejects_too_many_bound(void)
{
	const unsigned char arr_plain[] = { 1 };
	int arr_bound[SECURITY_MAX_BOUND + 1] = { 0 };
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	size_t cipher_len = 0;

	TEST_ASSERT(security_encrypt(TEST_TYPE, arr_bound, SECURITY_MAX_BOUND + 1,
		arr_plain, sizeof(arr_plain), arr_cipher, &cipher_len) == false,
		"more than SECURITY_MAX_BOUND bound values should be refused");
}

// --- Replay protection -------------------------------------------------

static void test_replay_is_rejected(void)
{
	const unsigned char arr_plain[] = { 'u' };
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain)];
	size_t cipher_len = 0;
	size_t plain_len = 0;
	SecurityReplayGuard guard = {0};

	security_encrypt(TEST_TYPE, NULL, 0, arr_plain, sizeof(arr_plain),
		arr_cipher, &cipher_len);
	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, &guard, arr_cipher, cipher_len,
		arr_decrypted, sizeof(arr_decrypted), &plain_len),
		"the first copy of a message should be accepted");
	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, &guard, arr_cipher, cipher_len,
		arr_decrypted, sizeof(arr_decrypted), &plain_len) == false,
		"a second copy of the same message should be rejected as a replay");

	// A guard for a different stream has its own history
	SecurityReplayGuard other_guard = {0};
	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, &other_guard, arr_cipher,
		cipher_len, arr_decrypted, sizeof(arr_decrypted), &plain_len),
		"a message should be accepted once on each independent stream");
}

static void test_older_message_is_rejected(void)
{
	// Messages delivered out of order (or an old one replayed after newer
	// ones) are dropped; newer ones keep being accepted
	const unsigned char arr_plain[] = { 'u' };
	unsigned char arr_old[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_new[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_newer[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain)];
	size_t old_len = 0, new_len = 0, newer_len = 0;
	size_t plain_len = 0;
	SecurityReplayGuard guard = {0};

	security_encrypt(TEST_TYPE, NULL, 0, arr_plain, sizeof(arr_plain), arr_old, &old_len);
	security_encrypt(TEST_TYPE, NULL, 0, arr_plain, sizeof(arr_plain), arr_new, &new_len);
	security_encrypt(TEST_TYPE, NULL, 0, arr_plain, sizeof(arr_plain), arr_newer, &newer_len);

	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, &guard, arr_new, new_len,
		arr_decrypted, sizeof(arr_decrypted), &plain_len), "newer message should be accepted");
	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, &guard, arr_old, old_len,
		arr_decrypted, sizeof(arr_decrypted), &plain_len) == false,
		"a message older than the last accepted one should be rejected");
	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, &guard, arr_newer, newer_len,
		arr_decrypted, sizeof(arr_decrypted), &plain_len),
		"later messages should still be accepted after a rejected replay");
}

static void test_forged_sequence_does_not_advance_guard(void)
{
	// An attacker without the key raises the sequence number of a real
	// message to the maximum. It must fail authentication, and must not
	// poison the guard into rejecting the sender's genuine later messages
	const unsigned char arr_plain[] = { 'u' };
	unsigned char arr_cipher[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_forged[SECURITY_CIPHERTEXT_LEN];
	unsigned char arr_decrypted[sizeof(arr_plain)];
	size_t cipher_len = 0;
	size_t plain_len = 0;
	SecurityReplayGuard guard = {0};

	security_encrypt(TEST_TYPE, NULL, 0, arr_plain, sizeof(arr_plain),
		arr_cipher, &cipher_len);
	memcpy(arr_forged, arr_cipher, cipher_len);
	memset(arr_forged, 0xFF, 8); // the sequence number is the first 8 bytes

	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, &guard, arr_forged, cipher_len,
		arr_decrypted, sizeof(arr_decrypted), &plain_len) == false,
		"a message with a tampered sequence number should fail authentication");
	TEST_ASSERT(guard.b_seen == false,
		"a message that fails authentication should not advance the guard");
	TEST_ASSERT(security_decrypt(TEST_TYPE, NULL, 0, &guard, arr_cipher, cipher_len,
		arr_decrypted, sizeof(arr_decrypted), &plain_len),
		"the genuine message should still be accepted after a forged one");
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
	test_nonces_are_random();
	test_encrypt_decrypt_round_trip();
	test_encrypt_is_randomized();
	test_decrypt_rejects_tampered_ciphertext();
	test_decrypt_rejects_truncated_ciphertext();
	test_decrypt_rejects_oversized_ciphertext();
	test_decrypt_forged_cipher_does_not_overflow();
	test_decrypt_rejects_output_too_small();
	test_decrypt_with_matching_binding();
	test_decrypt_rejects_wrong_type();
	test_decrypt_rejects_wrong_key_field();
	test_encrypt_rejects_too_many_bound();
	test_replay_is_rejected();
	test_older_message_is_rejected();
	test_forged_sequence_does_not_advance_guard();
	test_env_var_overrides_auth_key();
	test_env_var_invalid_is_rejected();

	printf("%d/%d tests passed\n",
		g_int_tests_run - g_int_tests_failed, g_int_tests_run);

	return g_int_tests_failed == 0 ? 0 : 1;
}
