#include "dbisam/row.hpp"

#include <cmath>
#include <cstring>

namespace dbisam {

namespace {

uint16_t read_u16_le(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_u32_le(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int16_t read_i16_le(const uint8_t *p) {
    return static_cast<int16_t>(read_u16_le(p));
}

int32_t read_i32_le(const uint8_t *p) {
    return static_cast<int32_t>(read_u32_le(p));
}

int64_t read_i64_le(const uint8_t *p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<int64_t>(u);
}

double read_f64_le(const uint8_t *p) {
    uint64_t u = static_cast<uint64_t>(read_i64_le(p));
    double d;
    std::memcpy(&d, &u, sizeof(d));
    return d;
}

// Date32: days since 1970-01-01. DBISAM Date is days since 0001-01-01
// (proleptic Gregorian). Difference = days from 0001-01-01 to 1970-01-01.
// 1969 full years × 365 + leap-day count from 0001-01-01 through 1969-12-31.
// Calculation: 1969*365 + 477 (leaps) = 718685. Verified against
// Python `(date(1970,1,1) - date(1,1,1)).days = 719162`. Wait — the
// proleptic Gregorian calendar with 0001-01-01 as day 1 has Python's
// .toordinal() returning 1 for that date, and 719163 for 1970-01-01;
// `delta = 719162`. DBISAM uses "days since 0001-01-01" with that day
// being day 0 (so add 1 to get Python ordinal). Net offset: subtract
// 719162 from DBISAM days to get Date32. Numbers above off by ±1 are
// caught by the live test.
constexpr int32_t DBISAM_DATE_TO_EPOCH = 719162;

constexpr int64_t MICROS_PER_DAY = 86'400'000'000LL;

// Delphi TDateTime epoch is 1899-12-30. Days from 1899-12-30 to
// 1970-01-01 = 25569 (well-known constant).
constexpr int64_t DELPHI_EPOCH_TO_UNIX_DAYS = 25569;

} // namespace

static CellValue decode_field(const Column &c, const uint8_t *bytes, size_t value_len);

std::vector<CellValue> decode_record(const uint8_t *record, size_t len,
                                     const std::vector<Column> &columns) {
    if (columns.empty()) {
        throw RowError("decode_record: empty schema");
    }
    size_t first_offset = columns.front().row_offset;
    std::vector<CellValue> out;
    out.reserve(columns.size());
    for (const auto &c : columns) {
        size_t null_pos = static_cast<size_t>(c.row_offset) - first_offset;
        size_t value_start = null_pos + 1;
        if (null_pos >= len) {
            throw RowError("row truncated; column " + c.name + " null-flag at " +
                           std::to_string(null_pos) + " past end (len " +
                           std::to_string(len) + ")");
        }
        uint8_t null_flag = record[null_pos];
        if (null_flag == 0) {
            out.emplace_back(NullValue{});
            continue;
        }
        if (null_flag != 1) {
            throw RowError("bad null-indicator 0x" + std::to_string(null_flag) +
                           " for column " + c.name);
        }
        size_t value_len = c.max;
        if (value_start + value_len > len) {
            throw RowError("row truncated within column " + c.name +
                           ": need " + std::to_string(value_len) +
                           " bytes, have " + std::to_string(len - value_start));
        }
        out.push_back(decode_field(c, record + value_start, value_len));
    }
    return out;
}

static CellValue decode_field(const Column &c, const uint8_t *bytes, size_t value_len) {
    using FT = FieldType;
    switch (c.field_type) {
    case FT::Calculated:
        return NullValue{};

    case FT::String: {
        // ASCII chars, null-terminated within `max` bytes. Trailing
        // bytes after first 0x00 are zero-padding.
        size_t end = value_len;
        for (size_t i = 0; i < value_len; ++i) {
            if (bytes[i] == 0) { end = i; break; }
        }
        // Production note: DBISAM ftString is Windows-1252, not UTF-8.
        // For now we pass bytes through verbatim — Phase 6 can layer
        // text-decoding when wiring up the DuckDB VARCHAR vector.
        return std::string(reinterpret_cast<const char *>(bytes), end);
    }

    case FT::Date: {
        // 4-byte LE u32, days since 0001-01-01.
        if (value_len < 4) return NullValue{};
        uint32_t days = read_u32_le(bytes);
        int64_t shifted = static_cast<int64_t>(days) - DBISAM_DATE_TO_EPOCH;
        if (shifted < INT32_MIN || shifted > INT32_MAX) return NullValue{};
        return static_cast<int32_t>(shifted);
    }

    case FT::DateTime: {
        // 8-byte LE binary64, days since 1899-12-30 (Delphi TDateTime).
        if (value_len < 8) return NullValue{};
        double serial = read_f64_le(bytes);
        if (!std::isfinite(serial)) return NullValue{};
        double whole_d = std::trunc(serial);
        if (std::fabs(whole_d) > 1e8) return NullValue{};
        int64_t whole_days = static_cast<int64_t>(whole_d);
        double frac = serial - whole_d;
        int64_t day_micros = static_cast<int64_t>(std::llround(std::fabs(frac) * 86'400'000'000.0));
        // Compose: (whole_days from Delphi epoch → Unix epoch) * micros/day + day_micros
        int64_t unix_days = whole_days - DELPHI_EPOCH_TO_UNIX_DAYS;
        // Overflow guard.
        if (std::abs(unix_days) > (1LL << 40)) return NullValue{};
        return unix_days * MICROS_PER_DAY + day_micros;
    }

    case FT::Time: {
        // 4-byte LE u32, milliseconds since midnight.
        if (value_len < 4) return NullValue{};
        uint32_t ms = read_u32_le(bytes);
        return static_cast<int64_t>(ms) * 1000LL;
    }

    case FT::Integer:
    case FT::AutoInc:
        if (value_len < 4) return NullValue{};
        return static_cast<int32_t>(read_i32_le(bytes));

    case FT::Smallint:
        if (value_len < 2) return NullValue{};
        return static_cast<int32_t>(read_i16_le(bytes));

    case FT::Largeint:
        if (value_len < 8) return NullValue{};
        return read_i64_le(bytes);

    case FT::Boolean: {
        // 2-byte WordBool: FFFF=true, 0000=false. Treat anything non-zero
        // as true.
        if (value_len < 2) return NullValue{};
        return read_u16_le(bytes) != 0;
    }

    case FT::Float:
        if (value_len < 8) return NullValue{};
        return read_f64_le(bytes);

    case FT::Currency: {
        // 8-byte LE i64, scaled by 10000.
        if (value_len < 8) return NullValue{};
        return static_cast<double>(read_i64_le(bytes)) / 10'000.0;
    }

    case FT::Blob:
    case FT::Memo:
    case FT::Graphic: {
        // 8-byte handle; caller fetches via 0x0280 in Phase 7.
        if (value_len < 8) return NullValue{};
        BlobHandle h;
        std::memcpy(h.bytes.data(), bytes, 8);
        return h;
    }

    case FT::Bytes:
    case FT::VarBytes:
        return std::vector<uint8_t>(bytes, bytes + value_len);

    case FT::Unknown:
        throw RowError("unsupported field type for column " + c.name +
                       " (sub=" + std::to_string(c.sub_raw) + ")");
    }
    return NullValue{};
}

} // namespace dbisam
