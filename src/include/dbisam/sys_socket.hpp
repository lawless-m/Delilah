// Thin POSIX/Winsock socket abstraction so framing.cpp can stay one
// file. Wraps the small set of differences that matter for our use:
// fd type, close call, non-blocking mode, errno semantics, timeout
// setting, and Winsock init/cleanup.

#pragma once

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    // Link with -lws2_32 (CMake handles this for MSVC/MinGW).
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <unistd.h>
#endif

#include <string>

namespace dbisam {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t DBISAM_INVALID_SOCKET = INVALID_SOCKET;
inline constexpr int DBISAM_SOCKET_ERROR = SOCKET_ERROR;
#else
using socket_t = int;
inline constexpr socket_t DBISAM_INVALID_SOCKET = -1;
inline constexpr int DBISAM_SOCKET_ERROR = -1;
#endif

// One-time Winsock initialization. POSIX no-op. Safe to call multiple
// times; uses std::call_once internally.
void ensure_wsa_started();

// close()/closesocket() wrapper.
int close_socket(socket_t s);

// Toggle non-blocking mode on a connected socket.
int set_socket_nonblocking(socket_t s, bool nonblocking);

// Apply SO_RCVTIMEO or SO_SNDTIMEO. `option` should be SO_RCVTIMEO or
// SO_SNDTIMEO. POSIX uses struct timeval; Windows uses DWORD ms.
int set_socket_timeout(socket_t s, int option, int seconds);

// Human-readable form of the most recent socket-layer error.
std::string socket_last_error();

// "Would have blocked" check after a connect/send/recv returned with
// the conventional error sentinel. EWOULDBLOCK/EINPROGRESS on POSIX,
// WSAEWOULDBLOCK/WSAEINPROGRESS on Windows.
bool socket_would_block();

// Interrupted-by-signal check (EINTR / WSAEINTR). Mostly a no-op on
// Windows in practice but kept for symmetry.
bool socket_was_interrupted();

// Wait until `s` becomes writable (a non-blocking connect completing)
// or `timeout_ms` elapses. Returns >0 on writable, 0 on timeout, <0 on
// error. poll() on POSIX (select() is UB for fd >= FD_SETSIZE);
// select() on Windows where fd_set is an array of SOCKETs.
int socket_wait_writable(socket_t s, int timeout_ms);

} // namespace dbisam
