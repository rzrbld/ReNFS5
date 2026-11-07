/* SHA-256 (FIPS 180-4), used to verify the user's original NFS5 files.
 *
 * Self-contained so the handheld launcher needs no OpenSSL or libcrypto.
 * This is a straightforward implementation of the standard; see
 * docs/packaging/LICENSE.md for attribution notes.
 */
#ifndef NFS5_SHA256_H
#define NFS5_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define NFS5_SHA256_HEX_LEN 64

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t buffer[64];
    size_t buffer_len;
} Nfs5Sha256;

void nfs5_sha256_init(Nfs5Sha256 *ctx);
void nfs5_sha256_update(Nfs5Sha256 *ctx, const void *data, size_t len);
void nfs5_sha256_final(Nfs5Sha256 *ctx, uint8_t digest[32]);
void nfs5_sha256_hex(const uint8_t digest[32], char out[NFS5_SHA256_HEX_LEN + 1]);

/* Returns 0 on success, -1 if @path cannot be opened/read. */
int nfs5_sha256_file(const char *path, char out_hex[NFS5_SHA256_HEX_LEN + 1]);

#endif
