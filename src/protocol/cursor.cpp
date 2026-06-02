#include "dbisam/cursor.hpp"

#include "dbisam/msg.hpp"
#include "dbisam/response.hpp"
#include "dbisam/wire.hpp"

#include <stdexcept>
#include <string>

namespace dbisam {

namespace {

constexpr uint32_t CURSOR_HANDLE = 1;
constexpr size_t MAX_RECEIVE_POLLS = 100;

enum class ReplyKind { SingleRow, RecordBlock };

// Detect the inner "RESULT_NOT_READY" code carried by some early
// post-Execute responses (Pack stream starts with <u32 len=2><u16 0x0003>).
bool inner_says_not_ready(const std::vector<uint8_t> &body) {
    if (body.size() < PACK_STREAM_OFFSET + 6) return false;
    const uint8_t *p = body.data() + PACK_STREAM_OFFSET;
    uint32_t len = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    if (len != 2) return false;
    uint16_t rc = static_cast<uint16_t>(p[4]) | (static_cast<uint16_t>(p[5]) << 8);
    return rc == RESULT_NOT_READY;
}

// Process one response body, invoking `on_row` per row. Returns true to
// signal stop (EoC reached or target_rows hit). Caller increments
// rows_seen inside the callback path.
bool process_body(const std::vector<uint8_t> &body, ReplyKind kind,
                  size_t record_size, size_t &rows_seen, size_t target_rows,
                  const RowCallback &on_row) {
    if (body.size() < PACK_STREAM_OFFSET + 6) return false;
    if (body_reqcode(body) == REQCODE_POLLING_SENTINEL) return false;
    Walker walker(body.data(), body.size(), PACK_STREAM_OFFSET);
    for (;;) {
        std::optional<CursorBatch> batch;
        try {
            batch = (kind == ReplyKind::SingleRow)
                        ? read_batch(walker, record_size)
                        : read_record_block_batch(walker, record_size);
        } catch (const std::exception &) {
            return false;
        }
        if (!batch) return false;
        for (size_t i = 0; i < batch->rows.size(); ++i) {
            if (rows_seen >= target_rows) return true;
            const uint8_t *bm = nullptr;
            size_t bmlen = 0;
            if (i < batch->bookmarks.size()) {
                bm = batch->bookmarks[i].first;
                bmlen = batch->bookmarks[i].second;
            }
            bool keep = on_row(batch->rows[i].first, batch->rows[i].second, bm, bmlen);
            ++rows_seen;
            if (!keep) return true;
        }
        if (batch->result_code != RESULT_OK) return true;
        if (walker.position() >= body.size()) return false;
    }
}

} // namespace

size_t compute_record_size(const std::vector<Column> &columns) {
    if (columns.empty()) {
        throw std::runtime_error("compute_record_size: schema must have at least one column");
    }
    const auto &last = columns.back();
    return static_cast<size_t>(last.row_offset) + static_cast<size_t>(last.max) + 1;
}

size_t drive_cursor(Transport &t,
                    const std::vector<Column> &columns,
                    size_t target_rows,
                    uint32_t batch_size,
                    bool compression,
                    const RowCallback &on_row) {
    size_t record_size = compute_record_size(columns);
    size_t rows_seen = 0;

    // Phase 1: ExecuteStatement.
    auto r = t.send_recv_auto(build_execute_statement(CURSOR_HANDLE), compression);
    if (process_body(r, ReplyKind::SingleRow, record_size, rows_seen, target_rows, on_row)) {
        return rows_seen;
    }

    // Phase 2: Receive poll loop.
    size_t polls = 0;
    for (;;) {
        r = t.send_recv_auto(build_receive(), compression);
        bool sentinel = body_reqcode(r) == REQCODE_POLLING_SENTINEL;
        bool inner_not_ready = inner_says_not_ready(r);
        if (process_body(r, ReplyKind::SingleRow, record_size, rows_seen, target_rows, on_row)) {
            return rows_seen;
        }
        if (!sentinel && !inner_not_ready) break;
        if (++polls >= MAX_RECEIVE_POLLS) {
            throw std::runtime_error("cursor still 'not ready' after " +
                                     std::to_string(MAX_RECEIVE_POLLS) + " Receive polls");
        }
    }

    // Phase 2.5: SetToBegin.
    r = t.send_recv_auto(build_set_to_begin(CURSOR_HANDLE), compression);
    if (process_body(r, ReplyKind::SingleRow, record_size, rows_seen, target_rows, on_row)) {
        return rows_seen;
    }

    // Phase 3: ReadFirstRecordBlock.
    uint32_t first_n = static_cast<uint32_t>(std::min<size_t>(target_rows, batch_size));
    if (first_n == 0) first_n = 1;
    r = t.send_recv_auto(build_read_first_record_block(CURSOR_HANDLE, first_n), compression);
    if (process_body(r, ReplyKind::RecordBlock, record_size, rows_seen, target_rows, on_row)) {
        return rows_seen;
    }

    // Phase 4: ReadNextRecordBlock loop.
    size_t max_batches = (target_rows / batch_size) + 10;
    for (size_t b = 0; b < max_batches; ++b) {
        if (rows_seen >= target_rows) break;
        size_t remaining = target_rows - rows_seen;
        uint32_t n = static_cast<uint32_t>(std::min<size_t>(remaining, batch_size));
        if (n == 0) n = 1;
        r = t.send_recv_auto(build_read_next_record_block(CURSOR_HANDLE, n), compression);
        if (process_body(r, ReplyKind::RecordBlock, record_size, rows_seen, target_rows, on_row)) {
            break;
        }
    }
    return rows_seen;
}

} // namespace dbisam
