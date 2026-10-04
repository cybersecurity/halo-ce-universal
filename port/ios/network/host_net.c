#include "posix.h"
#include "network_mode.h"
int halo_browser_rooms;
int ios_native_posix_socket_last_error(void);
int ios_web_posix_socket_last_error(void);
int posix_socket_last_error(void) { return halo_browser_rooms ? ios_web_posix_socket_last_error() : ios_native_posix_socket_last_error(); }
int ios_native_posix_socket(int family, int type, int protocol);
int ios_web_posix_socket(int family, int type, int protocol);
int posix_socket(int family, int type, int protocol) { return halo_browser_rooms ? ios_web_posix_socket(family, type, protocol) : ios_native_posix_socket(family, type, protocol); }
int ios_native_posix_socket_close(int socket);
int ios_web_posix_socket_close(int socket);
int posix_socket_close(int socket) { return halo_browser_rooms ? ios_web_posix_socket_close(socket) : ios_native_posix_socket_close(socket); }
int ios_native_posix_socket_bind(int socket, const void *address, int address_length);
int ios_web_posix_socket_bind(int socket, const void *address, int address_length);
int posix_socket_bind(int socket, const void *address, int address_length) { return halo_browser_rooms ? ios_web_posix_socket_bind(socket, address, address_length) : ios_native_posix_socket_bind(socket, address, address_length); }
int ios_native_posix_socket_connect(int socket, const void *address, int address_length);
int ios_web_posix_socket_connect(int socket, const void *address, int address_length);
int posix_socket_connect(int socket, const void *address, int address_length) { return halo_browser_rooms ? ios_web_posix_socket_connect(socket, address, address_length) : ios_native_posix_socket_connect(socket, address, address_length); }
int ios_native_posix_socket_listen(int socket, int backlog);
int ios_web_posix_socket_listen(int socket, int backlog);
int posix_socket_listen(int socket, int backlog) { return halo_browser_rooms ? ios_web_posix_socket_listen(socket, backlog) : ios_native_posix_socket_listen(socket, backlog); }
int ios_native_posix_socket_accept(int socket, void *address, int *address_length);
int ios_web_posix_socket_accept(int socket, void *address, int *address_length);
int posix_socket_accept(int socket, void *address, int *address_length) { return halo_browser_rooms ? ios_web_posix_socket_accept(socket, address, address_length) : ios_native_posix_socket_accept(socket, address, address_length); }
int ios_native_posix_socket_send(int socket, const void *buffer, int length, int flags);
int ios_web_posix_socket_send(int socket, const void *buffer, int length, int flags);
int posix_socket_send(int socket, const void *buffer, int length, int flags) { return halo_browser_rooms ? ios_web_posix_socket_send(socket, buffer, length, flags) : ios_native_posix_socket_send(socket, buffer, length, flags); }
int ios_native_posix_socket_sendto(int socket, const void *buffer, int length, int flags, const void *address, int address_length);
int ios_web_posix_socket_sendto(int socket, const void *buffer, int length, int flags, const void *address, int address_length);
int posix_socket_sendto(int socket, const void *buffer, int length, int flags, const void *address, int address_length) { return halo_browser_rooms ? ios_web_posix_socket_sendto(socket, buffer, length, flags, address, address_length) : ios_native_posix_socket_sendto(socket, buffer, length, flags, address, address_length); }
int ios_native_posix_socket_recv(int socket, void *buffer, int length, int flags);
int ios_web_posix_socket_recv(int socket, void *buffer, int length, int flags);
int posix_socket_recv(int socket, void *buffer, int length, int flags) { return halo_browser_rooms ? ios_web_posix_socket_recv(socket, buffer, length, flags) : ios_native_posix_socket_recv(socket, buffer, length, flags); }
int ios_native_posix_socket_recvfrom(int socket, void *buffer, int length, int flags, void *address, int *address_length);
int ios_web_posix_socket_recvfrom(int socket, void *buffer, int length, int flags, void *address, int *address_length);
int posix_socket_recvfrom(int socket, void *buffer, int length, int flags, void *address, int *address_length) { return halo_browser_rooms ? ios_web_posix_socket_recvfrom(socket, buffer, length, flags, address, address_length) : ios_native_posix_socket_recvfrom(socket, buffer, length, flags, address, address_length); }
int ios_native_posix_socket_shutdown(int socket, int how);
int ios_web_posix_socket_shutdown(int socket, int how);
int posix_socket_shutdown(int socket, int how) { return halo_browser_rooms ? ios_web_posix_socket_shutdown(socket, how) : ios_native_posix_socket_shutdown(socket, how); }
int ios_native_posix_socket_set_nonblocking(int socket, int nonblocking);
int ios_web_posix_socket_set_nonblocking(int socket, int nonblocking);
int posix_socket_set_nonblocking(int socket, int nonblocking) { return halo_browser_rooms ? ios_web_posix_socket_set_nonblocking(socket, nonblocking) : ios_native_posix_socket_set_nonblocking(socket, nonblocking); }
int ios_native_posix_socket_bytes_available(int socket, posix_ulong *count);
int ios_web_posix_socket_bytes_available(int socket, posix_ulong *count);
int posix_socket_bytes_available(int socket, posix_ulong *count) { return halo_browser_rooms ? ios_web_posix_socket_bytes_available(socket, count) : ios_native_posix_socket_bytes_available(socket, count); }
int ios_native_posix_socket_set_nodelay(int socket);
int ios_web_posix_socket_set_nodelay(int socket);
int posix_socket_set_nodelay(int socket) { return halo_browser_rooms ? ios_web_posix_socket_set_nodelay(socket) : ios_native_posix_socket_set_nodelay(socket); }
int ios_native_posix_socket_setsockopt(int socket, int level, int name, const void *value, int length);
int ios_web_posix_socket_setsockopt(int socket, int level, int name, const void *value, int length);
int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length) { return halo_browser_rooms ? ios_web_posix_socket_setsockopt(socket, level, name, value, length) : ios_native_posix_socket_setsockopt(socket, level, name, value, length); }
int ios_native_posix_socket_getsockopt(int socket, int level, int name, void *value, int *length);
int ios_web_posix_socket_getsockopt(int socket, int level, int name, void *value, int *length);
int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length) { return halo_browser_rooms ? ios_web_posix_socket_getsockopt(socket, level, name, value, length) : ios_native_posix_socket_getsockopt(socket, level, name, value, length); }
int ios_native_posix_socket_getsockname(int socket, void *address, int *address_length);
int ios_web_posix_socket_getsockname(int socket, void *address, int *address_length);
int posix_socket_getsockname(int socket, void *address, int *address_length) { return halo_browser_rooms ? ios_web_posix_socket_getsockname(socket, address, address_length) : ios_native_posix_socket_getsockname(socket, address, address_length); }
int ios_native_posix_socket_getpeername(int socket, void *address, int *address_length);
int ios_web_posix_socket_getpeername(int socket, void *address, int *address_length);
int posix_socket_getpeername(int socket, void *address, int *address_length) { return halo_browser_rooms ? ios_web_posix_socket_getpeername(socket, address, address_length) : ios_native_posix_socket_getpeername(socket, address, address_length); }
int ios_native_posix_socket_select(int *read, int *read_count, int *write, int *write_count, int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite);
int ios_web_posix_socket_select(int *read, int *read_count, int *write, int *write_count, int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite);
int posix_socket_select(int *read, int *read_count, int *write, int *write_count, int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite) { return halo_browser_rooms ? ios_web_posix_socket_select(read, read_count, write, write_count, error, error_count, timeout_seconds, timeout_microseconds, infinite) : ios_native_posix_socket_select(read, read_count, write, write_count, error, error_count, timeout_seconds, timeout_microseconds, infinite); }
posix_ulong ios_native_posix_local_ipv4_address(void);
posix_ulong ios_web_posix_local_ipv4_address(void);
posix_ulong posix_local_ipv4_address(void) { return halo_browser_rooms ? ios_web_posix_local_ipv4_address() : ios_native_posix_local_ipv4_address(); }
posix_ulong ios_native_posix_resolve_ipv4(const char *host);
posix_ulong ios_web_posix_resolve_ipv4(const char *host);
posix_ulong posix_resolve_ipv4(const char *host) { return halo_browser_rooms ? ios_web_posix_resolve_ipv4(host) : ios_native_posix_resolve_ipv4(host); }
