// Cursor advance: post-query message sequence + batched fetch loop.
//
// Flow per ANSWERS-TO-DEREK-2.md:
//   0x032A ExecuteStatement        → kicks off cursor execution
//   0x030C Receive  (poll loop)    → wait until server signals "ready"
//   0x00BE SetToBegin              → position cursor at row 1
//   0x050A ReadFirstRecordBlock    → first batch
//   0x04F6 ReadNextRecordBlock     → repeat until end-of-cursor
//
// Port of mrsflow-cli/src/exportmaster/cursor.rs (drive_cursor only;
// the find_row_starts_via_framing helper isn't needed — the response
// codecs locate rows for us).

#pragma once

#include "dbisam/framing.hpp"
#include "dbisam/schema.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace dbisam {

// Total on-disk record width per §6c: row_offset of last column + that
// column's max bytes + 1 byte for its null-flag.
size_t compute_record_size(const std::vector<Column> &columns);

// Called once per delivered row. `row` points to the start of the
// on-disk record (length record_size). `bookmark` is the per-row
// physical bookmark (empty if the batch was single-row-shaped).
// Return false from on_row to stop early; true to continue.
using RowCallback = std::function<bool(const uint8_t *row, size_t row_len,
                                       const uint8_t *bookmark, size_t bookmark_len)>;

// Drive a SELECT cursor to completion, invoking on_row per row.
// Returns the number of rows actually delivered.
size_t drive_cursor(Transport &t,
                    const std::vector<Column> &columns,
                    size_t target_rows,
                    uint32_t batch_size,
                    bool compression,
                    const RowCallback &on_row);

} // namespace dbisam
