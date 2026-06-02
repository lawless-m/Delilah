#include "dbisam/sys_socket.hpp"

#include <cstring>
#include <mutex>

namespace dbisam {

#ifdef _WIN32

void ensure_wsa_started() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA d;
        // WinSock 2.2 — first available since Windows 95/NT 3.51.
        (void)::WSAStartup(MAKEWORD(2, 2), &d);
        // Deliberately leak: we want WSACleanup to run at process exit
        // only, and the extension's unload path doesn't reliably fire.
    });
}

int close_socket(socket_t s) {
    return ::closesocket(s);
}

int set_socket_nonblocking(socket_t s, bool nonblocking) {
    u_long mode = nonblocking ? 1 : 0;
    return ::ioctlsocket(s, FIONBIO, &mode);
}

int set_socket_timeout(socket_t s, int option, int seconds) {
    DWORD ms = static_cast<DWORD>(seconds) * 1000u;
    return ::setsockopt(s, SOL_SOCKET, option,
                        reinterpret_cast<const char *>(&ms), sizeof(ms));
}

std::string socket_last_error() {
    int e = ::WSAGetLastError();
    char *msg = nullptr;
    DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(e),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&msg), 0, nullptr);
    std::string s = (n > 0 && msg) ? std::string(msg, n)
                                   : std::string("WSA error " + std::to_string(e));
    if (msg) ::LocalFree(msg);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

bool socket_would_block() {
    int e = ::WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}

bool socket_was_interrupted() {
    return ::WSAGetLastError() == WSAEINTR;
}

#else // POSIX

void ensure_wsa_started() {
    // no-op on POSIX
}

int close_socket(socket_t s) {
    return ::close(s);
}

int set_socket_nonblocking(socket_t s, bool nonblocking) {
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(s, F_SETFL, flags);
}

int set_socket_timeout(socket_t s, int option, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    return ::setsockopt(s, SOL_SOCKET, option, &tv, sizeof(tv));
}

std::string socket_last_error() {
    return std::string(std::strerror(errno));
}

bool socket_would_block() {
    return errno == EWOULDBLOCK || errno == EINPROGRESS;
}

bool socket_was_interrupted() {
    return errno == EINTR;
}

#endif

} // namespace dbisam
