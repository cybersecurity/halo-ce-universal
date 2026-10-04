/* The shared Winsock adapter with Darwin sockaddr layout translation. */
#ifndef HALO_MACOS
#define HALO_MACOS 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
static struct sockaddr_storage input_address(const struct sockaddr *source, socklen_t n) {
    struct sockaddr_storage result = {0};
    if (n > sizeof(result))
        n = sizeof(result);
    memcpy(&result, source, n);
    uint16_t family;
    memcpy(&family, source, 2);
    result.ss_len = (uint8_t)n;
    result.ss_family = (uint8_t)family;
    return result;
}
static void output_address(struct sockaddr *address) {
    uint16_t family = address->sa_family;
    memcpy(address, &family, 2);
}
static int darwin_bind(int fd, const struct sockaddr *a, socklen_t n) {
    struct sockaddr_storage s = input_address(a, n);
    return bind(fd, (void *)&s, n);
}
static int darwin_connect(int fd, const struct sockaddr *a, socklen_t n) {
    struct sockaddr_storage s = input_address(a, n);
    return connect(fd, (void *)&s, n);
}
static ssize_t darwin_sendto(int fd, const void *p, size_t n, int f, const struct sockaddr *a,
                             socklen_t an) {
    /* A connected socket can omit its destination. Preserve NULL: a
       non-NULL zero-length sockaddr makes Darwin reject the send. */
    if (!a)
        return sendto(fd, p, n, f, NULL, an);
    struct sockaddr_storage s = input_address(a, an);
    ssize_t r = sendto(fd, p, n, f, (void *)&s, an);
    if (r < 0 && errno == EISCONN) {
        /* Halo supplies a destination even after connecting its gameplay UDP
           socket. Darwin rejects that Winsock/Linux pattern. Use send() only
           when the explicit destination is the connected datagram peer. */
        struct sockaddr_storage peer;
        socklen_t peer_length = sizeof(peer), type_length = sizeof(int);
        int type;
        if (s.ss_family == AF_INET && an >= sizeof(struct sockaddr_in) &&
            getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) == 0 &&
            type == SOCK_DGRAM && getpeername(fd, (void *)&peer, &peer_length) == 0 &&
            peer.ss_family == AF_INET && peer_length >= sizeof(struct sockaddr_in)) {
            const struct sockaddr_in *destination = (const void *)&s;
            const struct sockaddr_in *connected = (const void *)&peer;
            if (destination->sin_port == connected->sin_port &&
                destination->sin_addr.s_addr == connected->sin_addr.s_addr)
                return send(fd, p, n, f);
        }
        errno = EISCONN;
    }
    return r;
}
static ssize_t darwin_recvmsg(int fd, struct msghdr *message, int flags) {
    ssize_t r = recvmsg(fd, message, flags);
    /* Upstream uses recvmsg to detect truncated UDP packets. Its sender
       address still needs the game's two-byte family instead of sa_len. */
    if (r >= 0 && message->msg_name && message->msg_namelen >= sizeof(uint16_t))
        output_address(message->msg_name);
    return r;
}
static int darwin_accept(int fd, struct sockaddr *a, socklen_t *n) {
    int r = accept(fd, a, n);
    if (r >= 0 && a)
        output_address(a);
    return r;
}
static int darwin_getsockname(int fd, struct sockaddr *a, socklen_t *n) {
    int r = getsockname(fd, a, n);
    if (!r && a)
        output_address(a);
    return r;
}
static int darwin_getpeername(int fd, struct sockaddr *a, socklen_t *n) {
    int r = getpeername(fd, a, n);
    if (!r && a)
        output_address(a);
    return r;
}
static ssize_t darwin_getrandom(void *p, size_t n, unsigned f) {
    (void)f;
    arc4random_buf(p, n);
    return (ssize_t)n;
}
static int darwin_socket(int family, int type, int protocol) {
    int fd = socket(family, type & ~0x80000, protocol);
    if (fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        /* PR #22: Darwin uses SO_NOSIGPIPE in place of MSG_NOSIGNAL. */
        int enabled = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
    }
    return fd;
}
static int darwin_accept4(int fd, struct sockaddr *a, socklen_t *n, int flags) {
    (void)flags;
    int r = darwin_accept(fd, a, n);
    if (r >= 0) {
        fcntl(r, F_SETFD, FD_CLOEXEC);
        int enabled = 1;
        setsockopt(r, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
    }
    return r;
}
#define SOCK_CLOEXEC 0x80000
#define socket darwin_socket
#define accept4 darwin_accept4
#define bind darwin_bind
#define connect darwin_connect
#define sendto darwin_sendto
#define recvmsg darwin_recvmsg
#define accept darwin_accept
#define getsockname darwin_getsockname
#define getpeername darwin_getpeername
#define getrandom darwin_getrandom
#include "port/linux/src/posix_net.c"
