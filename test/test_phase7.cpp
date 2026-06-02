// Phase 7 unit tests: blob slot builder, bookmark→phys, OpenBlob response.
// Mirrors the Rust unit tests in MrsFlow's exportmaster/blob.rs.

#include "dbisam/blob.hpp"

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

static void build_slot_56byte_niingred_layout() {
    // NIINGRED row NIEAN="0071567747844" (13 chars in 14-byte column,
    // PhysicalRecordNumber=5). Captured form had a 14-byte fixed trailer
    // at slot_length-14 with 2 middle pad bytes; the simplified layout
    // we emit puts the 9-byte trailer right after the PK column and
    // zero-fills the tail. Server accepts either (both encode the same
    // (phys, PK) lookup keys).
    uint32_t phys = 5;
    std::array<uint8_t, 16> md5 = {
        0xa2, 0x8d, 0x18, 0xe6, 0x39, 0xee, 0xa2, 0xfb,
        0x75, 0x0c, 0xdb, 0x26, 0x61, 0x3c, 0xca, 0x3a,
    };
    std::vector<uint8_t> pk_field(14, 0);
    std::memcpy(pk_field.data(), "0071567747844", 13);

    auto slot = build_slot(phys, md5, pk_field, 56);
    CHECK(slot.size() == 56);
    // Header phys at [1..5] and [5..9] LE.
    CHECK(slot[0] == 0x00);
    CHECK(slot[1] == 5 && slot[2] == 0 && slot[3] == 0 && slot[4] == 0);
    CHECK(slot[5] == 5 && slot[8] == 0);
    // MD5 at [9..25].
    CHECK(std::memcmp(slot.data() + 9, md5.data(), 16) == 0);
    // PK flag at [25] + PK at [26..40].
    CHECK(slot[25] == 0x01);
    CHECK(std::memcmp(slot.data() + 26, pk_field.data(), 14) == 0);
    // Trailer marker right after PK at [40], phys at [41..45] and [45..49] LE.
    CHECK(slot[40] == 0x01);
    CHECK(slot[41] == 5 && slot[42] == 0 && slot[43] == 0 && slot[44] == 0);
    CHECK(slot[45] == 5 && slot[48] == 0);
    // Tail [49..56] is zero pad.
    for (size_t i = 49; i < 56; ++i) CHECK(slot[i] == 0);
}

static void build_slot_48byte_customer_layout() {
    // CUSTOMER (PK CODE max=11, slot_length 48) — verified live against
    // YOURHOST. Trailer fits at offset 37 (right after 11-byte PK).
    uint32_t phys = 1;
    std::array<uint8_t, 16> md5 = {};
    for (size_t i = 0; i < 16; ++i) md5[i] = static_cast<uint8_t>(0xA0 + i);
    std::vector<uint8_t> pk_field(11, 0);
    pk_field[0] = '1';

    auto slot = build_slot(phys, md5, pk_field, 48);
    CHECK(slot.size() == 48);
    CHECK(slot[0] == 0x00 && slot[1] == 1);
    CHECK(std::memcmp(slot.data() + 9, md5.data(), 16) == 0);
    CHECK(slot[25] == 0x01 && slot[26] == '1');
    // Trailer at [37..46] (9 bytes), tail pad at [46..48].
    CHECK(slot[37] == 0x01);
    CHECK(slot[38] == 1 && slot[39] == 0 && slot[40] == 0 && slot[41] == 0);
    CHECK(slot[42] == 1 && slot[45] == 0);
    CHECK(slot[46] == 0 && slot[47] == 0);
}

static void build_slot_rejects_too_small() {
    std::array<uint8_t, 16> md5 = {};
    std::vector<uint8_t> pk(11, 0);
    // Minimum: 9 (header) + 16 (md5) + 1 (PK flag) + 11 (PK) + 9 (trailer) = 46.
    bool threw = false;
    try { build_slot(1, md5, pk, 45); }
    catch (const std::exception &) { threw = true; }
    CHECK(threw);
    bool ok = false;
    try { build_slot(1, md5, pk, 46); ok = true; }
    catch (const std::exception &) {}
    CHECK(ok);
}

static void extract_phys_from_22byte_niingred_bookmark() {
    // NIINGRED 22-byte bookmark: phys at trailing bytes [18..22] as
    // <high-bit-flag><3-byte BE>.
    const uint8_t bookmark[22] = {
        0x01, 0x30, 0x30, 0x37, 0x31, 0x35, 0x36, 0x37, 0x37, 0x34, 0x37, 0x38, 0x34, 0x34, 0x00,
        0x00, 0x00,
        0x01, 0x80, 0x00, 0x00, 0x05,
    };
    CHECK(physical_record_number_from_bookmark(bookmark, 22) == 5);
}

static void extract_phys_from_17byte_customer_bookmark() {
    // CUSTOMER 17-byte bookmark live-captured from YOURHOST: phys at
    // trailing bytes [13..17]. Same algorithm, different offset.
    const uint8_t bookmark[17] = {
        0x01, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01,
        0x80, 0x00, 0x00, 0x01,
    };
    CHECK(physical_record_number_from_bookmark(bookmark, 17) == 1);
}

static void extract_phys_handles_large_values() {
    uint8_t bookmark[22] = {0};
    bookmark[18] = 0x80;
    bookmark[19] = 0x01;
    bookmark[20] = 0x86;
    bookmark[21] = 0xA0;
    CHECK(physical_record_number_from_bookmark(bookmark, 22) == 100000);
}

static void extract_phys_too_short_returns_zero() {
    uint8_t bookmark[3] = {0};
    CHECK(physical_record_number_from_bookmark(bookmark, 3) == 0);
}

static void pack_unit(std::vector<uint8_t> &buf, const uint8_t *p, size_t n) {
    uint32_t len = static_cast<uint32_t>(n);
    buf.insert(buf.end(), {static_cast<uint8_t>(len),
                           static_cast<uint8_t>(len >> 8),
                           static_cast<uint8_t>(len >> 16),
                           static_cast<uint8_t>(len >> 24)});
    buf.insert(buf.end(), p, p + n);
}

static void parse_response_decodes_payload() {
    std::vector<uint8_t> body = {0x00, 0x80, 0x02, 0, 0, 0, 0}; // 7-byte header
    std::vector<uint8_t> slot_echo(56, 0xAA);
    pack_unit(body, slot_echo.data(), slot_echo.size());
    uint8_t size_bytes[4] = {12, 0, 0, 0};
    pack_unit(body, size_bytes, 4);
    pack_unit(body, reinterpret_cast<const uint8_t *>("Hello world!"), 12);

    auto out = parse_open_blob_response(body);
    CHECK(out.payload.size() == 12);
    CHECK(std::memcmp(out.payload.data(), "Hello world!", 12) == 0);
    CHECK(out.actual_slot_length == 56);
}

static void parse_response_errors_on_size_mismatch() {
    std::vector<uint8_t> body = {0x00, 0x80, 0x02, 0, 0, 0, 0};
    std::vector<uint8_t> slot(56, 0);
    pack_unit(body, slot.data(), slot.size());
    uint8_t declared20[4] = {20, 0, 0, 0};
    pack_unit(body, declared20, 4);
    pack_unit(body, reinterpret_cast<const uint8_t *>("short"), 5);
    bool threw = false;
    try { parse_open_blob_response(body); }
    catch (const std::exception &) { threw = true; }
    CHECK(threw);
}

static void parse_response_surfaces_mismatched_slot_echo() {
    std::vector<uint8_t> body = {0x00, 0x80, 0x02, 0, 0, 0, 0};
    std::vector<uint8_t> slot72(72, 0);
    pack_unit(body, slot72.data(), slot72.size());
    uint8_t zero4[4] = {0, 0, 0, 0};
    pack_unit(body, zero4, 4);
    pack_unit(body, nullptr, 0);
    auto out = parse_open_blob_response(body);
    CHECK(out.actual_slot_length == 72);
    CHECK(out.payload.empty());
}

static void parse_response_handles_empty_blob() {
    std::vector<uint8_t> body = {0x00, 0x80, 0x02, 0, 0, 0, 0};
    std::vector<uint8_t> slot(56, 0);
    pack_unit(body, slot.data(), slot.size());
    uint8_t zero4[4] = {0, 0, 0, 0};
    pack_unit(body, zero4, 4);
    pack_unit(body, nullptr, 0);
    auto out = parse_open_blob_response(body);
    CHECK(out.payload.empty());
    CHECK(out.actual_slot_length == 56);
}

int main() {
    build_slot_56byte_niingred_layout();
    build_slot_48byte_customer_layout();
    build_slot_rejects_too_small();
    extract_phys_from_22byte_niingred_bookmark();
    extract_phys_from_17byte_customer_bookmark();
    extract_phys_handles_large_values();
    extract_phys_too_short_returns_zero();
    parse_response_decodes_payload();
    parse_response_errors_on_size_mismatch();
    parse_response_surfaces_mismatched_slot_echo();
    parse_response_handles_empty_blob();
    if (g_failures == 0) {
        std::printf("all phase-7 tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
