/*
WEB_NET.C

The sockets of port/linux/src/posix.h for the web build: a network inside
the page.

Browsers have no UDP or plain TCP, but the game needs sockets even when it
plays alone: a split screen game is a network game whose host and clients
are the same machine, connected through Winsock (transport_endpoint_winsock.c).
Here every socket belongs to one machine with two addresses, loopback and
its address on the players' network (10.x.y.z, which the page chooses).
Datagrams sent to either, or to a broadcast address, go to the socket bound
to their port; stream sockets connect to the listening socket of their port
and exchange bytes through queues.

Online, the other players' machines have addresses on the same network.
Datagrams to them (and broadcasts) and streams connected to them become
packets in a ring the page reads (web_shared.h); the page carries them over
WebRTC to the other players' pages (port/web/site/net.js), which put them in
their games' incoming rings, from which the socket calls here take them.
System link's own discovery and connections then work between the players.

As with Winsock, each call returns -1 on failure with the error code in
posix_socket_last_error().
*/

#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "posix.h"
#include "web_shared.h"
#include "web_ping.h"

static unsigned int ping_address(unsigned int native)
{
	return (native >> 24) | ((native >> 8) & 0xff00u) |
		((native << 8) & 0xff0000u) | (native << 24);
}

static int web_net_host_ping_at(unsigned int address, unsigned int host,
	unsigned int epoch, unsigned int now)
{
	struct web_shared_state *shared = web_shared_state();
	unsigned int sequence = __atomic_load_n(&shared->ping_sequence, __ATOMIC_SEQ_CST);
	int count, result = -1, index;
	if (!sequence || (sequence & 1) || !address || !host)
		return -1;
	if ((unsigned int)__atomic_load_n(&shared->ping_host, __ATOMIC_SEQ_CST) != ping_address(host) ||
		(unsigned int)__atomic_load_n(&shared->ping_epoch, __ATOMIC_SEQ_CST) != epoch ||
		now - (unsigned int)__atomic_load_n(&shared->ping_updated, __ATOMIC_SEQ_CST) >= WEB_PING_STALE_MS)
		return -1;
	count = __atomic_load_n(&shared->ping_count, __ATOMIC_SEQ_CST);
	if (count < 0 || count > WEB_PING_PEERS)
		return -1;
	for (index = 0; index < count; index++)
	{
		if ((unsigned int)__atomic_load_n(&shared->ping_peers[index][0], __ATOMIC_SEQ_CST) == ping_address(address))
			result = __atomic_load_n(&shared->ping_peers[index][1], __ATOMIC_SEQ_CST);
	}
	if (sequence != (unsigned int)__atomic_load_n(&shared->ping_sequence, __ATOMIC_SEQ_CST) ||
		result < 0 || result >= WEB_PING_STALE_MS)
		return -1;
	return result;
}

int web_net_host_ping(unsigned int address, unsigned int host, unsigned int epoch)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return web_net_host_ping_at(address, host, epoch,
		(unsigned int)((unsigned long long)now.tv_sec * 1000 + now.tv_nsec / 1000000));
}

/* Winsock error codes (winerror.h) */
#define WSAEBADF 10009
#define WSAEFAULT 10014
#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAENOTSOCK 10038
#define WSAEMSGSIZE 10040
#define WSAEPROTONOSUPPORT 10043
#define WSAEOPNOTSUPP 10045
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAECONNRESET 10054
#define WSAENOBUFS 10055
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAECONNREFUSED 10061

#define AF_INET_VALUE 2
#define SOCK_STREAM_VALUE 1
#define SOCK_DGRAM_VALUE 2

/* 10.0.2.15, in network byte order: the machine's address until the page
gives it one */
#define WEB_DEFAULT_ADDRESS 0x0F02000AUL
#define WEB_LOCAL_ADDRESS local_address()
#define LOOPBACK_ADDRESS 0x0100007FUL
/* the largest payload of one packet to another machine */
#define MAXIMUM_LINK_PAYLOAD 16000
/* waits look at the incoming packets this often */
#define LINK_POLL_NANOSECONDS 4000000L

#define SOCKET_BASE 1000 /* descriptors apart from the file system's */
#define MAXIMUM_SOCKETS 256
#define MAXIMUM_DATAGRAMS 512
#define MAXIMUM_DATAGRAM 65536
#define STREAM_CAPACITY (1024 * 1024)
#define MAXIMUM_BACKLOG 16
#define FIRST_EPHEMERAL_PORT 49152

/* sockaddr_in as Winsock and BSD share it */
struct address
{
	unsigned short family;
	unsigned short port;     /* network byte order */
	unsigned int ip;         /* network byte order */
	unsigned char zero[8];
};

struct datagram
{
	struct datagram *next;
	struct address from;
	int length;
	unsigned char data[];
};

struct web_socket
{
	int used;
	int type;
	int nonblocking;
	int bound, listening, connected, peer_closed, shut_down;
	struct address local, remote;
	/* datagrams */
	struct datagram *first, *last;
	int datagram_count;
	/* a stream's received bytes (a ring) */
	unsigned char *stream;
	int stream_head, stream_count;
	int peer;                   /* the connected socket's index, or -1 */
	int remote_link;            /* a stream to another machine (through the page) */
	/* a listener's connections waiting to be accepted */
	int backlog[MAXIMUM_BACKLOG];
	int backlog_count;
};

static struct web_socket sockets[MAXIMUM_SOCKETS];
static pthread_mutex_t network_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t network_changed = PTHREAD_COND_INITIALIZER;
static unsigned short next_ephemeral_port = FIRST_EPHEMERAL_PORT;
static __thread int last_error;

static unsigned short swap16(unsigned short value)
{
	return (unsigned short)((value >> 8) | (value << 8));
}

/* HALO_NET_DEBUG=1 logs the network's traffic */
static int trace_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = getenv("HALO_NET_DEBUG") != NULL;
	return enabled;
}

#define TRACE(...) do { if (trace_enabled()) fprintf(stderr, "web_net: " __VA_ARGS__); } while (0)

static int fail(int error)
{
	last_error = error;
	if (error != WSAEWOULDBLOCK)
		TRACE("error %d\n", error);
	return -1;
}

static int succeed(int result)
{
	last_error = 0;
	return result;
}

/* the socket of a descriptor, with the lock held; NULL (and the error) if
there is none */
static struct web_socket *socket_get(int descriptor)
{
	int index = descriptor - SOCKET_BASE;

	if (index < 0 || index >= MAXIMUM_SOCKETS || !sockets[index].used)
	{
		last_error = WSAENOTSOCK;
		TRACE("no socket %d\n", descriptor);
		return NULL;
	}
	return &sockets[index];
}

static unsigned int local_address(void)
{
	unsigned int address = (unsigned int)web_shared_state()->net_local_address;

	return address ? address : WEB_DEFAULT_ADDRESS;
}

static int is_broadcast(unsigned int ip)
{
	return ip == 0xFFFFFFFFu || (unsigned char)(ip >> 24) == 255;
}

/* this machine's addresses (a broadcast reaches it too) */
static int is_local(unsigned int ip)
{
	return ip == 0 || ip == LOOPBACK_ADDRESS || ip == WEB_LOCAL_ADDRESS || is_broadcast(ip) || (ip & 0xFF) == 127;
}

static int port_in_use(int type, unsigned short port)
{
	int index;

	for (index = 0; index < MAXIMUM_SOCKETS; index++)
	{
		if (sockets[index].used && sockets[index].bound && sockets[index].type == type &&
			sockets[index].local.port == port && !(type == SOCK_STREAM_VALUE && sockets[index].connected &&
			!sockets[index].listening))
		{
			return 1;
		}
	}
	return 0;
}

static unsigned short ephemeral_port(int type)
{
	int tries;

	for (tries = 0; tries < 16384; tries++)
	{
		unsigned short port = swap16(next_ephemeral_port);

		next_ephemeral_port = next_ephemeral_port == 65535 ? FIRST_EPHEMERAL_PORT : next_ephemeral_port + 1;
		if (!port_in_use(type, port))
			return port;
	}
	return 0;
}

static int is_loopback(unsigned int ip)
{
	return (ip & 0xFF) == 127;
}

/* binds an unbound socket to any address and a free port */
static void bind_implicitly(struct web_socket *socket)
{
	if (!socket->bound)
	{
		memset(&socket->local, 0, sizeof(socket->local));
		socket->local.family = AF_INET_VALUE;
		socket->local.ip = 0;
		socket->local.port = ephemeral_port(socket->type);
		socket->bound = 1;
	}
}

/* the address a packet to destination comes from, as a host's routing
would choose it for a socket bound to any address */
static struct address source_address(const struct web_socket *socket, unsigned int destination)
{
	struct address source = socket->local;

	if (!source.ip)
		source.ip = is_loopback(destination) ? LOOPBACK_ADDRESS : WEB_LOCAL_ADDRESS;
	return source;
}

static void datagrams_free(struct web_socket *socket)
{
	while (socket->first)
	{
		struct datagram *next = socket->first->next;

		free(socket->first);
		socket->first = next;
	}
	socket->last = NULL;
	socket->datagram_count = 0;
}

/* ---------- packets to and from other machines (web_shared.h) */

static void ring_copy_in(unsigned char *ring, unsigned int capacity, unsigned int position, const void *data,
	unsigned int length)
{
	unsigned int start = position & (capacity - 1);
	unsigned int first = capacity - start < length ? capacity - start : length;

	memcpy(ring + start, data, first);
	memcpy(ring, (const unsigned char *)data + first, length - first);
}

static void ring_copy_out(const unsigned char *ring, unsigned int capacity, unsigned int position, void *data,
	unsigned int length)
{
	unsigned int start = position & (capacity - 1);
	unsigned int first = capacity - start < length ? capacity - start : length;

	memcpy(data, ring + start, first);
	memcpy((unsigned char *)data + first, ring, length - first);
}

/* queues a packet for the page to send; 0 if the ring is full */
static int link_send(unsigned int kind, const struct address *from, const struct address *to, const void *payload,
	unsigned int length)
{
	struct web_shared_state *shared = web_shared_state();
	struct web_packet_header header;
	unsigned int write = (unsigned int)__atomic_load_n(&shared->net_out_write, __ATOMIC_SEQ_CST);
	unsigned int read = (unsigned int)__atomic_load_n(&shared->net_out_read, __ATOMIC_SEQ_CST);
	unsigned int size = (unsigned int)(sizeof(header) + length + 3) & ~3u;

	if (WEB_NET_OUT_BYTES - (write - read) < size)
		return 0;
	header.size = size;
	header.kind = kind;
	header.source_ip = from->ip;
	header.destination_ip = to->ip;
	header.source_port = from->port;
	header.destination_port = to->port;
	header.length = length;
	ring_copy_in(shared->net_out, WEB_NET_OUT_BYTES, write, &header, sizeof(header));
	if (length)
		ring_copy_in(shared->net_out, WEB_NET_OUT_BYTES, write + (unsigned int)sizeof(header), payload, length);
	__atomic_store_n(&shared->net_out_write, (int32_t)(write + size), __ATOMIC_SEQ_CST);
	return 1;
}

static int free_slot(void)
{
	int index;

	for (index = 0; index < MAXIMUM_SOCKETS && sockets[index].used; index++)
		;
	return index < MAXIMUM_SOCKETS ? index : -1;
}

static int find_listener(unsigned short port);
static void socket_release(int index);
static void deliver_local_datagram(const struct address *from, const struct address *to, const void *buffer,
	int length);

/* the stream socket for a connection with another machine */
static struct web_socket *remote_stream(unsigned int local_port, unsigned int remote_ip, unsigned int remote_port)
{
	int index;

	for (index = 0; index < MAXIMUM_SOCKETS; index++)
	{
		struct web_socket *socket = &sockets[index];

		if (socket->used && socket->remote_link && !socket->peer_closed && socket->local.port == local_port &&
			socket->remote.ip == remote_ip && socket->remote.port == remote_port)
		{
			return socket;
		}
	}
	return NULL;
}

/* A reloaded page keeps its address but can reuse the same ephemeral port.
   Its OPEN replaces that connection; the game's old descriptor must see EOF
   without routing bytes or a later CLOSE to the replacement. */
static int replace_remote_stream(int listener, const struct address *from, const struct address *to)
{
	int index, waiting_for_release = 0;

	for (index = 0; index < MAXIMUM_SOCKETS; index++)
	{
		struct web_socket *socket = &sockets[index];
		int entry;

		if (!socket->used || !socket->remote_link || socket->local.port != to->port ||
			socket->remote.ip != from->ip || socket->remote.port != from->port)
			continue;
		socket->peer_closed = 1;
		socket->stream_head = socket->stream_count = 0;
		free(socket->stream);
		socket->stream = NULL;
		/* An unaccepted connection has no game-owned descriptor to retain. */
		for (entry = 0; entry < sockets[listener].backlog_count; entry++)
		{
			if (sockets[listener].backlog[entry] == index)
			{
				memmove(sockets[listener].backlog + entry, sockets[listener].backlog + entry + 1,
					(size_t)(--sockets[listener].backlog_count - entry) * sizeof(int));
				socket_release(index);
				break;
			}
		}
		if (socket->used)
			waiting_for_release = 1;
	}
	/* The game must remove the old endpoint before it can accept the new one.
	   Keep OPEN queued until the descriptor owner observes EOF and closes it. */
	if (waiting_for_release)
		pthread_cond_broadcast(&network_changed);
	return !waiting_for_release;
}

/* 0 leaves the complete packet in the shared ring until recv makes room or
   the game releases a replaced stream's old descriptor. */
static int link_receive(const struct web_packet_header *header, const unsigned char *payload)
{
	struct address from, to;
	struct web_socket *socket;

	memset(&from, 0, sizeof(from));
	memset(&to, 0, sizeof(to));
	from.family = to.family = AF_INET_VALUE;
	from.ip = header->source_ip;
	from.port = header->source_port;
	to.ip = header->destination_ip;
	to.port = header->destination_port;
	switch (header->kind)
	{
	case WEB_PACKET_DATAGRAM:
		deliver_local_datagram(&from, &to, payload, (int)header->length);
		break;
	case WEB_PACKET_OPEN:
	{
		int listener = find_listener(to.port);
		int server;

		if (listener >= 0 && !replace_remote_stream(listener, &from, &to))
			return 0;
		server = free_slot();

		if (listener < 0 || server < 0 || sockets[listener].backlog_count == MAXIMUM_BACKLOG)
		{
			struct address local = to;

			local.ip = WEB_LOCAL_ADDRESS;
			link_send(WEB_PACKET_REFUSE, &local, &from, NULL, 0);
			TRACE("refused a connection from %08x:%u to port %u\n", from.ip, swap16(from.port), swap16(to.port));
			break;
		}
		socket = &sockets[server];
		memset(socket, 0, sizeof(*socket));
		socket->used = 1;
		socket->type = SOCK_STREAM_VALUE;
		socket->bound = 1;
		socket->connected = 1;
		socket->remote_link = 1;
		socket->peer = -1;
		socket->local = to;
		socket->local.ip = WEB_LOCAL_ADDRESS;
		socket->remote = from;
		socket->stream = malloc(STREAM_CAPACITY);
		sockets[listener].backlog[sockets[listener].backlog_count++] = server;
		TRACE("connection from %08x:%u to port %u\n", from.ip, swap16(from.port), swap16(to.port));
		break;
	}
	case WEB_PACKET_DATA:
		socket = remote_stream(to.port, from.ip, from.port);
		if (socket && socket->stream)
		{
			unsigned int index;

			if (header->length > (unsigned int)(STREAM_CAPACITY - socket->stream_count))
				return 0;
			for (index = 0; index < header->length; index++)
			{
				socket->stream[(socket->stream_head + socket->stream_count) % STREAM_CAPACITY] = payload[index];
				socket->stream_count++;
			}
		}
		break;
	case WEB_PACKET_CLOSE:
	case WEB_PACKET_REFUSE:
		socket = remote_stream(to.port, from.ip, from.port);
		if (socket)
			socket->peer_closed = 1;
		break;
	default:
		break;
	}
	return 1;
}

/* takes the packets the page received from other machines; with the lock
held */
static void link_pump(void)
{
	static unsigned char *payload;
	struct web_shared_state *shared = web_shared_state();
	unsigned int read = (unsigned int)__atomic_load_n(&shared->net_in_read, __ATOMIC_SEQ_CST);
	unsigned int write = (unsigned int)__atomic_load_n(&shared->net_in_write, __ATOMIC_SEQ_CST);
	int any = 0;

	if (read == write)
		return;
	if (!payload)
		payload = malloc(MAXIMUM_DATAGRAM + 16);
	while (read != write && payload)
	{
		struct web_packet_header header;

		ring_copy_out(shared->net_in, WEB_NET_IN_BYTES, read, &header, sizeof(header));
		if (header.size < sizeof(header) || header.size > WEB_NET_IN_BYTES || header.length > MAXIMUM_DATAGRAM)
		{
			/* a damaged ring: start over */
			read = write;
			break;
		}
		if (header.length)
			ring_copy_out(shared->net_in, WEB_NET_IN_BYTES, read + (unsigned int)sizeof(header), payload, header.length);
		if (!link_receive(&header, payload))
			break;
		read += header.size;
		any = 1;
	}
	__atomic_store_n(&shared->net_in_read, (int32_t)read, __ATOMIC_SEQ_CST);
	if (any)
		pthread_cond_broadcast(&network_changed);
}

/* waits for a change here or a packet from another machine; with the lock
held */
static void wait_briefly(void)
{
	struct timespec deadline;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += LINK_POLL_NANOSECONDS;
	if (deadline.tv_nsec >= 1000000000L)
	{
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	pthread_cond_timedwait(&network_changed, &network_lock, &deadline);
	link_pump();
}

static void socket_release(int index)
{
	struct web_socket *socket = &sockets[index];
	int entry;

	datagrams_free(socket);
	free(socket->stream);
	if (socket->remote_link && !socket->peer_closed)
		link_send(WEB_PACKET_CLOSE, &socket->local, &socket->remote, NULL, 0);
	if (socket->peer >= 0 && sockets[socket->peer].used && sockets[socket->peer].peer == index)
	{
		sockets[socket->peer].peer_closed = 1;
		sockets[socket->peer].peer = -1;
	}
	for (entry = 0; entry < socket->backlog_count; entry++)
		socket_release(socket->backlog[entry]);
	memset(socket, 0, sizeof(*socket));
}

/* ---------- posix.h */

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	int index;

	(void)protocol;
	if (family != AF_INET_VALUE)
		return fail(WSAEAFNOSUPPORT);
	if (type != SOCK_STREAM_VALUE && type != SOCK_DGRAM_VALUE)
		return fail(WSAEPROTONOSUPPORT);
	pthread_mutex_lock(&network_lock);
	for (index = 0; index < MAXIMUM_SOCKETS && sockets[index].used; index++)
		;
	if (index == MAXIMUM_SOCKETS)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAEMFILE);
	}
	memset(&sockets[index], 0, sizeof(sockets[index]));
	sockets[index].used = 1;
	sockets[index].type = type;
	sockets[index].peer = -1;
	pthread_mutex_unlock(&network_lock);
	return succeed(SOCKET_BASE + index);
}

int posix_socket_close(int descriptor)
{
	struct web_socket *socket;

	TRACE("close %d\n", descriptor);
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
		socket_release(descriptor - SOCKET_BASE);
	pthread_cond_broadcast(&network_changed);
	pthread_mutex_unlock(&network_lock);
	return socket ? succeed(0) : -1;
}

int posix_socket_bind(int descriptor, const void *address, int address_length)
{
	struct web_socket *socket;
	struct address wanted;
	int result = -1;

	if (!address || address_length < (int)sizeof(struct address) - 8)
		return fail(WSAEFAULT);
	memcpy(&wanted, address, sizeof(wanted) - 8);
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
	{
		if (socket->bound)
			result = fail(WSAEINVAL);
		else if (wanted.port && port_in_use(socket->type, wanted.port))
			result = fail(WSAEADDRINUSE);
		else
		{
			memset(&socket->local, 0, sizeof(socket->local));
			socket->local.family = AF_INET_VALUE;
			socket->local.ip = wanted.ip;
			socket->local.port = wanted.port ? wanted.port : ephemeral_port(socket->type);
			socket->bound = 1;
			TRACE("bind %d (type %d) to %08x:%u\n", descriptor, socket->type, socket->local.ip, swap16(socket->local.port));
			result = succeed(0);
		}
	}
	pthread_mutex_unlock(&network_lock);
	return result;
}

int posix_socket_listen(int descriptor, int backlog)
{
	struct web_socket *socket;
	int result = -1;

	(void)backlog;
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
	{
		if (socket->type != SOCK_STREAM_VALUE)
			result = fail(WSAEOPNOTSUPP);
		else
		{
			bind_implicitly(socket);
			socket->listening = 1;
			result = succeed(0);
		}
	}
	pthread_mutex_unlock(&network_lock);
	return result;
}

static int find_listener(unsigned short port)
{
	int index;

	for (index = 0; index < MAXIMUM_SOCKETS; index++)
	{
		if (sockets[index].used && sockets[index].listening && sockets[index].local.port == port)
			return index;
	}
	return -1;
}

int posix_socket_connect(int descriptor, const void *address, int address_length)
{
	struct web_socket *socket;
	struct address target;
	int result = -1;

	if (!address || address_length < (int)sizeof(struct address) - 8)
		return fail(WSAEFAULT);
	memcpy(&target, address, sizeof(target) - 8);
	TRACE("connect %d to %08x:%u\n", descriptor, target.ip, swap16(target.port));
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
	{
		int client = descriptor - SOCKET_BASE;

		bind_implicitly(socket);
		if (socket->type == SOCK_STREAM_VALUE && !socket->local.ip)
			socket->local = source_address(socket, target.ip);
		memset(&socket->remote, 0, sizeof(socket->remote));
		socket->remote.family = AF_INET_VALUE;
		socket->remote.ip = target.ip;
		socket->remote.port = target.port;
		if (socket->type == SOCK_DGRAM_VALUE)
		{
			socket->connected = 1;
			result = succeed(0);
		}
		else if (socket->connected)
		{
			result = fail(WSAEISCONN);
		}
		else if (!is_local(target.ip))
		{
			/* another machine's: through the page */
			socket->stream = malloc(STREAM_CAPACITY);
			socket->remote_link = 1;
			socket->connected = 1;
			if (!socket->stream || !link_send(WEB_PACKET_OPEN, &socket->local, &socket->remote, NULL, 0))
				result = fail(WSAENOBUFS);
			else if (socket->nonblocking)
				result = fail(WSAEWOULDBLOCK);
			else
				result = succeed(0);
		}
		else
		{
			int listener = is_local(target.ip) ? find_listener(target.port) : -1;
			int server;

			for (server = 0; server < MAXIMUM_SOCKETS && sockets[server].used; server++)
				;
			if (listener < 0)
				result = fail(WSAECONNREFUSED);
			else if (server == MAXIMUM_SOCKETS || sockets[listener].backlog_count == MAXIMUM_BACKLOG)
				result = fail(WSAENOBUFS);
			else
			{
				/* the listener's end of the connection, until accepted */
				struct web_socket *end = &sockets[server];

				memset(end, 0, sizeof(*end));
				end->used = 1;
				end->type = SOCK_STREAM_VALUE;
				end->bound = 1;
				end->connected = 1;
				end->local = sockets[listener].local;
				end->local.ip = is_loopback(target.ip) ? LOOPBACK_ADDRESS : WEB_LOCAL_ADDRESS;
				end->remote = socket->local;
				end->peer = client;
				end->stream = malloc(STREAM_CAPACITY);
				socket->stream = malloc(STREAM_CAPACITY);
				socket->peer = server;
				socket->connected = 1;
				sockets[listener].backlog[sockets[listener].backlog_count++] = server;
				pthread_cond_broadcast(&network_changed);
				/* Winsock's non-blocking connect: under way, then writeable
				(transport_endpoint_winsock.c waits for that) */
				if (socket->nonblocking)
					result = fail(WSAEWOULDBLOCK);
				else
					result = succeed(0);
			}
		}
	}
	pthread_mutex_unlock(&network_lock);
	return result;
}

static int wait_changed(struct web_socket *socket)
{
	if (socket->nonblocking)
		return 0;
	wait_briefly();
	return 1;
}

int posix_socket_accept(int descriptor, void *address, int *address_length)
{
	struct web_socket *socket;
	int result = -1;

	pthread_mutex_lock(&network_lock);
	link_pump();
	for (;;)
	{
		socket = socket_get(descriptor);
		if (!socket)
			break;
		if (!socket->listening)
		{
			result = fail(WSAEINVAL);
			break;
		}
		if (socket->backlog_count)
		{
			int accepted = socket->backlog[0];

			memmove(socket->backlog, socket->backlog + 1, (size_t)--socket->backlog_count * sizeof(int));
			if (address && address_length && *address_length >= (int)sizeof(struct address))
			{
				memcpy(address, &sockets[accepted].remote, sizeof(struct address));
				*address_length = (int)sizeof(struct address);
			}
			TRACE("accept %d -> %d\n", descriptor, SOCKET_BASE + accepted);
			result = succeed(SOCKET_BASE + accepted);
			break;
		}
		if (!wait_changed(socket))
		{
			result = fail(WSAEWOULDBLOCK);
			break;
		}
	}
	pthread_mutex_unlock(&network_lock);
	return result;
}

static int stream_send(struct web_socket *socket, const unsigned char *buffer, int length)
{
	struct web_socket *peer;
	int written = 0;

	TRACE("send %d bytes on %d (connected %d peer %d closed %d)\n", length, (int)(socket - sockets) + SOCKET_BASE,
		socket->connected, socket->peer, socket->peer_closed);
	if (!socket->connected)
		return fail(WSAENOTCONN);
	if (socket->remote_link)
	{
		for (;;)
		{
			if (socket->peer_closed)
				return fail(WSAECONNRESET);
			while (written < length)
			{
				int chunk = length - written > MAXIMUM_LINK_PAYLOAD ? MAXIMUM_LINK_PAYLOAD : length - written;

				if (!link_send(WEB_PACKET_DATA, &socket->local, &socket->remote, buffer + written, (unsigned int)chunk))
					break;
				written += chunk;
			}
			if (written == length || (written && socket->nonblocking))
				return succeed(written);
			if (!wait_changed(socket))
				return fail(WSAEWOULDBLOCK);
		}
	}
	for (;;)
	{
		if (socket->peer < 0 || socket->peer_closed)
			return fail(WSAECONNRESET);
		peer = &sockets[socket->peer];
		while (written < length && peer->stream_count < STREAM_CAPACITY)
		{
			peer->stream[(peer->stream_head + peer->stream_count) % STREAM_CAPACITY] = buffer[written++];
			peer->stream_count++;
		}
		pthread_cond_broadcast(&network_changed);
		if (written == length || (written && socket->nonblocking))
			return succeed(written);
		if (!wait_changed(socket))
			return fail(WSAEWOULDBLOCK);
	}
}

/* puts a datagram in the queue of each socket of this machine bound to its
port */
static void deliver_local_datagram(const struct address *from, const struct address *to, const void *buffer,
	int length)
{
	int index, delivered = 0;

	for (index = 0; index < MAXIMUM_SOCKETS; index++)
	{
		struct web_socket *target = &sockets[index];
		struct datagram *datagram;

		if (!target->used || target->type != SOCK_DGRAM_VALUE || !target->bound || target->local.port != to->port)
			continue;
		if (target->datagram_count >= MAXIMUM_DATAGRAMS)
			continue;
		datagram = malloc(sizeof(*datagram) + (size_t)length);
		if (!datagram)
			continue;
		datagram->next = NULL;
		datagram->from = *from;
		datagram->length = length;
		memcpy(datagram->data, buffer, (size_t)length);
		if (target->last)
			target->last->next = datagram;
		else
			target->first = datagram;
		target->last = datagram;
		target->datagram_count++;
		delivered = 1;
	}
	if (delivered)
		pthread_cond_broadcast(&network_changed);
}

static int deliver_datagram(struct web_socket *from, const void *buffer, int length, const struct address *to)
{
	struct address source = source_address(from, to->ip);

	if (length > MAXIMUM_DATAGRAM)
		return fail(WSAEMSGSIZE);
	TRACE("datagram %d bytes from %08x:%u to %08x:%u\n", length, source.ip, swap16(source.port), to->ip,
		swap16(to->port));
	/* this machine's sockets, and through the page the other machines':
	a broadcast goes to both */
	if (is_local(to->ip))
		deliver_local_datagram(&source, to, buffer, length);
	if (!is_local(to->ip) || (is_broadcast(to->ip) && !is_loopback(to->ip)))
		link_send(WEB_PACKET_DATAGRAM, &source, to, buffer, (unsigned int)length);
	return succeed(length);
}

int posix_socket_send(int descriptor, const void *buffer, int length, int flags)
{
	struct web_socket *socket;
	int result = -1;

	(void)flags;
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
	{
		if (socket->type == SOCK_STREAM_VALUE)
			result = stream_send(socket, buffer, length);
		else if (!socket->connected)
			result = fail(WSAENOTCONN);
		else
			result = deliver_datagram(socket, buffer, length, &socket->remote);
	}
	pthread_mutex_unlock(&network_lock);
	return result;
}

int posix_socket_sendto(int descriptor, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	struct web_socket *socket;
	struct address to;
	int result = -1;

	if (!address || address_length < (int)sizeof(struct address) - 8)
		return posix_socket_send(descriptor, buffer, length, flags);
	memcpy(&to, address, sizeof(to) - 8);
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
	{
		if (socket->type == SOCK_STREAM_VALUE)
			result = stream_send(socket, buffer, length);
		else
		{
			bind_implicitly(socket);
			result = deliver_datagram(socket, buffer, length, &to);
		}
	}
	pthread_mutex_unlock(&network_lock);
	return result;
}

static int receive(int descriptor, void *buffer, int length, int flags, void *address, int *address_length)
{
	struct web_socket *socket;
	int peek = (flags & 2) != 0; /* MSG_PEEK */
	int result = -1;

	pthread_mutex_lock(&network_lock);
	link_pump();
	for (;;)
	{
		socket = socket_get(descriptor);
		if (!socket)
			break;
		if (socket->type == SOCK_DGRAM_VALUE)
		{
			if (socket->first)
			{
				struct datagram *datagram = socket->first;
				int datagram_length = datagram->length;
				int count = datagram_length < length ? datagram_length : length;

				memcpy(buffer, datagram->data, (size_t)count);
				if (address && address_length && *address_length >= (int)sizeof(struct address))
				{
					memcpy(address, &datagram->from, sizeof(struct address));
					*address_length = (int)sizeof(struct address);
				}
				if (!peek)
				{
					socket->first = datagram->next;
					if (!socket->first)
						socket->last = NULL;
					socket->datagram_count--;
					free(datagram);
				}
				result = count < datagram_length && !peek ? fail(WSAEMSGSIZE) : succeed(count);
				break;
			}
		}
		else
		{
			if (!socket->connected)
			{
				result = fail(WSAENOTCONN);
				break;
			}
			if (socket->stream_count)
			{
				int count = socket->stream_count < length ? socket->stream_count : length;
				int index;

				for (index = 0; index < count; index++)
					((unsigned char *)buffer)[index] = socket->stream[(socket->stream_head + index) % STREAM_CAPACITY];
				if (!peek)
				{
					socket->stream_head = (socket->stream_head + count) % STREAM_CAPACITY;
					socket->stream_count -= count;
					pthread_cond_broadcast(&network_changed);
				}
				if (address && address_length && *address_length >= (int)sizeof(struct address))
				{
					memcpy(address, &socket->remote, sizeof(struct address));
					*address_length = (int)sizeof(struct address);
				}
				TRACE("recv %d bytes on %d\n", count, descriptor);
				result = succeed(count);
				break;
			}
			if (socket->peer_closed)
			{
				/* the other end closed: end of stream */
				result = succeed(0);
				break;
			}
		}
		if (!wait_changed(socket))
		{
			result = fail(WSAEWOULDBLOCK);
			break;
		}
	}
	pthread_mutex_unlock(&network_lock);
	return result;
}

int posix_socket_recv(int descriptor, void *buffer, int length, int flags)
{
	return receive(descriptor, buffer, length, flags, NULL, NULL);
}

int posix_socket_recvfrom(int descriptor, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	return receive(descriptor, buffer, length, flags, address, address_length);
}

int posix_socket_shutdown(int descriptor, int how)
{
	struct web_socket *socket;

	TRACE("shutdown %d %d\n", descriptor, how);
	(void)how;
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
	{
		socket->shut_down = 1;
		if (socket->peer >= 0)
			sockets[socket->peer].peer_closed = 1;
		if (socket->remote_link && !socket->peer_closed)
			link_send(WEB_PACKET_CLOSE, &socket->local, &socket->remote, NULL, 0);
		pthread_cond_broadcast(&network_changed);
	}
	pthread_mutex_unlock(&network_lock);
	return socket ? succeed(0) : -1;
}

int posix_socket_set_nonblocking(int descriptor, int nonblocking)
{
	struct web_socket *socket;

	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
		socket->nonblocking = nonblocking != 0;
	pthread_mutex_unlock(&network_lock);
	return socket ? succeed(0) : -1;
}

int posix_socket_set_nodelay(int descriptor)
{
	(void)descriptor;
	return succeed(0);
}

int posix_socket_bytes_available(int descriptor, posix_ulong *count)
{
	struct web_socket *socket;

	pthread_mutex_lock(&network_lock);
	link_pump();
	socket = socket_get(descriptor);
	if (socket)
		*count = (posix_ulong)(socket->type == SOCK_DGRAM_VALUE ?
			(socket->first ? socket->first->length : 0) : socket->stream_count);
	pthread_mutex_unlock(&network_lock);
	return socket ? succeed(0) : -1;
}

int posix_socket_setsockopt(int descriptor, int level, int name, const void *value, int length)
{
	(void)level;
	(void)name;
	(void)value;
	(void)length;
	pthread_mutex_lock(&network_lock);
	if (!socket_get(descriptor))
	{
		pthread_mutex_unlock(&network_lock);
		return -1;
	}
	pthread_mutex_unlock(&network_lock);
	return succeed(0);
}

int posix_socket_getsockopt(int descriptor, int level, int name, void *value, int *length)
{
	struct web_socket *socket;

	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket && value && length && *length >= 4)
	{
		int result = 0;

		if (level == 0xffff && name == 0x1008) /* SO_TYPE */
			result = socket->type;
		if (level == 0xffff && (name == 0x1001 || name == 0x1002)) /* buffer sizes */
			result = STREAM_CAPACITY;
		memcpy(value, &result, 4);
		*length = 4;
	}
	pthread_mutex_unlock(&network_lock);
	return socket ? succeed(0) : -1;
}

int posix_socket_getsockname(int descriptor, void *address, int *address_length)
{
	struct web_socket *socket;
	int result = -1;

	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
	{
		if (!address || !address_length || *address_length < (int)sizeof(struct address))
			result = fail(WSAEFAULT);
		else
		{
			struct address local = socket->local;

			if (!socket->bound)
			{
				memset(&local, 0, sizeof(local));
				local.family = AF_INET_VALUE;
			}
			memcpy(address, &local, sizeof(local));
			*address_length = (int)sizeof(local);
			result = succeed(0);
		}
	}
	pthread_mutex_unlock(&network_lock);
	return result;
}

int posix_socket_getpeername(int descriptor, void *address, int *address_length)
{
	struct web_socket *socket;
	int result = -1;

	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
	{
		if (!socket->connected)
			result = fail(WSAENOTCONN);
		else if (!address || !address_length || *address_length < (int)sizeof(struct address))
			result = fail(WSAEFAULT);
		else
		{
			memcpy(address, &socket->remote, sizeof(struct address));
			*address_length = (int)sizeof(struct address);
			result = succeed(0);
		}
	}
	pthread_mutex_unlock(&network_lock);
	return result;
}

/* with the lock held */
static int readable(int descriptor)
{
	struct web_socket *socket = socket_get(descriptor);

	if (!socket)
		return 0;
	if (socket->listening)
		return socket->backlog_count > 0;
	if (socket->type == SOCK_DGRAM_VALUE)
		return socket->first != NULL;
	return socket->stream_count > 0 || socket->peer_closed;
}

static int writeable(int descriptor)
{
	struct web_socket *socket = socket_get(descriptor);

	if (!socket)
		return 0;
	if (socket->type == SOCK_DGRAM_VALUE)
		return 1;
	if (socket->remote_link)
		return socket->connected && !socket->peer_closed;
	return socket->connected && !socket->peer_closed && socket->peer >= 0 &&
		sockets[socket->peer].stream_count < STREAM_CAPACITY;
}

static int keep(int *descriptors, int *count, int (*ready)(int))
{
	int index, kept = 0;

	if (!descriptors || !count)
		return 0;
	for (index = 0; index < *count; index++)
	{
		if (ready(descriptors[index]))
			descriptors[kept++] = descriptors[index];
	}
	*count = kept;
	return kept;
}

static int any_ready(const int *descriptors, int count, int (*ready)(int))
{
	int index;

	for (index = 0; descriptors && index < count; index++)
	{
		if (ready(descriptors[index]))
			return 1;
	}
	return 0;
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	struct timespec deadline;
	int result;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeout_seconds + timeout_microseconds / 1000000;
	deadline.tv_nsec += (timeout_microseconds % 1000000) * 1000L;
	if (deadline.tv_nsec >= 1000000000L)
	{
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&network_lock);
	link_pump();
	for (;;)
	{
		struct timespec now;

		if (any_ready(read, read ? *read_count : 0, readable) ||
			any_ready(write, write ? *write_count : 0, writeable))
			break;
		if (!infinite && timeout_seconds <= 0 && timeout_microseconds <= 0)
			break;
		clock_gettime(CLOCK_REALTIME, &now);
		if (!infinite && (now.tv_sec > deadline.tv_sec ||
			(now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)))
			break;
		/* (packets from other machines arrive without a signal: look often) */
		wait_briefly();
	}
	result = keep(read, read_count, readable) + keep(write, write_count, writeable);
	if (error && error_count)
		*error_count = 0;
	pthread_mutex_unlock(&network_lock);
	/* like Winsock, a select with nothing ready leaves the last error as it
	was (a connect under way stays WSAEWOULDBLOCK) */
	if (result > 0)
		last_error = 0;
	return result;
}

posix_ulong posix_local_ipv4_address(void)
{
	return (posix_ulong)WEB_LOCAL_ADDRESS;
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	unsigned int parts[4];
	int index = 0;
	const char *cursor = host;

	if (!host)
		return 0;
	if (!strcmp(host, "localhost"))
		return (posix_ulong)LOOPBACK_ADDRESS;
	for (index = 0; index < 4; index++)
	{
		char *end;
		unsigned long value = strtoul(cursor, &end, 10);

		if (end == cursor || value > 255 || (index < 3 && *end != '.') || (index == 3 && *end))
			return 0;
		parts[index] = (unsigned int)value;
		cursor = end + 1;
	}
	return (posix_ulong)(parts[0] | parts[1] << 8 | parts[2] << 16 | parts[3] << 24);
}
