// MD5 — RFC 1321. Constants and round structure are a public mathematical
// specification; implementation is a clean-room port based on the
// reference pseudocode in RFC 1321 Appendix A.
//
// We expose only what's needed: one-shot digest of a contiguous byte
// buffer. Output is 16 bytes.

#pragma once

#include <cstddef>
#include <cstdint>

namespace dbisam_md5 {

constexpr size_t DIGEST_LENGTH = 16;

void compute(const uint8_t *data, size_t len, uint8_t out[16]);

} // namespace dbisam_md5
