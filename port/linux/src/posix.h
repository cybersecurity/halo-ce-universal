/*
POSIX.H

Boundary between the XDK-facing platform layer and glibc.

Everything except posix_*.c is compiled with the game's MSVC-compatible ABI
(-malign-double, 16-bit wchar_t, XDK headers), under which glibc structures
with 64-bit members, such as struct stat and struct dirent, have the wrong
layout. The posix_*.c files are compiled with the host ABI instead and expose
these helpers, whose parameters are all 32-bit scalars or pointers to structs
made only of 32-bit members so both sides agree on the layout.
*/

#ifndef __HALO_LINUX_POSIX_H
#define __HALO_LINUX_POSIX_H

/* 32-bit on both sides of the boundary. The Android port compiles these
files into its 64-bit host, where long is 64-bit, and calls them from its
ILP32 guest (port/android/README.md). */
#ifdef __LP64__
typedef int posix_long;
typedef unsigned int posix_ulong;
#else
typedef long posix_long;
typedef unsigned long posix_ulong;
#endif

enum
{
	_posix_file_is_directory = 1 << 0,
	_posix_file_is_read_only = 1 << 1,
};

struct posix_file_information
{
	posix_ulong flags;
	posix_ulong size_low;
	posix_ulong size_high;
	/* seconds and nanoseconds since the Unix epoch */
	posix_ulong modification_seconds;
	posix_ulong modification_nanoseconds;
	posix_ulong access_seconds;
	posix_ulong access_nanoseconds;
	posix_ulong creation_seconds;
	posix_ulong creation_nanoseconds;
};

/* stat()/fstat(); return 0 on success or -1 with errno set */
int posix_stat(const char *path, struct posix_file_information *information);
int posix_fstat(int descriptor, struct posix_file_information *information);

/* set access and modification times; a zero seconds value leaves it alone */
int posix_set_file_times(const char *path,
	posix_ulong access_seconds, posix_ulong access_nanoseconds,
	posix_ulong modification_seconds, posix_ulong modification_nanoseconds);

/* 64-bit file positioning on a descriptor */
/* names the code at an address (a symbol, or module+offset) without
allocating, for crash backtraces */
void posix_describe_address(void *address, char *buffer, posix_ulong size);

int posix_seek(int descriptor, posix_long offset_low, posix_long offset_high, int whence,
	posix_ulong *position_low, posix_ulong *position_high);
int posix_truncate(int descriptor, posix_ulong size_low, posix_ulong size_high);

/* free and total bytes on the file system holding path */
int posix_disk_space(const char *path,
	posix_ulong *free_low, posix_ulong *free_high,
	posix_ulong *total_low, posix_ulong *total_high);

/* permissions and directories */
int posix_set_read_only(const char *path, int read_only);
int posix_make_directory(const char *path);

/* directory enumeration; the handle is opaque */
void *posix_directory_open(const char *path);
/* copies the next entry name (excluding . and ..); returns 0 at the end */
int posix_directory_next(void *directory, char *name, posix_ulong name_size);
void posix_directory_close(void *directory);

/* case-insensitive lookup of one path component inside directory;
copies the on-disk spelling into result and returns nonzero if found */
int posix_find_entry_case_insensitive(const char *directory, const char *name,
	char *result, posix_ulong result_size);

/* ---------- sockets

Winsock and BSD share the sockaddr_in layout, so addresses pass through as
opaque pointers. Every call returns -1 on failure with the equivalent
Winsock error code available from posix_socket_last_error(). */

int posix_socket_last_error(void);
int posix_socket(int family, int type, int protocol);
int posix_socket_close(int socket);
int posix_socket_bind(int socket, const void *address, int address_length);
int posix_socket_connect(int socket, const void *address, int address_length);
int posix_socket_listen(int socket, int backlog);
int posix_socket_accept(int socket, void *address, int *address_length);
int posix_socket_send(int socket, const void *buffer, int length, int flags);
int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length);
int posix_socket_recv(int socket, void *buffer, int length, int flags);
int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length);
int posix_socket_shutdown(int socket, int how);
int posix_socket_set_nonblocking(int socket, int nonblocking);
int posix_socket_bytes_available(int socket, posix_ulong *count);
/* a stream socket sends each write at once (no Nagle delay): the game's
connections carry small messages every tick, which would otherwise wait on
the other end's delayed acknowledgement */
int posix_socket_set_nodelay(int socket);
/* Winsock option levels and names are translated for SOL_SOCKET options */
int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length);
int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length);
int posix_socket_getsockname(int socket, void *address, int *address_length);
int posix_socket_getpeername(int socket, void *address, int *address_length);
/* select over explicit descriptor lists (any descriptor numbers: poll on
Linux); each list is rewritten in place to hold only the ready descriptors,
in the order given, and its count updated */
int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite);
/* this machine's IPv4 address on its local network (network byte order):
the one its default route leaves from (on Android, first that of an
interface with broadcasts: Wi-Fi, not mobile data), else the first of an
interface that is up and not loopback; or 0 */
posix_ulong posix_local_ipv4_address(void);
/* fills buffer with cryptographically random bytes; aborts the process if
the system has none to give */
void posix_random_bytes(void *buffer, posix_ulong size);
/* the IPv4 address (network byte order) of host, a name or a dotted quad,
or 0 if it cannot be resolved; may block while a name is looked up */
posix_ulong posix_resolve_ipv4(const char *host);

/* ---------- UPnP (internet play, p2p.c; posix_upnp.c, with
port/third_party/miniupnpc) */

/* asks the local network's router (its UPnP Internet Gateway Device) to
forward a UDP port of the router's, preferred_port first, to port of this
machine (network byte order); blocks for a few seconds. 1 on success, with
the router's internet address and the port it forwards (network byte
order); else 0 and why in error. Asking again for the port it forwards
renews the forwarding. */
int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size);
/* stops the router forwarding that port (network byte order) of its, which
posix_upnp_forward_udp set up; blocks */
void posix_upnp_stop_forwarding_udp(unsigned short external_port);

/* ---------- the process and the desktop (internet play, p2p.c) */

/* copies the command line argument at index (0 is the program) into buffer;
returns 0 if there is none (always, on Android) */
int posix_command_line_argument(int index, char *buffer, posix_ulong size);
posix_ulong posix_process_id(void);
/* registers this executable as the desktop's handler of links with this
scheme (scheme://...); returns 0 where there is no such thing (Android,
whose app declares its links in its manifest). May wait for a program */
int posix_register_url_scheme(const char *scheme, const char *description);
/* a random secret of this user's, the same for all their processes, from a
file that only they can read (made the first time); 1 on success, 0 where
there is none (always, on Android) */
int posix_user_secret(unsigned char *secret, int size);

/* a connection to the Discord desktop client's local socket or pipe, or -1
if none is running (always, on Android) */
int posix_discord_connect(void);
/* writes what it can of buffer without waiting; returns the bytes written
(0 if none could be now), or -1 if the connection failed */
int posix_discord_write(int handle, const void *buffer, int length);
/* reads what has arrived, without waiting: the bytes read, 0 if nothing has,
or -1 if the connection closed */
int posix_discord_read(int handle, void *buffer, int length);
void posix_discord_close(int handle);

#endif
