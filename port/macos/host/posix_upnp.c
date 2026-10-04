/* UPnP uses the host's BSD sockets directly, with no guest structures. */
#include <sys/socket.h>
#include "posix.h"

/* miniupnpc's port probe uses a native Darwin sockaddr; the game's
   Winsock adapter expects the guest layout instead. */
static int upnp_bind_native(int socket, const void *address, int length) {
    return bind(socket, (const struct sockaddr *)address, (socklen_t)length);
}
#define posix_socket_bind upnp_bind_native
#include "port/linux/src/posix_upnp.c"
