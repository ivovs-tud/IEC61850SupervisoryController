#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>


#if defined(PLATFORM_WINDOWS)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <basetsd.h>

#pragma comment(lib, "ws2_32.lib")

using socket_t = SOCKET;
constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
using socklen_t = int;
using ssize_t = SSIZE_T;
using pollfd = WSAPOLLFD;

inline bool socket_init()
{
    WSADATA wsa_data;
    return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
}

inline void socket_cleanup()
{
    WSACleanup();
}

inline void socket_close(socket_t fd)
{
    if (fd != INVALID_SOCKET_FD) {
        closesocket(fd);
    }
}

inline bool socket_set_nonblocking(socket_t fd)
{
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
}

inline ssize_t socket_read(socket_t fd, void* buf, size_t len)
{
    return static_cast<ssize_t>(recv(fd, static_cast<char*>(buf), static_cast<int>(len), 0));
}

inline bool socket_would_block()
{
    const int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK;
}

inline const char* socket_strerror()
{
    thread_local char msg_buffer[256] = {0};
    const DWORD err = static_cast<DWORD>(WSAGetLastError());
    const DWORD len = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        msg_buffer,
        static_cast<DWORD>(sizeof(msg_buffer)),
        nullptr);

    if (len == 0) {
        std::snprintf(msg_buffer, sizeof(msg_buffer), "Winsock error %lu", static_cast<unsigned long>(err));
    } else {
        while (std::strlen(msg_buffer) > 0 &&
               (msg_buffer[std::strlen(msg_buffer) - 1] == '\n' || msg_buffer[std::strlen(msg_buffer) - 1] == '\r')) {
            msg_buffer[std::strlen(msg_buffer) - 1] = '\0';
        }
    }

    return msg_buffer;
}

inline int socket_poll(pollfd* pfds, std::size_t nfds, int timeout_ms)
{
    return WSAPoll(pfds, static_cast<ULONG>(nfds), timeout_ms);
}

#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using socket_t = int;
constexpr socket_t INVALID_SOCKET_FD = -1;

inline bool socket_init()
{
    return true;
}

inline void socket_cleanup() {}

inline void socket_close(socket_t fd)
{
    if (fd != INVALID_SOCKET_FD) {
        close(fd);
    }
}

inline bool socket_set_nonblocking(socket_t fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline ssize_t socket_read(socket_t fd, void* buf, size_t len)
{
    return read(fd, buf, len);
}

inline bool socket_would_block()
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

inline const char* socket_strerror()
{
    return strerror(errno);
}

inline int socket_poll(pollfd* pfds, std::size_t nfds, int timeout_ms)
{
    return poll(pfds, nfds, timeout_ms);
}

#endif