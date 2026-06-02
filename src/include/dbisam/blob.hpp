// Memo / blob fetch — reqcode 0x0280. See DBISAM-PROTOCOL.md §6a.
//
// Blob columns (sub=3, max=8) carry only an 8-byte handle on the row;
// content is fetched in a separate round-trip via OpenBlob (0x0280),
// then released with FreeBlob (0x028A). The entire payload arrives in
// one response — the server never paginates remote blob reads.
//
// Wire format from disassembly of TDataCursor.OpenBlob (RVA 0x0ADFAC),
// TDataCursor.ReadBlob (RVA 0x0AE624), TServerThread.DoOpenBlob
// (RVA 0x04ED60) in dbisamr439delphi7.bpl.
//
// SELECT-only: 0x0280, 0x028A, 0x0294 only manage server-side cache
// buffers; they never modify persistent table data.
//
// Port of mrsflow-cli/src/exportmaster/blob.rs.

#pragma once

#include "dbisam/framing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dbisam {

// Build the physical-record bookmark slot the server expects in 0x0280.
// `slot_length` is mode-dependent: 56 for natural-PK cursors on short-PK
// tables; 72 for WHERE-filtered / materialised cursors. Caller can adapt
// using the echoed slot length from a failed fetch.
std::vector<uint8_t> build_slot(uint32_t physical_record_number,
                                const std::array<uint8_t, 16> &row_md5,
                                const std::vector<uint8_t> &pk_field_bytes,
                                size_t slot_length);

// Extract PhysicalRecordNumber from a per-row cursor bookmark.
// Position is at offset 18 as `<u8 with high bit set><3 bytes BE value>`.
// Returns 0 if the bookmark is too short or malformed.
uint32_t physical_record_number_from_bookmark(const uint8_t *bookmark, size_t len);

struct BlobFetchOutcome {
    std::vector<uint8_t> payload;
    size_t actual_slot_length;
    // The slot bytes echoed back by the server — NOT identical to what
    // the request sent. The server modifies a few trailing bytes as an
    // "open in cache" marker. FreeBlob (0x028A) must use these echoed
    // bytes verbatim or the server can't find the cached buffer.
    std::vector<uint8_t> slot_echo;
};

// Parse a 0x0280 response body. Layout (§6a):
//   3 Pack units: slot_echo, <u32 size>, <size bytes>
BlobFetchOutcome parse_open_blob_response(const uint8_t *body, size_t len);

inline BlobFetchOutcome parse_open_blob_response(const std::vector<uint8_t> &body) {
    return parse_open_blob_response(body.data(), body.size());
}

// Fetch one blob payload. Caller supplies the slot via build_slot or
// the per-row bookmark from the cursor (when applicable).
BlobFetchOutcome fetch_blob(Transport &t, bool compression, uint32_t cursor_handle,
                            uint16_t field_ord, const std::vector<uint8_t> &slot);

} // namespace dbisam
