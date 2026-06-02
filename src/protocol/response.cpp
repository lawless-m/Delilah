#include "dbisam/response.hpp"

#include <string>

namespace dbisam {

uint16_t body_reqcode(const uint8_t *body, size_t len) {
    if (len < 3) return 0;
    return static_cast<uint16_t>(body[1]) | (static_cast<uint16_t>(body[2]) << 8);
}

namespace {

uint32_t read_u32_le(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16_le(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

} // namespace

std::optional<CursorBatch> read_batch(Walker &walker, size_t expected_record_size) {
    const uint8_t *p; size_t n;
    if (!walker.next_unit(p, n)) return std::nullopt;
    if (n != 2) {
        throw WireError("expected 2-byte result code, got " + std::to_string(n));
    }
    CursorBatch batch;
    batch.result_code = read_u16_le(p);
    batch.cursor_info = CursorInfo::read(walker);

    // Row records: each Pack unit whose length matches the expected
    // record size. First-row size disambiguation allows ±2 bytes from
    // the schema-derived estimate; once seen, subsequent rows must
    // match exactly.
    if (batch.result_code == RESULT_OK) {
        std::optional<size_t> locked_size;
        for (;;) {
            size_t saved = walker.position();
            const uint8_t *up; size_t un;
            if (!walker.next_unit(up, un)) break;
            bool matches;
            if (locked_size) {
                matches = (un == *locked_size);
            } else {
                size_t lo = expected_record_size > 2 ? expected_record_size - 2 : 0;
                size_t hi = expected_record_size + 2;
                if (un >= lo && un <= hi) {
                    locked_size = un;
                    matches = true;
                } else {
                    matches = false;
                }
            }
            if (matches) {
                batch.rows.emplace_back(up, un);
            } else {
                walker.seek(saved);
                break;
            }
        }
    }
    return batch;
}

std::optional<CursorBatch> read_record_block_batch(Walker &walker, size_t expected_record_size) {
    const uint8_t *p; size_t n;
    if (!walker.next_unit(p, n)) return std::nullopt;
    if (n != 2) {
        throw WireError("ReadRecordBlock: expected 2-byte result code, got " + std::to_string(n));
    }
    CursorBatch batch;
    batch.result_code = read_u16_le(p);
    batch.cursor_info = CursorInfo::read(walker);

    // Row count (4-byte u32 LE Pack unit).
    const uint8_t *cu; size_t cn;
    if (!walker.next_unit(cu, cn) || cn != 4) return batch;
    size_t row_count = read_u32_le(cu);

    // Row buffer (row_count × actual_record_size bytes packed).
    const uint8_t *rb; size_t rblen;
    if (!walker.next_unit(rb, rblen)) return batch;
    if (row_count > 0 && rblen % row_count == 0) {
        size_t actual_record_size = rblen / row_count;
        size_t lo = expected_record_size > 32 ? expected_record_size - 32 : 0;
        size_t hi = expected_record_size + 32;
        if (actual_record_size >= lo && actual_record_size <= hi) {
            for (size_t i = 0; i < row_count; ++i) {
                batch.rows.emplace_back(rb + i * actual_record_size, actual_record_size);
            }
        }
    }

    // Bookmark buffer — per-row physical bookmarks, one for each row.
    const uint8_t *bb; size_t bblen;
    if (walker.next_unit(bb, bblen)) {
        if (row_count > 0 && bblen % row_count == 0) {
            size_t per_row = bblen / row_count;
            for (size_t i = 0; i < batch.rows.size(); ++i) {
                batch.bookmarks.emplace_back(bb + i * per_row, per_row);
            }
        }
    }
    return batch;
}

} // namespace dbisam
