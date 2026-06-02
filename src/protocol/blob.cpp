#include "dbisam/blob.hpp"

#include "dbisam/msg.hpp"
#include "dbisam/response.hpp"
#include "dbisam/wire.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace dbisam {

namespace {

void put_u32_le_at(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t read_u32_le(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

// Slot layout (parametric on slot_length and pk width — verified
// against both 56-byte NIINGRED and 48-byte CUSTOMER cases on YOURHOST):
//
//   [0..9]            header: 0x00 + phys LE + phys LE
//   [9..25]           16-byte row MD5
//   [25]              0x01 PK null flag
//   [26..26+pk_w]     PK column bytes
//   [...]             trailer: 0x01 + phys LE + phys LE  (9 bytes)
//   [...slot_length]  zero tail pad
//
// The trailer is placed immediately after the PK block; the tail (and
// any over-allocation when slot_length > 9+16+1+pk_w+9) is zero-filled.
// The previous "fixed 14-byte trailer at slot_length - 14" placement
// is captured-data accurate for NIINGRED's 56-byte slot but doesn't
// fit a 48-byte slot with an 11-byte PK; the server appears to accept
// either placement because both forms produce the same set of (phys,
// PK) lookup keys.
std::vector<uint8_t> build_slot(uint32_t physical_record_number,
                                const std::array<uint8_t, 16> &row_md5,
                                const std::vector<uint8_t> &pk_field_bytes,
                                size_t slot_length) {
    constexpr size_t header_len = 9;
    constexpr size_t md5_len = 16;
    constexpr size_t trailer_marker_phys_len = 9; // 0x01 + 4 + 4
    size_t pk_block_len = 1 + pk_field_bytes.size();
    size_t used = header_len + md5_len + pk_block_len + trailer_marker_phys_len;
    if (used > slot_length) {
        throw std::runtime_error("slot_length=" + std::to_string(slot_length) +
                                 " too small for layout with " +
                                 std::to_string(pk_field_bytes.size()) + "-byte PK field");
    }
    std::vector<uint8_t> slot(slot_length, 0);
    slot[0] = 0x00;
    put_u32_le_at(slot.data() + 1, physical_record_number);
    put_u32_le_at(slot.data() + 5, physical_record_number);
    std::memcpy(slot.data() + 9, row_md5.data(), 16);
    slot[25] = 0x01;
    if (!pk_field_bytes.empty()) {
        std::memcpy(slot.data() + 26, pk_field_bytes.data(), pk_field_bytes.size());
    }
    size_t t = header_len + md5_len + pk_block_len; // trailer right after PK
    slot[t] = 0x01;
    put_u32_le_at(slot.data() + t + 1, physical_record_number);
    put_u32_le_at(slot.data() + t + 5, physical_record_number);
    return slot;
}

// Bookmark layout (variable length, parametric on PK width):
//   [0]            0x01 PK null flag
//   [1..1+pk_w]    PK column bytes
//   [1+pk_w]       0x01 marker
//   [last 4 bytes] phys as <high-bit-flag><3-byte BE>
//
// On CUSTOMER (PK=11) total = 17; on NIINGRED (PK=14) total = 22 with
// 2 middle pad bytes between PK and marker. Reading phys from the
// final 4 bytes works for both.
uint32_t physical_record_number_from_bookmark(const uint8_t *bookmark, size_t len) {
    if (len < 4) return 0;
    size_t off = len - 4;
    uint8_t b0 = bookmark[off] & 0x7F;
    return (static_cast<uint32_t>(b0) << 24) |
           (static_cast<uint32_t>(bookmark[off + 1]) << 16) |
           (static_cast<uint32_t>(bookmark[off + 2]) << 8) |
           static_cast<uint32_t>(bookmark[off + 3]);
}

BlobFetchOutcome parse_open_blob_response(const uint8_t *body, size_t len) {
    if (len < PACK_STREAM_OFFSET) {
        throw std::runtime_error("blob response too short (" + std::to_string(len) + " bytes)");
    }
    Walker w(body, len, PACK_STREAM_OFFSET);
    const uint8_t *p; size_t n;
    if (!w.next_unit(p, n)) {
        throw std::runtime_error("blob response missing slot echo unit");
    }
    BlobFetchOutcome out;
    out.actual_slot_length = n;
    out.slot_echo.assign(p, p + n);

    if (!w.next_unit(p, n)) {
        throw std::runtime_error("blob response missing size unit");
    }
    if (n != 4) {
        throw std::runtime_error("blob size unit expected 4 bytes, got " + std::to_string(n));
    }
    size_t blob_size = read_u32_le(p);

    if (!w.next_unit(p, n)) {
        throw std::runtime_error("blob response missing payload unit");
    }
    if (n != blob_size) {
        throw std::runtime_error("blob payload length " + std::to_string(n) +
                                 " doesn't match declared size " + std::to_string(blob_size));
    }
    out.payload.assign(p, p + n);
    return out;
}

BlobFetchOutcome fetch_blob(Transport &t, bool compression, uint32_t cursor_handle,
                            uint16_t field_ord, const std::vector<uint8_t> &slot) {
    auto body = build_open_blob(cursor_handle, field_ord, slot.data(), slot.size(), 0, 0);
    auto resp = t.send_recv_auto(body, compression);
    return parse_open_blob_response(resp);
}

} // namespace dbisam
