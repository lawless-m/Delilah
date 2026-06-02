// Phase 4 verification: connect+login helpers.
//
// End-to-end handshake requires a live Exportmaster server and isn't
// runnable here. These tests verify only the deterministic byte layout
// of the helper functions — if/when a server is available, drive
// Client::connect_and_login() against it.

#include "dbisam/client.hpp"
#include "dbisam/crypto.hpp"

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

static uint32_t read_u32_le(const std::vector<uint8_t> &b, size_t off) {
    return static_cast<uint32_t>(b[off]) |
           (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) |
           (static_cast<uint32_t>(b[off + 3]) << 24);
}

static void login_body_wraps_ciphertext() {
    // Use the §5 worked-example ciphertext for a real-world byte sequence.
    auto ct = encrypt_login(
        reinterpret_cast<const uint8_t *>("YOURUSER"), 6,
        reinterpret_cast<const uint8_t *>("YOURPASSWORD"), 9,
        reinterpret_cast<const uint8_t *>("elevatesoft"), 11);
    CHECK(ct.size() == 24);

    auto body = build_login_body(ct);
    // Layout: flag(1) + reqcode_le(2) + inner_len_le(4) + 3×u32(12) + ct(24) + trailer(1) = 44
    CHECK(body.size() == 44);
    CHECK(body[0] == 0x00);                                  // flag
    CHECK(body[1] == 0x14 && body[2] == 0x00);               // reqcode 0x0014 LE
    CHECK(read_u32_le(body, 3) == 4 + 4 + 4 + 24);           // inner_len = 36
    CHECK(read_u32_le(body, 7) == 4);                        // first field
    CHECK(read_u32_le(body, 11) == 24);                      // buf len
    CHECK(read_u32_le(body, 15) == 24);                      // buf max len
    CHECK(std::memcmp(body.data() + 19, ct.data(), 24) == 0);
    CHECK(body[43] == 0x00);                                 // trailer
}

static void catalog_attach_for_nisaint_cs_matches_capture() {
    // Captured byte sequence from pyodbc against YOURCATALOG.
    auto body = build_catalog_attach_body("YOURCATALOG");
    const std::array<uint8_t, 28> expected = {
        // flag + reqcode 0x003C LE
        0x00, 0x3C, 0x00,
        // inner_len = 4 + 10 + 5 = 19, LE
        0x13, 0x00, 0x00, 0x00,
        // inner: name_len 10 LE + "YOURCATALOG" + trailer 01 00 00 00 00
        0x0A, 0x00, 0x00, 0x00,
        'N', 'I', 'S', 'A', 'I', 'N', 'T', '_', 'C', 'S',
        0x01, 0x00, 0x00, 0x00, 0x00,
        // outer 2-byte trailer
        0x64, 0x00,
    };
    CHECK(body.size() == expected.size());
    CHECK(std::memcmp(body.data(), expected.data(), expected.size()) == 0);
}

static void catalog_attach_scales_with_name_length() {
    // Sanity: 7-char catalog name. inner_len = 4 + 7 + 5 = 16.
    auto body = build_catalog_attach_body("DATABASE");
    CHECK(body[1] == 0x3C && body[2] == 0x00);
    CHECK(read_u32_le(body, 3) == 4 + 8 + 5);
    CHECK(read_u32_le(body, 7) == 8);
    CHECK(std::memcmp(body.data() + 11, "DATABASE", 8) == 0);
    CHECK(body.back() == 0x00 && body[body.size() - 2] == 0x64);
}

int main() {
    login_body_wraps_ciphertext();
    catalog_attach_for_nisaint_cs_matches_capture();
    catalog_attach_scales_with_name_length();
    if (g_failures == 0) {
        std::printf("all phase-4 tests passed\n");
        std::printf("note: end-to-end handshake requires live Exportmaster server\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
