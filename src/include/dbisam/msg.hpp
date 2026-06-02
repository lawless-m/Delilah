// Pack-stream message builders for client→server requests.
//
// Every body has the layout
//   <flag:u8=0> <reqcode:u16 LE> <inner_len:u32 LE> <pack stream>
// per DBISAM-PROTOCOL.md §6c, §6g + ANSWERS-TO-DEREK.md.
//
// Pack stream is a sequence of `<u32 LE length><payload>` units; each
// unit's meaning depends on the reqcode's DoXxx handler on the server.
//
// Port of mrsflow-cli/src/exportmaster/msg.rs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dbisam {

namespace reqcode {
constexpr uint16_t CONNECT = 0x0000;
constexpr uint16_t LOGIN = 0x0014;
constexpr uint16_t SESSION_PARAMS = 0x0028;
constexpr uint16_t REMOVE_ALL_REMOTE_MEMORY_TABLES = 0x0029;
constexpr uint16_t OPEN_DATA_DIR = 0x003C;
constexpr uint16_t CLOSE_DATA_DIR = 0x0046;

constexpr uint16_t OPEN_CURSOR = 0x0096;
constexpr uint16_t CLOSE_CURSOR = 0x00A0;
constexpr uint16_t RESET_CURSOR = 0x00AA;
constexpr uint16_t SET_INDEX_NAME = 0x00B4;
constexpr uint16_t SET_TO_BEGIN = 0x00BE;
constexpr uint16_t SET_TO_END = 0x00C8;
constexpr uint16_t GET_CURRENT_RECORD = 0x00E6;
constexpr uint16_t GET_NEXT_RECORD = 0x00FA;
constexpr uint16_t GET_PRIOR_RECORD = 0x0104;
constexpr uint16_t SET_TO_BOOKMARK = 0x0154;

constexpr uint16_t OPEN_BLOB = 0x0280;
constexpr uint16_t FREE_BLOB = 0x028A;
constexpr uint16_t FREE_ALL_BLOBS = 0x0294;

constexpr uint16_t RECEIVE = 0x030C;
constexpr uint16_t BEGIN_DML = 0x0316;
constexpr uint16_t PREPARE_STATEMENT = 0x0320;
constexpr uint16_t EXECUTE_STATEMENT = 0x032A;
constexpr uint16_t RESET_STATEMENT = 0x0334;

// Batched record-block family (RecordBlock = batch of N rows per
// round-trip; single-row variants above are for cursor scrolling).
constexpr uint16_t READ_NEXT_RECORD_BLOCK = 0x04F6;
constexpr uint16_t READ_PRIOR_RECORD_BLOCK = 0x0500;
constexpr uint16_t READ_FIRST_RECORD_BLOCK = 0x050A;
constexpr uint16_t READ_LAST_RECORD_BLOCK = 0x0514;
constexpr uint16_t READ_ABSOLUTE_RECORD_BLOCK = 0x051E;
constexpr uint16_t READ_BOOKMARK_RECORD_BLOCK = 0x0528;
constexpr uint16_t REFRESH_RECORD_BLOCK = 0x0532;
constexpr uint16_t ADD_RECORD_BLOCK = 0x053C;
constexpr uint16_t UPDATE_RECORD_BLOCK = 0x0546;
constexpr uint16_t DELETE_RECORD_BLOCK = 0x0550;
} // namespace reqcode

// Builds a request body: writes the 7-byte header, then a sequence of
// Pack units, then patches inner_len based on bytes written.
class MsgBuilder {
public:
    explicit MsgBuilder(uint16_t reqcode);

    // Push one <u32 length><payload> Pack unit.
    MsgBuilder &pack(const uint8_t *payload, size_t len);
    MsgBuilder &pack(const std::vector<uint8_t> &payload) {
        return pack(payload.data(), payload.size());
    }

    MsgBuilder &pack_u8(uint8_t v);
    MsgBuilder &pack_u16(uint16_t v);
    MsgBuilder &pack_u32(uint32_t v);
    MsgBuilder &pack_u64(uint64_t v);

    // Finish: patch the inner_len field and return the body bytes.
    std::vector<uint8_t> finish();

    // Direct mutation hooks for builders that splice non-Pack bytes
    // into the inner section (currently just PrepareStatement, which
    // embeds a TStringField outside the Pack-unit framing).
    std::vector<uint8_t> &body() { return body_; }

private:
    std::vector<uint8_t> body_;
    size_t inner_start_;
};

// ---- Concrete request builders for the SELECT-only flow ----

std::vector<uint8_t> build_connect(bool compression, const std::string &hostname, uint32_t nonce);
std::vector<uint8_t> build_prepare_statement(const std::string &sql);
std::vector<uint8_t> build_execute_statement(uint32_t cursor_handle);
std::vector<uint8_t> build_execute_statement_ddl(uint32_t cursor_handle);
std::vector<uint8_t> build_receive();
std::vector<uint8_t> build_set_to_begin(uint32_t cursor_handle);
std::vector<uint8_t> build_get_next_record(uint32_t cursor_handle,
                                           const uint8_t *bookmark, size_t bookmark_len,
                                           uint32_t counter);
std::vector<uint8_t> build_reset_statement(uint32_t cursor_handle);
std::vector<uint8_t> build_close_cursor(uint32_t cursor_handle);
std::vector<uint8_t> build_remove_all_remote_memory_tables();
std::vector<uint8_t> build_begin_dml(uint32_t cursor_handle);
std::vector<uint8_t> build_set_to_bookmark(uint32_t cursor_handle,
                                           const uint8_t *bookmark, size_t bookmark_len,
                                           uint8_t flag1, uint8_t flag2);
std::vector<uint8_t> build_open_blob(uint32_t cursor_handle, uint16_t field_ord,
                                     const uint8_t *slot, size_t slot_len,
                                     uint8_t force_reread, uint8_t is_physical);
std::vector<uint8_t> build_free_blob(uint32_t cursor_handle, uint16_t field_ord,
                                     const uint8_t *slot, size_t slot_len, uint8_t flag);
std::vector<uint8_t> build_free_all_blobs(uint32_t cursor_handle, uint8_t flag);
std::vector<uint8_t> build_read_first_record_block(uint32_t cursor_handle, uint32_t max_records);
std::vector<uint8_t> build_read_next_record_block(uint32_t cursor_handle, uint32_t max_records);

} // namespace dbisam
