// Phase 5 unit tests: schema parser, cursor-info reader, row decoder.
// Mirrors the Rust unit tests in MrsFlow's schema.rs, cursor_info.rs,
// and row.rs (synthetic data, no live server needed).

#include "dbisam/cursor_info.hpp"
#include "dbisam/row.hpp"
#include "dbisam/schema.hpp"
#include "dbisam/wire.hpp"

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

// ---- schema ----

static void parse_one_synthetic_string_block() {
    std::vector<uint8_t> block(SCHEMA_BLOCK_STRIDE, 0);
    block[0] = 0x03; block[1] = 0x00; block[2] = 0x00;
    block[3] = 7; block[4] = 0;             // ord=7
    block[5] = 4;                           // namelen
    std::memcpy(block.data() + 6, "CODE", 4);
    block[0xA7] = 1;                        // sub=String
    block[0xA7 + 2] = 30;                   // decl
    block[0xA7 + 5] = 31;                   // max
    block[0xA7 + 8] = 25; block[0xA7 + 9] = 0; // row_offset
    auto [cols, end] = parse_schema(block.data(), block.size());
    CHECK(cols.size() == 1);
    CHECK(cols[0].ord == 7);  // ord=7 but expected=1 → schema parser stops here, which means we expected single block but it didn't match — actually...
    // Re-read: parse_schema walks blocks expecting ords 1,2,...; with ord=7
    // it accepts the first match because find_first_block_start looked for
    // ord=1 specifically. Adjust the synthetic block to ord=1.
}

static void parse_one_synthetic_block_ord_1() {
    std::vector<uint8_t> block(SCHEMA_BLOCK_STRIDE, 0);
    block[0] = 0x03; block[1] = 0x00; block[2] = 0x00;
    block[3] = 1; block[4] = 0;             // ord=1
    block[5] = 4;
    std::memcpy(block.data() + 6, "CODE", 4);
    block[0xA7] = 1;                        // sub=String
    block[0xA7 + 2] = 30; block[0xA7 + 5] = 31;
    block[0xA7 + 8] = 25;
    auto [cols, end] = parse_schema(block.data(), block.size());
    CHECK(cols.size() == 1);
    CHECK(cols[0].name == "CODE");
    CHECK(cols[0].field_type == FieldType::String);
    CHECK(cols[0].decl == 30);
    CHECK(cols[0].max == 31);
    CHECK(cols[0].row_offset == 25);
    CHECK(end == SCHEMA_BLOCK_STRIDE);
}

static void field_type_dispatch_matches_doc() {
    CHECK(field_type_from_sub(0, 0, 0)   == FieldType::Calculated);
    CHECK(field_type_from_sub(1, 0, 0)   == FieldType::String);
    CHECK(field_type_from_sub(2, 0, 0)   == FieldType::Date);
    CHECK(field_type_from_sub(3, 0x00, 0) == FieldType::Blob);
    CHECK(field_type_from_sub(3, 0x16, 0) == FieldType::Memo);
    CHECK(field_type_from_sub(3, 0x1A, 0) == FieldType::Graphic);
    CHECK(field_type_from_sub(4, 0, 0)   == FieldType::Boolean);
    CHECK(field_type_from_sub(5, 0, 0)   == FieldType::Smallint);
    CHECK(field_type_from_sub(6, 0x00, 0) == FieldType::Integer);
    CHECK(field_type_from_sub(6, 0x1D, 0) == FieldType::AutoInc);
    CHECK(field_type_from_sub(7, 0, 0x0A) == FieldType::Currency);
    CHECK(field_type_from_sub(7, 0, 0x00) == FieldType::Float);
    CHECK(field_type_from_sub(9, 0, 0)   == FieldType::Bytes);
    CHECK(field_type_from_sub(10, 0, 0)  == FieldType::Time);
    CHECK(field_type_from_sub(11, 0, 0)  == FieldType::DateTime);
    CHECK(field_type_from_sub(15, 0, 0)  == FieldType::VarBytes);
    CHECK(field_type_from_sub(18, 0, 0)  == FieldType::Largeint);
    CHECK(field_type_from_sub(99, 0, 0)  == FieldType::Unknown);
}

// ---- cursor_info ----

static void cursor_info_reads_full_record() {
    auto push_unit = [](std::vector<uint8_t> &buf, const uint8_t *p, size_t n) {
        uint32_t len = static_cast<uint32_t>(n);
        buf.insert(buf.end(), {static_cast<uint8_t>(len),
                               static_cast<uint8_t>(len >> 8),
                               static_cast<uint8_t>(len >> 16),
                               static_cast<uint8_t>(len >> 24)});
        buf.insert(buf.end(), p, p + n);
    };
    auto push_u32 = [&](std::vector<uint8_t> &buf, uint32_t v) {
        uint8_t b[4] = {static_cast<uint8_t>(v),
                        static_cast<uint8_t>(v >> 8),
                        static_cast<uint8_t>(v >> 16),
                        static_cast<uint8_t>(v >> 24)};
        push_unit(buf, b, 4);
    };
    std::vector<uint8_t> buf;
    push_u32(buf, 1);                       // RecordNumber
    push_u32(buf, 2);                       // PhysicalRecordNumber
    push_u32(buf, 25);                      // RecordCount
    push_u32(buf, 30);                      // PhysicalRecordsUsed
    push_u32(buf, 7);                       // LastAutoIncID
    uint8_t zeros[8] = {0};
    push_unit(buf, zeros, 8);               // LastUpdated
    push_u32(buf, 25);                      // TotalRecordCount
    const uint8_t bm[] = "BOOKMARK-BYTES";
    push_unit(buf, bm, 14);
    uint8_t flag_e = 0xAB; push_unit(buf, &flag_e, 1);
    uint8_t flag_d = 0xCD; push_unit(buf, &flag_d, 1);

    Walker w(buf.data(), buf.size());
    auto ci = CursorInfo::read(w);
    CHECK(ci.record_number == 1);
    CHECK(ci.record_count == 25);
    CHECK(ci.total_record_count == 25);
    CHECK(ci.bookmark.size() == 14);
    CHECK(std::memcmp(ci.bookmark.data(), "BOOKMARK-BYTES", 14) == 0);
    CHECK(ci.flag_60e == 0xAB);
    CHECK(ci.flag_60d == 0xCD);
    CHECK(ci.end_offset == buf.size());
}

// ---- row decoder ----

static Column make_col(uint16_t ord, const char *name, FieldType ft, uint8_t maxbytes, uint16_t row_offset) {
    Column c;
    c.ord = ord; c.name = name; c.field_type = ft;
    c.sub_raw = 0; c.byte_a8_raw = 0; c.byte_250_raw = 0;
    c.decl = maxbytes; c.max = maxbytes; c.row_offset = row_offset;
    return c;
}

static void decode_record_handles_null_indicator() {
    // Two columns: NAME (String, 4 max, row_offset=0), AGE (Integer, 4 max, row_offset=5).
    // Wire bytes: [null=1]["ABCD"][null=0][?][?][?][?]
    std::vector<Column> cols = {
        make_col(1, "NAME", FieldType::String, 4, 0),
        make_col(2, "AGE",  FieldType::Integer, 4, 5),
    };
    std::vector<uint8_t> rec = {1, 'A', 'B', 'C', 'D', 0, 0, 0, 0, 0};
    auto cells = decode_record(rec.data(), rec.size(), cols);
    CHECK(cells.size() == 2);
    CHECK(std::holds_alternative<std::string>(cells[0]));
    CHECK(std::get<std::string>(cells[0]) == "ABCD");
    CHECK(std::holds_alternative<NullValue>(cells[1]));
}

static void decode_record_string_trims_null_padding() {
    std::vector<Column> cols = {make_col(1, "S", FieldType::String, 8, 0)};
    std::vector<uint8_t> rec = {1, 'H', 'i', 0, 0, 0, 0, 0, 0};
    auto cells = decode_record(rec.data(), rec.size(), cols);
    CHECK(std::get<std::string>(cells[0]) == "Hi");
}

static void decode_record_smallint_signed() {
    std::vector<Column> cols = {make_col(1, "N", FieldType::Smallint, 2, 0)};
    // -1 = 0xFFFF LE
    std::vector<uint8_t> rec = {1, 0xFF, 0xFF};
    auto cells = decode_record(rec.data(), rec.size(), cols);
    CHECK(std::get<int32_t>(cells[0]) == -1);
}

static void decode_record_boolean() {
    std::vector<Column> cols = {make_col(1, "B", FieldType::Boolean, 2, 0)};
    std::vector<uint8_t> rt = {1, 0xFF, 0xFF};
    std::vector<uint8_t> rf = {1, 0x00, 0x00};
    CHECK(std::get<bool>(decode_record(rt.data(), rt.size(), cols)[0]) == true);
    CHECK(std::get<bool>(decode_record(rf.data(), rf.size(), cols)[0]) == false);
}

static void decode_record_currency_divides_by_10000() {
    std::vector<Column> cols = {make_col(1, "C", FieldType::Currency, 8, 0)};
    // 12345_6789 / 10_000 = 12345.6789
    int64_t raw = 123456789;
    std::vector<uint8_t> rec(9, 0);
    rec[0] = 1;
    for (int i = 0; i < 8; ++i) rec[1 + i] = static_cast<uint8_t>(raw >> (8 * i));
    auto cells = decode_record(rec.data(), rec.size(), cols);
    double v = std::get<double>(cells[0]);
    CHECK(v > 12345.67 && v < 12345.68);
}

int main() {
    parse_one_synthetic_block_ord_1();
    field_type_dispatch_matches_doc();
    cursor_info_reads_full_record();
    decode_record_handles_null_indicator();
    decode_record_string_trims_null_padding();
    decode_record_smallint_signed();
    decode_record_boolean();
    decode_record_currency_divides_by_10000();
    if (g_failures == 0) {
        std::printf("all phase-5 tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
