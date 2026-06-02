// Decode the structure of a cursor-response body per protocol §6c–§6f.
//
// Body layout (after framing has stripped the <GUID><total_len> envelope):
//
//   +0   u8        header flag (always 0x00)
//   +1   u16 LE    reqcode
//   +3   u32 LE    body_len
//   +7              Pack stream:
//       <u32 length=2><u16 LE result_code>     0x0000 OK, 0x2202 EoC, 0x0003 not-ready
//       10 cursor-info Pack units
//       [row Pack units]
//       (repeats for batched responses)
//
// Port of mrsflow-cli/src/exportmaster/response.rs.

#pragma once

#include "dbisam/cursor_info.hpp"
#include "dbisam/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dbisam {

constexpr uint16_t RESULT_OK = 0x0000;
constexpr uint16_t RESULT_NOT_READY = 0x0003;
constexpr uint16_t RESULT_END_OF_CURSOR = 0x2202;

// Reqcode the server emits in the body header when responding with
// "not ready, poll again". Not in the client→server dispatch table —
// pure server-pushed status marker.
constexpr uint16_t REQCODE_POLLING_SENTINEL = 0x2C14;

// Offset where the Pack stream begins (after the 7-byte body header).
constexpr size_t PACK_STREAM_OFFSET = 7;

// Read the body header's reqcode (u16 LE at offset 1). 0 if too short.
uint16_t body_reqcode(const uint8_t *body, size_t len);

inline uint16_t body_reqcode(const std::vector<uint8_t> &body) {
    return body_reqcode(body.data(), body.size());
}

// One server batch within a cursor response.
struct CursorBatch {
    uint16_t result_code;
    CursorInfo cursor_info;
    // Slices into the response body: each is one on-disk record of
    // `record_size` bytes (header + column data).
    std::vector<std::pair<const uint8_t *, size_t>> rows;
    // Per-row physical bookmarks (populated by read_record_block_batch
    // only; empty for read_batch). bookmarks[i] aligns with rows[i].
    std::vector<std::pair<const uint8_t *, size_t>> bookmarks;
};

// Parse one cursor batch (single-row response shape: GetNextRecord etc).
// Returns std::nullopt on clean exhaustion.
std::optional<CursorBatch> read_batch(Walker &walker, size_t expected_record_size);

// Parse a ReadFirstRecordBlock / ReadNextRecordBlock response. Rows
// arrive packed into a single Pack buffer of row_count × record_size
// bytes.
std::optional<CursorBatch> read_record_block_batch(Walker &walker, size_t expected_record_size);

} // namespace dbisam
