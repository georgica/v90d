/* Ed25519 signature check (RFC 8032), verification only: ed25519.c. */
#ifndef V90D_ED25519_H
#define V90D_ED25519_H
#include <stddef.h>
#include <stdint.h>

/* 0 when sig is pk's signature of m[0..n), -1 when it is not */
int  ed25519_verify(const uint8_t sig[64], const uint8_t *m, size_t n, const uint8_t pk[32]);
/* SHA-512 of m[0..n) (the check's own; for its tests) */
void ed25519_sha512(const uint8_t *m, size_t n, uint8_t out[64]);

#endif
