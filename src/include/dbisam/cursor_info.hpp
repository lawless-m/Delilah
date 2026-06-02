// Cursor info: the 10-field structure the server writes after each
// query/fetch (TServerThread.PackCursorInfo, RVA 0x49810). See
// DBISAM-PROTOCOL.md §6d.
//
// Each field is one wire unit (<u32 LE length><payload>). The 8th field
// — the bookmark — is the opaque cursor position the client must echo
// verbatim into the next fetch. Different cursor modes produce
// different bookmark sizes; client doesn't interpret it, just copies.
//
// Port of mrsflow-cli/src/exportmaster/cursor_info.rs.

#pragma once

#include "dbisam/wire.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace dbisam {

struct CursorInfo {
    uint32_t record_number;
    uint32_t physical_record_number;
    uint32_t record_count;
    uint32_t physical_records_used;
    uint32_t last_auto_inc_id;
    std::array<uint8_t, 8> last_updated;     // raw TDateTime — caller decodes
    uint32_t total_record_count;
    // THE opaque cursor position. Echo verbatim into the next fetch.
    std::vector<uint8_t> bookmark;
    uint8_t flag_60e;
    uint8_t flag_60d;
    // Position past the last cursor-info unit. Row data starts here.
    size_t end_offset;

    // Read the 10 cursor-info units starting at `walker`'s current
    // position; advances the walker past them.
    static CursorInfo read(Walker &walker);
};

} // namespace dbisam
