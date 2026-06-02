#include "dbisam/text.hpp"

namespace dbisam {

namespace {

bool is_valid_utf8(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t b = data[i];
        size_t need;
        if (b < 0x80) {
            need = 0;
        } else if ((b & 0xE0) == 0xC0 && b >= 0xC2) {
            need = 1;
        } else if ((b & 0xF0) == 0xE0) {
            need = 2;
        } else if ((b & 0xF8) == 0xF0 && b <= 0xF4) {
            need = 3;
        } else {
            return false;
        }
        if (i + need >= len) return false;
        for (size_t k = 1; k <= need; ++k) {
            if ((data[i + k] & 0xC0) != 0x80) return false;
        }
        i += 1 + need;
    }
    return true;
}

// Windows-1252 byte → Unicode codepoint. 0x00–0x7F and 0xA0–0xFF are
// identity (match Latin-1); only the 0x80–0x9F range is bespoke.
uint32_t cp1252_to_codepoint(uint8_t b) {
    switch (b) {
    case 0x80: return 0x20AC; // €
    case 0x82: return 0x201A; // ‚
    case 0x83: return 0x0192; // ƒ
    case 0x84: return 0x201E; // „
    case 0x85: return 0x2026; // …
    case 0x86: return 0x2020; // †
    case 0x87: return 0x2021; // ‡
    case 0x88: return 0x02C6; // ˆ
    case 0x89: return 0x2030; // ‰
    case 0x8A: return 0x0160; // Š
    case 0x8B: return 0x2039; // ‹
    case 0x8C: return 0x0152; // Œ
    case 0x8E: return 0x017D; // Ž
    case 0x91: return 0x2018; // '
    case 0x92: return 0x2019; // '
    case 0x93: return 0x201C; // "
    case 0x94: return 0x201D; // "
    case 0x95: return 0x2022; // •
    case 0x96: return 0x2013; // –
    case 0x97: return 0x2014; // —
    case 0x98: return 0x02DC; // ˜
    case 0x99: return 0x2122; // ™
    case 0x9A: return 0x0161; // š
    case 0x9B: return 0x203A; // ›
    case 0x9C: return 0x0153; // œ
    case 0x9E: return 0x017E; // ž
    case 0x9F: return 0x0178; // Ÿ
    default:   return b;       // ASCII + Latin-1 (0xA0–0xFF) identity
    }
}

void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        // All cp1252 codepoints fit in 3-byte UTF-8.
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

std::string decode_dbisam_text(const uint8_t *data, size_t len) {
    if (is_valid_utf8(data, len)) {
        return std::string(reinterpret_cast<const char *>(data), len);
    }
    std::string out;
    out.reserve(len + (len / 4)); // rough estimate for typical 8-bit content
    for (size_t i = 0; i < len; ++i) {
        append_utf8(out, cp1252_to_codepoint(data[i]));
    }
    return out;
}

} // namespace dbisam
