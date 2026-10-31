/* security.h
Shared by server.c and client.c, both of which already call every
function below at the right place (join handshake, Input/Position
publish and receive). Covers the two required security features:
1. Authentication of clients (during Join Request/Response)
2. Encryption of DDS traffic (Input and Position messages)

The declarations are final; only the .c bodies are still stubs -
fill in the TODOs in security.c. */

#ifndef SECURITY_H
#define SECURITY_H

#include <stddef.h>
#include <stdbool.h>

// ### CONSTANTS ###
// TODO: size these once the credential/token/crypto scheme is chosen
// Must match arr_token[] in messages.idl's JoinRequest
#define SECURITY_TOKEN_LEN   32
// Must match arr_cipher[] in messages.idl's Input and Position
#define SECURITY_CIPHERTEXT_LEN 64
#define SECURITY_MAX_CIPHER_OVERHEAD 32

// ### AUTHENTICATION ###

// Called once by server and client before any DDS traffic is sent.
// TODO: load/derive the pre-shared secret used for auth + encryption
bool security_init(void);

// Called on shutdown to release any state security_init() allocated
void security_cleanup(void);

// Client side: fills out_token with the credential to present in the
// (extended) JoinRequest message
// TODO: decide what the client presents (password, pre-shared key, cert, ...)
void security_make_client_token(char *out_token, size_t token_len);

// Server side: validates the credential received in a JoinRequest
// Returns true if the client should be admitted
// TODO: implement real validation instead of accepting everything
bool security_verify_client_token(const char *token);

// ### ENCRYPTION ###

// Encrypts in_len bytes from in into out. out must be at least
// in_len + SECURITY_MAX_CIPHER_OVERHEAD bytes. Writes the ciphertext
// length to out_len.
// Used by server.c before publishing Position samples and by client.c
// before publishing Input samples.
// TODO: implement (e.g. AES-GCM / ChaCha20-Poly1305)
bool security_encrypt(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t *out_len);

// Decrypts in_len bytes from in into out. Returns false if the data
// was tampered with / fails authentication.
// TODO: implement, matching security_encrypt()
bool security_decrypt(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t *out_len);

#endif // SECURITY_H
