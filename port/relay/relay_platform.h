#ifndef HALO_RELAY_PLATFORM_H
#define HALO_RELAY_PLATFORM_H
/* Only the native networking layer is linked: no XDK, renderer or game data. */
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
_Static_assert(sizeof(unsigned long) == 4, "Build the relay with the native 32-bit ABI (-m32)");
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
/* posix_net translates Winsock socket-option values, as for the game. */
#undef SOL_SOCKET
#undef SO_SNDBUF
#undef SO_RCVBUF
#define SOL_SOCKET 0xffff
#define SO_SNDBUF 0x1001
#define SO_RCVBUF 0x1002
unsigned long GetTickCount(void);
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
int relay_candidate_allowed(unsigned long address, unsigned short port);
#endif
