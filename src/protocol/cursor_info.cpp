#include "dbisam/cursor_info.hpp"

#include <cstring>
#include <string>

namespace dbisam {

namespace {

uint32_t read_u32_le(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t expect_u32(Walker &w, const char *name) {
    const uint8_t *p; size_t n;
    if (!w.next_unit(p, n)) {
        throw WireError(std::string("cursor-info: missing field ") + name);
    }
    if (n != 4) {
        throw WireError(std::string("cursor-info: field ") + name +
                        " expected 4 bytes, got " + std::to_string(n));
    }
    return read_u32_le(p);
}

std::array<uint8_t, 8> expect_8(Walker &w, const char *name) {
    const uint8_t *p; size_t n;
    if (!w.next_unit(p, n)) {
        throw WireError(std::string("cursor-info: missing field ") + name);
    }
    if (n != 8) {
        throw WireError(std::string("cursor-info: field ") + name +
                        " expected 8 bytes, got " + std::to_string(n));
    }
    std::array<uint8_t, 8> out;
    std::memcpy(out.data(), p, 8);
    return out;
}

std::vector<uint8_t> expect_bytes(Walker &w, const char *name) {
    const uint8_t *p; size_t n;
    if (!w.next_unit(p, n)) {
        throw WireError(std::string("cursor-info: missing field ") + name);
    }
    return std::vector<uint8_t>(p, p + n);
}

uint8_t expect_byte(Walker &w, const char *name) {
    const uint8_t *p; size_t n;
    if (!w.next_unit(p, n)) {
        throw WireError(std::string("cursor-info: missing field ") + name);
    }
    if (n != 1) {
        throw WireError(std::string("cursor-info: field ") + name +
                        " expected 1 byte, got " + std::to_string(n));
    }
    return p[0];
}

} // namespace

CursorInfo CursorInfo::read(Walker &w) {
    CursorInfo ci;
    ci.record_number          = expect_u32(w, "RecordNumber");
    ci.physical_record_number = expect_u32(w, "PhysicalRecordNumber");
    ci.record_count           = expect_u32(w, "RecordCount");
    ci.physical_records_used  = expect_u32(w, "PhysicalRecordsUsed");
    ci.last_auto_inc_id       = expect_u32(w, "LastAutoIncID");
    ci.last_updated           = expect_8(w, "LastUpdated");
    ci.total_record_count     = expect_u32(w, "TotalRecordCount");
    ci.bookmark               = expect_bytes(w, "Bookmark");
    ci.flag_60e               = expect_byte(w, "flag_60E");
    ci.flag_60d               = expect_byte(w, "flag_60D");
    ci.end_offset             = w.position();
    return ci;
}

} // namespace dbisam
