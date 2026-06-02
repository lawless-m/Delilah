#include "dbisam/framing.hpp"

#include "miniz.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace dbisam {

const std::array<uint8_t, 16> GUID = {
    0x8A, 0xBE, 0x8E, 0x59, 0x23, 0x64, 0xCB, 0x40,
    0x3D, 0x71, 0xD2, 0xE3, 0xBC, 0x64, 0xD0, 0x01,
};

static constexpr uint8_t SESSION_STATE_FLAG = 0x5A;

std::vector<uint8_t> wrap(const uint8_t *body, size_t body_len) {
    size_t raw_total = 20 + body_len;
    size_t aligned_total = (raw_total + 7) & ~static_cast<size_t>(7);
    size_t pad = aligned_total - raw_total;
    std::vector<uint8_t> out;
    out.reserve(aligned_total);
    out.insert(out.end(), GUID.begin(), GUID.end());
    uint32_t total_le = static_cast<uint32_t>(aligned_total);
    out.push_back(static_cast<uint8_t>(total_le & 0xff));
    out.push_back(static_cast<uint8_t>((total_le >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((total_le >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((total_le >> 24) & 0xff));
    out.insert(out.end(), body, body + body_len);
    out.insert(out.end(), pad, 0);
    return out;
}

std::vector<uint8_t> deflate(const uint8_t *data, size_t len) {
    duckdb_miniz::mz_stream s{};
    int ret = duckdb_miniz::mz_deflateInit(&s, 1); // level 1 = fast, matches dbsys.exe
    if (ret != duckdb_miniz::MZ_OK) {
        throw IoError("deflateInit failed");
    }
    std::vector<uint8_t> out;
    out.resize(duckdb_miniz::mz_deflateBound(&s, static_cast<duckdb_miniz::mz_ulong>(len)));
    s.next_in = data;
    s.avail_in = static_cast<unsigned int>(len);
    s.next_out = out.data();
    s.avail_out = static_cast<unsigned int>(out.size());
    ret = duckdb_miniz::mz_deflate(&s, duckdb_miniz::MZ_FINISH);
    if (ret != duckdb_miniz::MZ_STREAM_END) {
        duckdb_miniz::mz_deflateEnd(&s);
        throw IoError("deflate failed");
    }
    out.resize(s.total_out);
    duckdb_miniz::mz_deflateEnd(&s);
    return out;
}

std::vector<uint8_t> inflate(const uint8_t *data, size_t len) {
    duckdb_miniz::mz_stream s{};
    int ret = duckdb_miniz::mz_inflateInit(&s);
    if (ret != duckdb_miniz::MZ_OK) {
        throw IoError("inflateInit failed");
    }
    std::vector<uint8_t> out;
    out.reserve(len * 4);
    uint8_t scratch[4096];
    s.next_in = data;
    s.avail_in = static_cast<unsigned int>(len);
    for (;;) {
        s.next_out = scratch;
        s.avail_out = sizeof(scratch);
        ret = duckdb_miniz::mz_inflate(&s, duckdb_miniz::MZ_NO_FLUSH);
        size_t produced = sizeof(scratch) - s.avail_out;
        out.insert(out.end(), scratch, scratch + produced);
        if (ret == duckdb_miniz::MZ_STREAM_END) {
            break;
        }
        if (ret != duckdb_miniz::MZ_OK) {
            duckdb_miniz::mz_inflateEnd(&s);
            throw IoError(std::string("inflate failed: ret=") + std::to_string(ret));
        }
        if (s.avail_in == 0 && produced == 0) {
            duckdb_miniz::mz_inflateEnd(&s);
            throw IoError("inflate stalled (need more input but stream not at end)");
        }
    }
    duckdb_miniz::mz_inflateEnd(&s);
    return out;
}

Transport::~Transport() {
    if (fd_ != DBISAM_INVALID_SOCKET) {
        close_socket(fd_);
    }
}

void Transport::write_all(const uint8_t *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        // ::send takes (const char*) on Windows, (const void*) on POSIX;
        // reinterpret_cast for portability without #ifdef.
        auto n = ::send(fd_,
                        reinterpret_cast<const char *>(data + off),
                        static_cast<int>(len - off), 0);
        if (n == DBISAM_SOCKET_ERROR) {
            if (socket_was_interrupted()) continue;
            throw IoError("send: " + socket_last_error());
        }
        off += static_cast<size_t>(n);
    }
}

void Transport::read_exact(uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        auto n = ::recv(fd_,
                        reinterpret_cast<char *>(buf + off),
                        static_cast<int>(len - off), 0);
        if (n == 0) {
            throw IoError("connection closed (got " + std::to_string(off) + " of " +
                          std::to_string(len) + ")");
        }
        if (n == DBISAM_SOCKET_ERROR) {
            if (socket_was_interrupted()) continue;
            throw IoError("recv: " + socket_last_error());
        }
        off += static_cast<size_t>(n);
    }
}

std::vector<uint8_t> Transport::recv_msg() {
    uint8_t head[20];
    read_exact(head, sizeof(head));
    if (std::memcmp(head, GUID.data(), 16) != 0) {
        throw IoError("unexpected envelope prefix");
    }
    uint32_t total = static_cast<uint32_t>(head[16]) |
                     (static_cast<uint32_t>(head[17]) << 8) |
                     (static_cast<uint32_t>(head[18]) << 16) |
                     (static_cast<uint32_t>(head[19]) << 24);
    if (total < 20) {
        throw IoError("bad total_len in header: " + std::to_string(total));
    }
    std::vector<uint8_t> body(total - 20);
    if (!body.empty()) {
        read_exact(body.data(), body.size());
    }
    return body;
}

std::vector<uint8_t> Transport::send_recv(const uint8_t *body, size_t len) {
    auto pkt = wrap(body, len);
    write_all(pkt.data(), pkt.size());
    return recv_msg();
}

// Body layout per capture analysis of a live RemoteCompression=9 session:
//   body[0]      flag byte. Low bit = "inner section is deflated".
//                Post-Connect bodies use 0x5A | low-bit. Connect (reqcode
//                0x0000) is special: the flag byte encodes the requested
//                compression level (e.g. 0x09 for level 9).
//   body[1..3]   reqcode u16 LE (plaintext header)
//   body[3..7]   inner_len u32 LE (on-wire bytes of inner section)
//   body[7..]    plaintext or zlib-deflated Pack stream
//
// Bodies whose UNCOMPRESSED inner is <= 16 bytes are not deflated
// (overhead would grow them).
std::vector<uint8_t> Transport::send_recv_compressed(const uint8_t *body, size_t len) {
    std::vector<uint8_t> to_send;
    if (len < 7) {
        to_send.assign(body, body + len);
    } else {
        uint16_t reqcode = static_cast<uint16_t>(body[1]) |
                           (static_cast<uint16_t>(body[2]) << 8);
        bool is_connect = (reqcode == 0x0000);
        const uint8_t *inner = body + 7;
        size_t inner_len = len - 7;
        if (inner_len <= 16) {
            to_send.reserve(len);
            to_send.push_back(is_connect ? 0x09 : SESSION_STATE_FLAG);
            to_send.insert(to_send.end(), body + 1, body + len);
        } else {
            auto deflated = deflate(inner, inner_len);
            to_send.reserve(7 + deflated.size());
            to_send.push_back(is_connect ? 0x09 : (SESSION_STATE_FLAG | 0x01));
            to_send.push_back(body[1]);
            to_send.push_back(body[2]);
            uint32_t dl = static_cast<uint32_t>(deflated.size());
            to_send.push_back(static_cast<uint8_t>(dl & 0xff));
            to_send.push_back(static_cast<uint8_t>((dl >> 8) & 0xff));
            to_send.push_back(static_cast<uint8_t>((dl >> 16) & 0xff));
            to_send.push_back(static_cast<uint8_t>((dl >> 24) & 0xff));
            to_send.insert(to_send.end(), deflated.begin(), deflated.end());
        }
    }
    auto pkt = wrap(to_send.data(), to_send.size());
    write_all(pkt.data(), pkt.size());
    auto raw = recv_msg();

    // Receive side: any flag byte with low bit set triggers inflate.
    if (raw.size() < 7 || (raw[0] & 0x01) == 0) {
        if (!raw.empty()) raw[0] = 0x00;
        return raw;
    }
    uint32_t inner_len = static_cast<uint32_t>(raw[3]) |
                         (static_cast<uint32_t>(raw[4]) << 8) |
                         (static_cast<uint32_t>(raw[5]) << 16) |
                         (static_cast<uint32_t>(raw[6]) << 24);
    if (7 + inner_len > raw.size()) {
        throw IoError("compressed body inner_len " + std::to_string(inner_len) +
                      " exceeds available " + std::to_string(raw.size() - 7));
    }
    auto inflated = inflate(raw.data() + 7, inner_len);
    std::vector<uint8_t> out;
    out.reserve(7 + inflated.size());
    out.push_back(0x00);
    out.push_back(raw[1]);
    out.push_back(raw[2]);
    uint32_t il = static_cast<uint32_t>(inflated.size());
    out.push_back(static_cast<uint8_t>(il & 0xff));
    out.push_back(static_cast<uint8_t>((il >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((il >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((il >> 24) & 0xff));
    out.insert(out.end(), inflated.begin(), inflated.end());
    return out;
}

Transport connect(const std::string &host, uint16_t port) {
    ensure_wsa_started();

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0) {
        throw IoError("resolve " + host + ": " + std::string(gai_strerror(rc)));
    }

    // Prefer IPv4. A hostname can resolve to ::1 (IPv6 loopback) ahead of
    // the real IPv4 address — notably on Windows — and a *refused* ::1
    // connect can take ~2s to surface. Since we open a fresh connection
    // per schema probe (one per table), trying a dead ::1 first would add
    // ~2s × table-count to catalog enumeration. Order AF_INET first so the
    // reachable address is attempted before any IPv6 candidate.
    std::vector<addrinfo *> addrs;
    for (auto *ai = res; ai != nullptr; ai = ai->ai_next) addrs.push_back(ai);
    std::stable_sort(addrs.begin(), addrs.end(), [](addrinfo *a, addrinfo *b) {
        return a->ai_family == AF_INET && b->ai_family != AF_INET;
    });

    socket_t fd = DBISAM_INVALID_SOCKET;
    std::string last_err;
    for (auto *ai : addrs) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == DBISAM_INVALID_SOCKET) {
            last_err = socket_last_error();
            continue;
        }
        // 10s connect timeout via non-blocking + select.
        set_socket_nonblocking(fd, true);
        int crc = ::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool ok = false;
        if (crc == 0) {
            ok = true;
        } else if (socket_would_block()) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            timeval tv;
            tv.tv_sec = 10;
            tv.tv_usec = 0;
            // POSIX: nfds is highest fd + 1. Windows: nfds is ignored
            // (FD_SETSIZE check happens internally). Passing fd+1 works
            // on POSIX; on Windows the value is harmless.
            int nfds = static_cast<int>(fd) + 1;
            int sel = ::select(nfds, nullptr, &wfds, nullptr, &tv);
            if (sel > 0) {
                int so_err = 0;
                socklen_t so_len = sizeof(so_err);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                 reinterpret_cast<char *>(&so_err), &so_len) == 0 && so_err == 0) {
                    ok = true;
                } else {
                    last_err = so_err ? std::to_string(so_err) : socket_last_error();
                }
            } else if (sel == 0) {
                last_err = "connect timed out";
            } else {
                last_err = socket_last_error();
            }
        } else {
            last_err = socket_last_error();
        }
        set_socket_nonblocking(fd, false);
        if (ok) {
            set_socket_timeout(fd, SO_RCVTIMEO, 30);
            set_socket_timeout(fd, SO_SNDTIMEO, 30);
            ::freeaddrinfo(res);
            return Transport(fd);
        }
        close_socket(fd);
        fd = DBISAM_INVALID_SOCKET;
    }
    ::freeaddrinfo(res);
    throw IoError("connect " + host + ":" + std::to_string(port) + ": " +
                  (last_err.empty() ? "no addresses resolved" : last_err));
}

} // namespace dbisam
