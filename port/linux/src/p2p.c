/*
P2P.C

Internet play: machines that shared an invite reach each other's system
link games as if they were on one LAN, without a server of this project's.

- An invite is a link, halo://join/<host><token>: the hash of the X25519
  public key the hosting machine makes each run (16 bytes of it, which no
  other key can be found to have; the first 6 are the machine's identifier,
  which its XNADDR also carries) and a random 16-byte token. A machine
  makes one when its game starts hosting (the game listens for
  connections), logs it, puts it on the clipboard, and offers it through
  Discord (p2p_discord.c). Nothing about a game is published anywhere else:
  without an invite there is no way to find or join it.
- Signalling (p2p_signal.c) goes through public MQTT brokers, on topics
  that are hashes of the token, with messages sealed with a key derived
  from it (p2p_crypto.c). A joiner offers its public key and the addresses
  it can be reached at; the host answers with its own, and makes the
  session once the joiner, answered, proves that it holds its key. The
  secret of their session comes from the two keys (X25519) and a nonce of
  each, and never travels.
- The tunnel is one UDP socket. Each machine learns its public address from
  public STUN servers, and both then send to each other's addresses until
  packets get through (hole punching). There is no relay: two machines whose
  NATs both map every destination to a new port cannot connect, unless a
  router forwards one of them a port. So a host asks its router to forward
  the tunnel's port (UPnP, posix_upnp.c) as soon as a player reaches out
  with its invite, and a joiner asks its own when it has not reached the
  host in a few seconds (network.allow_upnp); the forwarded port is one more
  of the addresses a machine offers (the joiner asks the host again every
  few seconds until they meet, and the host answers with them all). Every
  tunnel packet is sealed with a key of the session's for its direction,
  its header (the sender and the packet's number, the nonce) authenticated
  too, and a packet already received, or from further back than the last
  64, is dropped.
- Each peer gets a virtual address in 100.64.0.0/10, which the game sees
  (XNetXnAddrToInAddr maps the peer's XNADDR to it). The game's datagrams
  to a peer go onto the tunnel from xnet.c at once (p2p_send_datagram);
  otherwise xnet.c rewrites the game's destinations there to local
  stand-ins: a UDP socket here per peer and port forwards datagrams (the
  peer's to the game, and the game's from a socket connected to the
  peer), and a TCP listener per peer and port takes the game's connections,
  carried reliably over the tunnel by KCP (port/third_party/kcp). Traffic
  arriving from a peer leaves these stand-ins, and xnet.c reports it as
  coming from the peer's address. The
  game's broadcasts also go to every peer, so a host's game shows up in its
  joiners' system link lists, and joining works as on a LAN. A peer reaches
  only the game's own ports (xnet.c says which it has), and has a few
  stand-ins and connections at a time.

The work happens on a thread of its own, under p2p_lock (let go of for
whatever may wait: DNS, the desktop's link handlers); the game's threads
only look up and create stand-ins.
*/

#ifdef HALO_RELAY
#include "relay_platform.h"
#else
#include "platform.h"
#endif
#include "posix.h"
#include "port_config.h"
#include "p2p_internal.h"
#include "ikcp.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
	/* the tunnel's version 2 (0x68 was the first) */
	TUNNEL_MAGIC = 0x69,
	/* the magic, the sender's identifier, and the packet's number */
	TUNNEL_HEADER_SIZE = 1 + P2P_IDENTIFIER_SIZE + 8,
	/* a tunnel packet's plaintext: a type, and at most the game's largest
	datagram (WSAStartup's iMaxUdpDg) with its ports */
	MAXIMUM_INNER_SIZE = 1400,
	MAXIMUM_PACKET_SIZE = TUNNEL_HEADER_SIZE + MAXIMUM_INNER_SIZE + P2P_TAG_SIZE,
	/* the packets received out of order that are still taken */
	REPLAY_WINDOW = 64,

	/* a host needs a UDP stand-in for two or three ports of every other
	machine, and a stream for each one's connection; one peer can have no
	more than these */
	MAXIMUM_PROXIES = 512,
	MAXIMUM_PEER_PROXIES = 4,
	MAXIMUM_LISTENERS = 64,
	MAXIMUM_STREAMS = 160,
	MAXIMUM_PEER_STREAMS = 4,
	MAXIMUM_PEER_OPENING_STREAMS = 2,
	/* the game's sockets' ports (xnet.c), and the ports it sent peers
	datagrams from */
	MAXIMUM_GAME_PORTS = 64,
	MAXIMUM_SENT_PORTS = 16,
	/* sessions that ended, which do not come back */
	MAXIMUM_RETIRED_SESSIONS = 16,
	/* the players a host is reaching at once (anyone with the invite can
	ask in any number of made-up names: each would have the host send to the
	addresses it gives) */
	MAXIMUM_OPENING_PEERS = 8,
	/* stand-ins closed lately, whose traffic the game may not have read yet
	(p2p_incoming) */
	MAXIMUM_CLOSED_PORTS = 128,
	CLOSED_PORT_TIME = 5000,
	/* a peer's stand-in used this lately is not closed for another: a peer
	sending from ever new ports has a few a second, not one a datagram */
	PROXY_REPLACE_TIME = 1000,
	MAXIMUM_STUN_SERVERS = 4,
	STREAM_BUFFER_SIZE = 16384,
	STREAM_CHUNK_SIZE = 1024,
	/* the most unacknowledged messages a stream sends ahead */
	STREAM_WINDOW = 128,
	KCP_MTU = 1200,

	/* milliseconds */
	LOOP_INTERVAL = 10,
	PUNCH_INTERVAL = 200,
	PING_INTERVAL = 1000,
	ENDPOINT_SWITCH_TIME = 3000,
	PEER_TIMEOUT = 20000,
	PUNCH_TIMEOUT = 30000,
	JOIN_TIMEOUT = 90000,
	STREAM_LINGER_TIME = 10000,
	PROXY_IDLE_TIME = 60000,
	STUN_RETRY_INTERVAL = 500,
	STUN_REFRESH_INTERVAL = 25000,
	STUN_ATTEMPTS = 6,
	/* a joiner asks its router to forward the tunnel's port when it has not
	reached a peer in this long; a forwarding is renewed this often (its
	lease is an hour), and one refused asked for again this long after */
	UPNP_JOIN_DELAY = 5000,
	UPNP_RENEW_INTERVAL = 30 * 60 * 1000,
	UPNP_RETRY_INTERVAL = 5 * 60 * 1000,
	/* how long the game's exit waits for a request under way */
	UPNP_RELEASE_WAIT = 3000,

	/* where a running copy of the game takes invites from another one
	started to open a link (127.0.0.1), sealed with a key of the user's */
	HANDOFF_PORT = 47315,
};

enum
{
	_packet_ping = 1,
	_packet_pong,
	_packet_datagram,
	_packet_stream,
	_packet_bye,
};

/* the messages of a stream, inside KCP */
enum
{
	_stream_open = 'O',
	_stream_data = 'D',
	_stream_close = 'C',
};

enum
{
	/* a peer's stream whose open has not arrived */
	_stream_awaiting_open,
	/* connecting to the local game */
	_stream_connecting,
	_stream_open_state,
};

struct peer
{
	int used;
	unsigned char identifier[P2P_IDENTIFIER_SIZE];
	char name[2 * P2P_IDENTIFIER_SIZE + 1];
	/* the session's secret, and the keys from it for each direction */
	unsigned char secret[P2P_SHA256_SIZE];
	unsigned char send_key[P2P_SHA256_SIZE];
	unsigned char receive_key[P2P_SHA256_SIZE];
	/* the last packet number sent; the highest received, and which of the
	REPLAY_WINDOW before it were (bit n: highest - n) */
	unsigned long long send_counter;
	unsigned long long receive_highest;
	unsigned long long receive_window;
	unsigned long virtual_address;
	int is_host;
	int connected;
	struct p2p_candidate candidates[P2P_MAXIMUM_CANDIDATES];
	int candidate_count;
	struct p2p_candidate endpoint;
	/* its UDP stand-ins (indices in p2p.proxies) */
	short proxies[MAXIMUM_PEER_PROXIES];
	int proxy_count;
	unsigned long offered_time;
	unsigned long heard_time;
	unsigned long endpoint_heard_time;
	unsigned long sent_time;
	unsigned long round_trip;
};

/* a UDP stand-in for one port of a peer */
struct proxy
{
	int socket;
	int peer;
	unsigned short remote_port;
	unsigned short local_port;
	unsigned long used_time;
	/* the game's socket connected to it, or -1: while there is one, it
	stays */
	int pinned_socket;
};

/* a port of the game's (xnet.c) */
struct game_port
{
	int socket;
	/* 0: none */
	unsigned short port;
	unsigned char stream;
	unsigned char listening;
};

/* a stand-in closed lately (p2p_incoming) */
struct closed_port
{
	unsigned short local_port;
	unsigned short remote_port;
	int stream;
	unsigned long virtual_address;
	unsigned long time;
};

struct retired_session
{
	unsigned char secret[P2P_SHA256_SIZE];
	unsigned long virtual_address;
};

/* a TCP stand-in for one port of a peer: takes the game's connections */
struct listener
{
	int socket;
	int peer;
	unsigned short remote_port;
	unsigned short local_port;
};

/* one of the game's TCP connections, carried over the tunnel */
struct stream
{
	int used;
	int peer;
	IUINT32 conversation;
	ikcpcb *kcp;
	int state;
	/* the local end: the game's connection to a listener, or a connection
	to the game made for a peer's (whose port stands for the peer's) */
	int socket;
	unsigned short local_port;
	unsigned short remote_port;
	int local_closed;
	int remote_closed;
	unsigned long created_time;
	unsigned long closed_time;
	unsigned char pending[STREAM_BUFFER_SIZE];
	int pending_size;
};

struct stun_server
{
	char host[128];
	unsigned short port;
	unsigned long address;
	unsigned char transaction[12];
	int attempts;
	unsigned long sent_time;
	int has_mapped;
	struct p2p_candidate mapped;
};

static pthread_mutex_t p2p_lock = PTHREAD_MUTEX_INITIALIZER;

static struct
{
	int running;
	unsigned long local_address;
	int tunnel_socket;
	unsigned short tunnel_port;
	int handoff_socket;

	struct peer peers[P2P_MAXIMUM_PEERS];
	struct retired_session retired[MAXIMUM_RETIRED_SESSIONS];
	int retired_next;
	struct proxy proxies[MAXIMUM_PROXIES];
	struct listener listeners[MAXIMUM_LISTENERS];
	struct stream streams[MAXIMUM_STREAMS];
	/* recently finished streams, whose late packets are ignored */
	IUINT32 finished[16];
	int finished_next;
	struct closed_port closed[MAXIMUM_CLOSED_PORTS];
	int closed_next;
	/* what peers may reach */
	struct game_port game_ports[MAXIMUM_GAME_PORTS];
	unsigned short sent_ports[MAXIMUM_SENT_PORTS];
	int sent_port_next;
	/* the key of invites handed over (handoff_readable) */
	int has_handoff_key;
	unsigned char handoff_key[P2P_SHA256_SIZE];

	struct stun_server stun[MAXIMUM_STUN_SERVERS];
	int stun_count;
	/* from the first time a game is hosted or joined */
	int stun_started;
	int reported_symmetric;

	/* hosting: while the game listens on hosting_socket */
	int hosting_socket;
	int hosting;
	int has_token;
	unsigned char token[P2P_TOKEN_SIZE];
	char invite[P2P_LINK_SIZE];
	int invite_copied;
	/* the game's players and its most (p2p_set_game_player_counts; 0: not
	said, and the machines the tunnel reaches are shown), and what Discord
	was told */
	int game_player_count;
	int game_player_maximum;
	int reported_player_count;
	int reported_player_maximum;

	/* joining: until the host is reached, or JOIN_TIMEOUT */
	int join_requested;
	int joining;
	unsigned char join_host[P2P_IDENTIFIER_SIZE];
	unsigned char join_host_hash[P2P_KEY_HASH_SIZE];
	unsigned char join_token[P2P_TOKEN_SIZE];
	unsigned long join_time;

	char clipboard[P2P_LINK_SIZE];
	int has_clipboard;

	/* UPnP (posix_upnp.c): a thread asking the router; the port it forwards
	here, and when it was last asked */
	int upnp_working;
	int upnp_asked;
	int upnp_forwarded;
	int upnp_release_registered;
	int upnp_released;
	struct p2p_candidate upnp_candidate;
	unsigned long upnp_time;
} p2p = { 0, 0, -1, 0, -1, .hosting_socket = -1 };

/* the proxy (its index + 1) with each local port (all of theirs are on
p2p.local_address) */
static unsigned short proxy_by_port[65536];

static unsigned char identifier[P2P_IDENTIFIER_SIZE];
/* this run's X25519 keys, which the identifier comes from */
static unsigned char secret_key[P2P_KEY_SIZE];
static unsigned char public_key[P2P_KEY_SIZE];
static int has_identifier;

/* ---------- helpers */

unsigned long p2p_now(void)
{
	return GetTickCount();
}

static int elapsed(unsigned long since, unsigned long time)
{
	/* (unsigned, as the clock wraps: a signed difference is negative for
	half of it, which had nothing lapse from a time of 0 from 24.8 days of
	uptime on) */
	return (unsigned int)(p2p_now() - since) >= (unsigned int)time;
}

unsigned long p2p_resolve(const char *host)
{
	unsigned long address;

	/* DNS can take seconds, which the game's threads must not wait for */
	pthread_mutex_unlock(&p2p_lock);
	address = posix_resolve_ipv4(host);
	pthread_mutex_lock(&p2p_lock);
	return address;
}

void p2p_register_url_scheme(const char *scheme, const char *description)
{
	if (config_real("debug.exit_after") > 0.0 || config_boolean("debug.hidden_window") ||
		config_boolean("debug.null_renderer"))
		return;
	/* it may run a program and wait for it */
	pthread_mutex_unlock(&p2p_lock);
	posix_register_url_scheme(scheme, description);
	pthread_mutex_lock(&p2p_lock);
}

void p2p_hex(const unsigned char *bytes, int size, char *text)
{
	static const char digits[] = "0123456789abcdef";
	int index;

	for (index = 0; index < size; index++)
	{
		text[index * 2] = digits[bytes[index] >> 4];
		text[index * 2 + 1] = digits[bytes[index] & 15];
	}
	text[size * 2] = 0;
}

static int hex_value(char digit)
{
	if (digit >= '0' && digit <= '9')
		return digit - '0';
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;
	if (digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;
	return -1;
}

static unsigned long network_long(unsigned long value)
{
	return __builtin_bswap32(value);
}

static unsigned short network_short(unsigned short value)
{
	return (unsigned short)(value << 8 | value >> 8);
}

static void put_short(unsigned char *bytes, unsigned short value)
{
	/* value is in network byte order already */
	memcpy(bytes, &value, 2);
}

static unsigned short get_short(const unsigned char *bytes)
{
	unsigned short value;

	memcpy(&value, bytes, 2);
	return value;
}

static void make_address(struct sockaddr_in *address, unsigned long ip, unsigned short port)
{
	memset(address, 0, sizeof(*address));
	address->sin_family = AF_INET;
	address->sin_port = port;
	address->sin_addr.s_addr = ip;
}

static const char *address_text(unsigned long ip, unsigned short port, char *text)
{
	unsigned long value = network_long(ip);

	sprintf(text, "%lu.%lu.%lu.%lu:%u", value >> 24, (value >> 16) & 255, (value >> 8) & 255, value & 255,
		network_short(port));
	return text;
}

/* a UDP or TCP socket bound to ip:port (0 for any), not blocking; its port
through bound_port; -1 on failure */
static int open_socket(int type, unsigned long ip, unsigned short port, unsigned short *bound_port)
{
	struct sockaddr_in address;
	int length = sizeof(address);
	int result = posix_socket(AF_INET, type, 0);

	if (result < 0)
		return -1;
	make_address(&address, ip, port);
	if (posix_socket_bind(result, &address, sizeof(address)) < 0 ||
		posix_socket_getsockname(result, &address, &length) < 0 ||
		posix_socket_set_nonblocking(result, 1) < 0)
	{
		posix_socket_close(result);
		return -1;
	}
	/* (the game's connections, carried over the tunnel: nothing held back) */
	if (type == SOCK_STREAM)
		posix_socket_set_nodelay(result);
	if (bound_port)
		*bound_port = address.sin_port;
	return result;
}

static int would_block(void)
{
	int error = posix_socket_last_error();

	return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

const unsigned char *p2p_identifier(void)
{
	/* its own lock: the p2p thread asks while holding p2p_lock */
	static pthread_mutex_t identifier_lock = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&identifier_lock);
	if (!has_identifier)
	{
		posix_random_bytes(secret_key, sizeof(secret_key));
		p2p_x25519(public_key, secret_key, NULL);
		p2p_identifier_for(public_key, identifier);
		has_identifier = 1;
	}
	pthread_mutex_unlock(&identifier_lock);
	return identifier;
}

void p2p_key_hash(const unsigned char *key, unsigned char *hash)
{
	unsigned char digest[P2P_SHA256_SIZE];

	p2p_sha256(key, P2P_KEY_SIZE, digest);
	memcpy(hash, digest, P2P_KEY_HASH_SIZE);
}

void p2p_identifier_from_hash(const unsigned char *hash, unsigned char *result)
{
	memcpy(result, hash, P2P_IDENTIFIER_SIZE);
	/* like a locally administered unicast MAC address, as XNADDR's abEnet
	holds one */
	result[0] = (unsigned char)((result[0] & 0xFC) | 0x02);
}

void p2p_identifier_for(const unsigned char *key, unsigned char *result)
{
	unsigned char hash[P2P_KEY_HASH_SIZE];

	p2p_key_hash(key, hash);
	p2p_identifier_from_hash(hash, result);
}

const unsigned char *p2p_public_key(void)
{
	p2p_identifier();
	return public_key;
}

int p2p_shared_secret(const unsigned char *key, unsigned char *shared)
{
	static const unsigned char zero[P2P_KEY_SIZE];

	p2p_identifier();
	/* (milliseconds of arithmetic, which the game's threads need not wait
	for: nothing here changes meanwhile) */
	pthread_mutex_unlock(&p2p_lock);
	p2p_x25519(shared, secret_key, key);
	pthread_mutex_lock(&p2p_lock);
	/* a key of small order gives a secret anyone knows */
	return !p2p_equal(shared, zero, P2P_KEY_SIZE);
}

/* ---------- peers */

static struct peer *find_peer(const unsigned char *peer_identifier)
{
	int index;

	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		if (p2p.peers[index].used && !memcmp(p2p.peers[index].identifier, peer_identifier, P2P_IDENTIFIER_SIZE))
			return &p2p.peers[index];
	}
	return NULL;
}

static struct peer *find_peer_by_address(unsigned long address)
{
	int index;

	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		if (p2p.peers[index].used && p2p.peers[index].virtual_address == address)
			return &p2p.peers[index];
	}
	return NULL;
}

/* ---------- this machine's hardware id */

#ifdef _WIN32
/* win32_p2p.c's: the SMBIOS system UUID, else the registry's MachineGuid */
int posix_hardware_id_source(char *text, int size);
#endif

/* what this machine is known by, as text (none: 0): Windows' SMBIOS UUID or
MachineGuid (win32_p2p.c); Linux's /etc/machine-id; Android's ANDROID_ID,
which only the app's Java can read and puts in hardware_id.txt
(LauncherActivity.java) */
static int hardware_id_source(char *text, int size)
{
#ifdef _WIN32
	return posix_hardware_id_source(text, size);
#else
	static const char *const linux_paths[] = { "/etc/machine-id", "/var/lib/dbus/machine-id" };
	char android_path[1024];
	const char *paths[2];
	int path_count = 0;
	int index;

#ifdef HALO_ANDROID
	snprintf(android_path, sizeof(android_path), "%s/hardware_id.txt", platform_data_root());
	paths[path_count++] = android_path;
#else
	(void)android_path;
	paths[path_count++] = linux_paths[0];
	paths[path_count++] = linux_paths[1];
#endif
	for (index = 0; index < path_count; index++)
	{
		FILE *file = fopen(paths[index], "rb");
		size_t length;

		if (!file)
			continue;
		length = fread(text, 1, (size_t)size - 1, file);
		fclose(file);
		text[length] = 0;
		/* (the line, without its end) */
		text[strcspn(text, "\r\n")] = 0;
		if (text[0])
			return 1;
	}
	return 0;
#endif
}

/* this machine's hardware id, as hex (empty if it has none to tell): a hash
of what it is known by (hardware_id_source) keyed for this game, so that
what is told is no raw serial and is this game's alone; a host it joins
logs it, and refuses one it banned. Anyone with administrator or root can
change what it is known by: a stable id, not a proof */
void p2p_hardware_id(char *hex, int size)
{
	static const char key[] = "halo-ce-universal hardware id v1";
	static char cached[2 * P2P_HARDWARE_ID_BYTES + 1];
	static int computed;

	if (!computed)
	{
		char source[256];
		unsigned char digest[P2P_SHA256_SIZE];

		computed = 1;
		cached[0] = 0;
		if (hardware_id_source(source, sizeof(source)))
		{
			p2p_hmac_sha256((const unsigned char *)key, (int)sizeof(key) - 1, source, (int)strlen(source), digest);
			p2p_hex(digest, P2P_HARDWARE_ID_BYTES, cached);
		}
	}
	snprintf(hex, (size_t)size, "%s", cached);
}

void p2p_hardware_id_sanitize(char *destination, int size, const char *source)
{
	int length = 0;

	for (; source && *source && length < size - 1 && length < 2 * P2P_HARDWARE_ID_BYTES; source++)
	{
		char character = *source >= 'A' && *source <= 'F' ? *source - 'A' + 'a' : *source;

		if ((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))
			destination[length++] = character;
	}
	if (size > 0)
		destination[length] = 0;
}

void p2p_discord_identity(char *id, int id_size, char *name, int name_size)
{
	if (id_size > 0)
		id[0] = 0;
	if (name_size > 0)
		name[0] = 0;
	if (!p2p.running || id_size <= 0 || name_size <= 0)
		return;
	pthread_mutex_lock(&p2p_lock);
	p2p_discord_user(id, id_size, name, name_size);
	pthread_mutex_unlock(&p2p_lock);
}

unsigned long p2p_peer_endpoint_address(unsigned long virtual_address)
{
	struct peer *peer;
	unsigned long address = 0;

	if (!p2p.running)
		return 0;
	pthread_mutex_lock(&p2p_lock);
	peer = find_peer_by_address(virtual_address);
	if (peer)
		address = peer->endpoint.address ? peer->endpoint.address :
			(peer->candidate_count > 0 ? peer->candidates[0].address : 0);
	pthread_mutex_unlock(&p2p_lock);
	return address;
}

static int is_virtual_address(unsigned long address)
{
	/* 100.64.0.0/10 */
	return (network_long(address) & 0xFFC00000) == 0x64400000;
}

/* an address in 100.64.0.0/10 from the identifier, not one another peer has */
static unsigned long virtual_address_for(const unsigned char *peer_identifier)
{
	unsigned char digest[P2P_SHA256_SIZE];
	unsigned long value;

	p2p_sha256(peer_identifier, P2P_IDENTIFIER_SIZE, digest);
	value = (unsigned long)digest[0] << 16 | (unsigned long)digest[1] << 8 | digest[2];
	for (;;)
	{
		unsigned long address = 0x64400000 | (value & 0x3FFFFF);

		/* no .0 or .255, which look like network and broadcast addresses */
		if ((address & 255) != 0 && (address & 255) != 255 && !find_peer_by_address(network_long(address)))
			return network_long(address);
		value++;
	}
}

/* a tunnel packet's number, from its header */
static unsigned long long packet_counter(const unsigned char *packet)
{
	unsigned long long counter = 0;
	int index;

	for (index = 7; index >= 0; index--)
		counter = counter << 8 | packet[1 + P2P_IDENTIFIER_SIZE + index];
	return counter;
}

/* its nonce: its number (each direction has a key of its own) */
static void packet_nonce(const unsigned char *packet, unsigned char *nonce)
{
	memset(nonce, 0, P2P_NONCE_SIZE - 8);
	memcpy(nonce + P2P_NONCE_SIZE - 8, packet + 1 + P2P_IDENTIFIER_SIZE, 8);
}

static void peer_send_to(struct peer *peer, const struct p2p_candidate *to, const unsigned char *inner, int size)
{
	unsigned char packet[MAXIMUM_PACKET_SIZE];
	unsigned char nonce[P2P_NONCE_SIZE];
	struct sockaddr_in address;
	unsigned long long counter;
	int sealed;
	int index;

	if (p2p.tunnel_socket < 0 || size > MAXIMUM_INNER_SIZE)
		return;
	/* the header, authenticated with the rest */
	counter = ++peer->send_counter;
	packet[0] = TUNNEL_MAGIC;
	memcpy(packet + 1, identifier, P2P_IDENTIFIER_SIZE);
	for (index = 0; index < 8; index++)
		packet[1 + P2P_IDENTIFIER_SIZE + index] = (unsigned char)(counter >> (index * 8));
	packet_nonce(packet, nonce);
	sealed = p2p_aead_seal(peer->send_key, nonce, packet, TUNNEL_HEADER_SIZE, inner, size,
		packet + TUNNEL_HEADER_SIZE);
	make_address(&address, to->address, to->port);
	posix_socket_sendto(p2p.tunnel_socket, packet, TUNNEL_HEADER_SIZE + sealed, 0, &address, sizeof(address));
}

/* to a peer the tunnel has reached; dropped otherwise */
static void peer_send(struct peer *peer, const unsigned char *inner, int size)
{
	if (!peer->connected)
		return;
	peer_send_to(peer, &peer->endpoint, inner, size);
	peer->sent_time = p2p_now();
}

static void peer_ping(struct peer *peer, const struct p2p_candidate *to)
{
	unsigned char inner[5];
	unsigned int now = (unsigned int)p2p_now();

	inner[0] = _packet_ping;
	memcpy(inner + 1, &now, 4);
	peer_send_to(peer, to, inner, sizeof(inner));
}

static void stream_free(struct stream *stream);
static void proxy_close(struct proxy *proxy);
static void listener_close(struct listener *listener);

/* everything carrying traffic for the peer (of this index) */
static void release_peer_links(int peer_index, int streams_only)
{
	struct peer *peer = &p2p.peers[peer_index];
	int index;

	for (index = 0; index < MAXIMUM_STREAMS; index++)
	{
		if (p2p.streams[index].used && p2p.streams[index].peer == peer_index)
			stream_free(&p2p.streams[index]);
	}
	if (streams_only)
		return;
	while (peer->proxy_count > 0)
		proxy_close(&p2p.proxies[peer->proxies[peer->proxy_count - 1]]);
	for (index = 0; index < MAXIMUM_LISTENERS; index++)
	{
		if (p2p.listeners[index].socket >= 0 && p2p.listeners[index].peer == peer_index)
			listener_close(&p2p.listeners[index]);
	}
}

static int session_retired(const unsigned char *secret)
{
	int index;

	for (index = 0; index < MAXIMUM_RETIRED_SESSIONS; index++)
	{
		if (!memcmp(p2p.retired[index].secret, secret, P2P_SHA256_SIZE))
			return 1;
	}
	return 0;
}

/* whether address was a peer's, recently */
static int address_retired(unsigned long address)
{
	int index;

	for (index = 0; index < MAXIMUM_RETIRED_SESSIONS; index++)
	{
		if (p2p.retired[index].virtual_address == address)
			return 1;
	}
	return 0;
}

static void drop_peer(struct peer *peer, const char *reason)
{
	struct retired_session *retired = &p2p.retired[p2p.retired_next++ % MAXIMUM_RETIRED_SESSIONS];
	int ask_again = p2p.joining && peer->is_host && !memcmp(peer->identifier, p2p.join_host, P2P_IDENTIFIER_SIZE);

	platform_log("Internet play: %s %s: %s", peer->is_host ? "host" : "player", peer->name, reason);
	if (peer->connected)
	{
		unsigned char bye = _packet_bye;

		peer_send(peer, &bye, 1);
	}
	release_peer_links((int)(peer - p2p.peers), 0);
	/* a session that ended does not come back: its packets would pass
	again */
	memcpy(retired->secret, peer->secret, P2P_SHA256_SIZE);
	retired->virtual_address = peer->virtual_address;
	memset(peer, 0, sizeof(*peer));
	/* still joining: a new request, with a nonce of its own (the host
	makes no second session from one request, which anyone who saw it could
	send again) */
	if (ask_again)
		p2p_signal_join(p2p.join_host_hash, p2p.join_token);
}

static void add_candidates(struct peer *peer, const struct p2p_candidate *candidates, int count)
{
	int index;

	for (index = 0; index < count; index++)
	{
		int known;

#ifdef HALO_RELAY
		/* An internet-facing relay must not punch into its private network. */
		if (!relay_candidate_allowed(candidates[index].address, candidates[index].port))
			continue;
#endif

		for (known = 0; known < peer->candidate_count; known++)
		{
			if (peer->candidates[known].address == candidates[index].address &&
				peer->candidates[known].port == candidates[index].port)
				break;
		}
		if (known < peer->candidate_count)
			continue;
		if (peer->candidate_count < P2P_MAXIMUM_CANDIDATES)
			peer->candidates[peer->candidate_count++] = candidates[index];
		else
			peer->candidates[P2P_MAXIMUM_CANDIDATES - 1] = candidates[index];
	}
}

/* a free peer, or NULL */
static struct peer *free_peer(void)
{
	int index;

	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		if (!p2p.peers[index].used)
			return &p2p.peers[index];
	}
	return NULL;
}

/* the players this host is reaching and has not reached yet */
static int opening_peer_count(void)
{
	int count = 0;
	int index;

	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
		count += p2p.peers[index].used && !p2p.peers[index].connected && !p2p.peers[index].is_host;
	return count;
}

int p2p_peer_turned_away(const unsigned char *peer_identifier, int is_host)
{
	static unsigned long logged_time;
	const char *reason = NULL;

	/* (another session with the machine waits until the one that lives
	lapses: anyone with the invite can ask in its name) */
	if (!memcmp(peer_identifier, identifier, P2P_IDENTIFIER_SIZE) || find_peer(peer_identifier))
		return 1;
	if (!free_peer())
		reason = "too many players";
	else if (!is_host && opening_peer_count() >= MAXIMUM_OPENING_PEERS)
		reason = "too many players connecting at once";
	if (!reason)
		return 0;
	/* (anyone with the invite can ask as often as they like) */
	if (!logged_time || elapsed(logged_time, 10000))
	{
		platform_log("Internet play: %s; one more was turned away (it asks again)", reason);
		logged_time = p2p_now() | 1;
	}
	return 1;
}

int p2p_peer_offered(const unsigned char *peer_identifier, const unsigned char *secret,
	const struct p2p_candidate *candidates, int count, int is_host)
{
	struct peer *peer = find_peer(peer_identifier);

	if (!memcmp(peer_identifier, identifier, P2P_IDENTIFIER_SIZE))
		return 0;
	if (peer)
	{
		/* another session with the machine waits until this one lapses:
		anyone with the invite can ask in its name */
		if (memcmp(peer->secret, secret, P2P_SHA256_SIZE))
			return 0;
	}
	else
	{
		unsigned char joiner_key[P2P_SHA256_SIZE], host_key[P2P_SHA256_SIZE];

		if (session_retired(secret) || p2p_peer_turned_away(peer_identifier, is_host))
			return 0;
		peer = free_peer();
		memset(peer, 0, sizeof(*peer));
		peer->used = 1;
		memcpy(peer->identifier, peer_identifier, P2P_IDENTIFIER_SIZE);
		p2p_hex(peer_identifier, P2P_IDENTIFIER_SIZE, peer->name);
		/* a key for each direction, so that nothing sent one way passes the
		other */
		memcpy(peer->secret, secret, P2P_SHA256_SIZE);
		p2p_hmac_sha256(secret, P2P_SHA256_SIZE, "joiner", 6, joiner_key);
		p2p_hmac_sha256(secret, P2P_SHA256_SIZE, "host", 4, host_key);
		memcpy(peer->send_key, is_host ? joiner_key : host_key, P2P_SHA256_SIZE);
		memcpy(peer->receive_key, is_host ? host_key : joiner_key, P2P_SHA256_SIZE);
		/* (packets are numbered from 1) */
		peer->receive_window = 1;
		peer->virtual_address = virtual_address_for(peer_identifier);
		peer->is_host = is_host;
		peer->offered_time = p2p_now();
		platform_log("Internet play: reaching %s %s", is_host ? "host" : "player", peer->name);
	}
	add_candidates(peer, candidates, count);
	return 1;
}

int p2p_peer_reoffered(const unsigned char *peer_identifier, const unsigned char *secret,
	const struct p2p_candidate *candidates, int count)
{
	struct peer *peer = find_peer(peer_identifier);

	if (!peer || memcmp(peer->secret, secret, P2P_SHA256_SIZE))
		return 0;
	add_candidates(peer, candidates, count);
	return 1;
}

/* whether a packet of this number from the peer is new: not received yet,
nor from before the window */
static int packet_fresh(const struct peer *peer, unsigned long long counter)
{
	unsigned long long behind;

	if (counter > peer->receive_highest)
		return 1;
	behind = peer->receive_highest - counter;
	return behind < REPLAY_WINDOW && !((peer->receive_window >> behind) & 1);
}

static void packet_received(struct peer *peer, unsigned long long counter)
{
	if (counter > peer->receive_highest)
	{
		unsigned long long ahead = counter - peer->receive_highest;

		peer->receive_window = ahead >= REPLAY_WINDOW ? 0 : peer->receive_window << ahead;
		peer->receive_highest = counter;
	}
	peer->receive_window |= 1ULL << (peer->receive_highest - counter);
}

/* newest: the packet is the highest numbered yet (a replayed or delayed one
does not move the peer's endpoint) */
static void peer_heard(struct peer *peer, unsigned long address, unsigned short port, int newest)
{
	unsigned long now = p2p_now();
	int same = peer->endpoint.address == address && peer->endpoint.port == port;

	peer->heard_time = now;
	if (!peer->connected)
	{
		char text[32];

		peer->connected = 1;
		peer->endpoint.address = address;
		peer->endpoint.port = port;
		peer->endpoint_heard_time = now;
		platform_log("Internet play: connected to %s %s at %s", peer->is_host ? "host" : "player", peer->name,
			address_text(address, port, text));
		if (peer->is_host)
		{
			if (p2p.joining && !memcmp(p2p.join_host, peer->identifier, P2P_IDENTIFIER_SIZE))
			{
				p2p.joining = 0;
				p2p_signal_stop_joining();
			}
			platform_log("Internet play: the host's game is listed under Multiplayer, System Link");
		}
	}
	else if (same)
	{
		peer->endpoint_heard_time = now;
	}
	else if (newest && elapsed(peer->endpoint_heard_time, ENDPOINT_SWITCH_TIME))
	{
		/* its address changed (a NAT's mapping, or a better path) */
		peer->endpoint.address = address;
		peer->endpoint.port = port;
		peer->endpoint_heard_time = now;
	}
}

static void update_peers(void)
{
	int index;

	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		struct peer *peer = &p2p.peers[index];

		if (!peer->used)
			continue;
		if (peer->connected)
		{
			if (elapsed(peer->heard_time, PEER_TIMEOUT))
				drop_peer(peer, "lost the connection");
			else if (elapsed(peer->sent_time, PING_INTERVAL))
			{
				peer_ping(peer, &peer->endpoint);
				peer->sent_time = p2p_now();
			}
		}
		else if (elapsed(peer->offered_time, PUNCH_TIMEOUT))
		{
			drop_peer(peer, "could not connect (both networks' NATs may be too strict for a direct "
				"connection; forwarding network.tunnel_port on one router helps, as UPnP does where the "
				"router allows it: network.allow_upnp)");
		}
		else if (elapsed(peer->sent_time, PUNCH_INTERVAL))
		{
			int candidate;

			for (candidate = 0; candidate < peer->candidate_count; candidate++)
				peer_ping(peer, &peer->candidates[candidate]);
			peer->sent_time = p2p_now();
		}
	}
}

/* ---------- STUN (RFC 5389): this machine's public address */

static void stun_send(struct stun_server *server)
{
	unsigned char request[20];
	struct sockaddr_in address;

	if (!server->address)
		return;
	memset(request, 0, sizeof(request));
	request[1] = 0x01; /* binding request */
	request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;
	/* a retry is the same transaction (RFC 5389), so a late answer to an
	earlier one is taken */
	if (!server->attempts)
		posix_random_bytes(server->transaction, sizeof(server->transaction));
	memcpy(request + 8, server->transaction, sizeof(server->transaction));
	make_address(&address, server->address, server->port);
	posix_socket_sendto(p2p.tunnel_socket, request, sizeof(request), 0, &address, sizeof(address));
	server->sent_time = p2p_now();
	server->attempts++;
}

static void stun_setup(void)
{
	const char *text = config_string("network.stun_servers");

	while (*text && p2p.stun_count < MAXIMUM_STUN_SERVERS)
	{
		const char *end = text + strcspn(text, ",");
		struct stun_server *server = &p2p.stun[p2p.stun_count];
		const char *colon;
		int length;

		while (text < end && *text == ' ')
			text++;
		length = (int)(end - text);
		while (length > 0 && text[length - 1] == ' ')
			length--;
		if (length > 0 && length < (int)sizeof(server->host))
		{
			memset(server, 0, sizeof(*server));
			memcpy(server->host, text, (size_t)length);
			colon = strchr(server->host, ':');
			server->port = network_short(3478);
			if (colon)
			{
				server->port = network_short((unsigned short)atoi(colon + 1));
				server->host[colon - server->host] = 0;
			}
			p2p.stun_count++;
		}
		text = *end ? end + 1 : end;
	}
}

static void stun_update(void)
{
	int index;

	if (!p2p.stun_started)
		return;
	for (index = 0; index < p2p.stun_count; index++)
	{
		struct stun_server *server = &p2p.stun[index];

		if (!server->address && !server->attempts)
		{
			/* looked up once, here on the p2p thread */
			server->address = p2p_resolve(server->host);
			if (!server->address)
			{
				platform_log("Internet play: cannot look up the STUN server %s", server->host);
				server->attempts = STUN_ATTEMPTS;
				continue;
			}
		}
		if (server->has_mapped)
		{
			/* also keeps the NAT's mapping of the tunnel alive */
			if (elapsed(server->sent_time, STUN_REFRESH_INTERVAL))
			{
				server->attempts = 0;
				stun_send(server);
			}
		}
		else if (server->attempts < STUN_ATTEMPTS && elapsed(server->sent_time, STUN_RETRY_INTERVAL))
		{
			stun_send(server);
		}
		else if (server->attempts >= STUN_ATTEMPTS && server->address && elapsed(server->sent_time, STUN_REFRESH_INTERVAL))
		{
			server->attempts = 0;
		}
	}
}

static void stun_received(const unsigned char *packet, int size, const struct sockaddr_in *from)
{
	int index;
	int offset;

	if (size < 20 || packet[0] != 0x01 || packet[1] != 0x01)
		return;
	/* the answer of a server asked, from it */
	for (index = 0; index < p2p.stun_count; index++)
	{
		struct stun_server const *server = &p2p.stun[index];

		if (server->address && server->address == from->sin_addr.s_addr && server->port == from->sin_port &&
			!memcmp(packet + 8, server->transaction, 12))
		{
			break;
		}
	}
	if (index == p2p.stun_count)
		return;
	for (offset = 20; offset + 4 <= size; )
	{
		int type = packet[offset] << 8 | packet[offset + 1];
		int length = packet[offset + 2] << 8 | packet[offset + 3];
		const unsigned char *value = packet + offset + 4;

		if (offset + 4 + length > size)
			break;
		/* XOR-MAPPED-ADDRESS, or MAPPED-ADDRESS from an old server; IPv4 */
		if ((type == 0x0020 || type == 0x0001) && length >= 8 && value[1] == 0x01)
		{
			struct stun_server *server = &p2p.stun[index];
			unsigned char port[2], ip[4];
			int byte;

			memcpy(port, value + 2, 2);
			memcpy(ip, value + 4, 4);
			if (type == 0x0020)
			{
				for (byte = 0; byte < 2; byte++)
					port[byte] ^= packet[4 + byte];
				for (byte = 0; byte < 4; byte++)
					ip[byte] ^= packet[4 + byte];
			}
			if (!server->has_mapped)
			{
				char text[32];
				int other;

				memcpy(&server->mapped.address, ip, 4);
				memcpy(&server->mapped.port, port, 2);
				server->has_mapped = 1;
				platform_log("Internet play: this machine's public address is %s (from %s)",
					address_text(server->mapped.address, server->mapped.port, text), server->host);
				for (other = 0; other < p2p.stun_count; other++)
				{
					if (other != index && p2p.stun[other].has_mapped &&
						p2p.stun[other].mapped.port != server->mapped.port && !p2p.reported_symmetric)
					{
						platform_log("Internet play: this network's NAT gives each destination its own "
							"port, so it can only connect to machines behind more lenient ones");
						p2p.reported_symmetric = 1;
					}
				}
			}
			else
			{
				memcpy(&server->mapped.address, ip, 4);
				memcpy(&server->mapped.port, port, 2);
			}
			break;
		}
		offset += 4 + ((length + 3) & ~3);
	}
}

int p2p_local_candidates(struct p2p_candidate *candidates, int maximum_count)
{
	unsigned long lan = posix_local_ipv4_address();
	int count = 0;
	int index;

	if (lan && count < maximum_count)
	{
		candidates[count].address = lan;
		candidates[count++].port = p2p.tunnel_port;
	}
	/* (the port the router forwards here, UPnP) */
	if (p2p.upnp_forwarded && count < maximum_count)
		candidates[count++] = p2p.upnp_candidate;
	for (index = 0; index < p2p.stun_count && count < maximum_count; index++)
	{
		int known;

		if (!p2p.stun[index].has_mapped)
			continue;
		for (known = 0; known < count; known++)
		{
			if (candidates[known].address == p2p.stun[index].mapped.address &&
				candidates[known].port == p2p.stun[index].mapped.port)
				break;
		}
		if (known == count)
			candidates[count++] = p2p.stun[index].mapped;
	}
	/* two copies of the game on one machine without a network */
	if (!count && maximum_count)
	{
		candidates[count].address = network_long(0x7F000001);
		candidates[count++].port = p2p.tunnel_port;
	}
	return count;
}

static int stun_settled(void)
{
	int index;

	for (index = 0; index < p2p.stun_count; index++)
	{
		if (!p2p.stun[index].has_mapped && p2p.stun[index].attempts < STUN_ATTEMPTS)
			return 0;
	}
	return 1;
}

/* ---------- stand-ins for peers' ports */

static void close_socket(int *socket)
{
	if (*socket >= 0)
		posix_socket_close(*socket);
	*socket = -1;
}

/* a stand-in of a peer's port closes: traffic from it that the game has not
read yet still comes from the peer (p2p_incoming) */
static void remember_closed(int stream, unsigned short local_port, int peer_index, unsigned short remote_port)
{
	struct closed_port *closed = &p2p.closed[p2p.closed_next++ % MAXIMUM_CLOSED_PORTS];

	closed->stream = stream;
	closed->local_port = local_port;
	closed->virtual_address = p2p.peers[peer_index].virtual_address;
	closed->remote_port = remote_port;
	closed->time = p2p_now();
}

/* the peer's address and port for a stand-in closed lately; 0 if none */
static int find_closed(int stream, unsigned long *address, unsigned short *port)
{
	int index;

	for (index = 0; index < MAXIMUM_CLOSED_PORTS; index++)
	{
		struct closed_port const *closed = &p2p.closed[index];

		/* (not for long: the system gives the port to other sockets again) */
		if (closed->local_port == *port && closed->stream == stream && closed->virtual_address &&
			!elapsed(closed->time, CLOSED_PORT_TIME))
		{
			*address = closed->virtual_address;
			*port = closed->remote_port;
			return 1;
		}
	}
	return 0;
}

/* a stand-in, or a socket of the game's, that has the port now: no closed
stand-in stands for it */
static void forget_closed(int stream, unsigned short local_port)
{
	int index;

	for (index = 0; index < MAXIMUM_CLOSED_PORTS; index++)
	{
		if (p2p.closed[index].local_port == local_port && p2p.closed[index].stream == stream)
			p2p.closed[index].virtual_address = 0;
	}
}

static void proxy_close(struct proxy *proxy)
{
	struct peer *peer = &p2p.peers[proxy->peer];
	int index = (int)(proxy - p2p.proxies);
	int entry;

	if (proxy->socket < 0)
		return;
	close_socket(&proxy->socket);
	proxy_by_port[proxy->local_port] = 0;
	remember_closed(0, proxy->local_port, proxy->peer, proxy->remote_port);
	for (entry = 0; entry < peer->proxy_count; entry++)
	{
		if (peer->proxies[entry] == index)
		{
			peer->proxies[entry] = peer->proxies[--peer->proxy_count];
			break;
		}
	}
}

static void listener_close(struct listener *listener)
{
	if (listener->socket < 0)
		return;
	close_socket(&listener->socket);
	remember_closed(1, listener->local_port, listener->peer, listener->remote_port);
}

static struct proxy *find_proxy(int peer_index, unsigned short remote_port, int create)
{
	struct peer *peer = &p2p.peers[peer_index];
	struct proxy *free_proxy = NULL;
	struct proxy *oldest = NULL;
	int index;

	/* (a peer has only a few) */
	for (index = 0; index < peer->proxy_count; index++)
	{
		struct proxy *proxy = &p2p.proxies[peer->proxies[index]];

		if (proxy->remote_port == remote_port)
		{
			proxy->used_time = p2p_now();
			return proxy;
		}
		if (proxy->pinned_socket < 0 && (!oldest || (long)(proxy->used_time - oldest->used_time) < 0))
			oldest = proxy;
	}
	if (!create)
		return NULL;
	/* past a peer's few, its least used goes: no peer takes them all */
	if (peer->proxy_count >= MAXIMUM_PEER_PROXIES)
	{
		if (!oldest || !elapsed(oldest->used_time, PROXY_REPLACE_TIME))
			return NULL;
		proxy_close(oldest);
		free_proxy = oldest;
	}
	for (index = 0; index < MAXIMUM_PROXIES && !free_proxy; index++)
	{
		if (p2p.proxies[index].socket < 0)
			free_proxy = &p2p.proxies[index];
	}
	if (!free_proxy)
		return NULL;
	free_proxy->socket = open_socket(SOCK_DGRAM, p2p.local_address, 0, &free_proxy->local_port);
	if (free_proxy->socket < 0)
		return NULL;
	free_proxy->peer = peer_index;
	free_proxy->remote_port = remote_port;
	free_proxy->used_time = p2p_now();
	free_proxy->pinned_socket = -1;
	peer->proxies[peer->proxy_count++] = (short)(free_proxy - p2p.proxies);
	proxy_by_port[free_proxy->local_port] = (unsigned short)(free_proxy - p2p.proxies + 1);
	forget_closed(0, free_proxy->local_port);
	return free_proxy;
}

/* stand-ins unused for a while go (a peer's ports change with its game's
sockets) */
static void expire_proxies(void)
{
	int index;

	for (index = 0; index < MAXIMUM_PROXIES; index++)
	{
		struct proxy *proxy = &p2p.proxies[index];

		if (proxy->socket >= 0 && proxy->pinned_socket < 0 && elapsed(proxy->used_time, PROXY_IDLE_TIME))
			proxy_close(proxy);
	}
}

static struct listener *find_listener(int peer_index, unsigned short remote_port)
{
	struct listener *free_listener = NULL;
	int index;

	for (index = 0; index < MAXIMUM_LISTENERS; index++)
	{
		struct listener *listener = &p2p.listeners[index];

		if (listener->socket < 0)
		{
			if (!free_listener)
				free_listener = listener;
		}
		else if (listener->peer == peer_index && listener->remote_port == remote_port)
			return listener;
	}
	if (!free_listener)
		return NULL;
	free_listener->socket = open_socket(SOCK_STREAM, p2p.local_address, 0, &free_listener->local_port);
	if (free_listener->socket < 0)
		return NULL;
	if (posix_socket_listen(free_listener->socket, 8) < 0)
	{
		close_socket(&free_listener->socket);
		return NULL;
	}
	free_listener->peer = peer_index;
	free_listener->remote_port = remote_port;
	forget_closed(1, free_listener->local_port);
	return free_listener;
}

int p2p_outgoing(int stream, int socket, unsigned long *address, unsigned short *port)
{
	struct peer *peer;
	int result = 0;

	if (!is_virtual_address(*address) || !p2p.running)
		return 0;
	pthread_mutex_lock(&p2p_lock);
	peer = find_peer_by_address(*address);
	/* a peer's, or one that was: never to the address itself (100.64.0.0/10
	is also a carrier's NAT's and some VPNs', which the game's traffic is not
	for) */
	if (!peer)
		result = address_retired(*address) ? -1 : 0;
	else if (!peer->connected)
		result = -1;
	else if (stream)
	{
		struct listener *listener = find_listener((int)(peer - p2p.peers), *port);

		result = -1;
		if (listener)
		{
			*address = p2p.local_address;
			*port = listener->local_port;
			result = 1;
		}
	}
	else
	{
		struct proxy *proxy = find_proxy((int)(peer - p2p.peers), *port, 1);

		result = -1;
		if (proxy)
		{
			*address = p2p.local_address;
			*port = proxy->local_port;
			if (socket >= 0)
				proxy->pinned_socket = socket;
			result = 1;
		}
	}
	pthread_mutex_unlock(&p2p_lock);
	return result;
}

int p2p_incoming(int stream, unsigned long *address, unsigned short *port)
{
	int result = 0;
	int index;

	if (!p2p.running || *address != p2p.local_address)
		return 0;
	pthread_mutex_lock(&p2p_lock);
	if (stream)
	{
		for (index = 0; index < MAXIMUM_LISTENERS && !result; index++)
		{
			struct listener *listener = &p2p.listeners[index];

			if (listener->socket >= 0 && listener->local_port == *port)
			{
				*address = p2p.peers[listener->peer].virtual_address;
				*port = listener->remote_port;
				result = 1;
			}
		}
		for (index = 0; index < MAXIMUM_STREAMS && !result; index++)
		{
			struct stream *entry = &p2p.streams[index];

			if (entry->used && entry->local_port && entry->local_port == *port)
			{
				*address = p2p.peers[entry->peer].virtual_address;
				*port = entry->remote_port;
				result = 1;
			}
		}
	}
	else if (proxy_by_port[*port])
	{
		struct proxy const *proxy = &p2p.proxies[proxy_by_port[*port] - 1];

		*address = p2p.peers[proxy->peer].virtual_address;
		*port = proxy->remote_port;
		result = 1;
	}
	/* one that closed since (a peer's stand-ins past its few, a stream
	ended before the game took its connection) is still the peer's: as
	127.0.0.1 it would be this machine's own */
	if (!result)
		result = find_closed(stream, address, port);
	pthread_mutex_unlock(&p2p_lock);
	return result;
}

int p2p_broadcast_targets(unsigned short port, unsigned long *addresses, unsigned short *ports, int maximum_count)
{
	int count = 0;
	int index;

	if (!p2p.running)
		return 0;
	pthread_mutex_lock(&p2p_lock);
	for (index = 0; index < P2P_MAXIMUM_PEERS && count < maximum_count; index++)
	{
		struct proxy *proxy;

		if (!p2p.peers[index].used || !p2p.peers[index].connected)
			continue;
		proxy = find_proxy(index, port, 1);
		if (proxy)
		{
			addresses[count] = p2p.local_address;
			ports[count++] = proxy->local_port;
		}
	}
	pthread_mutex_unlock(&p2p_lock);
	return count;
}

static void datagram_send(struct peer *peer, unsigned short source_port, unsigned short port, const void *data,
	int size);

int p2p_send_datagram(unsigned short source_port, unsigned long address, unsigned short port, const void *data,
	int size)
{
	struct peer *peer;
	int result;

	if (!is_virtual_address(address) || !p2p.running)
		return 0;
	pthread_mutex_lock(&p2p_lock);
	peer = find_peer_by_address(address);
	if (!peer)
		result = address_retired(address) ? -1 : 0;
	else if (!peer->connected)
		result = -1;
	else
	{
		datagram_send(peer, source_port, port, data, size);
		result = 1;
	}
	pthread_mutex_unlock(&p2p_lock);
	return result;
}

int p2p_broadcast_datagram(unsigned short source_port, unsigned short port, const void *data, int size)
{
	int count = 0;
	int index;

	if (!p2p.running)
		return 0;
	pthread_mutex_lock(&p2p_lock);
	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		if (p2p.peers[index].used && p2p.peers[index].connected)
		{
			datagram_send(&p2p.peers[index], source_port, port, data, size);
			count++;
		}
	}
	pthread_mutex_unlock(&p2p_lock);
	return count;
}

int p2p_peer_address(const unsigned char *peer_identifier, unsigned long *address)
{
	struct peer *peer;
	int result = 0;

	if (!p2p.running)
		return 0;
	pthread_mutex_lock(&p2p_lock);
	/* reached or not (until it is, sending there fails: p2p_outgoing) */
	peer = find_peer(peer_identifier);
	if (peer)
	{
		*address = peer->virtual_address;
		result = 1;
	}
	pthread_mutex_unlock(&p2p_lock);
	return result;
}

/* ---------- the game's ports: all that peers may reach */

void p2p_socket_port(int socket, int stream, int listening, unsigned short port)
{
	struct game_port *entry = NULL;
	int index;

	if (!p2p.running || !port)
		return;
	pthread_mutex_lock(&p2p_lock);
	for (index = 0; index < MAXIMUM_GAME_PORTS; index++)
	{
		struct game_port *known = &p2p.game_ports[index];

		if (known->port && known->socket == socket)
		{
			entry = known;
			break;
		}
		if (!known->port && !entry)
			entry = known;
	}
	if (entry)
	{
		entry->socket = socket;
		entry->port = port;
		entry->stream = (unsigned char)(stream != 0);
		entry->listening |= (unsigned char)(listening != 0);
	}
	/* the game listens for connections while it hosts */
	if (stream && listening)
		p2p.hosting_socket = socket;
	/* (and no stand-in that closed has the port now) */
	forget_closed(stream != 0, port);
	pthread_mutex_unlock(&p2p_lock);
}

void p2p_port_taken(int stream, unsigned short port)
{
	if (!p2p.running || !port)
		return;
	pthread_mutex_lock(&p2p_lock);
	forget_closed(stream != 0, port);
	pthread_mutex_unlock(&p2p_lock);
}

void p2p_socket_closed(int socket, unsigned short datagram_port)
{
	int index;

	if (!p2p.running)
		return;
	pthread_mutex_lock(&p2p_lock);
	/* (a port it sent peers datagrams from, which the system gives again) */
	for (index = 0; index < MAXIMUM_SENT_PORTS && datagram_port; index++)
	{
		if (p2p.sent_ports[index] == datagram_port)
			p2p.sent_ports[index] = 0;
	}
	for (index = 0; index < MAXIMUM_GAME_PORTS; index++)
	{
		if (p2p.game_ports[index].port && p2p.game_ports[index].socket == socket)
			memset(&p2p.game_ports[index], 0, sizeof(p2p.game_ports[index]));
	}
	for (index = 0; index < MAXIMUM_PROXIES; index++)
	{
		if (p2p.proxies[index].socket >= 0 && p2p.proxies[index].pinned_socket == socket)
			p2p.proxies[index].pinned_socket = -1;
	}
	if (p2p.hosting_socket == socket)
		p2p.hosting_socket = -1;
	pthread_mutex_unlock(&p2p_lock);
}

/* whether a peer may reach this port of the game's: one a socket of the
game's listens on (stream), or a datagram socket's, bound or sent from to a
peer */
static int game_port_open(int stream, unsigned short port)
{
	int index;

	for (index = 0; index < MAXIMUM_GAME_PORTS; index++)
	{
		struct game_port const *entry = &p2p.game_ports[index];

		if (entry->port == port && entry->stream == (stream != 0) && (!stream || entry->listening))
			return 1;
	}
	for (index = 0; index < MAXIMUM_SENT_PORTS && !stream; index++)
	{
		if (port && p2p.sent_ports[index] == port)
			return 1;
	}
	return 0;
}

/* ---------- datagrams */

/* a datagram from the game's port source_port to the peer's port */
static void datagram_send(struct peer *peer, unsigned short source_port, unsigned short port, const void *data,
	int size)
{
	unsigned char inner[MAXIMUM_INNER_SIZE];

	if (size < 0 || size > MAXIMUM_INNER_SIZE - 5)
		return;
	/* (the peer answers to that port) */
	if (!game_port_open(0, source_port))
		p2p.sent_ports[p2p.sent_port_next++ % MAXIMUM_SENT_PORTS] = source_port;
	inner[0] = _packet_datagram;
	put_short(inner + 1, source_port);
	put_short(inner + 3, port);
	memcpy(inner + 5, data, (size_t)size);
	peer_send(peer, inner, size + 5);
}

/* a datagram from the game to a peer, through its stand-in (from a socket
connected to it, or not bound yet: p2p_send_datagram sends the rest) */
static void proxy_readable(struct proxy *proxy)
{
	unsigned char data[MAXIMUM_INNER_SIZE - 5];
	int count;

	for (count = 0; count < 64; count++)
	{
		struct sockaddr_in from;
		int from_length = sizeof(from);
		int size = posix_socket_recvfrom(proxy->socket, data, sizeof(data), 0, &from, &from_length);

		if (size < 0)
			break;
		/* only the game's own sockets use a stand-in */
		if (from.sin_addr.s_addr != p2p.local_address && from.sin_addr.s_addr != network_long(0x7F000001))
			continue;
		proxy->used_time = p2p_now();
		datagram_send(&p2p.peers[proxy->peer], from.sin_port, proxy->remote_port, data, size);
	}
}

/* a datagram from a peer to the game */
static void datagram_received(struct peer *peer, const unsigned char *inner, int size)
{
	struct proxy *proxy;
	struct sockaddr_in to;

	/* only to the game */
	if (size < 5 || !game_port_open(0, get_short(inner + 3)))
		return;
	proxy = find_proxy((int)(peer - p2p.peers), get_short(inner + 1), 1);
	if (!proxy)
		return;
	make_address(&to, p2p.local_address, get_short(inner + 3));
	posix_socket_sendto(proxy->socket, inner + 5, size - 5, 0, &to, sizeof(to));
}

/* ---------- streams */

static int kcp_output(const char *buffer, int size, ikcpcb *kcp, void *user)
{
	struct stream *stream = user;
	unsigned char inner[MAXIMUM_INNER_SIZE];

	(void)kcp;
	if (size + 1 > (int)sizeof(inner))
		return -1;
	inner[0] = _packet_stream;
	memcpy(inner + 1, buffer, (size_t)size);
	peer_send(&p2p.peers[stream->peer], inner, size + 1);
	return 0;
}

static struct stream *stream_new(int peer_index, IUINT32 conversation)
{
	int index;

	for (index = 0; index < MAXIMUM_STREAMS; index++)
	{
		struct stream *stream = &p2p.streams[index];

		if (stream->used)
			continue;
		/* all but the buffer, which pending_size (after it) says is empty */
		memset(stream, 0, offsetof(struct stream, pending));
		stream->pending_size = 0;
		stream->kcp = ikcp_create(conversation, stream);
		if (!stream->kcp)
			return NULL;
		stream->used = 1;
		stream->peer = peer_index;
		stream->conversation = conversation;
		stream->socket = -1;
		stream->created_time = p2p_now();
		ikcp_setoutput(stream->kcp, kcp_output);
		ikcp_nodelay(stream->kcp, 1, LOOP_INTERVAL, 2, 1);
		ikcp_wndsize(stream->kcp, 256, 256);
		ikcp_setmtu(stream->kcp, KCP_MTU);
		return stream;
	}
	return NULL;
}

static void stream_free(struct stream *stream)
{
	/* (the game may not have taken the connection made for it yet) */
	if (stream->local_port)
		remember_closed(1, stream->local_port, stream->peer, stream->remote_port);
	close_socket(&stream->socket);
	if (stream->kcp)
		ikcp_release(stream->kcp);
	p2p.finished[p2p.finished_next++ % 16] = stream->conversation;
	memset(stream, 0, offsetof(struct stream, pending));
	stream->pending_size = 0;
	stream->socket = -1;
}

static void stream_message(struct stream *stream, unsigned char type, const void *data, int size)
{
	unsigned char message[1 + STREAM_CHUNK_SIZE];

	message[0] = type;
	memcpy(message + 1, data, (size_t)size);
	ikcp_send(stream->kcp, (const char *)message, size + 1);
}

static void stream_local_closed(struct stream *stream)
{
	close_socket(&stream->socket);
	/* (its port the system gives again now, not when the stream is let go
	once the peer has all it sent: remembered from now, and no longer the
	peer's while the stream lingers) */
	if (stream->local_port)
	{
		remember_closed(1, stream->local_port, stream->peer, stream->remote_port);
		stream->local_port = 0;
	}
	if (!stream->local_closed)
	{
		stream->local_closed = 1;
		stream->closed_time = p2p_now();
		if (!stream->remote_closed)
			stream_message(stream, _stream_close, NULL, 0);
	}
}

/* the game connected to a stand-in listener */
static void listener_readable(struct listener *listener)
{
	for (;;)
	{
		struct sockaddr_in from;
		int from_length = sizeof(from);
		int socket = posix_socket_accept(listener->socket, &from, &from_length);
		struct stream *stream;
		IUINT32 conversation;
		unsigned char open[4];

		if (socket < 0)
			return;
		/* only the game's own sockets use a stand-in (with network.address
		a LAN address, other machines could reach it) */
		if (from.sin_addr.s_addr != p2p.local_address && from.sin_addr.s_addr != network_long(0x7F000001))
		{
			posix_socket_close(socket);
			continue;
		}
		posix_socket_set_nonblocking(socket, 1);
		posix_socket_set_nodelay(socket);
		posix_random_bytes(&conversation, sizeof(conversation));
		stream = stream_new(listener->peer, conversation | 1);
		if (!stream)
		{
			posix_socket_close(socket);
			continue;
		}
		stream->socket = socket;
		stream->state = _stream_open_state;
		put_short(open, listener->remote_port);
		put_short(open + 2, from.sin_port);
		stream_message(stream, _stream_open, open, sizeof(open));
	}
}

/* a peer's connection opened: connect to the game for it */
static void stream_opened(struct stream *stream, const unsigned char *data, int size)
{
	struct sockaddr_in to;

	if (stream->state != _stream_awaiting_open || size < 4)
		return;
	stream->remote_port = get_short(data + 2);
	/* only to where the game listens: nothing else here is the peer's to
	reach */
	if (game_port_open(1, get_short(data)))
	{
		stream->socket = open_socket(SOCK_STREAM, p2p.local_address, 0, &stream->local_port);
		if (stream->socket >= 0)
			forget_closed(1, stream->local_port);
	}
	make_address(&to, p2p.local_address, get_short(data));
	if (stream->socket < 0 || (posix_socket_connect(stream->socket, &to, sizeof(to)) < 0 && !would_block()))
	{
		stream->local_port = 0;
		stream_local_closed(stream);
		return;
	}
	stream->state = _stream_connecting;
}

static void stream_received(struct peer *peer, const unsigned char *data, int size)
{
	int peer_index = (int)(peer - p2p.peers);
	IUINT32 conversation;
	struct stream *stream = NULL;
	int count = 0, opening = 0;
	int index;

	if (size < 24)
		return;
	conversation = ikcp_getconv(data);
	for (index = 0; index < MAXIMUM_STREAMS && !stream; index++)
	{
		struct stream *entry = &p2p.streams[index];

		if (!entry->used || entry->peer != peer_index)
			continue;
		if (entry->conversation == conversation)
			stream = entry;
		count++;
		opening += entry->state == _stream_awaiting_open;
	}
	if (!stream)
	{
		for (index = 0; index < 16; index++)
		{
			if (p2p.finished[index] == conversation)
				return;
		}
		/* a peer has a few at a time: no peer takes them all */
		if (count >= MAXIMUM_PEER_STREAMS || opening >= MAXIMUM_PEER_OPENING_STREAMS)
			return;
		stream = stream_new(peer_index, conversation);
		if (!stream)
			return;
		stream->state = _stream_awaiting_open;
	}
	ikcp_input(stream->kcp, (const char *)data, size);
}

static void stream_flush_pending(struct stream *stream)
{
	while (stream->pending_size > 0 && stream->socket >= 0 && stream->state == _stream_open_state)
	{
		int sent = posix_socket_send(stream->socket, stream->pending, stream->pending_size, 0);

		if (sent < 0)
		{
			if (!would_block())
				stream_local_closed(stream);
			return;
		}
		memmove(stream->pending, stream->pending + sent, (size_t)(stream->pending_size - sent));
		stream->pending_size -= sent;
	}
}

static void stream_update(struct stream *stream)
{
	unsigned char message[1 + STREAM_CHUNK_SIZE];

	ikcp_update(stream->kcp, p2p_now());
	/* what the peer sent */
	for (;;)
	{
		int size = ikcp_peeksize(stream->kcp);

		if (size > (int)sizeof(message))
		{
			/* larger than any this sends: nothing behind it would get
			through */
			stream_local_closed(stream);
			stream->remote_closed = 1;
			break;
		}
		if (size <= 0)
			break;
		if (size - 1 > STREAM_BUFFER_SIZE - stream->pending_size)
			break;
		size = ikcp_recv(stream->kcp, (char *)message, size);
		if (size <= 0)
			break;
		switch (message[0])
		{
		case _stream_open:
			stream_opened(stream, message + 1, size - 1);
			break;
		case _stream_data:
			if (stream->socket >= 0)
			{
				memcpy(stream->pending + stream->pending_size, message + 1, (size_t)(size - 1));
				stream->pending_size += size - 1;
			}
			break;
		case _stream_close:
			stream->remote_closed = 1;
			if (!stream->closed_time)
				stream->closed_time = p2p_now();
			break;
		}
	}
	stream_flush_pending(stream);
	if (stream->remote_closed && !stream->pending_size && stream->socket >= 0)
		stream_local_closed(stream);
	/* finished: both ends closed and everything delivered, or the tunnel
	gave up on it */
	if ((stream->local_closed && stream->remote_closed && !ikcp_waitsnd(stream->kcp)) ||
		(stream->closed_time && elapsed(stream->closed_time, STREAM_LINGER_TIME)) ||
		(stream->state == _stream_awaiting_open && elapsed(stream->created_time, STREAM_LINGER_TIME)) ||
		stream->kcp->state == (IUINT32)-1)
	{
		stream_free(stream);
	}
}

static void stream_readable(struct stream *stream)
{
	unsigned char buffer[STREAM_CHUNK_SIZE];
	int count;

	for (count = 0; count < 8 && ikcp_waitsnd(stream->kcp) < STREAM_WINDOW && stream->socket >= 0; count++)
	{
		int size = posix_socket_recv(stream->socket, buffer, sizeof(buffer), 0);

		if (size == 0 || (size < 0 && !would_block()))
		{
			stream_local_closed(stream);
			return;
		}
		if (size < 0)
			return;
		stream_message(stream, _stream_data, buffer, size);
	}
}

static void stream_writeable(struct stream *stream)
{
	if (stream->state == _stream_connecting)
		stream->state = _stream_open_state;
	stream_flush_pending(stream);
}

/* ---------- the tunnel */

static void tunnel_received(const unsigned char *packet, int size, const struct sockaddr_in *from)
{
	unsigned char inner[MAXIMUM_INNER_SIZE];
	unsigned char nonce[P2P_NONCE_SIZE];
	unsigned long long counter;
	struct peer *peer;
	int inner_size;
	int newest;

	/* (the magic first: a tunnel packet's bytes 4 to 7, of the sender and
	its number, can be STUN's magic cookie) */
	if (size < 1 || packet[0] != TUNNEL_MAGIC)
	{
		if (size >= 20 && packet[4] == 0x21 && packet[5] == 0x12 && packet[6] == 0xA4 && packet[7] == 0x42)
			stun_received(packet, size, from);
		return;
	}
	if (size < TUNNEL_HEADER_SIZE + P2P_TAG_SIZE + 1 || size - TUNNEL_HEADER_SIZE - P2P_TAG_SIZE > (int)sizeof(inner))
		return;
	peer = find_peer(packet + 1);
	if (!peer)
		return;
	/* each packet once, sealed by the peer for this direction, its header
	and all */
	counter = packet_counter(packet);
	if (!packet_fresh(peer, counter))
		return;
	packet_nonce(packet, nonce);
	inner_size = p2p_aead_open(peer->receive_key, nonce, packet, TUNNEL_HEADER_SIZE, packet + TUNNEL_HEADER_SIZE,
		size - TUNNEL_HEADER_SIZE, inner);
	if (inner_size < 1)
		return;
	newest = counter > peer->receive_highest;
	packet_received(peer, counter);
	peer_heard(peer, from->sin_addr.s_addr, from->sin_port, newest);
	switch (inner[0])
	{
	case _packet_ping:
		if (inner_size >= 5)
		{
			struct p2p_candidate to;

			inner[0] = _packet_pong;
			to.address = from->sin_addr.s_addr;
			to.port = from->sin_port;
			peer_send_to(peer, &to, inner, 5);
		}
		break;
	case _packet_pong:
		if (inner_size >= 5)
		{
			/* (4 bytes of the clock, as the ping carries) */
			unsigned int sent;

			memcpy(&sent, inner + 1, 4);
			peer->round_trip = (unsigned int)p2p_now() - sent;
		}
		break;
	case _packet_datagram:
		datagram_received(peer, inner, inner_size);
		break;
	case _packet_stream:
		stream_received(peer, inner + 1, inner_size - 1);
		break;
	case _packet_bye:
		drop_peer(peer, "left");
		break;
	}
}

static void tunnel_readable(void)
{
	unsigned char packet[2048];
	int count;

	for (count = 0; count < 256; count++)
	{
		struct sockaddr_in from;
		int from_length = sizeof(from);
		int size = posix_socket_recvfrom(p2p.tunnel_socket, packet, sizeof(packet), 0, &from, &from_length);

		if (size < 0)
			break;
		tunnel_received(packet, size, &from);
	}
}

/* ---------- invites */

/* the host's key hash and the token in an invite link or code within text:
1 if it holds one, -1 if it holds an older version's (with the host's
identifier alone, which a key made to have it could pass for), else 0 */
static int parse_invite(const char *text, unsigned char *host_hash, unsigned char *token)
{
	unsigned char bytes[P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE];
	const char *start = NULL;
	const char *search;
	int digits;
	int index;

	for (search = text; *search && !start; search++)
	{
		static const char prefix[] = "halo://join/";
		int length;

		for (length = 0; prefix[length] && search[length] &&
			(search[length] | 0x20) == prefix[length]; length++)
			;
		if (!prefix[length])
			start = search + length;
	}
	if (!start)
	{
		/* a bare code: the text is its digits alone, spaces around them
		allowed */
		start = text;
		while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
			start++;
		for (search = start; hex_value(*search) >= 0; search++)
			;
		for (; *search; search++)
		{
			if (*search != ' ' && *search != '\t' && *search != '\r' && *search != '\n')
				return 0;
		}
	}
	for (digits = 0; hex_value(start[digits]) >= 0; digits++)
		;
	if (digits == 2 * (P2P_IDENTIFIER_SIZE + P2P_TOKEN_SIZE))
		return -1;
	if (digits != (int)sizeof(bytes) * 2)
		return 0;
	for (index = 0; index < (int)sizeof(bytes); index++)
		bytes[index] = (unsigned char)(hex_value(start[index * 2]) << 4 | hex_value(start[index * 2 + 1]));
	memcpy(host_hash, bytes, P2P_KEY_HASH_SIZE);
	memcpy(token, bytes + P2P_KEY_HASH_SIZE, P2P_TOKEN_SIZE);
	return 1;
}

/* under p2p_lock: parse_invite's result */
static int join_invite(const char *text)
{
	unsigned char hash[P2P_KEY_HASH_SIZE], host[P2P_IDENTIFIER_SIZE], token[P2P_TOKEN_SIZE];
	struct peer *peer;
	int parsed = parse_invite(text, hash, token);

	if (parsed < 0)
		platform_log("Internet play: that invite is from an older version of the game, which this one "
			"cannot join");
	if (parsed <= 0)
		return parsed;
	p2p_identifier_from_hash(hash, host);
	if (!memcmp(host, identifier, P2P_IDENTIFIER_SIZE))
		return 1;
	peer = find_peer(host);
	if (peer && peer->connected)
	{
		platform_log("Internet play: already connected to that invite's host");
		return 1;
	}
	if ((p2p.joining || p2p.join_requested) && !memcmp(hash, p2p.join_host_hash, sizeof(hash)) &&
		!memcmp(token, p2p.join_token, sizeof(token)))
		return 1;
	memcpy(p2p.join_host, host, sizeof(host));
	memcpy(p2p.join_host_hash, hash, sizeof(hash));
	memcpy(p2p.join_token, token, sizeof(token));
	p2p.join_requested = 1;
	return 1;
}

int p2p_join_invite(const char *text)
{
	int result;

	p2p_identifier();
	pthread_mutex_lock(&p2p_lock);
	result = join_invite(text);
	pthread_mutex_unlock(&p2p_lock);
	if (result > 0 && !p2p.running)
		platform_log("Internet play is off (network.online in config.toml): the invite is ignored");
	return result > 0;
}

void p2p_invite_received(const char *text)
{
	/* (an older version's is logged as such) */
	if (!join_invite(text))
		platform_log("Internet play: that is not an invite");
}

static void update_joining(void)
{
	char name[2 * P2P_IDENTIFIER_SIZE + 1];

	if (p2p.join_requested)
	{
		/* the offer carries the public address, if there is one */
		p2p.stun_started = 1;
		if (!stun_settled())
			return;
		p2p.join_requested = 0;
		p2p.joining = 1;
		p2p.join_time = p2p_now();
		p2p_hex(p2p.join_host, P2P_IDENTIFIER_SIZE, name);
		platform_log("Internet play: joining %s's game", name);
		p2p_signal_start();
		p2p_signal_join(p2p.join_host_hash, p2p.join_token);
	}
	else if (p2p.joining && elapsed(p2p.join_time, JOIN_TIMEOUT))
	{
		p2p.joining = 0;
		p2p_signal_stop_joining();
		platform_log("Internet play: no answer from the invite's host; it may have stopped hosting or quit");
	}
}

static int connected_player_count(void)
{
	int count = 0;
	int index;

	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
		count += p2p.peers[index].used && p2p.peers[index].connected && !p2p.peers[index].is_host;
	return count;
}

static void update_hosting(void)
{
	int want = p2p.hosting_socket >= 0;

	if (want && !p2p.hosting)
	{
		char text[2 * (P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE) + 1];

		/* one invite for the whole run, so a link keeps working from game
		to game */
		if (!p2p.has_token)
		{
			unsigned char bytes[P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE];

			posix_random_bytes(p2p.token, sizeof(p2p.token));
			p2p.has_token = 1;
			p2p_key_hash(p2p_public_key(), bytes);
			memcpy(bytes + P2P_KEY_HASH_SIZE, p2p.token, P2P_TOKEN_SIZE);
			p2p_hex(bytes, sizeof(bytes), text);
			snprintf(p2p.invite, sizeof(p2p.invite), "halo://join/%s", text);
		}
		p2p.hosting = 1;
		p2p.stun_started = 1;
		p2p_signal_start();
		p2p_signal_host(p2p.token);
		platform_log("Internet play: hosting. Invite players with this link (it only works while this "
			"copy of the game runs): %s", p2p.invite);
		if (!p2p.invite_copied)
		{
			memcpy(p2p.clipboard, p2p.invite, sizeof(p2p.clipboard));
			p2p.has_clipboard = 1;
			p2p.invite_copied = 1;
		}
		p2p.reported_player_count = -1;
	}
	else if (!want && p2p.hosting)
	{
		p2p.hosting = 0;
		p2p_signal_stop_hosting();
		p2p_discord_set_hosting(NULL, 0, 0);
	}
	if (p2p.hosting)
	{
		/* the game's players, as the game says; else the host and the
		machines the tunnel reaches */
		int count = p2p.game_player_maximum > 0 ? p2p.game_player_count : connected_player_count() + 1;
		int maximum = p2p.game_player_maximum > 0 ? p2p.game_player_maximum : P2P_MAXIMUM_PEERS + 1;

		if (count != p2p.reported_player_count || maximum != p2p.reported_player_maximum)
		{
			p2p.reported_player_count = count;
			p2p.reported_player_maximum = maximum;
			p2p_discord_set_hosting(p2p.invite + strlen("halo://join/"), count, maximum);
		}
	}
}

void p2p_set_game_player_counts(int count, int maximum)
{
	/* (only the game's server writes these: unchanged, it need not wait
	for the lock) */
	count = count < 0 ? 0 : count;
	maximum = maximum < 0 ? 0 : maximum;
	if (count == p2p.game_player_count && maximum == p2p.game_player_maximum)
		return;
	pthread_mutex_lock(&p2p_lock);
	p2p.game_player_count = count;
	p2p.game_player_maximum = maximum;
	pthread_mutex_unlock(&p2p_lock);
}

/* ---------- UPnP (posix_upnp.c): the router forwards a port here */

/* the router asked, on a thread of its own (it takes seconds) */
static void *upnp_thread(void *unused)
{
	unsigned short port, previous_port = 0;
	posix_ulong address = 0;
	unsigned short external_port = 0;
	char error[160] = "";
	int forwarded;

	(void)unused;
	pthread_mutex_lock(&p2p_lock);
	port = p2p.tunnel_port;
	if (p2p.upnp_forwarded)
		previous_port = p2p.upnp_candidate.port;
	pthread_mutex_unlock(&p2p_lock);
	/* a renewal asks for the port the router gave before; if it gives
	another, the old forwarding goes */
	forwarded = posix_upnp_forward_udp(port, previous_port ? previous_port : port, &address, &external_port,
		error, sizeof(error));
	if (forwarded && previous_port && previous_port != external_port)
		posix_upnp_stop_forwarding_udp(previous_port);
	pthread_mutex_lock(&p2p_lock);
	p2p.upnp_working = 0;
	p2p.upnp_time = p2p_now();
	if (forwarded)
	{
		char text[32];

		if (!p2p.upnp_forwarded || p2p.upnp_candidate.address != address ||
			p2p.upnp_candidate.port != external_port)
		{
			platform_log("Internet play: the router forwards %s to this machine (UPnP)",
				address_text(address, external_port, text));
		}
		p2p.upnp_forwarded = 1;
		p2p.upnp_candidate.address = address;
		p2p.upnp_candidate.port = external_port;
	}
	else if (!p2p.upnp_forwarded)
	{
		platform_log("Internet play: UPnP: %s", error);
	}
	pthread_mutex_unlock(&p2p_lock);
	return NULL;
}

/* the game exits: the router forwards the port no longer (after a request
under way, which may forward one, if it ends soon) */
static void upnp_release(void)
{
	unsigned short external_port = 0;
	unsigned long start = p2p_now();

	pthread_mutex_lock(&p2p_lock);
	p2p.upnp_released = 1;
	while (p2p.upnp_working && !elapsed(start, UPNP_RELEASE_WAIT))
	{
		pthread_mutex_unlock(&p2p_lock);
		Sleep(50);
		pthread_mutex_lock(&p2p_lock);
	}
	/* (still under way: posix_upnp.c takes one caller at a time) */
	if (p2p.upnp_forwarded && !p2p.upnp_working)
	{
		external_port = p2p.upnp_candidate.port;
		p2p.upnp_forwarded = 0;
	}
	pthread_mutex_unlock(&p2p_lock);
	if (external_port)
		posix_upnp_stop_forwarding_udp(external_port);
}

/* whether a forwarded port would help: a player reaching this host (at
once: they may need it), or a peer not reached in a while (every copy of
the game listens, and so has an invite, from its start: a host asks only
when its invite is used) */
static int upnp_needed(void)
{
	int index;

	if (p2p.joining && elapsed(p2p.join_time, UPNP_JOIN_DELAY))
		return 1;
	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		struct peer const *peer = &p2p.peers[index];

		if (peer->used && !peer->connected &&
			((p2p.hosting && !peer->is_host) || elapsed(peer->offered_time, UPNP_JOIN_DELAY)))
		{
			return 1;
		}
	}
	return 0;
}

static void update_upnp(void)
{
	static int allowed = -1;
	int ask;
	pthread_t thread;

	if (allowed < 0)
		allowed = config_boolean("network.allow_upnp") ? 1 : 0;
	if (!allowed || p2p.upnp_working || p2p.upnp_released)
		return;
	if (p2p.upnp_forwarded)
		ask = elapsed(p2p.upnp_time, UPNP_RENEW_INTERVAL);
	else
		ask = upnp_needed() && (!p2p.upnp_asked || elapsed(p2p.upnp_time, UPNP_RETRY_INTERVAL));
	if (!ask)
		return;
	p2p.upnp_working = 1;
	p2p.upnp_asked = 1;
	if (!p2p.upnp_release_registered)
	{
		p2p.upnp_release_registered = 1;
		atexit(upnp_release);
	}
	if (pthread_create(&thread, NULL, upnp_thread, NULL) != 0)
	{
		p2p.upnp_working = 0;
		p2p.upnp_time = p2p_now();
		return;
	}
	pthread_detach(thread);
}

const char *p2p_take_clipboard_text(void)
{
	static char text[P2P_LINK_SIZE];
	const char *result = NULL;

	if (!p2p.running)
		return NULL;
	pthread_mutex_lock(&p2p_lock);
	if (p2p.has_clipboard)
	{
		memcpy(text, p2p.clipboard, sizeof(text));
		p2p.has_clipboard = 0;
		result = text;
	}
	pthread_mutex_unlock(&p2p_lock);
	return result;
}

/* ---------- invites from elsewhere */

/* the first command line argument holding an invite (or an older
version's, which the copy that takes it says it cannot join) */
static int command_line_invite(char *text, int size)
{
	int index;

	for (index = 1; posix_command_line_argument(index, text, (posix_ulong)size); index++)
	{
		unsigned char hash[P2P_KEY_HASH_SIZE], token[P2P_TOKEN_SIZE];

		if (parse_invite(text, hash, token))
			return 1;
	}
	return 0;
}

/* the key of this user's copies of the game, from a secret only they can
read: another user's program may have the port, and must neither read the
invites nor pass its own */
static int handoff_key(unsigned char *key)
{
	unsigned char secret[P2P_SHA256_SIZE];

	if (!posix_user_secret(secret, sizeof(secret)))
		return 0;
	p2p_hmac_sha256(secret, sizeof(secret), "halo handoff", 12, key);
	return 1;
}

/* the answer to a handed over invite: that the copy that took it has the
key */
static void handoff_answer(const unsigned char *key, const unsigned char *message, unsigned char *answer)
{
	unsigned char digest[P2P_SHA256_SIZE];

	p2p_hmac_sha256(key, P2P_SHA256_SIZE, message, 12 + P2P_NONCE_SIZE, digest);
	memcpy(answer, digest, 16);
}

int p2p_hand_off_invite(void)
{
#ifdef HALO_ANDROID
	return 0;
#else
	char invite[256];
	unsigned char key[P2P_SHA256_SIZE];
	unsigned char message[12 + sizeof(invite) + P2P_SEAL_OVERHEAD];
	unsigned char answer[16];
	struct sockaddr_in to;
	int size;
	int socket;
	int attempt;
	int result = 0;

	if (!command_line_invite(invite, sizeof(invite)) || !handoff_key(key))
		return 0;
	socket = open_socket(SOCK_DGRAM, network_long(0x7F000001), 0, NULL);
	if (socket < 0)
		return 0;
	memcpy(message, "halo-invite ", 12);
	size = 12 + p2p_seal(key, invite, (int)strlen(invite), message + 12);
	handoff_answer(key, message, answer);
	make_address(&to, network_long(0x7F000001), network_short(HANDOFF_PORT));
	for (attempt = 0; attempt < 3 && !result; attempt++)
	{
		int read[1] = { socket };
		int read_count = 1, write_count = 0, error_count = 0;
		unsigned char reply[32];

		posix_socket_sendto(socket, message, size, 0, &to, sizeof(to));
		if (posix_socket_select(read, &read_count, NULL, &write_count, NULL, &error_count, 0, 150000, 0) > 0 &&
			posix_socket_recv(socket, reply, sizeof(reply), 0) == (int)sizeof(answer) &&
			p2p_equal(reply, answer, sizeof(answer)))
		{
			result = 1;
		}
	}
	posix_socket_close(socket);
	if (result)
		platform_log("Internet play: passed the invite to the copy of the game already running");
	return result;
#endif
}

static void handoff_readable(void)
{
	unsigned char message[12 + 256 + P2P_SEAL_OVERHEAD];
	char invite[257];
	unsigned char answer[16];
	struct sockaddr_in from;
	int from_length = sizeof(from);
	int size = posix_socket_recvfrom(p2p.handoff_socket, message, sizeof(message), 0, &from, &from_length);

	if (size < 12 + P2P_SEAL_OVERHEAD || from.sin_addr.s_addr != network_long(0x7F000001) ||
		memcmp(message, "halo-invite ", 12) || !p2p.has_handoff_key)
		return;
	size = p2p_open(p2p.handoff_key, message + 12, size - 12, (unsigned char *)invite);
	if (size < 0)
		return;
	invite[size] = 0;
	handoff_answer(p2p.handoff_key, message, answer);
	posix_socket_sendto(p2p.handoff_socket, answer, sizeof(answer), 0, &from, sizeof(from));
	p2p_invite_received(invite);
}

#ifdef HALO_ANDROID
/* the app's activity writes a link it was opened with here */
static void poll_invite_file(void)
{
	static unsigned long checked_time;
	char path[512];
	char taken[512];
	char text[256];
	FILE *file;
	size_t size;

	if (!elapsed(checked_time, 1000))
		return;
	checked_time = p2p_now();
	snprintf(path, sizeof(path), "%s/join_link.txt", platform_data_root());
	snprintf(taken, sizeof(taken), "%s/join_link.taken", platform_data_root());
	/* (taken first: a link the launcher writes while this reads is left for
	the next look, not removed unread) */
	if (rename(path, taken) != 0)
		return;
	file = fopen(taken, "rb");
	if (!file)
		return;
	size = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	remove(taken);
	text[size] = 0;
	p2p_invite_received(text);
}
#endif

/* ---------- the thread */

static void *p2p_thread(void *unused)
{
	enum
	{
		MAXIMUM_SOCKETS = 2 + MAXIMUM_PROXIES + MAXIMUM_LISTENERS + MAXIMUM_STREAMS + 16,
		/* what each socket waited for is (owners) */
		_owner_tunnel = 0,
		_owner_handoff,
		_owner_proxy,
		_owner_listener,
		_owner_stream,
		_owner_signal,
	};
	/* the sockets waited for, and those that are ready (the same order,
	which posix_socket_select keeps), and whose each is: its kind in the low
	byte, its index above */
	static int read[MAXIMUM_SOCKETS], write[MAXIMUM_SOCKETS];
	static int asked_read[MAXIMUM_SOCKETS], asked_write[MAXIMUM_SOCKETS];
	static int read_owners[MAXIMUM_SOCKETS], write_owners[MAXIMUM_SOCKETS];

	(void)unused;
	pthread_mutex_lock(&p2p_lock);
#ifndef HALO_ANDROID
	/* (here: it may wait for a program) */
	p2p_register_url_scheme("halo", "Halo: Combat Evolved invite");
#endif
	for (;;)
	{
		int read_count = 0, write_count = 0, error_count = 0;
		int asked_read_count, asked_write_count;
		/* KCP's clock needs a pass every LOOP_INTERVAL while it carries
		streams; otherwise the thread can sleep longer */
		int wait = LOOP_INTERVAL * 5;
		int index, asked;

		/* what to wait for */
		read_owners[read_count] = _owner_tunnel;
		read[read_count++] = p2p.tunnel_socket;
		if (p2p.handoff_socket >= 0)
		{
			read_owners[read_count] = _owner_handoff;
			read[read_count++] = p2p.handoff_socket;
		}
		for (index = 0; index < MAXIMUM_PROXIES; index++)
		{
			if (p2p.proxies[index].socket >= 0)
			{
				read_owners[read_count] = _owner_proxy | index << 8;
				read[read_count++] = p2p.proxies[index].socket;
			}
		}
		for (index = 0; index < MAXIMUM_LISTENERS; index++)
		{
			if (p2p.listeners[index].socket >= 0)
			{
				read_owners[read_count] = _owner_listener | index << 8;
				read[read_count++] = p2p.listeners[index].socket;
			}
		}
		for (index = 0; index < MAXIMUM_STREAMS; index++)
		{
			struct stream *stream = &p2p.streams[index];

			if (stream->used)
				wait = LOOP_INTERVAL;
			if (!stream->used || stream->socket < 0)
				continue;
			/* (not while the tunnel's window is full, which stream_readable
			waits out, or the wait would return at once) */
			if (stream->state == _stream_open_state && ikcp_waitsnd(stream->kcp) < STREAM_WINDOW)
			{
				read_owners[read_count] = _owner_stream | index << 8;
				read[read_count++] = stream->socket;
			}
			if (stream->state == _stream_connecting || stream->pending_size)
			{
				write_owners[write_count] = _owner_stream | index << 8;
				write[write_count++] = stream->socket;
			}
		}
		asked_read_count = read_count;
		asked_write_count = write_count;
		p2p_signal_select_sets(read, &read_count, write, &write_count,
			MAXIMUM_SOCKETS - (read_count > write_count ? read_count : write_count));
		for (index = asked_read_count; index < read_count; index++)
			read_owners[index] = _owner_signal;
		for (index = asked_write_count; index < write_count; index++)
			write_owners[index] = _owner_signal;
		asked_read_count = read_count;
		asked_write_count = write_count;
		memcpy(asked_read, read, sizeof(*read) * (size_t)read_count);
		memcpy(asked_write, write, sizeof(*write) * (size_t)write_count);

		pthread_mutex_unlock(&p2p_lock);
		if (posix_socket_select(read, &read_count, write, &write_count, NULL, &error_count, 0,
			wait * 1000, 0) < 0)
		{
			read_count = write_count = 0;
		}
		pthread_mutex_lock(&p2p_lock);

		/* what is ready (the lists now hold only ready sockets, in the order
		asked, so each one's owner is found going along both); a socket
		closed meanwhile is not its owner's any more */
		for (index = 0, asked = 0; index < read_count; index++)
		{
			int socket = read[index];
			int owner, entry;

			while (asked < asked_read_count && asked_read[asked] != socket)
				asked++;
			if (asked == asked_read_count)
				break;
			owner = read_owners[asked++];
			entry = owner >> 8;
			switch (owner & 255)
			{
			case _owner_tunnel:
				tunnel_readable();
				break;
			case _owner_handoff:
				if (socket == p2p.handoff_socket)
					handoff_readable();
				break;
			case _owner_proxy:
				if (p2p.proxies[entry].socket == socket)
					proxy_readable(&p2p.proxies[entry]);
				break;
			case _owner_listener:
				if (p2p.listeners[entry].socket == socket)
					listener_readable(&p2p.listeners[entry]);
				break;
			case _owner_stream:
				if (p2p.streams[entry].used && p2p.streams[entry].socket == socket)
					stream_readable(&p2p.streams[entry]);
				break;
			}
		}
		for (index = 0, asked = 0; index < write_count; index++)
		{
			int socket = write[index];
			int entry;

			while (asked < asked_write_count && asked_write[asked] != socket)
				asked++;
			if (asked == asked_write_count)
				break;
			entry = write_owners[asked] >> 8;
			if ((write_owners[asked++] & 255) == _owner_stream && p2p.streams[entry].used &&
				p2p.streams[entry].socket == socket)
			{
				stream_writeable(&p2p.streams[entry]);
			}
		}
		/* a connection to the game that failed is in neither list */
		for (index = 0; index < MAXIMUM_STREAMS; index++)
		{
			struct stream *stream = &p2p.streams[index];

			if (stream->used && stream->state == _stream_connecting && elapsed(stream->created_time, 5000))
				stream_local_closed(stream);
		}
		p2p_signal_update(read, read_count, write, write_count);

		for (index = 0; index < MAXIMUM_STREAMS; index++)
		{
			if (p2p.streams[index].used)
				stream_update(&p2p.streams[index]);
		}
		update_peers();
		expire_proxies();
		stun_update();
		update_hosting();
		update_joining();
		update_upnp();
		p2p_discord_update();
#ifdef HALO_ANDROID
		poll_invite_file();
#endif
	}
	return NULL;
}

void p2p_initialize(unsigned long local_address)
{
	char invite[256];
	pthread_t thread;
	long tunnel_port = config_integer("network.tunnel_port");
	int index;

	p2p_identifier();
	if (p2p.running || !config_boolean("network.online"))
		return;
	if (tunnel_port < 0 || tunnel_port > 65535)
	{
		platform_log("Internet play: network.tunnel_port %ld is not a port (0 to 65535); the game selects one",
			tunnel_port);
		tunnel_port = 0;
	}
	for (index = 0; index < MAXIMUM_PROXIES; index++)
		p2p.proxies[index].socket = -1;
	for (index = 0; index < MAXIMUM_LISTENERS; index++)
		p2p.listeners[index].socket = -1;
	for (index = 0; index < MAXIMUM_STREAMS; index++)
		p2p.streams[index].socket = -1;
	p2p.local_address = local_address;
	p2p.tunnel_socket = open_socket(SOCK_DGRAM, 0, network_short((unsigned short)tunnel_port), &p2p.tunnel_port);
	if (p2p.tunnel_socket < 0)
	{
		platform_log("Internet play: cannot open its socket (network.tunnel_port %ld in use?); it is off",
			tunnel_port);
		return;
	}
	{
		/* all of a host's traffic, up to a 128-machine game's, goes through
		this one socket: buffers as large as the game's own sockets' (at
		least 1 MB, transport_endpoint_winsock.c) */
		int size = 1 << 20;

		posix_socket_setsockopt(p2p.tunnel_socket, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
		posix_socket_setsockopt(p2p.tunnel_socket, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
	}
#if !defined(HALO_ANDROID) && !defined(HALO_RELAY)
	/* the first copy of the game takes the invites later ones are opened
	with */
	p2p.has_handoff_key = handoff_key(p2p.handoff_key);
	if (p2p.has_handoff_key)
		p2p.handoff_socket = open_socket(SOCK_DGRAM, network_long(0x7F000001), network_short(HANDOFF_PORT), NULL);
#endif
	stun_setup();
	if (pthread_create(&thread, NULL, p2p_thread, NULL) != 0)
	{
		close_socket(&p2p.tunnel_socket);
		close_socket(&p2p.handoff_socket);
		return;
	}
	pthread_detach(thread);
	p2p.running = 1;
#ifndef HALO_RELAY
	if (command_line_invite(invite, sizeof(invite)))
		p2p_join_invite(invite);
#endif
}
