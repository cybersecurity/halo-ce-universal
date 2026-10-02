/*
XNET.C

Xbox Winsock and XNet for the Linux build, over BSD sockets.

Game code reaches these under the halo_ws_ names (see
halo_linux_winsock_names.h), since glibc exports its own cdecl socket(),
bind(), ... that must not be confused with the __stdcall Winsock ones.

XNet's secure addressing collapses to plain IPv4: a host's XNADDR carries its
real address, key exchange keys are random but unused, and XNADDR to
IN_ADDR translation is the identity. That is enough for system link play on
a LAN.

Two settings in config.toml's [network] (port_config.c) adjust the
addressing:

- address = "a.b.c.d" binds the game's sockets to that local address
  instead of every interface, and reports it as this machine's system link
  address. Several instances can then share one computer, each on its own
  loopback address (127.0.0.2, 127.0.0.3, ...), or system link can be
  pinned to one network interface. That address then stands in for
  127.0.0.1, which the game uses for itself (a host joins its own game
  through it, and admits only it to a split screen game): connections and
  datagrams to 127.0.0.1 go to the address, and traffic from the address
  is reported as coming from 127.0.0.1.
- broadcast = "a.b.c.d[,e.f.g.h...]" sends the game's broadcasts (a
  client's system link game search, a host's game advertisement) to those
  addresses instead of 255.255.255.255, to reach machines that broadcasts
  do not: other loopback addresses, or machines across a VPN (listing
  255.255.255.255 too still broadcasts). A socket bound to one address
  receives no broadcasts, so machines with an address find each
  other only through these lists: each must list the others.

Both are read once, when the game starts its networking; a value that is
not an IPv4 address is reported and ignored.

Internet play (p2p.c) adds machines that shared an invite to this LAN: an
XNADDR's abEnet carries its machine's identifier, which XNetXnAddrToInAddr
maps to the peer's virtual address, and traffic to and from those addresses
is rewritten to and from p2p.c's local stand-ins here (or fails, with
WSAEHOSTUNREACH, while the peer is not reached); datagrams to a peer, and
broadcasts, which also go to every peer, go onto the tunnel at once
(p2p_send_datagram). p2p.c is told the ports of the game's sockets, which
alone peers reach.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "p2p.h"
#ifdef HALO_WEB
#include "web_shared.h"
#endif

#include <stdlib.h>
#include <string.h>

/* ---------- address settings */

/* Winsock's, which the SDK headers here leave out */
#ifndef SO_TYPE
#define SO_TYPE 0x1008
#endif

enum
{
	MAXIMUM_BROADCAST_TARGETS = 256,
	/* a system link game's other machines (include/halo_port_limits.h) */
	P2P_BROADCAST_PEERS = 127,
};

/* network.address and network.broadcast, read once (net_settings_read),
in network byte order */
static struct
{
	int read;
	int has_local_address;
	unsigned long local_address;
	int broadcast_count;
	unsigned long broadcast_targets[MAXIMUM_BROADCAST_TARGETS];
} net_settings;

/* a dotted quad of decimal numbers up to 255 filling [text, end), spaces
around it allowed; the address in network byte order */
static int parse_ipv4(const char *text, const char *end, unsigned long *address)
{
	unsigned long value = 0;
	int part;

	while (text < end && (*text == ' ' || *text == '\t'))
		text++;
	while (end > text && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	for (part = 0; part < 4; part++)
	{
		unsigned long number = 0;
		int digits = 0;

		while (text < end && *text >= '0' && *text <= '9' && digits < 3)
		{
			number = number * 10 + (unsigned long)(*text++ - '0');
			digits++;
		}
		if (!digits || number > 255)
			return 0;
		value = value << 8 | number;
		if (part < 3)
		{
			if (text >= end || *text != '.')
				return 0;
			text++;
		}
	}
	if (text != end)
		return 0;
	*address = halo_ws_htonl(value);
	return 1;
}

static void net_settings_read(void)
{
	const char *text;

	if (net_settings.read)
		return;
	text = config_string("network.address");
	if (text && *text)
	{
		unsigned long address;

		/* neither 0.0.0.0 nor 255.255.255.255 is a machine's address */
		if (parse_ipv4(text, text + strlen(text), &address) && address != 0 && address != INADDR_BROADCAST)
		{
			net_settings.local_address = address;
			net_settings.has_local_address = 1;
		}
		else
		{
			platform_log("network.address \"%s\" is not a usable IPv4 address: ignored", text);
		}
	}
	text = config_string("network.broadcast");
	while (text && *text)
	{
		const char *end = text + strcspn(text, ",");
		unsigned long address;

		if (parse_ipv4(text, end, &address) && address != 0)
		{
			if (net_settings.broadcast_count < MAXIMUM_BROADCAST_TARGETS)
				net_settings.broadcast_targets[net_settings.broadcast_count++] = address;
			else if (net_settings.broadcast_count++ == MAXIMUM_BROADCAST_TARGETS)
				platform_log("network.broadcast: only the first %d addresses are used", MAXIMUM_BROADCAST_TARGETS);
		}
		else if (end > text)
		{
			platform_log("network.broadcast: %.*s is not an IPv4 address: ignored", (int)(end - text), text);
		}
		text = *end ? end + 1 : end;
	}
	if (net_settings.broadcast_count > MAXIMUM_BROADCAST_TARGETS)
		net_settings.broadcast_count = MAXIMUM_BROADCAST_TARGETS;
	if (net_settings.has_local_address && !net_settings.broadcast_count)
	{
		platform_log("network.address without network.broadcast: sockets bound to one address "
			"receive no broadcasts, so this machine sees other machines' games and searches only if "
			"they list its address in their network.broadcast");
	}
	net_settings.read = 1;
}

/* the address to use in place of INADDR_ANY, if network.address is set */
static int local_address_setting(unsigned long *address)
{
	net_settings_read();
	if (!net_settings.has_local_address)
		return 0;
	*address = net_settings.local_address;
	return 1;
}

/* 127.0.0.1 in network byte order */
static unsigned long loopback_address(void)
{
	return halo_ws_htonl(0x7F000001);
}

/* a destination of 127.0.0.1 means the network.address address */
static const struct sockaddr *outgoing_address(const struct sockaddr *address, int address_length,
	struct sockaddr_in *storage)
{
	unsigned long local;

	if (address && address->sa_family == AF_INET && address_length >= (int)sizeof(*storage) &&
		((const struct sockaddr_in *)address)->sin_addr.s_addr == loopback_address() &&
		local_address_setting(&local))
	{
		memcpy(storage, address, sizeof(*storage));
		storage->sin_addr.s_addr = local;
		return (const struct sockaddr *)storage;
	}
	return address;
}

/* traffic from the network.address address comes from 127.0.0.1 */
static void incoming_address(struct sockaddr *address, const int *address_length)
{
	unsigned long local;

	if (address && address_length && *address_length >= (int)sizeof(struct sockaddr_in) &&
		address->sa_family == AF_INET && local_address_setting(&local) &&
		((struct sockaddr_in *)address)->sin_addr.s_addr == local)
	{
		((struct sockaddr_in *)address)->sin_addr.s_addr = loopback_address();
	}
}

/* a destination that is an internet play peer's goes to its stand-in (1);
-1, with WSAEHOSTUNREACH, if it is a peer's that cannot be reached now; 0
if it is not a peer's. connecting: the socket (UDP) is connected to it, else
-1 */
static int peer_outgoing_address(int stream, int connecting, const struct sockaddr **address, int address_length,
	struct sockaddr_in *storage)
{
	unsigned long ip;
	unsigned short port;
	int result;

	if (!*address || (*address)->sa_family != AF_INET || address_length < (int)sizeof(*storage))
		return 0;
	ip = ((const struct sockaddr_in *)*address)->sin_addr.s_addr;
	port = ((const struct sockaddr_in *)*address)->sin_port;
	result = p2p_outgoing(stream, connecting, &ip, &port);
	if (result < 0)
		WSASetLastError(WSAEHOSTUNREACH);
	if (result <= 0)
		return result;
	memcpy(storage, *address, sizeof(*storage));
	storage->sin_addr.s_addr = ip;
	storage->sin_port = port;
	*address = (const struct sockaddr *)storage;
	return 1;
}

/* tells internet play the local port the game's socket has (bound, given
one by a connection or a send, or listening) */
static void note_socket_port(SOCKET socket, int listening)
{
	struct sockaddr_in bound;
	int length = sizeof(bound);
	int type = SOCK_DGRAM;
	int type_length = sizeof(type);

	if (posix_socket_getsockname((int)socket, &bound, &length) < 0 || bound.sin_family != AF_INET ||
		!bound.sin_port)
	{
		return;
	}
	posix_socket_getsockopt((int)socket, SOL_SOCKET, SO_TYPE, &type, &type_length);
	/* (one bound to this machine alone, as the telnet console's, is not for
	peers to reach, and is not hosting; its port is still no stand-in's) */
	if (bound.sin_addr.s_addr == loopback_address())
		p2p_port_taken(type == SOCK_STREAM, bound.sin_port);
	else
		p2p_socket_port((int)socket, type == SOCK_STREAM, listening, bound.sin_port);
}

/* traffic from an internet play peer's stand-in comes from the peer, and
from the network.address address from 127.0.0.1 */
static void peer_incoming_address(int stream, struct sockaddr *address, const int *address_length)
{
	if (address && address_length && *address_length >= (int)sizeof(struct sockaddr_in) &&
		address->sa_family == AF_INET)
	{
		struct sockaddr_in *in = (struct sockaddr_in *)address;
		unsigned long ip = in->sin_addr.s_addr;
		unsigned short port = in->sin_port;

		if (p2p_incoming(stream, &ip, &port))
		{
			in->sin_addr.s_addr = ip;
			in->sin_port = port;
			return;
		}
	}
	incoming_address(address, address_length);
}

/* the local port the game's socket is bound to (network byte order), or 0
if it is not yet */
static unsigned short socket_port(SOCKET socket)
{
	struct sockaddr_in bound;
	int length = sizeof(bound);

	if (posix_socket_getsockname((int)socket, &bound, &length) < 0 || bound.sin_family != AF_INET)
		return 0;
	return bound.sin_port;
}

/* the addresses to send broadcasts to instead, if network.broadcast is
set (255.255.255.255 among them sends a real broadcast too); returns their
count */
static int broadcast_targets(unsigned long *targets, int maximum_count)
{
	int count;

	net_settings_read();
	count = net_settings.broadcast_count < maximum_count ? net_settings.broadcast_count : maximum_count;
	memcpy(targets, net_settings.broadcast_targets, (size_t)count * sizeof(*targets));
	return count;
}

/* ---------- Winsock */

/* (debug.network_latency, below) */
static int delayed_enabled(void);
static void delayed_closed(int socket);

static int winsock_result(int result)
{
	if (result < 0)
	{
		WSASetLastError(posix_socket_last_error());
		return SOCKET_ERROR;
	}
	return result;
}

static __thread int winsock_last_error;

int WSAAPI WSAGetLastError(void)
{
	return winsock_last_error;
}

void WSAAPI WSASetLastError(int error)
{
	winsock_last_error = error;
}

int WSAAPI WSAStartup(WORD version_requested, LPWSADATA data)
{
	unsigned long local;

	/* here, before the game's network threads start */
	net_settings_read();
	delayed_enabled();
	p2p_initialize(local_address_setting(&local) ? local : loopback_address());
	if (data)
	{
		memset(data, 0, sizeof(*data));
		data->wVersion = version_requested;
		data->wHighVersion = MAKEWORD(2, 2);
		data->iMaxSockets = 64;
		data->iMaxUdpDg = 1264;
	}
	return 0;
}

int WSAAPI WSACleanup(void)
{
	return 0;
}

SOCKET WSAAPI halo_ws_socket(int family, int type, int protocol)
{
	int result = posix_socket(family, type, protocol);

	if (result < 0)
	{
		WSASetLastError(posix_socket_last_error());
		return INVALID_SOCKET;
	}
	/* (the game's connections: every tick's messages go at once) */
	if (type == SOCK_STREAM)
		posix_socket_set_nodelay(result);
	return (SOCKET)result;
}

int WSAAPI halo_ws_closesocket(SOCKET socket)
{
	int type = SOCK_STREAM;
	int type_length = sizeof(type);

	posix_socket_getsockopt((int)socket, SOL_SOCKET, SO_TYPE, &type, &type_length);
	p2p_socket_closed((int)socket, type == SOCK_DGRAM ? socket_port(socket) : 0);
	delayed_closed((int)socket);
	return winsock_result(posix_socket_close((int)socket));
}

int WSAAPI halo_ws_bind(SOCKET socket, const struct sockaddr *address, int address_length)
{
	struct sockaddr_in local;
	unsigned long override;
	int result;

	if (address && address->sa_family == AF_INET && address_length >= (int)sizeof(local) &&
		((const struct sockaddr_in *)address)->sin_addr.s_addr == INADDR_ANY &&
		local_address_setting(&override))
	{
		memcpy(&local, address, sizeof(local));
		local.sin_addr.s_addr = override;
		address = (const struct sockaddr *)&local;
	}
	result = posix_socket_bind((int)socket, address, address_length);
	if (result == 0)
		note_socket_port(socket, 0);
	return winsock_result(result);
}

int WSAAPI halo_ws_connect(SOCKET socket, const struct sockaddr *address, int address_length)
{
	struct sockaddr_in target;
	unsigned long override;
	int type = SOCK_STREAM;
	int type_length = sizeof(type);
	int result;
	int error;

	/* an internet play peer's TCP or UDP port */
	if (address && address->sa_family == AF_INET &&
		(halo_ws_ntohl(((const struct sockaddr_in *)address)->sin_addr.s_addr) & 0xFFC00000) == 0x64400000)
	{
		posix_socket_getsockopt((int)socket, SOL_SOCKET, SO_TYPE, &type, &type_length);
	}
	switch (peer_outgoing_address(type == SOCK_STREAM, (int)socket, &address, address_length, &target))
	{
	case -1:
		return SOCKET_ERROR;
	case 0:
		address = outgoing_address(address, address_length, &target);
		break;
	}
	/* a connection from an unbound socket would leave from whichever address
	the route picks; with network.address it leaves from that address */
	if (address && address->sa_family == AF_INET && local_address_setting(&override))
	{
		struct sockaddr_in bound;
		int bound_length = sizeof(bound);

		if (posix_socket_getsockname((int)socket, &bound, &bound_length) < 0 ||
			(bound.sin_port == 0 && bound.sin_addr.s_addr == INADDR_ANY))
		{
			memset(&bound, 0, sizeof(bound));
			bound.sin_family = AF_INET;
			bound.sin_addr.s_addr = override;
			posix_socket_bind((int)socket, &bound, sizeof(bound));
		}
	}
	result = posix_socket_connect((int)socket, address, address_length);
	error = result < 0 ? posix_socket_last_error() : 0;
	/* the port the system gave it (a connection under way has one too, and
	a datagram socket's that failed keeps its: a stream socket's that failed
	has none) */
	note_socket_port(socket, 0);
	if (result < 0)
	{
		WSASetLastError(error);
		return SOCKET_ERROR;
	}
	return result;
}

int WSAAPI halo_ws_listen(SOCKET socket, int backlog)
{
	int result = posix_socket_listen((int)socket, backlog);

	/* (and it listens for connections while it hosts) */
	if (result == 0)
		note_socket_port(socket, 1);
	return winsock_result(result);
}

SOCKET WSAAPI halo_ws_accept(SOCKET socket, struct sockaddr *address, int *address_length)
{
	int result = posix_socket_accept((int)socket, address, address_length);

	if (result < 0)
	{
		WSASetLastError(posix_socket_last_error());
		return INVALID_SOCKET;
	}
	posix_socket_set_nodelay(result);
	peer_incoming_address(1, address, address_length);
	return (SOCKET)result;
}

int WSAAPI halo_ws_send(SOCKET socket, const char *buffer, int length, int flags)
{
	return winsock_result(posix_socket_send((int)socket, buffer, length, flags));
}

int WSAAPI halo_ws_sendto(SOCKET socket, const char *buffer, int length, int flags,
	const struct sockaddr *address, int address_length)
{
	/* the rewritten destination: it must outlive the send */
	struct sockaddr_in target;
	/* 0: not bound yet, which the first send binds it */
	unsigned short source_port = socket_port(socket);
	int result;
	int send_error;

	if (address && address->sa_family == AF_INET && address_length >= (int)sizeof(struct sockaddr_in) &&
		((const struct sockaddr_in *)address)->sin_addr.s_addr == INADDR_BROADCAST)
	{
		unsigned long targets[MAXIMUM_BROADCAST_TARGETS];
		unsigned short ports[P2P_BROADCAST_PEERS];
		int target_count = broadcast_targets(targets, MAXIMUM_BROADCAST_TARGETS);
		int peer_count;
		int index;

		/* one datagram per target; the broadcast counts as sent if any is */
		memcpy(&target, address, sizeof(target));
		if (target_count)
		{
			result = 0;
			for (index = 0; index < target_count; index++)
			{
				int sent;

				target.sin_addr.s_addr = targets[index];
				sent = posix_socket_sendto((int)socket, buffer, length, flags, &target, sizeof(target));
				if (sent >= 0 || index == 0)
					result = sent;
			}
		}
		else
		{
			result = posix_socket_sendto((int)socket, buffer, length, flags, address, address_length);
		}
		/* (the broadcast's own error, which what follows would overwrite) */
		send_error = result < 0 ? posix_socket_last_error() : 0;
		/* and to every internet play peer (after the send above, which binds
		the socket if it was not): onto the tunnel at once, else through the
		stand-ins */
		if (!source_port)
		{
			source_port = socket_port(socket);
			if (source_port)
				note_socket_port(socket, 0);
		}
		if (source_port)
		{
			if (p2p_broadcast_datagram(source_port, ((const struct sockaddr_in *)address)->sin_port, buffer,
				length) > 0 && result < 0)
			{
				result = length;
			}
			if (result < 0)
			{
				WSASetLastError(send_error);
				return SOCKET_ERROR;
			}
			return result;
		}
		peer_count = p2p_broadcast_targets(((const struct sockaddr_in *)address)->sin_port, targets, ports,
			P2P_BROADCAST_PEERS);
		for (index = 0; index < peer_count; index++)
		{
			int sent;

			target.sin_addr.s_addr = targets[index];
			target.sin_port = ports[index];
			sent = posix_socket_sendto((int)socket, buffer, length, flags, &target, sizeof(target));
			if (result < 0 && sent >= 0)
				result = sent;
		}
		if (result < 0)
		{
			WSASetLastError(send_error);
			return SOCKET_ERROR;
		}
		return result;
	}
	/* an internet play peer's: onto the tunnel at once, from the socket's
	port (one not bound yet goes through a stand-in, which the system's send
	binds it for) */
	if (address && address->sa_family == AF_INET && address_length >= (int)sizeof(struct sockaddr_in) &&
		(halo_ws_ntohl(((const struct sockaddr_in *)address)->sin_addr.s_addr) & 0xFFC00000) == 0x64400000)
	{
		switch (source_port ? p2p_send_datagram(source_port, ((const struct sockaddr_in *)address)->sin_addr.s_addr,
			((const struct sockaddr_in *)address)->sin_port, buffer, length) : 0)
		{
		case 1:
			return length;
		case -1:
			WSASetLastError(WSAEHOSTUNREACH);
			return SOCKET_ERROR;
		}
	}
	switch (peer_outgoing_address(0, -1, &address, address_length, &target))
	{
	case -1:
		return SOCKET_ERROR;
	case 0:
		address = outgoing_address(address, address_length, &target);
		break;
	}
	result = posix_socket_sendto((int)socket, buffer, length, flags, address, address_length);
	send_error = result < 0 ? posix_socket_last_error() : 0;
	/* the port the send gave it (one that failed binds it too) */
	if (!source_port)
		note_socket_port(socket, 0);
	if (result < 0)
	{
		WSASetLastError(send_error);
		return SOCKET_ERROR;
	}
	return result;
}

/* debug.network_latency and debug.network_loss: what this machine receives
is held back that many milliseconds (both ways between two machines: a
round trip of twice it) and that share of its datagrams lost, to test the
netcode as over the internet */
#define DELAYED_PACKET_SIZE 1500
#define MAXIMUM_DELAYED_PACKETS 4096

struct delayed_packet
{
	int socket;
	int length;
	int offset;
	int address_length;
	DWORD time;
	/* (large enough for any address the sockets give) */
	char address[128];
	char data[DELAYED_PACKET_SIZE];
};

static struct
{
	int checked;
	DWORD latency;
	int loss_percent;
	struct delayed_packet *packets;
	int first;
	int count;
} delayed;

/* (the game's network threads share the queue) */
static pthread_mutex_t delayed_lock = PTHREAD_MUTEX_INITIALIZER;

/* first from WSAStartup, before the game's network threads */
static int delayed_enabled(void)
{
	if (!delayed.checked)
	{
		double latency = config_real("debug.network_latency");
		double loss = config_real("debug.network_loss");

		/* up to ten seconds, and a share */
		latency = !(latency > 0.0) ? 0.0 : latency > 10000.0 ? 10000.0 : latency;
		loss = !(loss > 0.0) ? 0.0 : loss > 100.0 ? 100.0 : loss;
		delayed.latency = (DWORD)latency;
		delayed.loss_percent = (int)loss;
		delayed.checked = 1;
		if (delayed.latency || delayed.loss_percent)
		{
			delayed.packets = calloc(MAXIMUM_DELAYED_PACKETS, sizeof(*delayed.packets));
			platform_log("network: receiving %lu ms late, losing %d%% of datagrams (testing)",
				(unsigned long)delayed.latency, delayed.loss_percent);
		}
	}
	return delayed.packets != NULL;
}

/* drops what is held back for socket, and what has been read (socket -1),
keeping the rest in order */
static void delayed_drop(int socket)
{
	int index, kept = 0;

	for (index = 0; index < delayed.count; index++)
	{
		struct delayed_packet *packet = &delayed.packets[(delayed.first + index) % MAXIMUM_DELAYED_PACKETS];

		if (packet->socket == socket || packet->socket == -1)
			continue;
		if (kept != index)
			delayed.packets[(delayed.first + kept) % MAXIMUM_DELAYED_PACKETS] = *packet;
		kept++;
	}
	delayed.count = kept;
}

/* the socket closes: what it had goes (and a socket given its number later
must not have it) */
static void delayed_closed(int socket)
{
	if (!delayed.packets)
		return;
	pthread_mutex_lock(&delayed_lock);
	delayed_drop(socket);
	pthread_mutex_unlock(&delayed_lock);
}

/* whether something held back for the socket is due */
static int delayed_due(int socket)
{
	int result = 0;
	int index;

	pthread_mutex_lock(&delayed_lock);
	for (index = 0; index < delayed.count; index++)
	{
		struct delayed_packet const *packet = &delayed.packets[(delayed.first + index) % MAXIMUM_DELAYED_PACKETS];

		if (packet->socket == socket)
		{
			result = GetTickCount() - packet->time >= delayed.latency;
			break;
		}
	}
	pthread_mutex_unlock(&delayed_lock);
	return result;
}

/* what the socket has now, held back; then the oldest of the socket's that
has waited long enough, or would-block */
static int delayed_receive_locked(SOCKET socket, char *buffer, int length, int flags,
	struct sockaddr *address, int *address_length, int datagram)
{
	int index;

	/* (full: what was read, behind what was not, goes) */
	if (delayed.count == MAXIMUM_DELAYED_PACKETS)
		delayed_drop(-1);
	while (delayed.count < MAXIMUM_DELAYED_PACKETS)
	{
		struct delayed_packet *packet = &delayed.packets[(delayed.first + delayed.count) % MAXIMUM_DELAYED_PACKETS];
		int address_size = (int)sizeof(packet->address);
		int readable = (int)socket;
		int readable_count = 1;
		int result;

		/* (only what is there now: some of the game's sockets block) */
		if (posix_socket_select(&readable, &readable_count, NULL, NULL, NULL, NULL, 0, 0, 0) <= 0 ||
			readable_count == 0)
		{
			break;
		}
		result = datagram ?
			posix_socket_recvfrom((int)socket, packet->data, DELAYED_PACKET_SIZE, flags,
				(struct sockaddr *)&packet->address, &address_size) :
			posix_socket_recv((int)socket, packet->data, DELAYED_PACKET_SIZE, flags);

		if (result < 0)
		{
			/* (an error but would-block is the caller's now) */
			if (posix_socket_last_error() != WSAEWOULDBLOCK)
				return winsock_result(result);
			break;
		}
		/* (a stream closing is at once) */
		if (!datagram && result == 0)
			return 0;
		if (datagram && rand() % 100 < delayed.loss_percent)
			continue;
		packet->socket = (int)socket;
		packet->length = result;
		packet->offset = 0;
		packet->address_length = address_size;
		packet->time = GetTickCount();
		delayed.count++;
	}
	for (index = 0; index < delayed.count; index++)
	{
		struct delayed_packet *packet = &delayed.packets[(delayed.first + index) % MAXIMUM_DELAYED_PACKETS];
		int size;
		int truncated;

		if (packet->socket != (int)socket)
			continue;
		if (GetTickCount() - packet->time < delayed.latency)
			break;
		size = packet->length - packet->offset;
		/* (a datagram larger than the buffer: its start, and WSAEMSGSIZE, as
		Winsock does) */
		truncated = datagram && size > length;
		if (size > length)
			size = length;
		memcpy(buffer, packet->data + packet->offset, (size_t)size);
		if (datagram && address && address_length)
		{
			int copied = packet->address_length < *address_length ? packet->address_length : *address_length;

			memcpy(address, &packet->address, (size_t)copied);
			*address_length = copied;
			peer_incoming_address(0, address, address_length);
		}
		packet->offset += size;
		if (datagram || packet->offset >= packet->length)
			packet->socket = -1;
		/* (the spent ones at the front go) */
		while (delayed.count > 0 && delayed.packets[delayed.first].socket == -1)
		{
			delayed.first = (delayed.first + 1) % MAXIMUM_DELAYED_PACKETS;
			delayed.count--;
		}
		if (truncated)
		{
			WSASetLastError(WSAEMSGSIZE);
			return SOCKET_ERROR;
		}
		return size;
	}
	WSASetLastError(WSAEWOULDBLOCK);
	return SOCKET_ERROR;
}

static int delayed_receive(SOCKET socket, char *buffer, int length, int flags,
	struct sockaddr *address, int *address_length, int datagram)
{
	int result;

	pthread_mutex_lock(&delayed_lock);
	result = delayed_receive_locked(socket, buffer, length, flags, address, address_length, datagram);
	pthread_mutex_unlock(&delayed_lock);
	return result;
}

int WSAAPI halo_ws_recv(SOCKET socket, char *buffer, int length, int flags)
{
	if (delayed_enabled())
		return delayed_receive(socket, buffer, length, flags, NULL, NULL, 0);
	return winsock_result(posix_socket_recv((int)socket, buffer, length, flags));
}

int WSAAPI halo_ws_recvfrom(SOCKET socket, char *buffer, int length, int flags,
	struct sockaddr *address, int *address_length)
{
	int result;

	if (delayed_enabled())
		return delayed_receive(socket, buffer, length, flags, address, address_length, 1);
	result = posix_socket_recvfrom((int)socket, buffer, length, flags, address, address_length);
	if (result >= 0)
		peer_incoming_address(0, address, address_length);
	return winsock_result(result);
}

int WSAAPI halo_ws_shutdown(SOCKET socket, int how)
{
	return winsock_result(posix_socket_shutdown((int)socket, how));
}

int WSAAPI halo_ws_ioctlsocket(SOCKET socket, long command, u_long *argument)
{
	switch ((unsigned long)command)
	{
	case (unsigned long)FIONBIO:
		return winsock_result(posix_socket_set_nonblocking((int)socket, *argument != 0));
	case (unsigned long)FIONREAD:
		return winsock_result(posix_socket_bytes_available((int)socket, argument));
	default:
		WSASetLastError(WSAEINVAL);
		return SOCKET_ERROR;
	}
}

int WSAAPI halo_ws_setsockopt(SOCKET socket, int level, int name, const char *value, int length)
{
	return winsock_result(posix_socket_setsockopt((int)socket, level, name, value, length));
}

int WSAAPI halo_ws_getsockopt(SOCKET socket, int level, int name, char *value, int *length)
{
	return winsock_result(posix_socket_getsockopt((int)socket, level, name, value, length));
}

int WSAAPI halo_ws_getsockname(SOCKET socket, struct sockaddr *address, int *address_length)
{
	return winsock_result(posix_socket_getsockname((int)socket, address, address_length));
}

int WSAAPI halo_ws_getpeername(SOCKET socket, struct sockaddr *address, int *address_length)
{
	int result = posix_socket_getpeername((int)socket, address, address_length);

	if (result >= 0)
		peer_incoming_address(1, address, address_length);
	return winsock_result(result);
}

/* ---------- select and fd_set */

static int descriptors_from_set(halo_ws_fd_set *set, int *descriptors)
{
	u_int index;

	for (index = 0; index < set->fd_count && index < FD_SETSIZE; index++)
		descriptors[index] = (int)set->fd_array[index];
	return (int)index;
}

static void set_from_descriptors(halo_ws_fd_set *set, const int *descriptors, int count)
{
	int index;

	for (index = 0; index < count; index++)
		set->fd_array[index] = (SOCKET)descriptors[index];
	set->fd_count = (u_int)count;
}

int WSAAPI halo_ws_select(int descriptor_count, halo_ws_fd_set *read_set, halo_ws_fd_set *write_set,
	halo_ws_fd_set *error_set, const struct halo_ws_timeval *timeout)
{
	int read[FD_SETSIZE], write[FD_SETSIZE], error[FD_SETSIZE];
	int read_count = read_set ? descriptors_from_set(read_set, read) : 0;
	int write_count = write_set ? descriptors_from_set(write_set, write) : 0;
	int error_count = error_set ? descriptors_from_set(error_set, error) : 0;
	int result;
	int asked[FD_SETSIZE];
	int asked_count = read_count;

	(void)descriptor_count;
	memcpy(asked, read, sizeof(int) * (size_t)read_count);
	result = posix_socket_select(
		read_set ? read : NULL, &read_count,
		write_set ? write : NULL, &write_count,
		error_set ? error : NULL, &error_count,
		timeout ? timeout->tv_sec : 0, timeout ? timeout->tv_usec : 0, timeout == NULL);
	if (result < 0)
		return winsock_result(result);
	/* (debug.network_latency: what is held back and due is there to read) */
	if (read_set && delayed_enabled())
	{
		int asked_index;

		for (asked_index = 0; asked_index < asked_count; asked_index++)
		{
			int index;
			int present = 0;

			for (index = 0; index < read_count; index++)
				present |= read[index] == asked[asked_index];
			if (!present && read_count < FD_SETSIZE && delayed_due(asked[asked_index]))
			{
				read[read_count++] = asked[asked_index];
				result++;
			}
		}
	}
	if (read_set)
		set_from_descriptors(read_set, read, read_count);
	if (write_set)
		set_from_descriptors(write_set, write, write_count);
	if (error_set)
		set_from_descriptors(error_set, error, error_count);
	return result;
}

int PASCAL __WSAFDIsSet(SOCKET socket, halo_ws_fd_set *set)
{
	u_int index;

	for (index = 0; index < set->fd_count; index++)
	{
		if (set->fd_array[index] == socket)
			return 1;
	}
	return 0;
}

/* ---------- byte order and addresses */

u_long WSAAPI halo_ws_htonl(u_long value)
{
	return __builtin_bswap32(value);
}

u_long WSAAPI halo_ws_ntohl(u_long value)
{
	return __builtin_bswap32(value);
}

u_short WSAAPI halo_ws_htons(u_short value)
{
	return (u_short)((value << 8) | (value >> 8));
}

u_short WSAAPI halo_ws_ntohs(u_short value)
{
	return (u_short)((value << 8) | (value >> 8));
}

unsigned long WSAAPI halo_ws_inet_addr(const char *text)
{
	unsigned long parts[4];
	int count = 0;

	while (count < 4)
	{
		unsigned long value = 0;
		int digits = 0;

		while (*text >= '0' && *text <= '9')
		{
			value = value * 10 + (unsigned long)(*text++ - '0');
			digits++;
		}
		if (!digits || value > 255)
			return INADDR_NONE;
		parts[count++] = value;
		if (*text != '.')
			break;
		text++;
	}
	if (count != 4 || *text)
		return INADDR_NONE;
	/* network byte order on a little-endian host */
	return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

/* ---------- XNet */

INT WSAAPI XNetStartup(const XNetStartupParams *parameters)
{
	(void)parameters;
	return 0;
}

INT WSAAPI XNetCleanup(void)
{
	return 0;
}

INT WSAAPI XNetRandom(BYTE *buffer, UINT size)
{
	posix_random_bytes(buffer, size);
	return 0;
}

INT WSAAPI XNetCreateKey(XNKID *key_identifier, XNKEY *key)
{
	posix_random_bytes(key_identifier, sizeof(*key_identifier));
	posix_random_bytes(key, sizeof(*key));
	return 0;
}

INT WSAAPI XNetRegisterKey(const XNKID *key_identifier, const XNKEY *key)
{
	(void)key_identifier;
	(void)key;
	return 0;
}

INT WSAAPI XNetUnregisterKey(const XNKID *key_identifier)
{
	(void)key_identifier;
	return 0;
}

INT WSAAPI XNetXnAddrToInAddr(const XNADDR *address, const XNKID *key_identifier, IN_ADDR *result)
{
	unsigned long peer;
#ifdef HALO_WEB
	struct web_shared_state *web = web_shared_state();
	if (__atomic_load_n(&web->gateway_enabled, __ATOMIC_ACQUIRE))
	{
		unsigned int identifier[2] = {0, 0};
		int index;
		memcpy(identifier, address->abEnet, 6);
		for (index = 0; index < WEB_GATEWAY_PEERS; index++)
		{
			unsigned int ip = __atomic_load_n(&web->gateway_peers[index][2], __ATOMIC_ACQUIRE);
			if (ip && identifier[0] == (unsigned int)__atomic_load_n(&web->gateway_peers[index][0], __ATOMIC_RELAXED)
				&& identifier[1] == (unsigned int)__atomic_load_n(&web->gateway_peers[index][1], __ATOMIC_RELAXED))
			{
				result->s_addr = ip;
				return 0;
			}
		}
		*result = address->ina;
		return 0;
	}
#endif

	(void)key_identifier;
	/* an internet play peer's XNADDR carries its identifier */
	if (p2p_peer_address(address->abEnet, &peer))
		result->s_addr = peer;
	else
		*result = address->ina;
	return 0;
}

/* this machine's system link address: network.address, else the first
non-loopback IPv4 address */
static unsigned long title_address(void)
{
	unsigned long override;

	if (local_address_setting(&override))
		return override;
	return posix_local_ipv4_address();
}

DWORD WSAAPI XNetGetTitleXnAddr(XNADDR *address)
{
	unsigned long ip = title_address();

	memset(address, 0, sizeof(*address));
	address->bSizeOfStruct = sizeof(*address);
	address->ina.s_addr = ip;
	memcpy(address->abEnet, p2p_identifier(), sizeof(address->abEnet));
#ifdef HALO_WEB
	{
		struct web_shared_state *web = web_shared_state();
		if (__atomic_load_n(&web->gateway_enabled, __ATOMIC_ACQUIRE))
		{
			unsigned int identifier[2];
			identifier[0] = __atomic_load_n(&web->gateway_identifier[0], __ATOMIC_RELAXED);
			identifier[1] = __atomic_load_n(&web->gateway_identifier[1], __ATOMIC_RELAXED);
			memcpy(address->abEnet, identifier, sizeof(address->abEnet));
		}
	}
#endif
	return ip ? (XNET_GET_XNADDR_ETHERNET | XNET_GET_XNADDR_DHCP) : XNET_GET_XNADDR_ETHERNET;
}

DWORD WSAAPI XNetGetEthernetLinkStatus(void)
{
	return title_address() ?
		(XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS | XNET_ETHERNET_LINK_FULL_DUPLEX) : 0;
}
