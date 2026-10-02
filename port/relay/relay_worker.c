/* One invite, one native P2P peer and one browser per process. No game code. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "p2p.h"
#include "posix.h"

#define MAX_RECORD 65536u
#define MAX_STREAMS 16
#define STREAM_PENDING 131072u
#define HEADER 24u
#define DATAGRAM 1u
#define OPEN 2u
#define DATA 3u
#define CLOSE 4u
#define REFUSE 5u
extern unsigned long GetTickCount(void);
extern void p2p_sha256(const void *data, int size, unsigned char *digest);
struct stream {
    int used, fd, closing;
    uint16_t local_port, remote_port;
    uint32_t remote_ip;
    size_t pending_size;
    unsigned char pending[STREAM_PENDING];
#ifdef HALO_RELAY_TEST
    uint32_t observed, checksum;
    int test_echo;
#endif
};
static struct stream streams[MAX_STREAMS];
static int udp[2] = {-1, -1};
static uint32_t local_ip, browser_ip, host_ip;
static unsigned char host_identifier[6];
static int joined, connected;
static unsigned long started, last_input;
static unsigned char input[MAX_RECORD + 4];
static size_t input_size;
#ifdef HALO_RELAY_TEST
static int echo_host, host_listener = -1, echo_connections;
#endif

static uint32_t get32(const unsigned char *b)
{ return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static void put32(unsigned char *b, uint32_t n)
{ b[0] = n; b[1] = n >> 8; b[2] = n >> 16; b[3] = n >> 24; }
static uint16_t get_port(const unsigned char *b) { uint16_t n; memcpy(&n, b, 2); return n; }
static void put_port(unsigned char *b, uint16_t n) { memcpy(b, &n, 2); }
static int nonblock(int fd) { return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }
static void write_all(const unsigned char *bytes, size_t size)
{
    unsigned long began = GetTickCount();
    while (size) {
        ssize_t count = write(STDOUT_FILENO, bytes, size);
        if (count > 0) { bytes += count; size -= (size_t)count; continue; }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            GetTickCount() - began < 1000) {
            struct pollfd fd = {STDOUT_FILENO, POLLOUT, 0};
            poll(&fd, 1, 20); continue;
        }
        _exit(2); /* Close a stalled session; never drop reliable bytes. */
    }
}
static void record(unsigned char type, const void *payload, size_t size)
{
    unsigned char prefix[5];
    put32(prefix, (uint32_t)size + 1); prefix[4] = type;
    write_all(prefix, sizeof(prefix)); write_all(payload, size);
}
static void event(const char *json) { record(3, json, strlen(json)); }
static void fail(const char *code)
{
    char message[160];
    snprintf(message, sizeof(message), "{\"type\":\"error\",\"code\":\"%s\"}", code);
    event(message); exit(1);
}
static int make_socket(int type, uint16_t port)
{
    struct sockaddr_in local = {0};
    int fd = socket(AF_INET, type, 0);
    local.sin_family = AF_INET; local.sin_addr.s_addr = local_ip; local.sin_port = port;
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) || nonblock(fd)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}
static void send_frame(uint32_t kind, uint32_t source, uint16_t source_port,
    uint16_t destination_port, const void *payload, size_t length)
{
    unsigned char frame[HEADER + 16000 + 4] = {0};
    size_t size = (HEADER + length + 3) & ~(size_t)3;
    if (length > 16000) fail("packet_too_large");
    put32(frame, (uint32_t)size); put32(frame + 4, kind);
    put32(frame + 8, source); put32(frame + 12, browser_ip);
    put_port(frame + 16, source_port); put_port(frame + 18, destination_port);
    put32(frame + 20, (uint32_t)length);
    if (length) memcpy(frame + HEADER, payload, length);
    record(2, frame, size);
}
static void hex_identifier(const unsigned char *identifier, char *text)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        text[i*2] = digits[identifier[i] >> 4]; text[i*2+1] = digits[identifier[i] & 15];
    }
    text[12] = 0;
}
static void peer_event(int value)
{
    char id[13], message[200];
    hex_identifier(host_identifier, id);
    snprintf(message, sizeof(message),
        "{\"type\":\"peer\",\"identifier\":\"%s\",\"address\":%u,\"connected\":%s}",
        id, host_ip, value ? "true" : "false");
    event(message);
}
static void initialize_peer(void)
{
    char id[13], message[180];
    const unsigned char *identifier = p2p_identifier();
    unsigned char digest[32];
    uint32_t virtual_value;
    /* Each worker's fixed game ports get a separate loopback address. */
    local_ip = htonl(0x7f010000u | (uint32_t)identifier[4] << 8 | (1 + identifier[5] % 254));
    p2p_sha256(identifier, 6, digest);
    virtual_value = (uint32_t)digest[0] << 16 | (uint32_t)digest[1] << 8 | digest[2];
    while ((virtual_value & 255) == 0 || (virtual_value & 255) == 255) virtual_value++;
    browser_ip = htonl(0x64400000u | (virtual_value & 0x3fffffu));
    udp[0] = make_socket(SOCK_DGRAM, htons(5150));
    udp[1] = make_socket(SOCK_DGRAM, htons(5151));
    if (udp[0] < 0 || udp[1] < 0) fail("local_socket_unavailable");
    p2p_initialize(local_ip);
    hex_identifier(identifier, id);
    snprintf(message, sizeof(message),
        "{\"type\":\"ready\",\"protocol\":1,\"identifier\":\"%s\",\"address\":%u}", id, browser_ip);
    event(message);
}
static void start_join(const unsigned char *code, size_t size)
{
    char invite[45];
    if (joined || size != 44) fail("invalid_invite");
    for (size_t i = 0; i < size; i++) {
        unsigned char c = code[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            fail("invalid_invite");
        invite[i] = (char)c;
    }
    invite[44] = 0;
    for (int i = 0; i < 6; i++) {
        char pair[3] = {invite[2*i], invite[2*i+1], 0};
        host_identifier[i] = (unsigned char)strtoul(pair, NULL, 16);
    }
    initialize_peer();
    joined = 1; started = GetTickCount();
    if (!p2p_join_invite(invite)) fail("invalid_invite");
    memset(invite, 0, sizeof(invite));
}
static struct stream *find_stream(uint16_t local_port, uint32_t remote, uint16_t remote_port)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i].used && streams[i].local_port == local_port &&
            streams[i].remote_ip == remote && streams[i].remote_port == remote_port) return &streams[i];
    return NULL;
}
static void close_stream(struct stream *s, int notify)
{
#ifdef HALO_RELAY_TEST
    if (echo_host) {
        char message[160];
        snprintf(message, sizeof(message),
            "{\"type\":\"test_stream_closed\",\"bytes\":%u,\"checksum\":%u}", s->observed, s->checksum);
        event(message);
        notify = 0;
    }
#endif
    if (notify) send_frame(CLOSE, s->remote_ip, s->remote_port, s->local_port, NULL, 0);
    close(s->fd); memset(s, 0, sizeof(*s));
}
static void game_packet(const unsigned char *frame, size_t size)
{
    uint32_t kind, source, destination, length;
    uint16_t source_port, destination_port;
    struct sockaddr_in target = {0};
    unsigned long translated;
    unsigned short translated_port;
    if (!connected || size < HEADER || get32(frame) != size) fail("invalid_packet");
    kind = get32(frame + 4); source = get32(frame + 8); destination = get32(frame + 12);
    source_port = get_port(frame + 16); destination_port = get_port(frame + 18);
    length = get32(frame + 20);
    if (source != browser_ip || length > 16000 || ((HEADER + length + 3) & ~3u) != size ||
        (destination != host_ip && destination != 0xffffffffu)) fail("invalid_packet");
    target.sin_family = AF_INET;
    if (kind == DATAGRAM) {
        int source_index = ntohs(source_port) - 5150;
        int destination_number = ntohs(destination_port);
        if (source_index < 0 || source_index > 1 || length > 1395 ||
            (destination_number != 5150 && destination_number != 5151)) fail("invalid_datagram");
        /* There is one permitted peer: native broadcasts are sent only there. */
        translated = host_ip; translated_port = destination_port;
        if (!p2p_outgoing(0, &translated, &translated_port)) fail("peer_unavailable");
        target.sin_addr.s_addr = (uint32_t)translated; target.sin_port = translated_port;
        sendto(udp[source_index], frame + HEADER, length, 0, (struct sockaddr *)&target, sizeof(target));
        return;
    }
    if (destination != host_ip || ntohs(destination_port) != 5150 || ntohs(source_port) < 1024)
        fail("invalid_stream");
    struct stream *s = find_stream(source_port, destination, destination_port);
    if (kind == OPEN) {
        if (s || length) fail("invalid_stream");
        for (int i = 0; i < MAX_STREAMS; i++) if (!streams[i].used) { s = &streams[i]; break; }
        if (!s) fail("stream_limit");
        translated = destination; translated_port = destination_port;
        if (!p2p_outgoing(1, &translated, &translated_port)) fail("peer_unavailable");
        target.sin_addr.s_addr = (uint32_t)translated; target.sin_port = translated_port;
        int fd = make_socket(SOCK_STREAM, 0);
        if (fd < 0 || (connect(fd, (struct sockaddr *)&target, sizeof(target)) < 0 && errno != EINPROGRESS)) {
            if (fd >= 0) close(fd);
            send_frame(REFUSE, destination, destination_port, source_port, NULL, 0); return;
        }
        s->used = 1; s->fd = fd; s->local_port = source_port;
        s->remote_port = destination_port; s->remote_ip = destination;
    } else if (kind == DATA) {
        if (!s || s->closing || !length || length > STREAM_PENDING - s->pending_size) fail("stream_backpressure");
        memcpy(s->pending + s->pending_size, frame + HEADER, length); s->pending_size += length;
    } else if (kind == CLOSE) {
        if (length) fail("invalid_stream");
        if (s) s->closing = 1;
    } else fail("invalid_packet_kind");
}
static void consume_input(void)
{
    ssize_t size = read(STDIN_FILENO, input + input_size, sizeof(input) - input_size);
    if (!size) exit(0);
    if (size < 0) { if (errno == EAGAIN || errno == EINTR) return; exit(1); }
    input_size += (size_t)size;
    while (input_size >= 4) {
        uint32_t record_size = get32(input);
        if (!record_size || record_size > MAX_RECORD) fail("invalid_record");
        if (input_size < 4 + record_size) return;
        if (input[4] == 1) start_join(input + 5, record_size - 1);
        else if (input[4] == 2) game_packet(input + 5, record_size - 1);
        else fail("invalid_record_type");
        last_input = GetTickCount();
        size_t consumed = 4 + record_size;
        memmove(input, input + consumed, input_size - consumed); input_size -= consumed;
    }
}
static void receive_udp(int index)
{
    unsigned char payload[2048];
    struct sockaddr_in from;
    socklen_t from_size = sizeof(from);
    ssize_t size;
    while ((size = recvfrom(udp[index], payload, sizeof(payload), 0, (struct sockaddr *)&from, &from_size)) >= 0) {
#ifdef HALO_RELAY_TEST
        if (echo_host) {
            sendto(udp[index], payload, (size_t)size, 0, (struct sockaddr *)&from, from_size); continue;
        }
#endif
        unsigned long address = from.sin_addr.s_addr;
        unsigned short port = from.sin_port;
        if (size <= 1395 && p2p_incoming(0, &address, &port) && address == host_ip)
            send_frame(DATAGRAM, (uint32_t)address, port, htons(5150 + index), payload, (size_t)size);
    }
}
static void service_stream(struct stream *s, short ready)
{
    if (ready & POLLOUT) {
        if (s->pending_size) {
            ssize_t sent = send(s->fd, s->pending, s->pending_size, 0);
            if (sent > 0) {
                memmove(s->pending, s->pending + sent, s->pending_size - (size_t)sent);
                s->pending_size -= (size_t)sent;
            } else if (sent < 0 && errno != EAGAIN && errno != EINTR) { close_stream(s, 1); return; }
        }
        if (s->closing && !s->pending_size) { close_stream(s, 0); return; }
    }
    if (ready & (POLLIN | POLLHUP | POLLERR)) {
        unsigned char bytes[16000];
        ssize_t got = recv(s->fd, bytes, sizeof(bytes), 0);
        if (got > 0) {
#ifdef HALO_RELAY_TEST
            if (echo_host) {
                s->observed += (uint32_t)got;
                for (ssize_t i = 0; i < got; i++) s->checksum = s->checksum * 16777619u ^ bytes[i];
                if (s->test_echo) {
                    if ((size_t)got > STREAM_PENDING - s->pending_size) fail("test_backpressure");
                    memcpy(s->pending + s->pending_size, bytes, (size_t)got); s->pending_size += (size_t)got;
                }
                return;
            }
#endif
            send_frame(DATA, s->remote_ip, s->remote_port, s->local_port, bytes, (size_t)got);
        } else if (!got || (errno != EAGAIN && errno != EINTR)) close_stream(s, 1);
    }
}
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN); nonblock(STDIN_FILENO); nonblock(STDOUT_FILENO);
    started = last_input = GetTickCount();
#ifdef HALO_RELAY_TEST
    echo_host = argc == 2 && !strcmp(argv[1], "--echo-host") && getenv("HALO_RELAY_TEST_BROKER");
    if (echo_host) {
        initialize_peer();
        host_listener = make_socket(SOCK_STREAM, htons(5150));
        if (host_listener < 0 || listen(host_listener, MAX_STREAMS)) fail("test_listener");
        p2p_socket_listening(host_listener); joined = 1;
    } else
#endif
    if (argc != 1) return 2;
    (void)argv;
    for (;;) {
        struct pollfd fds[4 + MAX_STREAMS];
        int stream_indexes[4 + MAX_STREAMS];
        int count = 0;
        fds[count++] = (struct pollfd){STDIN_FILENO, POLLIN, 0};
        for (int i = 0; i < 2; i++) fds[count++] = (struct pollfd){udp[i], POLLIN, 0};
#ifdef HALO_RELAY_TEST
        int listener_index = count;
        if (echo_host) fds[count++] = (struct pollfd){host_listener, POLLIN, 0};
#endif
        int first_stream = count;
        for (int i = 0; i < MAX_STREAMS; i++) if (streams[i].used) {
            stream_indexes[count] = i;
            fds[count++] = (struct pollfd){streams[i].fd, POLLIN |
                ((streams[i].pending_size || streams[i].closing) ? POLLOUT : 0), 0};
        }
        if (poll(fds, (nfds_t)count, 10) < 0 && errno != EINTR) return 1;
        if (fds[0].revents & (POLLIN | POLLHUP)) consume_input();
        for (int i = 0; i < 2; i++) if (fds[i+1].revents & POLLIN) receive_udp(i);
#ifdef HALO_RELAY_TEST
        if (echo_host) {
            const char *invite = p2p_take_clipboard_text();
            if (invite) { char message[160]; snprintf(message, sizeof(message),
                "{\"type\":\"test_invite\",\"invite\":\"%s\"}", invite); event(message); }
            if (fds[listener_index].revents & POLLIN) {
                int fd = accept(host_listener, NULL, NULL);
                if (fd >= 0) {
                    int i; for (i = 0; i < MAX_STREAMS && streams[i].used; i++);
                    if (i == MAX_STREAMS) close(fd);
                    else {
                        nonblock(fd); streams[i].used = 1; streams[i].fd = fd;
                        streams[i].test_echo = ++echo_connections == 1;
                    }
                }
            }
        } else
#endif
        if (joined) {
            unsigned long address;
            if (p2p_peer_address(host_identifier, &address)) {
                if (!connected) { host_ip = (uint32_t)address; connected = 1; peer_event(1); }
            } else if (connected) { peer_event(0); fail("peer_disconnected"); }
            else if (GetTickCount() - started > 95000) fail("host_unavailable");
        }
        for (int i = first_stream; i < count; i++)
            if (streams[stream_indexes[i]].used && fds[i].revents) service_stream(&streams[stream_indexes[i]], fds[i].revents);
        if (!joined && GetTickCount() - started > 10000) fail("invite_timeout");
        if (GetTickCount() - last_input > 300000) fail("session_idle");
        if (GetTickCount() - started > 4ul*60*60*1000) fail("session_expired");
    }
}
