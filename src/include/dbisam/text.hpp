// DBISAM text decoding. ftString and ftMemo columns are stored as
// Windows-1252; values that happen to be ASCII-clean are also valid
// UTF-8. Production tables hold accented characters, smart quotes,
// the euro sign, etc. — all 0x80–0xFF — so a naive pass-through
// produces strings that fail UTF-8 validation (rejected by Parquet,
// Arrow, JSON sinks).
//
// decode_dbisam_text: try UTF-8 first (preserves bytes if already
// valid); else decode as Windows-1252. Never lossy — every byte
// turns into a real Unicode codepoint.
//
// Port of MrsFlow's decode_dbisam_text in mrsflow-core/src/eval/value.rs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dbisam {

std::string decode_dbisam_text(const uint8_t *data, size_t len);

inline std::string decode_dbisam_text(const char *data, size_t len) {
    return decode_dbisam_text(reinterpret_cast<const uint8_t *>(data), len);
}

inline std::string decode_dbisam_text(const std::string &s) {
    return decode_dbisam_text(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

} // namespace dbisam
