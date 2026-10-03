/* Complete PR #12 packets, bounded queues, no cross-process shared pointers. */
#include "room_bridge.h"
#include <pthread.h>
#include <string.h>
static struct web_shared_state shared;
static pthread_mutex_t command_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t commands[64][4], command_read, command_write;
void ios_room_reset(void) {
    pthread_mutex_lock(&command_lock);
    memset(&shared,0,sizeof(shared));command_read=command_write=0;
    pthread_mutex_unlock(&command_lock);
}
struct web_shared_state *web_shared_state(void) { return &shared; }
static void ring_copy(void *out, const unsigned char *ring, size_t capacity,
    uint32_t offset, size_t size) {
    size_t start = offset & (capacity - 1), first = capacity - start;
    if (first > size) first = size;
    memcpy(out, ring + start, first);
    memcpy((unsigned char *)out + first, ring, size - first);
}
size_t ios_room_read_output(void *bytes, size_t capacity, uint32_t *end) {
    uint32_t read = __atomic_load_n(&shared.net_out_read, __ATOMIC_ACQUIRE);
    uint32_t write = __atomic_load_n(&shared.net_out_write, __ATOMIC_ACQUIRE);
    size_t copied = 0;
    *end = read;
    if (write - read > WEB_NET_OUT_BYTES) return 0;
    while (write - read >= sizeof(struct web_packet_header)) {
        struct web_packet_header h;
        ring_copy(&h, shared.net_out, WEB_NET_OUT_BYTES, read, sizeof(h));
        if (h.size < sizeof(h) || h.size % 4 || h.size > write - read || h.size > capacity - copied) break;
        ring_copy((unsigned char *)bytes + copied, shared.net_out, WEB_NET_OUT_BYTES, read, h.size);
        copied += h.size; read += h.size;
    }
    *end = read; return copied;
}
int ios_room_ack_output(uint32_t end) {
    uint32_t read = __atomic_load_n(&shared.net_out_read, __ATOMIC_ACQUIRE);
    uint32_t write = __atomic_load_n(&shared.net_out_write, __ATOMIC_ACQUIRE);
    if (end - read > write - read) return 0;
    __atomic_store_n(&shared.net_out_read, end, __ATOMIC_RELEASE); return 1;
}
int ios_room_write_input(const void *bytes, size_t size) {
    uint32_t read = __atomic_load_n(&shared.net_in_read, __ATOMIC_ACQUIRE);
    uint32_t write = __atomic_load_n(&shared.net_in_write, __ATOMIC_ACQUIRE);
    const unsigned char *source = bytes; size_t cursor = 0;
    if (!size) return 1;
    if (!bytes) return 0;
    if (write - read > WEB_NET_IN_BYTES || size > WEB_NET_IN_BYTES - (write - read)) return 0;
    while (cursor < size) {
        struct web_packet_header h;
        if (size - cursor < sizeof(h)) return 0;
        memcpy(&h, source + cursor, sizeof(h));
        if (h.size < sizeof(h) || h.size % 4 || h.size > size - cursor ||
            h.length > h.size - sizeof(h) || h.kind < WEB_PACKET_DATAGRAM || h.kind > WEB_PACKET_REFUSE) return 0;
        cursor += h.size;
    }
    size_t start = write & (WEB_NET_IN_BYTES - 1), first = WEB_NET_IN_BYTES - start;
    if (first > size) first = size;
    memcpy(shared.net_in + start, source, first);
    memcpy(shared.net_in, source + first, size - first);
    __atomic_store_n(&shared.net_in_write, write + (uint32_t)size, __ATOMIC_RELEASE); return 1;
}
int ios_room_queue_command(const uint32_t command[4]) {
    int accepted = 0; pthread_mutex_lock(&command_lock);
    if (command_write - command_read < 64) {
        memcpy(commands[command_write++ % 64], command, 16); accepted = 1;
    }
    pthread_mutex_unlock(&command_lock); return accepted;
}
int host_ios_room_poll(unsigned int *command) {
    int available = 0; pthread_mutex_lock(&command_lock);
    if (command_write != command_read) {
        memcpy(command, commands[command_read++ % 64], 16); available = 1;
    }
    pthread_mutex_unlock(&command_lock); return available;
}
int web_net_host_ping(unsigned int address, unsigned int host, unsigned int epoch);
int host_ios_room_ping(unsigned int address, unsigned int host, unsigned int epoch) {
    return web_net_host_ping(address, host, epoch);
}

void ios_room_update_pings(uint32_t host, uint32_t epoch, uint32_t updated,
    const int32_t rows[][2], size_t count) {
    if (count > WEB_PING_PEERS) count = WEB_PING_PEERS;
    __atomic_add_fetch(&shared.ping_sequence, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shared.ping_host, (int32_t)host, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shared.ping_epoch, (int32_t)epoch, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shared.ping_updated, (int32_t)updated, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shared.ping_count, (int32_t)count, __ATOMIC_SEQ_CST);
    for (size_t i = 0; i < count; i++) {
        __atomic_store_n(&shared.ping_peers[i][0], rows[i][0], __ATOMIC_SEQ_CST);
        __atomic_store_n(&shared.ping_peers[i][1], rows[i][1], __ATOMIC_SEQ_CST);
    }
    __atomic_add_fetch(&shared.ping_sequence, 1, __ATOMIC_SEQ_CST);
}
