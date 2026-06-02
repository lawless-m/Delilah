// Phase 3 verification: message builders.
// Mirrors the Rust unit tests in MrsFlow's exportmaster/msg.rs.

#include "dbisam/msg.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace dbisam;

static int g_failures = 0;

#define CHECK(cond) do {                                                       \
    if (!(cond)) {                                                             \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

static uint32_t read_u32_le(const std::vector<uint8_t> &b, size_t off) {
    return static_cast<uint32_t>(b[off]) |
           (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) |
           (static_cast<uint32_t>(b[off + 3]) << 24);
}

static void header_is_flag_reqcode_le_innerlen_le() {
    MsgBuilder m(0x0320);
    m.pack_u32(1);
    auto body = m.finish();
    CHECK(body[0] == 0x00);                          // flag
    CHECK(body[1] == 0x20 && body[2] == 0x03);       // reqcode 0x0320 LE
    CHECK(read_u32_le(body, 3) == 8);                // inner_len = 4-byte len + 4-byte payload
    CHECK(read_u32_le(body, 7) == 4);                // Pack unit length
    CHECK(read_u32_le(body, 11) == 1);               // value
}

static void execute_statement_layout() {
    auto body = build_execute_statement(1);
    CHECK(body[0] == 0x00);
    CHECK(body[1] == 0x2A && body[2] == 0x03);
    // 6 Pack units: u32(1) + 2-byte payload + 4×u8 = (4+4)+(4+2)+4×(4+1) = 34
    CHECK(read_u32_le(body, 3) == 34);
}

static void set_to_begin_layout() {
    auto body = build_set_to_begin(1);
    CHECK(body.size() == 15);                        // 7-byte header + 1 Pack unit
    CHECK(body[1] == 0xBE && body[2] == 0x00);       // reqcode 0x00BE LE
}

// Body shape for the single-Pack-u32(handle) family — used by BeginDML,
// ResetStatement, and CloseCursor. All three capture as
// `04 00 00 00 01 00 00 00` for handle=1.
static void single_handle_body_matches(const std::vector<uint8_t> &body, uint16_t expected_reqcode) {
    CHECK(body.size() == 15);
    CHECK(body[0] == 0x00);
    CHECK(body[1] == static_cast<uint8_t>(expected_reqcode));
    CHECK(body[2] == static_cast<uint8_t>(expected_reqcode >> 8));
    CHECK(read_u32_le(body, 3) == 8);                // inner_len
    CHECK(read_u32_le(body, 7) == 4);                // Pack len
    CHECK(read_u32_le(body, 11) == 1);               // handle
}

static void begin_dml_layout() {
    single_handle_body_matches(build_begin_dml(1), 0x0316);
}

static void reset_statement_layout() {
    single_handle_body_matches(build_reset_statement(1), reqcode::RESET_STATEMENT);
}

static void close_cursor_layout() {
    single_handle_body_matches(build_close_cursor(1), reqcode::CLOSE_CURSOR);
}

static void remove_all_remote_memory_tables_is_just_the_header() {
    auto body = build_remove_all_remote_memory_tables();
    const std::vector<uint8_t> expected{0x00, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(body == expected);
}

static void open_blob_layout_matches_disassembly() {
    std::vector<uint8_t> slot(56, 0xAA);
    auto body = build_open_blob(1, 9, slot.data(), slot.size(), 0, 0);
    CHECK(body[0] == 0x00);
    CHECK(body[1] == 0x80 && body[2] == 0x02);       // 0x0280 LE
    // inner = 8 (handle) + 6 (field_ord) + 60 (slot) + 5*3 (3 byte flags) = 89
    CHECK(read_u32_le(body, 3) == 89);
    // cursor handle = 1
    CHECK(read_u32_le(body, 7) == 4);
    CHECK(read_u32_le(body, 11) == 1);
    // field_ord = 9 (u16)
    CHECK(read_u32_le(body, 15) == 2);
    CHECK(body[19] == 0x09 && body[20] == 0x00);
    // slot
    CHECK(read_u32_le(body, 21) == 56);
    CHECK(std::memcmp(body.data() + 25, slot.data(), 56) == 0);
    // three trailing byte-flag units
    CHECK(read_u32_le(body, 81) == 1); CHECK(body[85] == 0x00);
    CHECK(read_u32_le(body, 86) == 1); CHECK(body[90] == 0x00);
    CHECK(read_u32_le(body, 91) == 1); CHECK(body[95] == 0x00);
    CHECK(body.size() == 96);
}

static void free_blob_layout_matches_disassembly() {
    std::vector<uint8_t> slot(56, 0xCD);
    auto body = build_free_blob(1, 9, slot.data(), slot.size(), 0);
    CHECK(body[0] == 0x00);
    CHECK(body[1] == 0x8A && body[2] == 0x02);       // 0x028A LE
    // inner = 8 + 6 + 60 + 5 = 79
    CHECK(read_u32_le(body, 3) == 79);
    CHECK(body.size() == 7 + 79);
}

static void execute_statement_ddl_differs_at_offset_30() {
    auto dml = build_execute_statement(1);
    auto ddl = build_execute_statement_ddl(1);
    CHECK(dml.size() == ddl.size());
    CHECK(dml.size() == 41);
    CHECK(dml[30] == 0x01);
    CHECK(ddl[30] == 0x00);
    for (size_t i = 0; i < dml.size(); ++i) {
        if (i == 30) continue;
        CHECK(dml[i] == ddl[i]);
    }
}

int main() {
    header_is_flag_reqcode_le_innerlen_le();
    execute_statement_layout();
    set_to_begin_layout();
    begin_dml_layout();
    reset_statement_layout();
    close_cursor_layout();
    remove_all_remote_memory_tables_is_just_the_header();
    open_blob_layout_matches_disassembly();
    free_blob_layout_matches_disassembly();
    execute_statement_ddl_differs_at_offset_30();
    if (g_failures == 0) {
        std::printf("all phase-3 tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
