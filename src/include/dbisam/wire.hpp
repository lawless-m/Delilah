// Wire-strict walker for the universal `<u32 LE length><payload>` framing
// rule (DBISAM-PROTOCOL.md §6c, confirmed by disassembly of
// TDataSession.Unpack at RVA 0x07752C). Every higher-level structure
// (cursor info, row data, field defs) is a sequence of these units.
//
// Port of mrsflow-cli/src/exportmaster/wire.rs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace dbisam {

class WireError : public std::runtime_error {
public:
    explicit WireError(const std::string &msg) : std::runtime_error(msg) {}
};

class Walker {
public:
    Walker(const uint8_t *buf, size_t len, size_t start = 0)
        : buf_(buf), len_(len), pos_(start) {}

    size_t position() const { return pos_; }
    const uint8_t *buf() const { return buf_; }
    size_t buf_len() const { return len_; }
    void seek(size_t pos) { pos_ = pos; }

    // Read the next <u32 LE length><payload> unit. Returns true and sets
    // out_ptr/out_len if a unit was read; returns false on clean exhaustion.
    // Throws WireError if the length prefix would overrun the buffer.
    bool next_unit(const uint8_t *&out_ptr, size_t &out_len);

private:
    const uint8_t *buf_;
    size_t len_;
    size_t pos_;
};

} // namespace dbisam
