#include "dbisam/client.hpp"

#include "dbisam/blob.hpp"
#include "dbisam/crypto.hpp"
#include "dbisam/cursor.hpp"
#include "dbisam/msg.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <variant>

namespace dbisam {

namespace {

// Fixed session-setup messages sent immediately after a successful login.
// Replayed verbatim from a captured dbsys.exe session — see client.rs
// for the byte sources.

// C[2] — 44-byte body.
constexpr uint8_t SESSION_SETUP_C2[] = {
    0x00, 0x28, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x02, 0x00, 0x00, 0x00, 0x64, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x02, 0x00, 0x00, 0x00, 0x14, 0x00, 0x17, 0xF2, 0x43, 0x90, 0x00,
};

// C[3] — 12-byte body.
constexpr uint8_t SESSION_SETUP_C3[] = {
    0x00, 0x84, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
};

// C[5] — 20-byte trailing session-setup body, sent after the catalog
// attach. The trailing "INT_C" appears in every session capture; meaning
// undecoded but the server accepts it.
constexpr uint8_t SESSION_SETUP_POST[] = {
    0x00, 0x16, 0x03, 0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x49,
    0x4E, 0x54, 0x5F, 0x43,
};

// Stable workstation name + nonce, copied from the PoC capture. Server
// stores both but doesn't validate strictly (hostname) or echo
// meaningfully (nonce).
constexpr const char *FIXED_HOSTNAME = "RIVSEM048692";
constexpr uint32_t FIXED_NONCE = 0xE5A21BE8u;

void put_u32_le(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

} // namespace

std::vector<uint8_t> build_login_body(const uint8_t *ct, size_t ct_len) {
    uint32_t inner_len = static_cast<uint32_t>(4 + 4 + 4 + ct_len);
    std::vector<uint8_t> body;
    body.reserve(3 + 4 + inner_len + 1);
    body.insert(body.end(), {0x00, 0x14, 0x00});                // flag + reqcode 0x14
    put_u32_le(body, inner_len);
    put_u32_le(body, 4);                                        // first inner field length
    put_u32_le(body, static_cast<uint32_t>(ct_len));            // buf len
    put_u32_le(body, static_cast<uint32_t>(ct_len));            // buf max len
    body.insert(body.end(), ct, ct + ct_len);
    body.push_back(0x00);
    return body;
}

std::vector<uint8_t> build_catalog_attach_body(const std::string &catalog) {
    const uint8_t *name = reinterpret_cast<const uint8_t *>(catalog.data());
    size_t name_len = catalog.size();
    uint32_t inner_len = static_cast<uint32_t>(4 + name_len + 5);
    std::vector<uint8_t> body;
    body.reserve(3 + 4 + inner_len + 2);
    body.insert(body.end(), {0x00, 0x3C, 0x00});                // flag + reqcode 0x3C
    put_u32_le(body, inner_len);
    put_u32_le(body, static_cast<uint32_t>(name_len));
    body.insert(body.end(), name, name + name_len);
    body.insert(body.end(), {0x01, 0x00, 0x00, 0x00, 0x00});    // inner trailer
    body.insert(body.end(), {0x64, 0x00});                      // outer 2-byte trailer
    return body;
}

// Build a PrepareStatement (reqcode 0x0320) body carrying `sql`. The
// inner_pre / inner_trail / outer_trail blobs are captured verbatim
// from `select count(*) from analysis\r\n` against the live server —
// stable across queries. The SQL is twice-length-prefixed (Delphi
// TStringField convention) and spliced into the inner section as raw
// bytes, NOT as Pack units.
//
// This differs slightly from msg::build_prepare_statement (which uses
// three Pack u32 units as the prelude). build_query_body is the
// captured/working layout, used by every live-tested code path.
static std::vector<uint8_t> build_query_body(const std::string &sql) {
    std::vector<uint8_t> sql_bytes(sql.begin(), sql.end());
    sql_bytes.push_back('\r');
    sql_bytes.push_back('\n');
    uint32_t n = static_cast<uint32_t>(sql_bytes.size());

    static const uint8_t INNER_PRE[] = {
        0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    };
    static const uint8_t INNER_TRAIL[] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
        0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
    };
    static const uint8_t OUTER_TRAIL[] = {0x00, 0x00, 0x00, 0x00, 0x00};

    uint32_t inner_len = static_cast<uint32_t>(sizeof(INNER_PRE) + 8 + sql_bytes.size() + sizeof(INNER_TRAIL));
    std::vector<uint8_t> body;
    body.reserve(3 + 4 + inner_len + sizeof(OUTER_TRAIL));
    body.insert(body.end(), {0x00, 0x20, 0x03});                                 // flag + reqcode 0x0320
    put_u32_le(body, inner_len);
    body.insert(body.end(), INNER_PRE, INNER_PRE + sizeof(INNER_PRE));
    put_u32_le(body, n);                                                         // sql_len
    put_u32_le(body, n);                                                         // sql_max_len
    body.insert(body.end(), sql_bytes.begin(), sql_bytes.end());
    body.insert(body.end(), INNER_TRAIL, INNER_TRAIL + sizeof(INNER_TRAIL));
    body.insert(body.end(), OUTER_TRAIL, OUTER_TRAIL + sizeof(OUTER_TRAIL));
    return body;
}

std::vector<uint8_t> Client::query_raw(const std::string &sql) {
    auto body = build_query_body(sql);
    return transport_.send_recv_auto(body, compression_);
}

Client::StreamingScan Client::start_streaming(const std::string &sql) {
    StreamingScan out;
    auto schema_resp = query_raw(sql);
    auto [columns, _end] = parse_schema(schema_resp);
    out.columns = columns;
    out.runner = std::make_unique<CursorRunner>(transport_, std::move(columns),
                                                batch_size_, compression_);
    return out;
}

// Decode a batch of row spans into CellValues. For Blob/Memo/Graphic
// columns, replace the row's BlobHandle cell with the fetched payload
// (via the live transport). Mirrors the per-batch portion of the
// old materialised path; safe to call repeatedly over a streaming
// CursorRunner since each batch's bookmarks remain server-addressable
// for the duration of that batch.
std::vector<std::vector<CellValue>> Client::decode_batch_with_blobs(
    const std::vector<Column> &columns,
    const std::vector<std::pair<const uint8_t *, size_t>> &rows,
    const std::vector<std::pair<const uint8_t *, size_t>> &bookmarks) {

    if (columns.empty()) return {};

    std::vector<size_t> blob_col_indices;
    for (size_t i = 0; i < columns.size(); ++i) {
        auto ft = columns[i].field_type;
        if (ft == FieldType::Blob || ft == FieldType::Memo || ft == FieldType::Graphic) {
            blob_col_indices.push_back(i);
        }
    }
    size_t pk_field_width = columns.front().max;
    size_t first_off = columns.front().row_offset;
    const auto &last = columns.back();
    size_t col_data_span = (last.row_offset - first_off) + 1 + last.max;
    size_t col_end_off = first_off + col_data_span;

    struct BlobTask {
        size_t row_idx;
        size_t col_idx;
        std::array<uint8_t, 16> md5;
        std::vector<uint8_t> pk_field;
        uint32_t phys;
    };
    std::vector<BlobTask> blob_tasks;

    std::vector<std::vector<CellValue>> decoded;
    decoded.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const uint8_t *row = rows[i].first;
        size_t row_len = rows[i].second;
        if (row_len < col_end_off) {
            decoded.emplace_back(); // skip malformed
            continue;
        }
        std::vector<CellValue> cells;
        try {
            cells = decode_record(row + first_off, col_data_span, columns);
        } catch (const std::exception &) {
            decoded.emplace_back();
            continue;
        }
        const uint8_t *bm = (i < bookmarks.size()) ? bookmarks[i].first : nullptr;
        size_t bmlen = (i < bookmarks.size()) ? bookmarks[i].second : 0;
        for (size_t ci : blob_col_indices) {
            if (auto *h = std::get_if<BlobHandle>(&cells[ci])) {
                bool all_zero = true;
                for (uint8_t b : h->bytes) {
                    if (b != 0) { all_zero = false; break; }
                }
                if (!all_zero && row_len >= 25 && row_len >= first_off + 1 + pk_field_width) {
                    BlobTask t;
                    t.row_idx = decoded.size();
                    t.col_idx = ci;
                    std::memcpy(t.md5.data(), row + 9, 16);
                    t.pk_field.assign(row + first_off + 1, row + first_off + 1 + pk_field_width);
                    t.phys = physical_record_number_from_bookmark(bm, bmlen);
                    blob_tasks.push_back(std::move(t));
                }
                cells[ci] = NullValue{};
            }
        }
        decoded.push_back(std::move(cells));
    }

    // Resolve deferred blob fetches while the cursor is still open.
    size_t effective_slot_length = blob_slot_length_;
    constexpr size_t FLUSH_EVERY = 50;
    bool blob_debug = std::getenv("DBISAM_BLOB_DEBUG") != nullptr;
    for (size_t i = 0; i < blob_tasks.size(); ++i) {
        if (i > 0 && i % FLUSH_EVERY == 0) {
            (void)transport_.send_recv_auto(build_free_all_blobs(1, 0), compression_);
        }
        const auto &task = blob_tasks[i];
        try {
            auto slot = build_slot(task.phys, task.md5, task.pk_field, effective_slot_length);
            auto outcome = fetch_blob(transport_, compression_, 1,
                                      static_cast<uint16_t>(task.col_idx + 1), slot);
            if (outcome.actual_slot_length != effective_slot_length) {
                effective_slot_length = outcome.actual_slot_length;
                slot = build_slot(task.phys, task.md5, task.pk_field, effective_slot_length);
                outcome = fetch_blob(transport_, compression_, 1,
                                     static_cast<uint16_t>(task.col_idx + 1), slot);
            }
            decoded[task.row_idx][task.col_idx] = std::move(outcome.payload);
            (void)transport_.send_recv_auto(
                build_free_blob(1, static_cast<uint16_t>(task.col_idx + 1),
                                outcome.slot_echo.data(), outcome.slot_echo.size(), 0),
                compression_);
        } catch (const std::exception &e) {
            if (blob_debug) {
                std::fprintf(stderr, "[em-blob] task %zu FAIL: %s\n", i, e.what());
            }
        }
    }

    return decoded;
}

Client::QueryResult Client::query_decoded(const std::string &sql, size_t target_rows) {
    auto schema_resp = query_raw(sql);
    auto [columns, _end] = parse_schema(schema_resp);

    QueryResult out;
    out.columns = columns;

    if (target_rows == 0) target_rows = SIZE_MAX;

    size_t first_off = columns.front().row_offset;
    const auto &last = columns.back();
    size_t col_data_span = (last.row_offset - first_off) + 1 + last.max;
    size_t col_end_off = first_off + col_data_span;

    // Identify blob/memo/graphic columns up front for the row-pass loop.
    std::vector<size_t> blob_col_indices;
    for (size_t i = 0; i < columns.size(); ++i) {
        auto ft = columns[i].field_type;
        if (ft == FieldType::Blob || ft == FieldType::Memo || ft == FieldType::Graphic) {
            blob_col_indices.push_back(i);
        }
    }
    size_t pk_field_width = columns.front().max;

    // Deferred blob fetches captured during the cursor pass. Resolved
    // AFTER drive_cursor finishes — the cursor stays open through cleanup,
    // so handles remain valid.
    struct BlobTask {
        size_t row_idx;
        size_t col_idx;
        std::array<uint8_t, 16> md5;
        std::vector<uint8_t> pk_field;
        uint32_t phys;
    };
    std::vector<BlobTask> blob_tasks;

    drive_cursor(transport_, columns, target_rows, batch_size_, compression_,
                 [&](const uint8_t *row, size_t row_len,
                     const uint8_t *bm, size_t bmlen) {
        if (row_len < col_end_off) return true;
        std::vector<CellValue> cells;
        try {
            cells = decode_record(row + first_off, col_data_span, columns);
        } catch (const std::exception &) {
            return true; // skip individual bad rows
        }

        // For each blob column with a non-zero handle, capture a task
        // and replace the cell with NullValue as a placeholder.
        for (size_t ci : blob_col_indices) {
            if (auto *h = std::get_if<BlobHandle>(&cells[ci])) {
                bool all_zero = true;
                for (uint8_t b : h->bytes) {
                    if (b != 0) { all_zero = false; break; }
                }
                if (!all_zero
                    && row_len >= 25
                    && row_len >= first_off + 1 + pk_field_width) {
                    BlobTask t;
                    t.row_idx = out.rows.size();
                    t.col_idx = ci;
                    std::memcpy(t.md5.data(), row + 9, 16);
                    t.pk_field.assign(row + first_off + 1,
                                      row + first_off + 1 + pk_field_width);
                    t.phys = physical_record_number_from_bookmark(bm, bmlen);
                    if (std::getenv("DBISAM_BLOB_DEBUG") && blob_tasks.size() < 3) {
                        std::fprintf(stderr, "[em-blob] capture row %zu col %zu: bmlen=%zu bm=",
                                     out.rows.size(), ci, bmlen);
                        for (size_t k = 0; k < bmlen && k < 32; ++k) {
                            std::fprintf(stderr, "%02x", bm[k]);
                        }
                        std::fprintf(stderr, "\n");
                    }
                    blob_tasks.push_back(std::move(t));
                }
                cells[ci] = NullValue{};
            }
        }

        out.rows.push_back(std::move(cells));
        return true;
    });

    // Resolve deferred blob fetches while the cursor is still open.
    size_t effective_slot_length = blob_slot_length_;
    constexpr size_t FLUSH_EVERY = 50;
    bool blob_debug = std::getenv("DBISAM_BLOB_DEBUG") != nullptr;
    if (blob_debug) {
        std::fprintf(stderr, "[em-blob] %zu blob tasks; %zu blob columns; pk_width=%zu\n",
                     blob_tasks.size(), blob_col_indices.size(), pk_field_width);
    }
    for (size_t i = 0; i < blob_tasks.size(); ++i) {
        if (i > 0 && i % FLUSH_EVERY == 0) {
            (void)transport_.send_recv_auto(build_free_all_blobs(1, 0), compression_);
        }
        const auto &task = blob_tasks[i];
        try {
            auto slot = build_slot(task.phys, task.md5, task.pk_field, effective_slot_length);
            auto outcome = fetch_blob(transport_, compression_, 1,
                                      static_cast<uint16_t>(task.col_idx + 1), slot);
            if (blob_debug) {
                std::fprintf(stderr, "[em-blob] task %zu (row=%zu col=%zu phys=%u): payload=%zu bytes, slot_echo=%zu\n",
                             i, task.row_idx, task.col_idx, task.phys,
                             outcome.payload.size(), outcome.actual_slot_length);
            }
            if (outcome.actual_slot_length != effective_slot_length) {
                effective_slot_length = outcome.actual_slot_length;
                slot = build_slot(task.phys, task.md5, task.pk_field, effective_slot_length);
                outcome = fetch_blob(transport_, compression_, 1,
                                     static_cast<uint16_t>(task.col_idx + 1), slot);
                if (blob_debug) {
                    std::fprintf(stderr, "[em-blob]   retry: payload=%zu bytes, slot_echo=%zu\n",
                                 outcome.payload.size(), outcome.actual_slot_length);
                }
            }
            out.rows[task.row_idx][task.col_idx] = std::move(outcome.payload);
            (void)transport_.send_recv_auto(
                build_free_blob(1, static_cast<uint16_t>(task.col_idx + 1),
                                outcome.slot_echo.data(), outcome.slot_echo.size(), 0),
                compression_);
        } catch (const std::exception &e) {
            if (blob_debug) {
                std::fprintf(stderr, "[em-blob] task %zu FAIL: %s\n", i, e.what());
            }
        }
    }

    // Cleanup: close cursor, reset statement, release server-side temp tables.
    (void)transport_.send_recv_auto(build_close_cursor(1), compression_);
    (void)transport_.send_recv_auto(build_reset_statement(1), compression_);
    (void)transport_.send_recv_auto(build_remove_all_remote_memory_tables(), compression_);

    return out;
}

// SQLTables-equivalent native request (reqcode 0x0032).
//
// Request body (20 bytes): reqcode 0x0032 | sub-flag 0x00 | inner_len u32 LE = 8
//   inner: `04 00 00 00 01 00 00 00` (cursor handle = 1)
//   trailing 5 bytes "INT_C" replayed verbatim.
//
// Response body header is 11 bytes; the table count is a u32 LE at the
// payload's +7 offset (== response offset 11). Then `count` entries of
// `[u32 LE name_len][ASCII name]`.
//
// Verbatim port of MrsFlow's list_tables + parse_sqltables_response.
std::vector<std::string> Client::list_tables() {
    static const uint8_t SQLTABLES_BODY[] = {
        0x00, 0x32, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x49, 0x4E, 0x54, 0x5F, 0x43,
    };
    std::vector<uint8_t> body(SQLTABLES_BODY, SQLTABLES_BODY + sizeof(SQLTABLES_BODY));
    auto resp = transport_.send_recv_auto(body, compression_);

    if (resp.size() < 15) {
        throw std::runtime_error("list_tables: response too short (" + std::to_string(resp.size()) + " bytes)");
    }
    constexpr size_t count_off = 4 + 7; // 4-byte envelope (reqcode + inner_len) + 7-byte header
    uint32_t count = static_cast<uint32_t>(resp[count_off]) |
                     (static_cast<uint32_t>(resp[count_off + 1]) << 8) |
                     (static_cast<uint32_t>(resp[count_off + 2]) << 16) |
                     (static_cast<uint32_t>(resp[count_off + 3]) << 24);
    if (count > 1'000'000) {
        throw std::runtime_error("list_tables: implausible table count " + std::to_string(count) +
                                 " — wire layout may have changed");
    }

    std::vector<std::string> names;
    names.reserve(count);
    size_t pos = 4 + 11; // envelope + 11-byte header
    for (uint32_t k = 0; k < count; ++k) {
        if (pos + 4 > resp.size()) {
            throw std::runtime_error("list_tables: truncated at name " + std::to_string(k) + "/" + std::to_string(count));
        }
        uint32_t slen = static_cast<uint32_t>(resp[pos]) |
                        (static_cast<uint32_t>(resp[pos + 1]) << 8) |
                        (static_cast<uint32_t>(resp[pos + 2]) << 16) |
                        (static_cast<uint32_t>(resp[pos + 3]) << 24);
        pos += 4;
        if (slen == 0 || slen > 256 || pos + slen > resp.size()) {
            throw std::runtime_error("list_tables: bad name length " + std::to_string(slen) +
                                     " at name " + std::to_string(k));
        }
        names.emplace_back(reinterpret_cast<const char *>(resp.data() + pos), slen);
        pos += slen;
    }
    return names;
}

Client Client::connect_and_login(const ConnOpts &opts) {
    Transport t = connect(opts.host, opts.port);

    // 1) Connect — body itself is also compressed when RemoteCompression
    //    is on (the server detects the comp byte and inflates).
    auto connect_body = build_connect(opts.compression, FIXED_HOSTNAME, FIXED_NONCE);
    (void)t.send_recv_auto(connect_body, opts.compression);

    // 2) Login.
    auto ct = encrypt_login(
        reinterpret_cast<const uint8_t *>(opts.user.data()), opts.user.size(),
        reinterpret_cast<const uint8_t *>(opts.password.data()), opts.password.size(),
        reinterpret_cast<const uint8_t *>(opts.encrypt_password.data()), opts.encrypt_password.size());
    auto login_body = build_login_body(ct);
    (void)t.send_recv_auto(login_body, opts.compression);

    // 3) Session-setup: 2 fixed messages, then catalog attach, then trailer.
    std::vector<uint8_t> c2(SESSION_SETUP_C2, SESSION_SETUP_C2 + sizeof(SESSION_SETUP_C2));
    (void)t.send_recv_auto(c2, opts.compression);

    std::vector<uint8_t> c3(SESSION_SETUP_C3, SESSION_SETUP_C3 + sizeof(SESSION_SETUP_C3));
    (void)t.send_recv_auto(c3, opts.compression);

    auto catalog_body = build_catalog_attach_body(opts.catalog);
    (void)t.send_recv_auto(catalog_body, opts.compression);

    std::vector<uint8_t> post(SESSION_SETUP_POST, SESSION_SETUP_POST + sizeof(SESSION_SETUP_POST));
    (void)t.send_recv_auto(post, opts.compression);

    return Client(std::move(t), opts.compression, opts.batch_size, opts.blob_slot_length);
}

} // namespace dbisam
