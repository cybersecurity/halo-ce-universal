#ifndef HALO_IOS_ROOM_BRIDGE_H
#define HALO_IOS_ROOM_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#include "../../web/src/web_shared.h"
/* One main-thread producer/consumer pairs with the socket backend. */
size_t ios_room_read_output(void *bytes, size_t capacity, uint32_t *end);
int ios_room_ack_output(uint32_t end);
int ios_room_write_input(const void *bytes, size_t size);
int ios_room_queue_command(const uint32_t command[4]);
void ios_room_update_pings(uint32_t host, uint32_t epoch, uint32_t updated,
    const int32_t rows[][2], size_t count);
void host_ios_room_prepare(void);
void host_ios_room_tick(void);
void host_ios_room_open(void);
void ios_room_reset(void);
#endif
