/* Compile the POSIX network backend under a private host namespace. */
#include "posix_backend_names.h"
#undef posix_socket_sendto
#define posix_socket_sendto mac_native_posix_socket_sendto_original
#include "../linux/src/posix_net.c"
#undef posix_socket_sendto

/* Preserve the backend's Winsock error namespace for bridge validation too. */
int mac_native_posix_bridge_error(int error)
{
 errno = error;
 return fail();
}

/* Darwin rejects a destination on a connected socket even when it is its
 * existing peer. Linux/Winsock callers use sendto in this situation. Retry
 * with send only after proving exact IPv4 endpoint equality; alternate peers
 * retain the original failure rather than silently reaching the wrong peer. */
int mac_native_posix_socket_sendto(int socket, const void *buffer, int length,
 int flags, const void *address, int address_length)
{
 int result = mac_native_posix_socket_sendto_original(socket, buffer, length, flags, address, address_length);
 if (result < 0 && errno == EISCONN && address && address_length == sizeof(struct sockaddr_in))
 {
  int saved_error = errno;
  struct sockaddr_in peer;
  socklen_t peer_length = sizeof(peer);
  struct sockaddr_in const *destination = address;
  if (destination->sin_family == AF_INET && !getpeername(socket, (struct sockaddr *)&peer, &peer_length) &&
      peer_length == sizeof(peer) && peer.sin_family == AF_INET &&
      peer.sin_addr.s_addr == destination->sin_addr.s_addr && peer.sin_port == destination->sin_port)
   return succeed((int)send(socket, buffer, (size_t)length, flags | MSG_NOSIGNAL));
  errno = saved_error;
 }
 return result;
}
