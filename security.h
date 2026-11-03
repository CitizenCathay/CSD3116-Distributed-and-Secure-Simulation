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
   detection) via OpenSSL's EVP API

See security.c's header comment for the pre-shared-key scheme and why it's
a course-project simplification, not production key management. */

#ifndef SECURITY_H
#define SECURITY_H

#include <stddef.h>
#include <stdbool.h>

// ### CONSTANTS ###
// Must match arr_token[] in messages.idl's JoinRequest
#define SECURITY_TOKEN_LEN   32
// Must match arr_cipher[] in messages.idl's Input, Position, CellClaimed
// and GameState. security_encrypt() refuses to run if a payload wouldn't
// fit (12-byte IV + plaintext + 16-byte GCM tag)
#define SECURITY_CIPHERTEXT_LEN 64
#define SECURITY_MAX_CIPHER_OVERHEAD 32

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

// Server side: validates the credential received in a JoinRequest against
// that request's nonce. token must be SECURITY_TOKEN_LEN bytes.
// Returns true if the client should be admitted
bool security_verify_client_token(int a_int_nonce, const char *token);

// ### ENCRYPTION ###

// Encrypts in_len bytes from in into out. out must be at least
// in_len + SECURITY_MAX_CIPHER_OVERHEAD bytes. Writes the ciphertext
// length to out_len. Each call uses a fresh random IV, so encrypting the
// same plaintext twice never produces the same ciphertext.
// Used by server.c before publishing Position/CellClaimed/GameState
// samples and by client.c before publishing Input samples.
bool security_encrypt(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t *out_len);

// Decrypts in_len bytes from in into out, which holds out_cap bytes.
// Returns false if the data was tampered with, used the wrong key, is
// longer than SECURITY_CIPHERTEXT_LEN, decrypts to more than out_cap bytes,
// or is otherwise not authentic - callers must treat a false return as
// "drop this message", not as equivalent to receiving nothing. out is only
// written once the data has been authenticated.
bool security_decrypt(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t out_cap, size_t *out_len);

#endif // SECURITY_H
