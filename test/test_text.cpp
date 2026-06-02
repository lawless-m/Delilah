// Unit tests for dbisam::decode_dbisam_text — the Windows-1252 → UTF-8
// transcoder used by the VARCHAR write path. Pure C++ (no DuckDB
// harness needed).

#include "dbisam/text.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dbisam;

static int g_failures = 0;

#define CHECK(cond) do {                                                       \
    if (!(cond)) {                                                             \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

#define CHECK_EQ(a, b) do {                                                    \
    if (!((a) == (b))) {                                                       \
        std::fprintf(stderr, "FAIL: %s == %s (%s:%d)\n",                       \
                     #a, #b, __FILE__, __LINE__);                              \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

static std::string s_of(const std::vector<uint8_t> &v) {
    return decode_dbisam_text(v.data(), v.size());
}

static void empty_string_returns_empty() {
    CHECK_EQ(decode_dbisam_text(static_cast<const uint8_t *>(nullptr), 0), std::string(""));
    CHECK_EQ(decode_dbisam_text(std::string()), std::string(""));
}

static void ascii_passes_through_unchanged() {
    CHECK_EQ(decode_dbisam_text(std::string("hello world")), std::string("hello world"));
    CHECK_EQ(decode_dbisam_text(std::string("\t\r\n!?#$%")), std::string("\t\r\n!?#$%"));
}

static void valid_utf8_passes_through_unchanged() {
    // "Café" in UTF-8: 43 61 66 C3 A9 — must not double-encode.
    std::vector<uint8_t> utf8_cafe{0x43, 0x61, 0x66, 0xC3, 0xA9};
    CHECK_EQ(s_of(utf8_cafe), std::string("Caf\xC3\xA9"));
    // "€100" in UTF-8: E2 82 AC 31 30 30
    std::vector<uint8_t> utf8_euro{0xE2, 0x82, 0xAC, 0x31, 0x30, 0x30};
    CHECK_EQ(s_of(utf8_euro), std::string("\xE2\x82\xAC""100"));
}

static void win1252_high_bit_decodes_to_utf8() {
    // "Café" as Windows-1252: 43 61 66 E9 — the 0xE9 (é) byte alone isn't
    // valid UTF-8, so decoder should treat the whole thing as Win-1252.
    // 0xE9 → U+00E9 → C3 A9 in UTF-8.
    std::vector<uint8_t> win_cafe{0x43, 0x61, 0x66, 0xE9};
    CHECK_EQ(s_of(win_cafe), std::string("Caf\xC3\xA9"));
}

static void win1252_euro_sign_at_0x80() {
    // Single byte 0x80 → € (U+20AC) → E2 82 AC in UTF-8.
    std::vector<uint8_t> v{0x80};
    CHECK_EQ(s_of(v), std::string("\xE2\x82\xAC"));
}

static void win1252_smart_quotes() {
    // 0x91 = ' (U+2018 = E2 80 98), 0x92 = ' (U+2019 = E2 80 99)
    std::vector<uint8_t> v{0x91, 0x92};
    CHECK_EQ(s_of(v), std::string("\xE2\x80\x98\xE2\x80\x99"));
}

static void win1252_em_dash() {
    // 0x97 = — (U+2014 = E2 80 94)
    std::vector<uint8_t> v{0x97};
    CHECK_EQ(s_of(v), std::string("\xE2\x80\x94"));
}

static void latin1_identity_range_0xA0_to_0xFF() {
    // 0xA0 (nbsp) through 0xFF (ÿ) all map to U+00A0..U+00FF.
    // U+00A0 → C2 A0 (UTF-8); U+00FF → C3 BF.
    std::vector<uint8_t> v{0xA0};
    CHECK_EQ(s_of(v), std::string("\xC2\xA0"));
    std::vector<uint8_t> v2{0xFF};
    CHECK_EQ(s_of(v2), std::string("\xC3\xBF"));
}

static void undefined_win1252_slots_fall_through_as_latin1() {
    // 0x81, 0x8D, 0x8F, 0x90, 0x9D are unassigned in Windows-1252;
    // we map them through to the same numeric codepoint as Latin-1
    // (so the raw byte value is preserved). 0x81 → U+0081 → C2 81.
    std::vector<uint8_t> v{0x81};
    CHECK_EQ(s_of(v), std::string("\xC2\x81"));
}

static void invalid_utf8_then_win1252_decode() {
    // Lone continuation byte 0x80 makes UTF-8 detection fail; whole
    // buffer falls through to Win-1252. Same as the euro test above
    // but with a leading ASCII char — confirms mixed contexts work.
    std::vector<uint8_t> v{'X', 0x80, 'Y'};
    CHECK_EQ(s_of(v), std::string("X\xE2\x82\xAC""Y"));
}

static void truncated_utf8_falls_back_to_win1252() {
    // 0xC3 starts a 2-byte UTF-8 sequence but there's no continuation
    // byte. is_valid_utf8 → false → Win-1252 decode. 0xC3 → U+00C3
    // = C3 83 in UTF-8.
    std::vector<uint8_t> v{0xC3};
    CHECK_EQ(s_of(v), std::string("\xC3\x83"));
}

static void real_world_dbisam_company_name() {
    // "Hermès" with the Win-1252 è (0xE8). UTF-8 expected: 48 65 72 6D C3 A8 73
    std::vector<uint8_t> v{'H', 'e', 'r', 'm', 0xE8, 's'};
    CHECK_EQ(s_of(v), std::string("Herm\xC3\xA8s"));
}

int main() {
    empty_string_returns_empty();
    ascii_passes_through_unchanged();
    valid_utf8_passes_through_unchanged();
    win1252_high_bit_decodes_to_utf8();
    win1252_euro_sign_at_0x80();
    win1252_smart_quotes();
    win1252_em_dash();
    latin1_identity_range_0xA0_to_0xFF();
    undefined_win1252_slots_fall_through_as_latin1();
    invalid_utf8_then_win1252_decode();
    truncated_utf8_falls_back_to_win1252();
    real_world_dbisam_company_name();
    if (g_failures == 0) {
        std::printf("all text tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
