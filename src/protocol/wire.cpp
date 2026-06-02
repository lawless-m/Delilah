#include "dbisam/wire.hpp"

namespace dbisam {

bool Walker::next_unit(const uint8_t *&out_ptr, size_t &out_len) {
    if (pos_ >= len_) {
        return false;
    }
    if (pos_ + 4 > len_) {
        throw WireError("wire walker ran past end at " + std::to_string(pos_) +
                        " (need 4-byte length, have " + std::to_string(len_ - pos_) + ")");
    }
    uint32_t length = static_cast<uint32_t>(buf_[pos_]) |
                      (static_cast<uint32_t>(buf_[pos_ + 1]) << 8) |
                      (static_cast<uint32_t>(buf_[pos_ + 2]) << 16) |
                      (static_cast<uint32_t>(buf_[pos_ + 3]) << 24);
    size_t start = pos_ + 4;
    size_t end = start + length;
    if (end > len_) {
        throw WireError("wire walker length " + std::to_string(length) + " at pos " +
                        std::to_string(pos_) + " would overrun (buf len " +
                        std::to_string(len_) + ")");
    }
    pos_ = end;
    out_ptr = buf_ + start;
    out_len = length;
    return true;
}

} // namespace dbisam
