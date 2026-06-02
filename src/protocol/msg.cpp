#include "dbisam/msg.hpp"

#include <cstring>

namespace dbisam {

namespace {

void put_u32_le(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

void patch_u32_le(std::vector<uint8_t> &out, size_t off, uint32_t v) {
    out[off + 0] = static_cast<uint8_t>(v);
    out[off + 1] = static_cast<uint8_t>(v >> 8);
    out[off + 2] = static_cast<uint8_t>(v >> 16);
    out[off + 3] = static_cast<uint8_t>(v >> 24);
}

} // namespace

MsgBuilder::MsgBuilder(uint16_t reqcode) {
    body_.reserve(64);
    body_.push_back(0x00);                                  // flag
    body_.push_back(static_cast<uint8_t>(reqcode));         // reqcode lo
    body_.push_back(static_cast<uint8_t>(reqcode >> 8));    // reqcode hi
    put_u32_le(body_, 0);                                   // inner_len placeholder
    inner_start_ = body_.size();
}

MsgBuilder &MsgBuilder::pack(const uint8_t *payload, size_t len) {
    put_u32_le(body_, static_cast<uint32_t>(len));
    body_.insert(body_.end(), payload, payload + len);
    return *this;
}

MsgBuilder &MsgBuilder::pack_u8(uint8_t v) {
    return pack(&v, 1);
}

MsgBuilder &MsgBuilder::pack_u16(uint16_t v) {
    uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    return pack(b, 2);
}

MsgBuilder &MsgBuilder::pack_u32(uint32_t v) {
    uint8_t b[4] = {
        static_cast<uint8_t>(v),
        static_cast<uint8_t>(v >> 8),
        static_cast<uint8_t>(v >> 16),
        static_cast<uint8_t>(v >> 24),
    };
    return pack(b, 4);
}

MsgBuilder &MsgBuilder::pack_u64(uint64_t v) {
    uint8_t b[8] = {
        static_cast<uint8_t>(v),
        static_cast<uint8_t>(v >> 8),
        static_cast<uint8_t>(v >> 16),
        static_cast<uint8_t>(v >> 24),
        static_cast<uint8_t>(v >> 32),
        static_cast<uint8_t>(v >> 40),
        static_cast<uint8_t>(v >> 48),
        static_cast<uint8_t>(v >> 56),
    };
    return pack(b, 8);
}

std::vector<uint8_t> MsgBuilder::finish() {
    uint32_t inner_len = static_cast<uint32_t>(body_.size() - inner_start_);
    patch_u32_le(body_, 3, inner_len);
    return std::move(body_);
}

// ---- Concrete builders ----

// Connect handshake (reqcode 0x0000). Per live-capture analysis of a
// dbsys session with RemoteCompression=9, the inner field 2 is always
// 0 — actual session compression behaviour is driven by the framing
// layer's body[0] flag byte, not by this inner field. Followed by
// 4 trailing zero bytes (padding).
std::vector<uint8_t> build_connect(bool /*compression*/, const std::string &hostname, uint32_t nonce) {
    MsgBuilder m(reqcode::CONNECT);
    m.pack_u64(0xAB7Cu);                                                 // field 1: version
    m.pack_u8(0);                                                        // field 2: always 0 per capture
    m.pack(reinterpret_cast<const uint8_t *>(hostname.data()), hostname.size()); // field 3
    m.pack_u32(nonce);                                                   // field 4
    auto body = m.finish();
    body.insert(body.end(), {0, 0, 0, 0});                               // padding
    return body;
}

// PrepareStatement (reqcode 0x0320). Prelude/trailing flags are observed
// verbatim from PoC capture; exact field semantics not fully decoded but
// the values are stable across queries. SQL is twice-length-prefixed
// (Delphi TStringField convention) and spliced into the inner section
// as raw bytes, NOT as Pack units.
std::vector<uint8_t> build_prepare_statement(const std::string &sql) {
    MsgBuilder m(reqcode::PREPARE_STATEMENT);
    m.pack_u32(4).pack_u32(1).pack_u32(4);

    std::vector<uint8_t> sql_bytes(sql.begin(), sql.end());
    sql_bytes.push_back('\r');
    sql_bytes.push_back('\n');
    uint32_t n = static_cast<uint32_t>(sql_bytes.size());

    auto &b = m.body();
    auto push_u32 = [&](uint32_t v) {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v >> 16));
        b.push_back(static_cast<uint8_t>(v >> 24));
    };
    push_u32(n);
    push_u32(n);
    b.insert(b.end(), sql_bytes.begin(), sql_bytes.end());

    static const uint8_t TRAIL[] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
        0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
    };
    b.insert(b.end(), TRAIL, TRAIL + sizeof(TRAIL));

    uint32_t inner_len = static_cast<uint32_t>(b.size() - 7);
    b[3] = static_cast<uint8_t>(inner_len);
    b[4] = static_cast<uint8_t>(inner_len >> 8);
    b[5] = static_cast<uint8_t>(inner_len >> 16);
    b[6] = static_cast<uint8_t>(inner_len >> 24);

    // Final 5-byte outer trailer (stable across queries).
    auto body = std::move(b);
    body.insert(body.end(), {0x00, 0x00, 0x00, 0x00, 0x00});
    return body;
}

// ExecuteStatement (reqcode 0x032A). Inner offset +23 (the 6th Pack
// unit's payload) is the produces-cursor flavour byte per §7h:
// 0x01 for SELECT/INSERT/UPDATE/DELETE, 0x00 for pure DDL.
std::vector<uint8_t> build_execute_statement(uint32_t cursor_handle) {
    MsgBuilder m(reqcode::EXECUTE_STATEMENT);
    m.pack_u32(cursor_handle);
    uint8_t two[2] = {0x00, 0x00};
    m.pack(two, 2);
    m.pack_u8(0x00).pack_u8(0x01).pack_u8(0x00).pack_u8(0x00);
    return m.finish();
}

std::vector<uint8_t> build_execute_statement_ddl(uint32_t cursor_handle) {
    MsgBuilder m(reqcode::EXECUTE_STATEMENT);
    m.pack_u32(cursor_handle);
    uint8_t two[2] = {0x00, 0x00};
    m.pack(two, 2);
    m.pack_u8(0x00).pack_u8(0x00).pack_u8(0x00).pack_u8(0x00);
    return m.finish();
}

std::vector<uint8_t> build_receive() {
    MsgBuilder m(reqcode::RECEIVE);
    m.pack_u8(0x00);
    return m.finish();
}

std::vector<uint8_t> build_set_to_begin(uint32_t cursor_handle) {
    MsgBuilder m(reqcode::SET_TO_BEGIN);
    m.pack_u32(cursor_handle);
    return m.finish();
}

std::vector<uint8_t> build_get_next_record(uint32_t cursor_handle,
                                           const uint8_t *bookmark, size_t bookmark_len,
                                           uint32_t counter) {
    MsgBuilder m(reqcode::GET_NEXT_RECORD);
    m.pack_u32(cursor_handle);
    m.pack(bookmark, bookmark_len);
    m.pack_u8(0x00).pack_u8(0x00);
    m.pack_u32(counter);
    return m.finish();
}

std::vector<uint8_t> build_reset_statement(uint32_t cursor_handle) {
    MsgBuilder m(reqcode::RESET_STATEMENT);
    m.pack_u32(cursor_handle);
    return m.finish();
}

std::vector<uint8_t> build_close_cursor(uint32_t cursor_handle) {
    MsgBuilder m(reqcode::CLOSE_CURSOR);
    m.pack_u32(cursor_handle);
    return m.finish();
}

std::vector<uint8_t> build_remove_all_remote_memory_tables() {
    MsgBuilder m(reqcode::REMOVE_ALL_REMOTE_MEMORY_TABLES);
    return m.finish();
}

std::vector<uint8_t> build_begin_dml(uint32_t cursor_handle) {
    MsgBuilder m(reqcode::BEGIN_DML);
    m.pack_u32(cursor_handle);
    return m.finish();
}

std::vector<uint8_t> build_set_to_bookmark(uint32_t cursor_handle,
                                           const uint8_t *bookmark, size_t bookmark_len,
                                           uint8_t flag1, uint8_t flag2) {
    MsgBuilder m(reqcode::SET_TO_BOOKMARK);
    m.pack_u32(cursor_handle);
    m.pack(bookmark, bookmark_len);
    m.pack_u8(flag1).pack_u8(flag2);
    return m.finish();
}

std::vector<uint8_t> build_open_blob(uint32_t cursor_handle, uint16_t field_ord,
                                     const uint8_t *slot, size_t slot_len,
                                     uint8_t force_reread, uint8_t is_physical) {
    MsgBuilder m(reqcode::OPEN_BLOB);
    m.pack_u32(cursor_handle);
    m.pack_u16(field_ord);
    m.pack(slot, slot_len);
    m.pack_u8(force_reread);
    m.pack_u8(is_physical);
    m.pack_u8(0); // vestigial — official client sends, server tolerates
    return m.finish();
}

std::vector<uint8_t> build_free_blob(uint32_t cursor_handle, uint16_t field_ord,
                                     const uint8_t *slot, size_t slot_len, uint8_t flag) {
    MsgBuilder m(reqcode::FREE_BLOB);
    m.pack_u32(cursor_handle);
    m.pack_u16(field_ord);
    m.pack(slot, slot_len);
    m.pack_u8(flag);
    return m.finish();
}

std::vector<uint8_t> build_free_all_blobs(uint32_t cursor_handle, uint8_t flag) {
    MsgBuilder m(reqcode::FREE_ALL_BLOBS);
    m.pack_u32(cursor_handle);
    m.pack_u8(flag);
    return m.finish();
}

std::vector<uint8_t> build_read_first_record_block(uint32_t cursor_handle, uint32_t max_records) {
    MsgBuilder m(reqcode::READ_FIRST_RECORD_BLOCK);
    m.pack_u32(cursor_handle);
    m.pack_u32(max_records);
    return m.finish();
}

std::vector<uint8_t> build_read_next_record_block(uint32_t cursor_handle, uint32_t max_records) {
    MsgBuilder m(reqcode::READ_NEXT_RECORD_BLOCK);
    m.pack_u32(cursor_handle);
    m.pack_u32(max_records);
    return m.finish();
}

} // namespace dbisam
