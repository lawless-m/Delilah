#include "dbisam/schema.hpp"

#include <cstring>

namespace dbisam {

FieldType field_type_from_sub(uint8_t sub, uint8_t byte_a8, uint8_t byte_250) {
    switch (sub) {
    case 0:  return FieldType::Calculated;
    case 1:  return FieldType::String;
    case 2:  return FieldType::Date;
    case 3:
        if (byte_a8 == 0x00) return FieldType::Blob;
        if (byte_a8 == 0x16) return FieldType::Memo;
        if (byte_a8 == 0x1A) return FieldType::Graphic;
        return FieldType::Unknown;
    case 4:  return FieldType::Boolean;
    case 5:  return FieldType::Smallint;
    case 6:  return (byte_a8 == 0x1D) ? FieldType::AutoInc : FieldType::Integer;
    case 7:  return (byte_250 == 0x0A) ? FieldType::Currency : FieldType::Float;
    case 9:  return FieldType::Bytes;
    case 10: return FieldType::Time;
    case 11: return FieldType::DateTime;
    case 15: return FieldType::VarBytes;
    case 18: return FieldType::Largeint;
    default: return FieldType::Unknown;
    }
}

namespace {

constexpr uint8_t BLOCK_MARKER[3] = {0x03, 0x00, 0x00};

// Find the first column block: marker + ord=1 (LE u16) + non-zero namelen.
size_t find_first_block_start(const uint8_t *payload, size_t len) {
    const uint8_t needle[5] = {0x03, 0x00, 0x00, 0x01, 0x00};
    for (size_t i = 0; i + 6 <= len; ++i) {
        if (std::memcmp(payload + i, needle, 5) == 0 && payload[i + 5] > 0) {
            return i;
        }
    }
    throw SchemaError("no schema block marker found in response");
}

// Parse one 772-byte block. Returns false if the leading marker doesn't
// match (signals "no more blocks here").
bool parse_one_block(const uint8_t *block, Column &out) {
    if (std::memcmp(block, BLOCK_MARKER, 3) != 0) return false;
    uint16_t ord = static_cast<uint16_t>(block[3]) | (static_cast<uint16_t>(block[4]) << 8);
    size_t namelen = block[5];
    if (namelen == 0 || 6 + namelen > SCHEMA_BLOCK_STRIDE) return false;
    out.ord = ord;
    out.name.assign(reinterpret_cast<const char *>(block + 6), namelen);
    // 12-byte column descriptor at +0xA7. Layout per DBISAM-PROTOCOL.md §4:
    //   +0  sub          ftType code
    //   +2  decl         declared length
    //   +5  max          on-disk storage width
    //   +8  row_offset   u16 LE — byte offset within the record
    const uint8_t *meta = block + 0xA7;
    out.sub_raw = meta[0];
    out.byte_a8_raw = block[0xA8];
    out.byte_250_raw = block[0x250];
    out.decl = meta[2];
    out.max = meta[5];
    out.row_offset = static_cast<uint16_t>(meta[8]) | (static_cast<uint16_t>(meta[9]) << 8);
    out.field_type = field_type_from_sub(out.sub_raw, out.byte_a8_raw, out.byte_250_raw);
    return true;
}

} // namespace

std::pair<std::vector<Column>, size_t> parse_schema(const uint8_t *payload, size_t len) {
    size_t first = find_first_block_start(payload, len);
    std::vector<Column> columns;
    size_t off = first;
    uint16_t expected_ord = 1;
    while (off + SCHEMA_BLOCK_STRIDE <= len) {
        Column col;
        if (!parse_one_block(payload + off, col)) break;
        if (col.ord != expected_ord) break;
        columns.push_back(std::move(col));
        ++expected_ord;
        off += SCHEMA_BLOCK_STRIDE;
    }
    if (columns.empty()) {
        throw SchemaError("schema parser found no columns");
    }
    return {std::move(columns), off};
}

} // namespace dbisam
