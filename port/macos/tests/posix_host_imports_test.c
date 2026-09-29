#include "../guest_address.h"
#include "../guest_call.h"
#include "../host_imports.h"
#include "../posix_host_imports.h"
#include "../../linux/src/posix.h"

#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* This isolated adapter test records native errno. Actual translated TLS
 * behavior is exercised by translated_errno_test.c, not this leaf. */
static int recorded_file_errno;
int mac_guest_errno_set_native(int error){recorded_file_errno=error;return 0;}

static int alias_mode;
static uint32_t active_esp;
static uint32_t eax_value;

uint32_t mac_guest_host_import_esp(void) { return active_esp; }
uint32_t mac_guest_host_import_arg32(uint32_t index)
{
	uint32_t value;
	assert(mac_guest_stack_arg32(active_esp, index, &value) == 0);
	return value;
}
uint64_t mac_guest_host_import_arg64_words(uint32_t index)
{
	return (uint64_t)mac_guest_host_import_arg32(index) |
		((uint64_t)mac_guest_host_import_arg32(index + 1) << 32);
}
void mac_guest_host_import_return32(uint32_t value)
{
	uint32_t next;
	eax_value = value;
	assert(mac_guest_stack_after_return(active_esp, 0, &next) == 0);
	active_esp = next;
}
void mac_guest_host_import_return64(int64_t value) { mac_guest_host_import_return32((uint32_t)value); }
void mac_guest_host_import_return_void(void)
{
	uint32_t next;
	assert(mac_guest_stack_after_return(active_esp, 0, &next) == 0);
	active_esp = next;
}
void mac_host_abort(const char *message) { fprintf(stderr, "abort: %s\n", message); abort(); }

static int32_t invoke(const char *name, const uint32_t *args, size_t count)
{
 uint32_t frame[10] = {0xdeadbeef};
 mac_guest_import_bridge bridge = mac_macos_posix_host_import_bridge(name);
 assert(bridge && count <= 9);
 memcpy(frame + 1, args, count * 4);
 assert(mac_guest_write(0x1400, frame, (count + 1) * 4) == 0);
 active_esp = 0x1400; bridge(); assert(active_esp == 0x1404);
 return (int32_t)eax_value;
}
#define CALL(name, ...) invoke(#name, (uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__}) / 4)
static void put32(uint32_t va, uint32_t value) { assert(!mac_guest_write(va, &value, 4)); }
static uint32_t get32(uint32_t va) { uint32_t value; assert(!mac_guest_read(va, &value, 4)); return value; }
static void network_roundtrips(void)
{
 /* Linux/Xbox wire sockaddr, deliberately not Darwin sockaddr_in. */
 uint8_t address[16] = {2,0,0,0,127,0,0,1};
 uint8_t actual[16], peer[16];
 if(alias_mode) address[7]=2;
 char text[8];
 int udp, sender, listener, client, accepted;
 assert(!mac_guest_write(0x2000, address, 16));
 assert(!mac_guest_write(0x2300, "protocol", 8));
 udp = CALL(posix_socket, 2, 2, 17); sender = CALL(posix_socket, 2, 2, 17);
 assert(udp >= 0 && sender >= 0);
 assert(CALL(posix_socket_bind, udp, 0x2000, 15) == -1);
 assert(CALL(posix_socket_last_error, 0) == 10022);
 assert(CALL(posix_socket_bind, udp, 0x2000, 16) == 0);
 put32(0x2200, 16);
 assert(CALL(posix_socket_getsockname, udp, 0x2100, 0x2200) == 0 && get32(0x2200) == 16);
 assert(!mac_guest_read(0x2100, actual, 16));
 assert(actual[0] == 2 && actual[1] == 0 && actual[7] == (alias_mode?2:1) && (actual[2] || actual[3]));
 assert(CALL(posix_socket_sendto, sender, 0x2300, 8, 0, 0x2100, 16) == 8);
 put32(0x2500, udp); put32(0x2510, 1); put32(0x2520, sender); put32(0x2530, 1); put32(0x2550, 0);
 assert(CALL(posix_socket_select, 0x2500, 0x2510, 0, 0, 0, 0, 1, 0, 0) == 1);
 put32(0x2510, 1);
 assert(CALL(posix_socket_select, 0x2500, 0x2510, 0x2520, 0x2530, 0, 0x2550, 1, 0, 0) == 2);
 assert(get32(0x2510) == 1 && get32(0x2500) == (uint32_t)udp);
 assert(get32(0x2530) == 1 && get32(0x2520) == (uint32_t)sender);
 assert(CALL(posix_socket_bytes_available, udp, 0x2560) == 0 && get32(0x2560) >= 8);
 put32(0x2200, 16);
 assert(CALL(posix_socket_recvfrom, udp, 0x2400, 8, 0, 0x10ff9, 0x2200) == -1);
 assert(CALL(posix_socket_last_error, 0) == 10014);
 assert(CALL(posix_socket_recvfrom, udp, 0x2400, 8, 0, 0x2000, 0x2200) == 8);
 assert(!mac_guest_read(0x2400, text, 8) && !memcmp(text, "protocol", 8));
 assert(!mac_guest_read(0x2000, peer, 16) && peer[0] == 2 && peer[1] == 0);
 /* Actual connected UDP reproduces Darwin's EISCONN at this boundary. */
 assert(CALL(posix_socket_connect, sender, 0x2100, 16) == 0);
 put32(0x2200, 16); assert(CALL(posix_socket_getpeername, sender, 0x2800, 0x2200) == 0);
 assert(!mac_guest_read(0x2800, actual, 16)); assert(actual[7] == (alias_mode?2:1));
 struct sockaddr_in native_peer; socklen_t native_length = sizeof(native_peer);
 assert(getpeername(sender, (struct sockaddr *)&native_peer, &native_length) == 0);
 assert(sendto(sender, "protocol", 8, 0, (struct sockaddr *)&native_peer, native_length) == -1 && errno == EISCONN);
 assert(CALL(posix_socket_sendto, sender, 0x2300, 8, 0, 0x2100, 16) == 8);
 put32(0x2200,16); assert(CALL(posix_socket_recvfrom, udp, 0x2400, 8, 0, 0x2000, 0x2200) == 8);
 assert(!mac_guest_read(0x2400,text,8) && !memcmp(text,"protocol",8));
 uint8_t alternate[16]; assert(!mac_guest_read(0x2100,alternate,16)); alternate[3] ^= 1;
 assert(!mac_guest_write(0x2900,alternate,16));
 assert(CALL(posix_socket_sendto, sender, 0x2300, 8, 0, 0x2900, 16) == -1);
 assert(CALL(posix_socket_last_error, 0) == 10056);
 assert(CALL(posix_socket_bytes_available, udp, 0x2560) == 0 && get32(0x2560) == 0);
 put32(0x2200,16);assert(CALL(posix_socket_getpeername,sender,0x2800,0x2200)==0);
 assert(!mac_guest_read(0x2800,actual,16) && actual[2]==native_peer.sin_port%256 && actual[3]==native_peer.sin_port/256);
 put32(0x2200, 16); assert(CALL(posix_socket_getsockname, sender, 0x2100, 0x2200) == 0);
 assert(!mac_guest_read(0x2100, actual, 16) && !memcmp(peer, actual, 4));
 assert(CALL(posix_socket_close, sender) == 0 && CALL(posix_socket_close, udp) == 0);
 assert(!mac_guest_write(0x2000, address, 16));
 listener = CALL(posix_socket, 2, 1, 6); client = CALL(posix_socket, 2, 1, 6);
 assert(listener >= 0 && client >= 0);
 assert(CALL(posix_socket_bind, listener, 0x2000, 16) == 0);
 assert(CALL(posix_socket_listen, listener, 1) == 0);
 put32(0x2200, 16); assert(CALL(posix_socket_getsockname, listener, 0x2100, 0x2200) == 0);
 /* Two servers cannot own the same fixed endpoint. Diagnostics must retain
 the actual Darwin-to-Winsock error instead of obscuring EADDRINUSE. */
 assert(setenv("HALO_NETWORK_TRACE", "1", 1) == 0);
 int contender = CALL(posix_socket, 2, 1, 6);
 assert(contender >= 0 && CALL(posix_socket_bind, contender, 0x2100, 16) == -1);
 assert(CALL(posix_socket_last_error, 0) == 10048);
 assert(CALL(posix_socket_listen, -1, 1) == -1);
 assert(CALL(posix_socket_last_error, 0) == 10009);
 assert(CALL(posix_socket_connect, -1, 0x2100, 16) == -1);
 assert(CALL(posix_socket_last_error, 0) == 10009);
 assert(CALL(posix_socket_close, contender) == 0);
 assert(unsetenv("HALO_NETWORK_TRACE") == 0);
 assert(CALL(posix_socket_connect, client, 0x2100, 16) == 0);
 put32(0x2200, 16); accepted = CALL(posix_socket_accept, listener, 0x2000, 0x2200);
 assert(accepted >= 0 && get32(0x2200) == 16);
 assert(!mac_guest_read(0x2000, peer, 16) && peer[0] == 2 && peer[1] == 0);
 assert(CALL(posix_socket_set_nodelay, client) == 0);
 put32(0x2540, accepted); put32(0x2550, 1);
 assert(CALL(posix_socket_select, 0, 0, 0, 0, 0x2540, 0x2550, 0, 0, 0) == 0);
 assert(get32(0x2550) == 0);
 assert(CALL(posix_socket_send, client, 0x2300, 8, 0) == 8);
 assert(CALL(posix_socket_recv, accepted, 0x2400, 8, 0) == 8);
 assert(!mac_guest_read(0x2400, text, 8) && !memcmp(text, "protocol", 8));
 put32(0x2200, 16); assert(CALL(posix_socket_getpeername, accepted, 0x2100, 0x2200) == 0);
 assert(!mac_guest_read(0x2100, actual, 16) && !memcmp(peer, actual, 8));
 memset(actual, 0xcc, 16); assert(!mac_guest_write(0x2100, actual, 16));
 put32(0x2200, 2); assert(CALL(posix_socket_getsockname, client, 0x2100, 0x2200) == 0 && get32(0x2200) == 16);
 assert(!mac_guest_read(0x2100, actual, 16) && actual[0] == 2 && actual[1] == 0 && actual[2] == 0xcc);
 assert(CALL(posix_socket_close, accepted) == 0 && CALL(posix_socket_close, client) == 0 && CALL(posix_socket_close, listener) == 0);
}

int main(int argc, char **argv)
{
 if(argc==2 && !strcmp(argv[1],"--alias")) {
  alias_mode=1;
  assert(!setenv("HALO_LOCAL_TEST_IPV4_ALIAS","127.0.0.2",1));
  assert(!setenv("HALO_NET_ONLINE","false",1));
  assert(!setenv("HALO_NET_ADDRESS","127.0.0.2",1));
 }
 else if(argc==2) {
  uint8_t memory[256]={0}, wire[16]={2,0,0,0,127,0,0,2}; int socket;
  assert(!setenv("HALO_LOCAL_TEST_IPV4_ALIAS", !strcmp(argv[1],"--invalid-ip")?"192.168.1.2":"127.0.0.2",1));
  assert(!setenv("HALO_NET_ONLINE", !strcmp(argv[1],"--online")?"true":"false",1));
  assert(!setenv("HALO_NET_ADDRESS", !strcmp(argv[1],"--collision")?"127.0.0.1":"127.0.0.2",1));
  assert(!mac_guest_address_register(0x1400,memory,sizeof(memory)));
  assert(!mac_guest_write(0x1480,wire,sizeof(wire)));
  socket=CALL(posix_socket,2,2,17); assert(socket>=0);
  assert(CALL(posix_socket_bind,socket,0x1480,16)==-1);
  assert(CALL(posix_socket_last_error,0)==10022);
  assert(CALL(posix_socket_close,socket)==0);
  mac_guest_address_reset(); puts("local test alias rejects invalid/online/collision setup"); return 0;
 }

	uint8_t *memory = calloc(1, 0x10000);
	char temp_path[] = "/tmp/halo-posix-bridge-XXXXXX";
	char lookup_directory[] = "/tmp/halo-posix-find-XXXXXX";
	char lookup_path[256];
	int fd = mkstemp(temp_path);
	uint32_t frame[5] = { 0xdeadbeef, 0x1200, 0x1300, 0, 0 };
	struct posix_file_information info;
	mac_guest_import_bridge stat_bridge;
	assert(memory && fd >= 0);
	assert(sizeof(struct posix_file_information) == 36);
	assert(offsetof(struct posix_file_information, modification_seconds) == 12);
	assert(mac_guest_address_register(0x1000, memory, 0x10000) == 0);
	strcpy((char *)memory + 0x200, temp_path);
	assert(write(fd, "bridge", 6) == 6);
	close(fd);
	memcpy(memory + 0x400, frame, sizeof(frame));
	active_esp = 0x1400;
	stat_bridge = mac_macos_posix_host_import_bridge("posix_stat");
	assert(stat_bridge);
	stat_bridge();
	assert(active_esp == 0x1404 && (int32_t)eax_value == 0);
	memcpy(&info, memory + 0x300, sizeof(info));
	assert(info.size_low == 6 && info.size_high == 0);
	assert(info.modification_seconds != 0);
	strcpy((char *)memory + 0x200, "/nonexistent-halo-errno-fixture/file");
	recorded_file_errno=0;active_esp=0x1400;stat_bridge();
	assert((int32_t)eax_value == -1 && recorded_file_errno == ENOENT);
	strcpy((char *)memory + 0x200, temp_path);
	assert(mkdtemp(lookup_directory) != NULL);
	assert(snprintf(lookup_path, sizeof(lookup_path), "%s/%s", lookup_directory,
		"Map Cache.bin") > 0);
	fd = open(lookup_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0 && write(fd, "x", 1) == 1);
	close(fd);
	strcpy((char *)memory + 0x200, lookup_directory);
	strcpy((char *)memory + 0x300, "map cache.BIN");
	frame[0] = 0xdeadbeef; frame[1] = 0x1200; frame[2] = 0x1300;
	frame[3] = 0x1500; frame[4] = 64;
	memcpy(memory + 0x400, frame, sizeof(frame));
	active_esp = 0x1400;
	stat_bridge = mac_macos_posix_host_import_bridge("posix_find_entry_case_insensitive");
	assert(stat_bridge);
	stat_bridge();
	assert(active_esp == 0x1404 && eax_value == 1);
	assert(strcmp((char *)memory + 0x500, "Map Cache.bin") == 0);
	network_roundtrips();
	assert(mac_macos_posix_host_import_bridge("posix_upnp_stop_forwarding_udp"));
	assert(CALL(posix_upnp_forward_udp, 0, 0x10fff, 0x2200, 0x2300, 64) == 0);
	assert(mac_macos_posix_host_import_bridge("posix_spawn") == NULL);
	assert(mac_guest_address_unregister(0x1000) == 0);
	assert(unlink(temp_path) == 0);
	assert(unlink(lookup_path) == 0);
	assert(rmdir(lookup_directory) == 0);
	free(memory);
	puts("posix_host_imports_test: ok");
	return 0;
}
