// Phase 2 verification: wire/framing/crypto.
// Mirrors the Rust unit tests in MrsFlow's exportmaster/{wire,framing,crypto}.rs.

#include "dbisam/crypto.hpp"
#include "dbisam/framing.hpp"
#include "dbisam/wire.hpp"
#include "md5.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace dbisam;

static int g_failures = 0;

#define CHECK(cond) do {                                                       \
    if (!(cond)) {                                                             \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

static void wire_walks_three_units() {
    std::vector<uint8_t> buf;
    auto push_u32 = [&](uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v >> 16));
        buf.push_back(static_cast<uint8_t>(v >> 24));
    };
    push_u32(4); buf.insert(buf.end(), {'w','x','y','z'});
    push_u32(2); buf.insert(buf.end(), {'a','b'});
    push_u32(0);

    Walker w(buf.data(), buf.size());
    const uint8_t *p; size_t n;
    CHECK(w.next_unit(p, n));  CHECK(n == 4 && std::memcmp(p, "wxyz", 4) == 0);
    CHECK(w.next_unit(p, n));  CHECK(n == 2 && std::memcmp(p, "ab", 2) == 0);
    CHECK(w.next_unit(p, n));  CHECK(n == 0);
    CHECK(!w.next_unit(p, n));
}

static void wire_errors_on_overrun() {
    std::vector<uint8_t> buf{10, 0, 0, 0, 'a', 'b'};
    Walker w(buf.data(), buf.size());
    const uint8_t *p; size_t n;
    bool threw = false;
    try { w.next_unit(p, n); } catch (const WireError &) { threw = true; }
    CHECK(threw);
}

static void wire_empty_returns_false() {
    Walker w(nullptr, 0);
    const uint8_t *p; size_t n;
    CHECK(!w.next_unit(p, n));
}

static void wire_starts_at_offset() {
    std::vector<uint8_t> buf{0xAA, 0xBB, 0xCC, 3,0,0,0, 'f','o','o'};
    Walker w(buf.data(), buf.size(), 3);
    const uint8_t *p; size_t n;
    CHECK(w.next_unit(p, n));
    CHECK(n == 3 && std::memcmp(p, "foo", 3) == 0);
}

static void wrap_layout_matches_poc() {
    // 4-byte body → 24-byte packet: GUID + u32(24) + body.
    std::vector<uint8_t> body{0xAA, 0xBB, 0xCC, 0xDD};
    auto pkt = wrap(body);
    CHECK(pkt.size() == 24);
    CHECK(std::memcmp(pkt.data(), GUID.data(), 16) == 0);
    CHECK(pkt[16] == 24 && pkt[17] == 0 && pkt[18] == 0 && pkt[19] == 0);
    CHECK(std::memcmp(pkt.data() + 20, body.data(), 4) == 0);
}

static void wrap_pads_to_8() {
    // 5-byte body → raw 25, aligned 32. 3 pad bytes at the end.
    std::vector<uint8_t> body{1, 2, 3, 4, 5};
    auto pkt = wrap(body);
    CHECK(pkt.size() == 32);
    CHECK(pkt[16] == 32);
    CHECK(pkt[25] == 0 && pkt[26] == 0 && pkt[27] == 0);
}

static void deflate_inflate_roundtrip() {
    std::vector<uint8_t> orig;
    for (int i = 0; i < 1000; ++i) orig.push_back(static_cast<uint8_t>(i * 31));
    auto compressed = deflate(orig.data(), orig.size());
    CHECK(compressed.size() > 2);
    // zlib header byte for level-1 compression is 0x78 0x01 (per dbsys.exe capture).
    CHECK(compressed[0] == 0x78);
    CHECK(compressed[1] == 0x01);
    auto round = inflate(compressed.data(), compressed.size());
    CHECK(round == orig);
}

static void md5_known_vector() {
    // MD5("elevatesoft") per DBISAM-PROTOCOL.md §5 worked example.
    uint8_t out[16];
    const char *s = "elevatesoft";
    dbisam_md5::compute(reinterpret_cast<const uint8_t *>(s), 11, out);
    const std::array<uint8_t, 16> expected = {
        0xCE, 0x85, 0x01, 0xAA, 0xC5, 0x39, 0xB4, 0xBD,
        0x4C, 0x54, 0x32, 0x7E, 0x41, 0xD9, 0x75, 0xB0,
    };
    CHECK(std::memcmp(out, expected.data(), 16) == 0);
}

static void md5_empty_string() {
    // RFC 1321 test vector: MD5("") = d41d8cd98f00b204e9800998ecf8427e
    uint8_t out[16];
    dbisam_md5::compute(nullptr, 0, out);
    const std::array<uint8_t, 16> expected = {
        0xD4, 0x1D, 0x8C, 0xD9, 0x8F, 0x00, 0xB2, 0x04,
        0xE9, 0x80, 0x09, 0x98, 0xEC, 0xF8, 0x42, 0x7E,
    };
    CHECK(std::memcmp(out, expected.data(), 16) == 0);
}

static void login_ciphertext_matches_doc_worked_example() {
    // DBISAM-PROTOCOL.md §5 worked example.
    const char *user = "YOURUSER";
    const char *pass = "YOURPASSWORD";
    const char *epw  = "elevatesoft";
    auto ct = encrypt_login(
        reinterpret_cast<const uint8_t *>(user), 6,
        reinterpret_cast<const uint8_t *>(pass), 9,
        reinterpret_cast<const uint8_t *>(epw),  11);
    const std::array<uint8_t, 24> expected = {
        0x57, 0x25, 0x56, 0x8E, 0x56, 0x01, 0xB0, 0x58,
        0xD1, 0x7E, 0xE1, 0x77, 0x20, 0xB6, 0x95, 0x24,
        0x78, 0x1F, 0x5A, 0x02, 0x17, 0xF2, 0x43, 0x90,
    };
    CHECK(ct.size() == expected.size());
    CHECK(std::memcmp(ct.data(), expected.data(), expected.size()) == 0);
}

int main() {
    wire_walks_three_units();
    wire_errors_on_overrun();
    wire_empty_returns_false();
    wire_starts_at_offset();
    wrap_layout_matches_poc();
    wrap_pads_to_8();
    deflate_inflate_roundtrip();
    md5_known_vector();
    md5_empty_string();
    login_ciphertext_matches_doc_worked_example();
    if (g_failures == 0) {
        std::printf("all phase-2 tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
