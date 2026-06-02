// MD5 — RFC 1321. Clean-room port of the reference pseudocode.

#include "md5.hpp"

#include <cstring>

namespace dbisam_md5 {

namespace {

// Per-round shift amounts (RFC 1321 §3.4).
constexpr uint32_t S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
};

// floor(abs(sin(i+1)) * 2^32), i in 0..63 (RFC 1321 §3.4 step 4).
constexpr uint32_t K[64] = {
    0xD76AA478u,0xE8C7B756u,0x242070DBu,0xC1BDCEEEu,
    0xF57C0FAFu,0x4787C62Au,0xA8304613u,0xFD469501u,
    0x698098D8u,0x8B44F7AFu,0xFFFF5BB1u,0x895CD7BEu,
    0x6B901122u,0xFD987193u,0xA679438Eu,0x49B40821u,
    0xF61E2562u,0xC040B340u,0x265E5A51u,0xE9B6C7AAu,
    0xD62F105Du,0x02441453u,0xD8A1E681u,0xE7D3FBC8u,
    0x21E1CDE6u,0xC33707D6u,0xF4D50D87u,0x455A14EDu,
    0xA9E3E905u,0xFCEFA3F8u,0x676F02D9u,0x8D2A4C8Au,
    0xFFFA3942u,0x8771F681u,0x6D9D6122u,0xFDE5380Cu,
    0xA4BEEA44u,0x4BDECFA9u,0xF6BB4B60u,0xBEBFBC70u,
    0x289B7EC6u,0xEAA127FAu,0xD4EF3085u,0x04881D05u,
    0xD9D4D039u,0xE6DB99E5u,0x1FA27CF8u,0xC4AC5665u,
    0xF4292244u,0x432AFF97u,0xAB9423A7u,0xFC93A039u,
    0x655B59C3u,0x8F0CCC92u,0xFFEFF47Du,0x85845DD1u,
    0x6FA87E4Fu,0xFE2CE6E0u,0xA3014314u,0x4E0811A1u,
    0xF7537E82u,0xBD3AF235u,0x2AD7D2BBu,0xEB86D391u,
};

inline uint32_t left_rotate(uint32_t x, uint32_t c) {
    return (x << c) | (x >> (32 - c));
}

inline uint32_t load32_le(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void store32_le(uint32_t v, uint8_t *p) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void process_block(uint32_t state[4], const uint8_t block[64]) {
    uint32_t M[16];
    for (int i = 0; i < 16; ++i) M[i] = load32_le(block + i * 4);

    uint32_t A = state[0], B = state[1], C = state[2], D = state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t F, g;
        if (i < 16) {
            F = (B & C) | (~B & D);
            g = i;
        } else if (i < 32) {
            F = (D & B) | (~D & C);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            F = B ^ C ^ D;
            g = (3 * i + 5) % 16;
        } else {
            F = C ^ (B | ~D);
            g = (7 * i) % 16;
        }
        uint32_t t = D;
        D = C;
        C = B;
        B = B + left_rotate(A + F + K[i] + M[g], S[i]);
        A = t;
    }
    state[0] += A;
    state[1] += B;
    state[2] += C;
    state[3] += D;
}

} // namespace

void compute(const uint8_t *data, size_t len, uint8_t out[16]) {
    uint32_t state[4] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u};

    // Process all full 64-byte blocks.
    size_t i = 0;
    for (; i + 64 <= len; i += 64) {
        process_block(state, data + i);
    }

    // Tail handling: append 0x80, then zero-pad so the final block
    // (containing the 64-bit length) ends on a 64-byte boundary.
    uint8_t tail[128] = {0};
    size_t rem = len - i;
    if (rem > 0) std::memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    size_t total_pad = (rem + 1 <= 56) ? 64 : 128;
    uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    for (int k = 0; k < 8; ++k) {
        tail[total_pad - 8 + k] = static_cast<uint8_t>(bit_len >> (8 * k));
    }
    process_block(state, tail);
    if (total_pad == 128) process_block(state, tail + 64);

    store32_le(state[0], out);
    store32_le(state[1], out + 4);
    store32_le(state[2], out + 8);
    store32_le(state[3], out + 12);
}

} // namespace dbisam_md5
