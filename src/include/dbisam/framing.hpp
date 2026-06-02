// TCP framing for the DBISAM wire protocol (DBISAM-PROTOCOL.md §2).
//
//   <16-byte session GUID> <u32 LE total_len> <body>
// where total_len = 20 + len(body), aligned up to a multiple of 8 with
// zero-pad bytes appended to the body (ANSWERS-TO-DEREK-4.md:
// BlockOffset(size, 8) = (size + 7) & ~7).
//
// Port of mrsflow-cli/src/exportmaster/framing.rs.

#pragma once

#include "dbisam/sys_socket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dbisam {

class IoError : public std::runtime_error {
public:
    explicit IoError(const std::string &msg) : std::runtime_error(msg) {}
};

// 16-byte session GUID — fixed protocol marker copied from PoC captures.
// Hypothesis: client-runtime constant baked into dbsys.exe. If a real
// DBISAM server ever rejects it, we'd need to negotiate.
extern const std::array<uint8_t, 16> GUID;

// Wrap `body` in the framing envelope. Total length is 8-byte aligned;
// any padding is zero bytes appended to the body before sending.
std::vector<uint8_t> wrap(const uint8_t *body, size_t body_len);

inline std::vector<uint8_t> wrap(const std::vector<uint8_t> &body) {
    return wrap(body.data(), body.size());
}

// Raw zlib deflate / inflate using miniz at compression level 1
// (matches `78 01` zlib header observed from dbsys.exe).
std::vector<uint8_t> deflate(const uint8_t *data, size_t len);
std::vector<uint8_t> inflate(const uint8_t *data, size_t len);

// Synchronous transport bound to a socket. Owns the socket lifetime.
class Transport {
public:
    explicit Transport(socket_t fd) : fd_(fd) {}
    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;
    Transport(Transport &&other) noexcept : fd_(other.fd_) { other.fd_ = DBISAM_INVALID_SOCKET; }
    ~Transport();

    socket_t fd() const { return fd_; }

    // Send a framed message and receive one framed reply. Returns the
    // reply body without the 20-byte envelope. DBISAM is strict
    // request/response — call repeatedly on the same Transport.
    std::vector<uint8_t> send_recv(const uint8_t *body, size_t len);

    inline std::vector<uint8_t> send_recv(const std::vector<uint8_t> &body) {
        return send_recv(body.data(), body.size());
    }

    // Same as send_recv but with per-body zlib compression layer
    // (DBISAM "RemoteCompression" mode). Caller still passes the
    // uncompressed body; compression of inner section happens here.
    std::vector<uint8_t> send_recv_compressed(const uint8_t *body, size_t len);

    inline std::vector<uint8_t> send_recv_compressed(const std::vector<uint8_t> &body) {
        return send_recv_compressed(body.data(), body.size());
    }

    inline std::vector<uint8_t> send_recv_auto(const std::vector<uint8_t> &body,
                                               bool compression) {
        return compression ? send_recv_compressed(body) : send_recv(body);
    }

private:
    socket_t fd_;

    void write_all(const uint8_t *data, size_t len);
    void read_exact(uint8_t *buf, size_t len);
    std::vector<uint8_t> recv_msg();
};

// Open a TCP connection with sensible timeouts (10s connect, 30s r/w).
// `host` may be a hostname (resolved via getaddrinfo) or an IP literal.
Transport connect(const std::string &host, uint16_t port);

} // namespace dbisam
