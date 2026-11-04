/*****************************************************************************
\file security.h
\author regine wong (w.shiyuregine@digipen.edu)
\co-author choi meng yew (choi.m@digipen.edu)
\par Course: CSD3116
\par Assignment: Week 1 Team Lab
\date 11/04/2026 (MM/DD/YYYY)
\brief Authentication and encryption API shared by the server and client: client
       token creation/verification and encrypt/decrypt of DDS payloads.
*******************************************************************************/
/* Shared by server.c and client.c, both of which call every function below
at the right place (join handshake, Input/Position/CellClaimed/GameState
publish and receive). Implements the two required security features
(Slide 14, Tasks):
1. Authentication of clients (during Join Request/Response) - an
   HMAC-SHA256 of the request's nonce under a 32-byte pre-shared key, so
   the key itself is never sent over the network
2. Encryption of DDS traffic - AES-256-GCM (confidentiality + tamper
   detection) via OpenSSL's EVP API. Each ciphertext is also bound, via
   GCM associated data, to its message type and the DDS key fields it is
   published under, and carries a sequence number receivers use to drop
   replays

See security.c's header comment for the pre-shared-key scheme and why it's
a course-project simplification, not production key management - in
particular, every client holds the same key, so this protects against
outsiders on the DDS domain, not against a malicious client. */

#ifndef SECURITY_H
#define SECURITY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// ### CONSTANTS ###
// Must match arr_token[] in messages.idl's JoinRequest
#define SECURITY_TOKEN_LEN   32
// Must match arr_cipher[] in messages.idl's Input, Position, CellClaimed
// and GameState. security_encrypt() refuses to run if a payload wouldn't
// fit (8-byte sequence number + 12-byte IV + plaintext + 16-byte GCM tag)
#define SECURITY_CIPHERTEXT_LEN 64
#define SECURITY_MAX_CIPHER_OVERHEAD 36
// Most DDS key/session fields one ciphertext can be bound to
#define SECURITY_MAX_BOUND 4

// Which message a ciphertext belongs to. Part of the GCM associated data,
// so a ciphertext copied from one message type into another (eg a Position
// into a CellClaimed) fails to decrypt
typedef enum
{
	SECURITY_MSG_JOIN_RESPONSE = 1,
	SECURITY_MSG_INPUT,
	SECURITY_MSG_POSITION,
	SECURITY_MSG_CELL_CLAIMED,
	SECURITY_MSG_GAME_STATE
} SecurityMsgType;

// Receiver-side replay state for one stream of messages (eg one player's
// Position instance). Zero-initialise ({0}) for a stream nothing has been
// received on yet. security_decrypt() only accepts a sequence number
// higher than the last one it authenticated for this stream
typedef struct
{
	bool b_seen;
	uint64_t uint_last_seq;
} SecurityReplayGuard;

// ### AUTHENTICATION ###

// Called once by server and client before any DDS traffic is sent.
// Returns false (and the caller must exit) if a key environment variable
// is set but malformed, rather than silently using the built-in key
bool security_init(void);

// Called on shutdown to release any state security_init() allocated
void security_cleanup(void);

// Client side: fills out_token with the credential to present in the
// JoinRequest message - an HMAC of a_int_nonce (JoinRequest.int_nonce)
void security_make_client_token(int a_int_nonce, char *out_token, size_t token_len);

// Client side: returns a random nonce for JoinRequest.int_nonce from
// OpenSSL's CSPRNG, so it cannot be predicted from the time or process ID
// the way rand() could be. The server binds that client's Input to it
int security_make_nonce(void);

// Server side: validates the credential received in a JoinRequest against
// that request's nonce. token must be SECURITY_TOKEN_LEN bytes.
// Returns true if the client should be admitted
bool security_verify_client_token(int a_int_nonce, const char *token);

// ### ENCRYPTION ###

// Encrypts in_len bytes from in into out. out must be at least
// in_len + SECURITY_MAX_CIPHER_OVERHEAD bytes. Writes the ciphertext
// length to out_len. Each call uses a fresh random IV, so encrypting the
// same plaintext twice never produces the same ciphertext, and a fresh
// sequence number (strictly increasing within this process, and seeded
// from the clock so a restarted process keeps counting upwards).
// The ciphertext is bound to a_type and the a_num_bound values in
// a_arr_bound (the sample's DDS key fields, plus any session value such as
// a join nonce): decrypting with a different type or bound values fails.
// Those values are authenticated but not encrypted, so they can stay in
// the clear where DDS needs them for routing.
// Used by server.c before publishing Position/CellClaimed/GameState/
// JoinResponse samples and by client.c before publishing Input samples.
bool security_encrypt(SecurityMsgType a_type, const int *a_arr_bound,
                       size_t a_num_bound, const unsigned char *in,
                       size_t in_len, unsigned char *out, size_t *out_len);

// Decrypts in_len bytes from in into out, which holds out_cap bytes.
// a_type/a_arr_bound/a_num_bound must be the values the sender passed to
// security_encrypt(), rebuilt from the received sample. If a_guard is not
// NULL, the message is also rejected unless its sequence number is higher
// than the last one accepted on a_guard, which is then advanced.
// Returns false if the data was tampered with, used the wrong key, type or
// bound values, is a replay, is longer than SECURITY_CIPHERTEXT_LEN,
// decrypts to more than out_cap bytes, or is otherwise not authentic -
// callers must treat a false return as "drop this message", not as
// equivalent to receiving nothing. out and a_guard are only written once
// the data has been authenticated.
bool security_decrypt(SecurityMsgType a_type, const int *a_arr_bound,
                       size_t a_num_bound, SecurityReplayGuard *a_guard,
                       const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t out_cap, size_t *out_len);

#endif // SECURITY_H
