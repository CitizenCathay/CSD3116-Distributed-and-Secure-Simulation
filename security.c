/* security.c
Empty skeleton for security.h - see that file for how each function
is meant to be used by server.c and client.c.

TODO: pick a crypto library (e.g. OpenSSL EVP, libsodium) and include
its headers here once implementation starts. */

#include "security.h"

bool security_init(void)
{
    // TODO: load pre-shared key / initialize crypto library state
    return true;
}

void security_cleanup(void)
{
    // TODO: free crypto library state
}

void security_make_client_token(char *out_token, size_t token_len)
{
    (void)out_token;
    (void)token_len;
    // TODO: fill out_token with the client's credential
}

bool security_verify_client_token(const char *token)
{
    (void)token;
    // TODO: compare against expected credential(s)
    return true;
}

bool security_encrypt(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t *out_len)
{
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    // TODO: implement encryption
    return false;
}

bool security_decrypt(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t *out_len)
{
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    // TODO: implement decryption
    return false;
}
