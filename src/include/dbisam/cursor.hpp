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

// Incremental cursor: lets callers pull one batch at a time so DuckDB's
// LIMIT n naturally stops the scan after the first batch that satisfies
// it, instead of materialising the whole result up front.
//
// Lifecycle:
//   constructor → sends ExecuteStatement, drives Receive-poll, SetToBegin
//                 (so the cursor is positioned but no rows yet pulled)
//   next_block() → ReadFirstRecordBlock first time, ReadNextRecordBlock
//                  thereafter. Returns rows+bookmarks for one block.
//                  Sets `eoc` when the server returns end-of-cursor.
//   destructor   → sends CloseCursor + ResetStatement +
//                  RemoveAllRemoteMemoryTables (best-effort; swallows
//                  errors since destruction runs in error paths too).
//
// Borrows the Transport — caller owns its lifetime (typically a
// short-lived per-scan Client). Holding a CursorRunner keeps that
// connection busy until the runner is destroyed.
class CursorRunner {
public:
    struct Block {
        // Spans into the response buffer owned by this CursorRunner —
        // valid until the next next_block() call.
        std::vector<std::pair<const uint8_t *, size_t>> rows;
        std::vector<std::pair<const uint8_t *, size_t>> bookmarks;
        bool eoc = false;
    };

    CursorRunner(Transport &transport, std::vector<Column> columns,
                 uint32_t batch_size, bool compression);
    ~CursorRunner();

    CursorRunner(const CursorRunner &) = delete;
    CursorRunner &operator=(const CursorRunner &) = delete;
    CursorRunner(CursorRunner &&) = delete;
    CursorRunner &operator=(CursorRunner &&) = delete;

    const std::vector<Column> &columns() const { return columns_; }

    // Fetch the next batch from the server. Repeated calls after EoC
    // return Block{.eoc=true, rows={}}.
    Block next_block();

private:
    Transport &transport_;
    std::vector<Column> columns_;
    uint32_t batch_size_;
    bool compression_;
    size_t record_size_;
    bool first_block_ = true;
    bool eoc_ = false;
    bool opened_ = false; // cursor_open succeeded → cleanup needed in dtor
    // Response buffer for the most recent next_block call; spans in
    // the returned Block point into this.
    std::vector<uint8_t> response_;
};

} // namespace dbisam
