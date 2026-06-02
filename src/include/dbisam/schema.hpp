// Schema parser — decodes the 772-byte column-block region of a SELECT
// response into typed Column descriptors. See DBISAM-PROTOCOL.md §4 + §6b.
//
// Port of mrsflow-cli/src/exportmaster/schema.rs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dbisam {

class SchemaError : public std::runtime_error {
public:
    explicit SchemaError(const std::string &msg) : std::runtime_error(msg) {}
};

constexpr size_t SCHEMA_BLOCK_STRIDE = 772;

// DBISAM field type codes, mapped from the `sub` byte at +0xA7 in each
// column block. Three of them (Blob, Integer, Float) are refined by
// auxiliary bytes at +0xA8 or +0x250 — see field_type_from_sub().
enum class FieldType : uint8_t {
    Calculated,  // sub=0 — no storage
    String,      // sub=1
    Date,        // sub=2  — 4-byte LE u32, days since 0001-01-01
    Blob,        // sub=3 +A8=0x00
    Memo,        // sub=3 +A8=0x16
    Graphic,     // sub=3 +A8=0x1A
    Boolean,     // sub=4  — 2-byte WordBool: FFFF=true, 0000=false
    Smallint,    // sub=5  — 2-byte LE signed
    Integer,     // sub=6 +A8≠0x1D
    AutoInc,     // sub=6 +A8=0x1D
    Currency,    // sub=7 +250=0x0A  — 8-byte LE i64 / 10000
    Float,       // sub=7 +250≠0x0A — 8-byte LE binary64
    Bytes,       // sub=9
    Time,        // sub=10 — 4-byte LE u32, ms since midnight
    DateTime,    // sub=11 — 8-byte LE binary64, days since 1899-12-30
    VarBytes,    // sub=15
    Largeint,    // sub=18 — 8-byte LE signed
    Unknown,
};

FieldType field_type_from_sub(uint8_t sub, uint8_t byte_a8, uint8_t byte_250);

struct Column {
    uint16_t ord;             // 1-based ordinal in the result set
    std::string name;
    FieldType field_type;
    uint8_t sub_raw;          // original sub byte (for Unknown reporting)
    uint8_t byte_a8_raw;
    uint8_t byte_250_raw;
    uint8_t decl;             // declared length
    uint8_t max;              // on-disk storage width
    uint16_t row_offset;      // null-flag byte offset within the record
};

// Parse all column descriptors from the schema region of a SELECT
// response. Returns the columns and the byte offset just past the last
// block. Throws SchemaError if no blocks are found.
std::pair<std::vector<Column>, size_t> parse_schema(const uint8_t *payload, size_t len);

inline std::pair<std::vector<Column>, size_t> parse_schema(const std::vector<uint8_t> &payload) {
    return parse_schema(payload.data(), payload.size());
}

} // namespace dbisam
