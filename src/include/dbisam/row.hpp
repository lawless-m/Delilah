// Row parser — decodes record bytes into per-column values, dispatching
// on FieldType from schema.hpp. See DBISAM-PROTOCOL.md §6b.
//
// Wire layout per row record (protocol §4):
//   +0    9 bytes    header (8-byte LE TDateTime at +1..+8, byte +0 = type/flag)
//   +9    16 bytes   MD5 hash of record[25..end]
//   +25   N bytes    field data
// Per field:
//   +0    1 byte     null-indicator: 0x00 = NULL, 0x01 = not null
//   +1    max bytes  value data, format depends on FieldType
//
// Port of mrsflow-cli/src/exportmaster/row.rs (decode_record only; the
// Arrow ColumnBuilders machinery is DuckDB-specific and lives elsewhere).

#pragma once

#include "dbisam/schema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace dbisam {

class RowError : public std::runtime_error {
public:
    explicit RowError(const std::string &msg) : std::runtime_error(msg) {}
};

constexpr size_t RECORD_HEADER_LEN = 25;

struct NullValue {};
struct BlobHandle { std::array<uint8_t, 8> bytes; };

// One decoded cell. The variant carries only the *storage type*; the
// schema's FieldType decides how Phase 6 interprets it (e.g. int32 may
// be either an Integer column or a Date32 day-count).
//
// Native units chosen to match DuckDB's internal representations:
//   Date      → int32_t days since 1970-01-01 (Date32)
//   Timestamp → int64_t microseconds since 1970-01-01 UTC
//   Time      → int64_t microseconds since midnight
using CellValue = std::variant<
    NullValue,
    std::string,          // String / Memo (after blob resolve)
    int32_t,              // Integer / AutoInc / Smallint / Date
    int64_t,              // Largeint / Timestamp / Time
    double,               // Float / Currency
    bool,                 // Boolean
    std::vector<uint8_t>, // Bytes / VarBytes / resolved Blob
    BlobHandle            // unresolved 8-byte blob handle
>;

// Decode one record into per-column CellValues.
//
// `record` points to the first column's null-flag byte on the wire
// (i.e. the on-disk 25-byte header has already been skipped by the
// caller; record[0] is columns[0].row_offset on the wire is at offset 0).
std::vector<CellValue> decode_record(const uint8_t *record, size_t len,
                                     const std::vector<Column> &columns);

} // namespace dbisam
