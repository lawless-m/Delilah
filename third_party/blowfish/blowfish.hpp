// Blowfish block cipher — Bruce Schneier, 1993.
//
// Constants (P-array and S-boxes) are derived from the hexadecimal digits
// of pi and are a mathematical artifact placed in the public domain by
// the algorithm's author. The implementation here is a clean-room port
// based on Paul Kocher's widely-vendored public-domain C reference.
//
// We expose ECB block encryption (used by the login path) plus
// decryption (used by tests to round-trip-verify the CBC layer).
// Higher layers add CBC chaining on top.

#pragma once

#include <cstddef>
#include <cstdint>

namespace dbisam_blowfish {

struct Context {
    uint32_t P[18];
    uint32_t S[4][256];
};

// Initialise from a variable-length key (1..56 bytes).
void key_setup(Context &ctx, const uint8_t *key, size_t key_len);

// Encrypt one 8-byte block in place (big-endian on the wire).
void encrypt_block(const Context &ctx, uint8_t block[8]);

// Decrypt one 8-byte block in place.
void decrypt_block(const Context &ctx, uint8_t block[8]);

} // namespace dbisam_blowfish
