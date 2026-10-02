/* Focused socket regressions, using the real browser socket implementation.
 * Run from the repository root:
 *   clang -std=c11 -pthread -fsanitize=address -g -Iport/linux/src \
 *     tools/test_web_net.c -o build/test_web_net && build/test_web_net
 */
#include <assert.h>
#include "../port/web/src/web_net.c"

static struct web_shared_state shared;
struct web_shared_state *web_shared_state(void) { return &shared; }

static void incoming(unsigned int kind, unsigned short local_port,
	const void *payload, unsigned int length)
{
	struct web_packet_header header = {
		.size = (sizeof(header) + length + 3) & ~3u,
		.kind = kind,
		.source_ip = 0x02404064u,
		.destination_ip = WEB_DEFAULT_ADDRESS,
		.source_port = swap16(5150),
		.destination_port = local_port,
		.length = length,
	};
	unsigned int write = (unsigned int)shared.net_in_write;

	ring_copy_in(shared.net_in, WEB_NET_IN_BYTES, write, &header, sizeof(header));
	if (length)
		ring_copy_in(shared.net_in, WEB_NET_IN_BYTES, write + sizeof(header), payload, length);
	shared.net_in_write = (int32_t)(write + header.size);
}

static void test_stream_backpressure(void)
{
	int descriptor = posix_socket(AF_INET_VALUE, SOCK_STREAM_VALUE, 0);
	struct web_socket *socket = socket_get(descriptor);
	unsigned char payload[64], small[16];
	unsigned char *received = malloc(STREAM_CAPACITY);
	posix_ulong available;
	unsigned int blocked_read;

	assert(descriptor >= SOCKET_BASE && received);
	assert(posix_socket_set_nonblocking(descriptor, 1) == 0);
	socket->connected = socket->bound = socket->remote_link = 1;
	socket->local.port = swap16(49152);
	socket->remote.ip = 0x02404064u;
	socket->remote.port = swap16(5150);
	socket->stream = malloc(STREAM_CAPACITY);
	assert(socket->stream);
	memset(socket->stream, 'a', STREAM_CAPACITY);
	socket->stream_head = STREAM_CAPACITY - 16; /* Exercise socket-ring wrap. */
	socket->stream_count = STREAM_CAPACITY - 32;
	for (unsigned int i = 0; i < sizeof(payload); i++) payload[i] = (unsigned char)i;
	/* Exercise shared-ring wrap too, and keep CLOSE behind the blocked DATA. */
	shared.net_in_read = shared.net_in_write = WEB_NET_IN_BYTES - 16;
	blocked_read = (unsigned int)shared.net_in_read;
	incoming(WEB_PACKET_DATA, socket->local.port, payload, sizeof(payload));
	incoming(WEB_PACKET_CLOSE, socket->local.port, NULL, 0);
	assert(posix_socket_bytes_available(descriptor, &available) == 0);
	assert(available == STREAM_CAPACITY - 32);
	assert((unsigned int)shared.net_in_read == blocked_read && !socket->peer_closed);
	assert(posix_socket_recv(descriptor, small, sizeof(small), 0) == sizeof(small));
	assert(posix_socket_bytes_available(descriptor, &available) == 0);
	assert((unsigned int)shared.net_in_read == blocked_read); /* Still 16 bytes short. */
	assert(posix_socket_recv(descriptor, small, sizeof(small), 0) == sizeof(small));
	/* The next pump has room for all 64 bytes, then observes the ordered CLOSE. */
	assert(posix_socket_bytes_available(descriptor, &available) == 0);
	assert(available == STREAM_CAPACITY && socket->peer_closed);
	assert(shared.net_in_read == shared.net_in_write);
	assert(posix_socket_recv(descriptor, received, STREAM_CAPACITY, 0) == STREAM_CAPACITY);
	for (int i = 0; i < STREAM_CAPACITY - (int)sizeof(payload); i++) assert(received[i] == 'a');
	assert(memcmp(received + STREAM_CAPACITY - sizeof(payload), payload, sizeof(payload)) == 0);
	assert(posix_socket_recv(descriptor, small, sizeof(small), 0) == 0);
	assert(posix_socket_close(descriptor) == 0);
	free(received);
}

static void test_datagram_peek_and_truncation(void)
{
	int descriptor = posix_socket(AF_INET_VALUE, SOCK_DGRAM_VALUE, 0);
	struct address to = { .family = AF_INET_VALUE, .port = swap16(5151), .ip = WEB_DEFAULT_ADDRESS };
	unsigned char received[32];
	const char payload[] = "datagram bytes";
	posix_ulong available;

	assert(descriptor >= SOCKET_BASE);
	assert(posix_socket_bind(descriptor, &to, sizeof(to)) == 0);
	assert(posix_socket_set_nonblocking(descriptor, 1) == 0);
	incoming(WEB_PACKET_DATAGRAM, to.port, payload, sizeof(payload));
	assert(posix_socket_recv(descriptor, received, 4, 2) == 4); /* MSG_PEEK */
	assert(memcmp(received, payload, 4) == 0);
	assert(posix_socket_bytes_available(descriptor, &available) == 0 && available == sizeof(payload));
	assert(posix_socket_recv(descriptor, received, sizeof(received), 2) == sizeof(payload));
	assert(posix_socket_recv(descriptor, received, sizeof(received), 0) == sizeof(payload));
	assert(memcmp(received, payload, sizeof(payload)) == 0);
	assert(posix_socket_bytes_available(descriptor, &available) == 0 && available == 0);
	incoming(WEB_PACKET_DATAGRAM, to.port, payload, sizeof(payload));
	assert(posix_socket_recv(descriptor, received, 4, 0) == -1);
	assert(posix_socket_last_error() == WSAEMSGSIZE);
	assert(memcmp(received, payload, 4) == 0);
	assert(posix_socket_bytes_available(descriptor, &available) == 0 && available == 0);
	assert(posix_socket_close(descriptor) == 0);
}

static int stream_listener(unsigned short port)
{
	int descriptor = posix_socket(AF_INET_VALUE, SOCK_STREAM_VALUE, 0);
	struct address local = { .family = AF_INET_VALUE, .port = swap16(port), .ip = WEB_DEFAULT_ADDRESS };

	assert(descriptor >= SOCKET_BASE);
	assert(posix_socket_bind(descriptor, &local, sizeof(local)) == 0);
	assert(posix_socket_listen(descriptor, MAXIMUM_BACKLOG) == 0);
	assert(posix_socket_set_nonblocking(descriptor, 1) == 0);
	return descriptor;
}

static void test_reloaded_stream_waits_for_old_endpoint_release(void)
{
	int listener = stream_listener(5152), old_descriptor, fresh_descriptor;
	unsigned short port = swap16(5152);
	const char old_payload[] = "old session bytes", fresh_payload[] = "new session bytes";
	char received[64];
	posix_ulong available;
	unsigned int replacement_read, outgoing_write;

	incoming(WEB_PACKET_OPEN, port, NULL, 0);
	incoming(WEB_PACKET_DATA, port, old_payload, sizeof(old_payload));
	old_descriptor = posix_socket_accept(listener, NULL, NULL);
	assert(old_descriptor >= SOCKET_BASE);
	assert(posix_socket_set_nonblocking(old_descriptor, 1) == 0);
	assert(posix_socket_bytes_available(old_descriptor, &available) == 0 && available == sizeof(old_payload));
	replacement_read = (unsigned int)shared.net_in_read;
	incoming(WEB_PACKET_OPEN, port, NULL, 0);
	incoming(WEB_PACKET_DATA, port, fresh_payload, sizeof(fresh_payload));
	/* A reload must not be accepted until the game has removed the old endpoint. */
	assert(posix_socket_accept(listener, NULL, NULL) == -1);
	assert(posix_socket_last_error() == WSAEWOULDBLOCK);
	assert((unsigned int)shared.net_in_read == replacement_read);
	assert(posix_socket_recv(old_descriptor, received, sizeof(received), 0) == 0);
	assert(posix_socket_send(old_descriptor, old_payload, sizeof(old_payload), 0) == -1);
	assert(posix_socket_last_error() == WSAECONNRESET);
	/* Closing that descriptor must not close the replacement at the peer. */
	outgoing_write = (unsigned int)shared.net_out_write;
	assert(posix_socket_shutdown(old_descriptor, 2) == 0);
	assert(posix_socket_close(old_descriptor) == 0);
	assert((unsigned int)shared.net_out_write == outgoing_write);
	fresh_descriptor = posix_socket_accept(listener, NULL, NULL);
	assert(fresh_descriptor >= SOCKET_BASE);
	assert(posix_socket_set_nonblocking(fresh_descriptor, 1) == 0);
	assert(shared.net_in_read == shared.net_in_write);
	assert(posix_socket_recv(fresh_descriptor, received, sizeof(received), 0) == sizeof(fresh_payload));
	assert(memcmp(received, fresh_payload, sizeof(fresh_payload)) == 0);
	assert(posix_socket_recv(fresh_descriptor, received, sizeof(received), 0) == -1);
	assert(posix_socket_last_error() == WSAEWOULDBLOCK);
	assert(posix_socket_send(fresh_descriptor, fresh_payload, sizeof(fresh_payload), 0) == sizeof(fresh_payload));
	incoming(WEB_PACKET_CLOSE, port, NULL, 0);
	assert(posix_socket_recv(fresh_descriptor, received, sizeof(received), 0) == 0);
	assert(posix_socket_close(fresh_descriptor) == 0);
	assert(posix_socket_close(listener) == 0);
}

static void test_reloaded_pending_stream_replaces_backlog_entry(void)
{
	int listener = stream_listener(5153), descriptor;
	unsigned short port = swap16(5153);
	posix_ulong available;
	int received = -1;
	unsigned int outgoing_write = (unsigned int)shared.net_out_write;

	/* Repeated reloads before accept must not fill the listener's backlog. */
	for (int attempt = 0; attempt < MAXIMUM_BACKLOG * 3; attempt++)
	{
		incoming(WEB_PACKET_OPEN, port, NULL, 0);
		incoming(WEB_PACKET_DATA, port, &attempt, sizeof(attempt));
		assert(posix_socket_bytes_available(listener, &available) == 0);
		assert(socket_get(listener)->backlog_count == 1);
		assert(shared.net_in_read == shared.net_in_write);
		assert((unsigned int)shared.net_out_write == outgoing_write);
	}
	descriptor = posix_socket_accept(listener, NULL, NULL);
	assert(descriptor >= SOCKET_BASE);
	assert(posix_socket_recv(descriptor, &received, sizeof(received), 0) == sizeof(received));
	assert(received == MAXIMUM_BACKLOG * 3 - 1);
	assert(posix_socket_accept(listener, NULL, NULL) == -1);
	assert(posix_socket_last_error() == WSAEWOULDBLOCK);
	assert(posix_socket_close(descriptor) == 0);
	assert(posix_socket_close(listener) == 0);
}


static void test_host_ping_table(void)
{
    const unsigned int host = 0x0a010102, player = 0x0a010103;
    shared.ping_sequence = 2;
    shared.ping_host = ping_address(host);
    shared.ping_epoch = 3;
    shared.ping_updated = 1000;
    shared.ping_count = 2;
    shared.ping_peers[0][0] = ping_address(host); shared.ping_peers[0][1] = 0;
    shared.ping_peers[1][0] = ping_address(player); shared.ping_peers[1][1] = 83;
    assert(web_net_host_ping_at(host, host, 3, 1100) == 0);
    assert(web_net_host_ping_at(player, host, 3, 1100) == 83);
    assert(web_net_host_ping_at(0x0a010104, host, 3, 1100) == -1);
    assert(web_net_host_ping_at(player, player, 3, 1100) == -1);
    assert(web_net_host_ping_at(player, host, 4, 1100) == -1);
    assert(web_net_host_ping_at(player, host, 3, 11000) == -1);
    shared.ping_sequence = 3; // Do not block the renderer on a publishing page.
    assert(web_net_host_ping_at(player, host, 3, 1100) == -1);
    shared.ping_sequence = 4;
    shared.ping_count = WEB_PING_PEERS + 1;
    assert(web_net_host_ping_at(player, host, 3, 1100) == -1);
    shared.ping_count = -1;
    assert(web_net_host_ping_at(player, host, 3, 1100) == -1);
    shared.ping_count = 2;
    shared.ping_peers[1][1] = -1;
    assert(web_net_host_ping_at(player, host, 3, 1100) == -1);
    shared.ping_peers[1][1] = 10000;
    assert(web_net_host_ping_at(player, host, 3, 1100) == -1);
    shared.ping_peers[1][1] = 42;
    shared.ping_updated = (int32_t)0xfffffff0u; // Date.now() low-word rollover.
    assert(web_net_host_ping_at(player, host, 3, 0x10) == 42);
}

int main(void)
{
	test_host_ping_table();
	test_stream_backpressure();
	test_datagram_peek_and_truncation();
	test_reloaded_stream_waits_for_old_endpoint_release();
	test_reloaded_pending_stream_replaces_backlog_entry();
	puts("web_net ping, backpressure, datagram, and stream reload regression tests passed");
	return 0;
}
