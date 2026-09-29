#include "posix_host_imports.h"

#include "guest_address.h"
#include "guest_call.h"
#include "guest_errno.h"
#include "host_imports.h"
#include "host_services.h"
#include "posix_backend_names.h"
#include "../linux/src/posix.h"

#include <errno.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <strings.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int mac_native_posix_bridge_error(int error);

#define POSIX_STRING_CAP 4096u
#define POSIX_IO_CAP (16u * 1024u * 1024u)

static int guest_string(uint32_t address, char output[POSIX_STRING_CAP])
{
	const char *input;
	size_t available, limit, length;
	if (!address || !(input = mac_guest_address_resolve(address, 1)))
		{ errno = EFAULT; return -1; }
	available = mac_guest_address_available(address);
	limit = available < POSIX_STRING_CAP ? available : POSIX_STRING_CAP;
	length = strnlen(input, limit);
	if (length == limit)
		{ errno = EFAULT; return -1; }
	memcpy(output, input, length + 1);
	return 0;
}

static void *guest_span(uint32_t address, size_t size)
{
	if (size > POSIX_IO_CAP)
		{ errno = EFAULT; return NULL; }
	void *span = mac_guest_address_resolve(address, size);
	if (!span) errno = EFAULT;
	return span;
}

#define ARG(n) mac_guest_host_import_arg32(n)
#define RET(v) mac_guest_host_import_return32((uint32_t)(v))
#define VOIDRET() mac_guest_host_import_return_void()
/* Filesystem -1 results follow guest musl errno, unlike Winsock errors. */
#define FILERET(v) do { int file_result_ = (v); if (file_result_ < 0) { \
 int file_error_ = errno; (void)mac_guest_errno_set_native(file_error_); } \
 RET(file_result_); } while (0)


/* Diagnostic transport alias for two real local game instances on Darwin,
 * where unassigned 127/8 addresses cannot bind. This changes endpoint address
 * records only; network payloads and the guest's self-address logic are intact.
 * It is disabled unless explicitly requested with internet play disabled. */
static pthread_once_t test_alias_once = PTHREAD_ONCE_INIT;
static struct { int enabled, valid; uint32_t logical, physical; } test_alias;
static void initialize_test_alias(void)
{
 const char *setting = getenv("HALO_LOCAL_TEST_IPV4_ALIAS");
 const char *online = getenv("HALO_NET_ONLINE");
 const char *local = getenv("HALO_NET_ADDRESS");
 struct in_addr logical, physical, configured;
 uint32_t host;
 test_alias.valid = 1;
 if (!setting) return;
 test_alias.enabled = 1; test_alias.valid = 0;
 if (!online || (strcasecmp(online,"false") && strcmp(online,"0") &&
     strcasecmp(online,"off") && strcasecmp(online,"no"))) goto invalid;
 if (inet_pton(AF_INET,setting,&logical)!=1 || !local || inet_pton(AF_INET,local,&configured)!=1) goto invalid;
 host=ntohl(logical.s_addr);
 if ((host & 0xff000000u)!=0x7f000000u || (host & 0xffffffu)<=1 ||
     (host & 0xffffffu)==0xffffffu) goto invalid;
 if (inet_pton(AF_INET,"127.0.0.1",&physical)!=1 || configured.s_addr==physical.s_addr) goto invalid;
 test_alias.logical=logical.s_addr; test_alias.physical=physical.s_addr; test_alias.valid=1;
 fprintf(stderr,"[local-test-ipv4] logical=%s native=127.0.0.1 offline=1\n",setting);
 return;
invalid:
 fprintf(stderr,"[local-test-ipv4] rejected: require distinct 127/8 alias, explicit network address, and HALO_NET_ONLINE=false\n");
}
static int test_alias_map(uint32_t address, int to_native, uint32_t *result)
{
 pthread_once(&test_alias_once,initialize_test_alias);
 if (!test_alias.valid) return mac_native_posix_bridge_error(EINVAL);
 *result=address;
 if (!test_alias.enabled) return 0;
 /* The physical loopback is reserved for this mapping. Supplying it from
  * the guest would collide with its own special self-address semantics. */
 if (to_native && address==test_alias.physical) return mac_native_posix_bridge_error(EINVAL);
 if (address==(to_native?test_alias.logical:test_alias.physical))
  *result=to_native?test_alias.physical:test_alias.logical;
 return 0;
}

/* Xbox/Linux sockaddr_in begins with a 16-bit AF_INET. Darwin stores an
 * eight-bit length followed by an eight-bit family; port/address keep their
 * network byte order. Marshal this record rather than passing it through. */
static int ipv4_input(uint32_t va, uint32_t length, struct sockaddr_in *native)
{
	uint8_t wire[16];
	uint16_t family;
	if (length != sizeof(wire)) return mac_native_posix_bridge_error(EINVAL);
	if (mac_guest_read(va, wire, sizeof(wire))) return mac_native_posix_bridge_error(EFAULT);
	memcpy(&family, wire, 2);
	if (family != 2) return mac_native_posix_bridge_error(EAFNOSUPPORT);
	memset(native, 0, sizeof(*native));
	native->sin_len = sizeof(*native);
	native->sin_family = AF_INET;
	memcpy((uint8_t *)native + 2, wire + 2, 14);
	return test_alias_map(native->sin_addr.s_addr, 1, &native->sin_addr.s_addr);
}

static int ipv4_output(uint32_t va, uint32_t capacity, const struct sockaddr_in *native)
{
	uint8_t wire[16];
	uint16_t family = 2;
	if (native->sin_family != AF_INET) return -1;
	memcpy(wire, &family, 2);
	memcpy(wire + 2, (const uint8_t *)native + 2, 14);
	uint32_t address;
	if (test_alias_map(native->sin_addr.s_addr, 0, &address)) return -1;
	memcpy(wire + 4, &address, 4);
	return mac_guest_write(va, wire, capacity < sizeof(wire) ? capacity : sizeof(wire));
}

static void b_posix_stat(void)
{
	char path[POSIX_STRING_CAP];
	struct posix_file_information information;
	uint32_t out = ARG(1);
	int result;
	if (guest_string(ARG(0), path) || !guest_span(out, sizeof(information))) { FILERET(-1); return; }
	result = posix_stat(path, &information);
	if (!result && mac_guest_write(out, &information, sizeof(information))) result = -1;
	FILERET(result);
}

static void b_posix_fstat(void)
{
	struct posix_file_information information;
	uint32_t out = ARG(1);
	int result;
	if (!guest_span(out, sizeof(information))) { FILERET(-1); return; }
	result = posix_fstat((int32_t)ARG(0), &information);
	if (!result && mac_guest_write(out, &information, sizeof(information))) result = -1;
	FILERET(result);
}

static void b_posix_set_file_times(void)
{
	char path[POSIX_STRING_CAP];
	if (guest_string(ARG(0), path)) { FILERET(-1); return; }
	FILERET(posix_set_file_times(path, ARG(1), ARG(2), ARG(3), ARG(4)));
}

static void b_posix_seek(void)
{
	posix_ulong low, high;
	uint32_t low_va = ARG(4), high_va = ARG(5);
	int result;
	if (!guest_span(low_va, 4) || !guest_span(high_va, 4)) { FILERET(-1); return; }
	result = posix_seek((int32_t)ARG(0), (int32_t)ARG(1), (int32_t)ARG(2),
		(int32_t)ARG(3), &low, &high);
	if (!result && (mac_guest_write(low_va, &low, 4) || mac_guest_write(high_va, &high, 4))) result = -1;
	FILERET(result);
}

static void b_posix_truncate(void)
{
	FILERET(posix_truncate((int32_t)ARG(0), ARG(1), ARG(2)));
}

static void b_posix_disk_space(void)
{
	char path[POSIX_STRING_CAP];
	posix_ulong free_low, free_high, total_low, total_high;
	uint32_t va[4] = { ARG(1), ARG(2), ARG(3), ARG(4) };
	int result;
	if (guest_string(ARG(0), path)) { FILERET(-1); return; }
	for (unsigned i = 0; i < 4; ++i) if (!guest_span(va[i], 4)) { FILERET(-1); return; }
	result = posix_disk_space(path, &free_low, &free_high, &total_low, &total_high);
	if (!result && (mac_guest_write(va[0], &free_low, 4) || mac_guest_write(va[1], &free_high, 4) ||
		mac_guest_write(va[2], &total_low, 4) || mac_guest_write(va[3], &total_high, 4))) result = -1;
	FILERET(result);
}

static void b_posix_set_read_only(void)
{
	char path[POSIX_STRING_CAP];
	if (guest_string(ARG(0), path)) { FILERET(-1); return; }
	FILERET(posix_set_read_only(path, (int32_t)ARG(1)));
}

static void b_posix_make_directory(void)
{
	char path[POSIX_STRING_CAP];
	if (guest_string(ARG(0), path)) { FILERET(-1); return; }
	FILERET(posix_make_directory(path));
}

static void b_posix_directory_open(void)
{
	char path[POSIX_STRING_CAP];
	void *handle;
	if (guest_string(ARG(0), path)) { (void)mac_guest_errno_set_native(errno); RET(0); return; }
	handle = posix_directory_open(path);
	if (!handle) (void)mac_guest_errno_set_native(errno);
	RET((uintptr_t)handle);
}

static void b_posix_directory_next(void)
{
	uint32_t name_va = ARG(1), capacity = ARG(2);
	void *name = guest_span(name_va, capacity);
	if (!name || !capacity) { RET(0); return; }
	RET(posix_directory_next((void *)(uintptr_t)ARG(0), name, capacity));
}

static void b_posix_directory_close(void)
{
	posix_directory_close((void *)(uintptr_t)ARG(0));
	VOIDRET();
}

static void b_posix_find_entry(void)
{
	char directory[POSIX_STRING_CAP], name[POSIX_STRING_CAP];
	uint32_t out_va = ARG(2), capacity = ARG(3);
	void *out = guest_span(out_va, capacity);
	if (guest_string(ARG(0), directory) || guest_string(ARG(1), name) || !out || !capacity) { RET(0); return; }
	RET(posix_find_entry_case_insensitive(directory, name, out, capacity));
}

static void b_posix_local_ipv4(void) { RET(posix_local_ipv4_address()); }

static void b_posix_random_bytes(void)
{
	uint32_t va = ARG(0), size = ARG(1);
	void *buffer = guest_span(va, size);
	if (!buffer && size) { mac_host_abort("posix_random_bytes received invalid guest span"); }
	posix_random_bytes(buffer, size);
	VOIDRET();
}

static void b_posix_resolve_ipv4(void)
{
	char host[POSIX_STRING_CAP];
	if (guest_string(ARG(0), host)) { RET(0); return; }
	RET(posix_resolve_ipv4(host));
}

static void b_posix_command_line(void)
{
	uint32_t buffer_va = ARG(1), capacity = ARG(2);
	void *buffer = guest_span(buffer_va, capacity);
	if (!buffer && capacity) { RET(0); return; }
	RET(posix_command_line_argument((int32_t)ARG(0), buffer, capacity));
}

static void b_posix_process_id(void) { RET(posix_process_id()); }

static void b_posix_register_url(void)
{
	char scheme[POSIX_STRING_CAP], description[POSIX_STRING_CAP];
	if (guest_string(ARG(0), scheme) || guest_string(ARG(1), description)) { RET(0); return; }
	RET(posix_register_url_scheme(scheme, description));
}

static void b_posix_discord_connect(void) { RET(posix_discord_connect()); }
static void b_posix_discord_close(void) { posix_discord_close((int32_t)ARG(0)); VOIDRET(); }

static void b_posix_discord_read(void)
{
	uint32_t va = ARG(1), size = ARG(2);
	void *buffer = guest_span(va, size);
	if (!buffer && size) { RET(-1); return; }
	RET(posix_discord_read((int32_t)ARG(0), buffer, (int32_t)size));
}

static void b_posix_discord_write(void)
{
	uint32_t va = ARG(1), size = ARG(2);
	const void *buffer = guest_span(va, size);
	if (!buffer && size) { RET(-1); return; }
	RET(posix_discord_write((int32_t)ARG(0), buffer, (int32_t)size));
}

static void b_posix_socket(void) { RET(posix_socket((int32_t)ARG(0), (int32_t)ARG(1), (int32_t)ARG(2))); }
static void b_posix_socket_close(void) { RET(posix_socket_close((int32_t)ARG(0))); }
static void socket_failure_trace(const char *operation, int descriptor,
	const struct sockaddr_in *target, int result)
{
	int saved_errno = errno, error = posix_socket_last_error();
	const char *enabled = getenv("HALO_NETWORK_TRACE");
	struct sockaddr_in local = {0};
	socklen_t length = sizeof(local);
	char endpoint[INET_ADDRSTRLEN] = "unknown";
	if (result >= 0 || !enabled || strcmp(enabled, "1") != 0)
		return;
	if (!target && getsockname(descriptor, (struct sockaddr *)&local, &length) == 0)
		target = &local;
	if (target && target->sin_family == AF_INET)
		(void)inet_ntop(AF_INET, &target->sin_addr, endpoint, sizeof(endpoint));
	fprintf(stderr, "[network-socket-failure] op=%s fd=%d native_ipv4=%s port=%u errno=%d winsock=%d\n",
		operation, descriptor, endpoint, target ? ntohs(target->sin_port) : 0,
		saved_errno, error);
	/* Logging must not change native errno or the backend's Winsock error. */
	errno = saved_errno;
}
static void datagram_trace(const char *operation, int descriptor,
	const struct sockaddr_in *peer, const void *buffer, uint32_t requested, int result)
{
	static _Thread_local unsigned long sent_count, received_count;
	unsigned long *count = strcmp(operation, "sendto") == 0 ? &sent_count : &received_count;
	const char *enabled = getenv("HALO_NETWORK_TRACE");
	if (!enabled || strcmp(enabled, "1") != 0) return;
	++*count;
	if (result < 0) { socket_failure_trace(operation, descriptor, peer, result); return; }
	if (*count > 16 && *count % 300 != 0) return;
	int saved_errno = errno;
	char ip[INET_ADDRSTRLEN] = "unknown", local_ip[INET_ADDRSTRLEN] = "unknown", header[25] = {0};
	struct sockaddr_in local={0}; socklen_t local_length=sizeof(local);
	if (getsockname(descriptor,(struct sockaddr *)&local,&local_length)==0)
		(void)inet_ntop(AF_INET,&local.sin_addr,local_ip,sizeof(local_ip));
	const unsigned char *bytes = buffer;
	unsigned length = (unsigned)result < 12 ? (unsigned)result : 12;
	if (peer) (void)inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip));
	for (unsigned i=0;i<length;i++) snprintf(header+i*2,3,"%02x",bytes[i]);
	fprintf(stderr,"[network-datagram] op=%s count=%lu fd=%d local_ipv4=%s local_port=%u native_ipv4=%s port=%u requested=%u result=%d header=%s\n",
		operation,*count,descriptor,local_ip,ntohs(local.sin_port),ip,peer?ntohs(peer->sin_port):0,requested,result,header);
	errno = saved_errno;
}
static void b_posix_socket_listen(void)
{
	int descriptor = (int32_t)ARG(0);
	int result = posix_socket_listen(descriptor, (int32_t)ARG(1));
	socket_failure_trace("listen", descriptor, NULL, result);
	RET(result);
}
static void b_posix_socket_connect(void)
{
	struct sockaddr_in address;
	if (ipv4_input(ARG(1), ARG(2), &address)) { RET(-1); return; }
	int descriptor = (int32_t)ARG(0);
	int result = posix_socket_connect(descriptor, &address, sizeof(address));
	socket_failure_trace("connect", descriptor, &address, result);
	RET(result);
}
static void b_posix_socket_bind(void)
{
	struct sockaddr_in address;
	if (ipv4_input(ARG(1), ARG(2), &address)) { RET(-1); return; }
	int descriptor = (int32_t)ARG(0);
	int result = posix_socket_bind(descriptor, &address, sizeof(address));
	socket_failure_trace("bind", descriptor, &address, result);
	RET(result);
}
static void b_posix_socket_accept(void)
{
	uint32_t va = ARG(1), len_va = ARG(2), capacity = 0;
	struct sockaddr_in address;
	int length = sizeof(address), result;
	if (va && (!len_va || mac_guest_read(len_va, &capacity, 4) ||
		!guest_span(va, capacity))) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
	result = posix_socket_accept((int32_t)ARG(0), va ? &address : NULL, va ? &length : NULL);
	if (result >= 0 && va && (ipv4_output(va, capacity, &address) || mac_guest_write(len_va, &length, 4)))
	{ posix_socket_close(result); result = -1; }
	RET(result);
}
static void b_posix_socket_send(void)
{
	uint32_t va = ARG(1), n = ARG(2); const void *p = guest_span(va, n);
	if (!p && n) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
	RET(posix_socket_send((int32_t)ARG(0), p, (int32_t)n, (int32_t)ARG(3)));
}
static void b_posix_socket_sendto(void)
{
	uint32_t n = ARG(2); const void *buffer = guest_span(ARG(1), n);
	struct sockaddr_in address;
	if (!buffer && n) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
	if (ipv4_input(ARG(4), ARG(5), &address)) { RET(-1); return; }
	int descriptor=(int32_t)ARG(0);
	int result=posix_socket_sendto(descriptor,buffer,(int32_t)n,(int32_t)ARG(3),&address,sizeof(address));
	datagram_trace("sendto",descriptor,&address,buffer,n,result);
	RET(result);
}
static void b_posix_socket_recv(void)
{
	uint32_t va = ARG(1), n = ARG(2); void *p = guest_span(va, n);
	if (!p && n) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
	RET(posix_socket_recv((int32_t)ARG(0), p, (int32_t)n, (int32_t)ARG(3)));
}
static void b_posix_socket_recvfrom(void)
{
	uint32_t n = ARG(2), va = ARG(4), len_va = ARG(5), capacity = 0;
	void *buffer = guest_span(ARG(1), n);
	struct sockaddr_in address;
	int length = sizeof(address), result;
	if ((!buffer && n) || (va && (!len_va || mac_guest_read(len_va, &capacity, 4) || !guest_span(va, capacity))))
	{ RET(mac_native_posix_bridge_error(EFAULT)); return; }
	result = posix_socket_recvfrom((int32_t)ARG(0), buffer, (int32_t)n, (int32_t)ARG(3), va ? &address : NULL, va ? &length : NULL);
	if (result >= 0) datagram_trace("recvfrom",(int32_t)ARG(0),va?&address:NULL,buffer,n,result);
	if (result >= 0 && va && (ipv4_output(va, capacity, &address) || mac_guest_write(len_va, &length, 4))) result = -1;
	RET(result);
}
static void b_posix_socket_shutdown(void) { RET(posix_socket_shutdown((int32_t)ARG(0), (int32_t)ARG(1))); }
static void b_posix_socket_nonblocking(void) { RET(posix_socket_set_nonblocking((int32_t)ARG(0), (int32_t)ARG(1))); }
static void b_posix_socket_nodelay(void) { RET(posix_socket_set_nodelay((int32_t)ARG(0))); }
static void b_posix_socket_last_error(void) { RET(posix_socket_last_error()); }
static void b_posix_socket_bytes_available(void)
{
	uint32_t out_va = ARG(1); posix_ulong value; int result;
	if (!guest_span(out_va, 4)) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
	result = posix_socket_bytes_available((int32_t)ARG(0), &value);
	if (!result && mac_guest_write(out_va, &value, 4)) result = -1;
	RET(result);
}
static void b_posix_socket_setsockopt(void)
{
	uint32_t va = ARG(3), n = ARG(4); const void *p = guest_span(va, n);
	if (!p && n) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
	RET(posix_socket_setsockopt((int32_t)ARG(0), (int32_t)ARG(1), (int32_t)ARG(2), p, (int32_t)n));
}
static void b_posix_socket_getsockopt(void)
{
	uint32_t va = ARG(3), len_va = ARG(4), n = 0; int *length = guest_span(len_va, 4), result; void *p;
	if (!length || mac_guest_read(len_va, &n, 4)) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
	p = guest_span(va, n); if (!p && n) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
	result = posix_socket_getsockopt((int32_t)ARG(0), (int32_t)ARG(1), (int32_t)ARG(2), p, (int *)&n);
	if (mac_guest_write(len_va, &n, 4)) result = -1;
	RET(result);
}
static void b_posix_socket_getsockname(void)
{
	uint32_t va = ARG(1), len_va = ARG(2), capacity;
	struct sockaddr_in address;
	int length = sizeof(address), result;
	if (mac_guest_read(len_va, &capacity, 4) || !guest_span(va, capacity)) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
	result = posix_socket_getsockname((int32_t)ARG(0), &address, &length);
	if (!result && (ipv4_output(va, capacity, &address) || mac_guest_write(len_va, &length, 4))) result = -1;
	RET(result);
}
static void b_posix_socket_getpeername(void)
{
	uint32_t va = ARG(1), len_va = ARG(2), capacity;
	struct sockaddr_in address;
	int length = sizeof(address), result;
	if (mac_guest_read(len_va, &capacity, 4) || !guest_span(va, capacity)) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
	result = posix_socket_getpeername((int32_t)ARG(0), &address, &length);
	if (!result && (ipv4_output(va, capacity, &address) || mac_guest_write(len_va, &length, 4))) result = -1;
	RET(result);
}

/* Select lists are guest arrays of signed 32-bit descriptors. Convert them to
 * bounded host arrays; Darwin fd_set is never exposed to guest storage. */
static void b_posix_socket_select(void)
{
	int32_t counts[3]; uint32_t list_va[3] = {ARG(0),ARG(2),ARG(4)};
	int32_t count_va[3] = {(int32_t)ARG(1),(int32_t)ARG(3),(int32_t)ARG(5)};
	int *lists[3] = {NULL,NULL,NULL}; int result;
	for (int i=0;i<3;i++) {
		uint32_t cap = 0;
		if (count_va[i] && mac_guest_read((uint32_t)count_va[i], &cap, 4)) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
		counts[i]=(int32_t)cap;
		if (counts[i] < 0 || counts[i] > 1024) { RET(mac_native_posix_bridge_error(EFAULT)); return; }
		if (counts[i]) {
			lists[i]=calloc((size_t)counts[i], sizeof(int));
			if (!lists[i] || mac_guest_read(list_va[i], lists[i], (size_t)counts[i]*4)) { for (int j=0;j<3;j++) free(lists[j]); RET(mac_native_posix_bridge_error(EFAULT)); return; }
		}
	}
	result=posix_socket_select(lists[0],(int*)&counts[0],lists[1],(int*)&counts[1],lists[2],(int*)&counts[2],
		(int32_t)ARG(6),(int32_t)ARG(7),(int32_t)ARG(8));
	for (int i=0;i<3;i++) {
		if (counts[i] > 0 && mac_guest_write(list_va[i],lists[i],(size_t)counts[i]*4)) result=-1;
		if (count_va[i] && mac_guest_write((uint32_t)count_va[i],&counts[i],4)) result=-1;
		free(lists[i]);
	}
	RET(result);
}

static void b_posix_upnp_forward_udp(void)
{
 uint32_t address_va = ARG(1), port_va = ARG(2), error_va = ARG(3), capacity = ARG(4);
 posix_ulong address = 0; unsigned short port = 0; int result;
 char *error;
 if (!guest_span(address_va, 4) || !guest_span(port_va, 2) || !capacity ||
     capacity > POSIX_STRING_CAP || !guest_span(error_va, capacity)) { RET(0); return; }
 error = calloc(capacity, 1);
 if (!error) { RET(0); return; }
 result = posix_upnp_forward_udp((unsigned short)ARG(0), &address, &port, error, (int)capacity);
 error[capacity - 1] = 0;
 if (mac_guest_write(error_va, error, capacity)) result = 0;
 if (result && (mac_guest_write(address_va, &address, 4) || mac_guest_write(port_va, &port, 2))) result = 0;
 free(error); RET(result);
}
static void b_posix_upnp_stop_forwarding_udp(void)
{
 posix_upnp_stop_forwarding_udp((unsigned short)ARG(0));
 mac_guest_host_import_return_void();
}

#define BIND(n,f) { #n, b_##f }
static const struct { const char *name; mac_guest_import_bridge bridge; } entries[] = {
	BIND(posix_stat,posix_stat), BIND(posix_fstat,posix_fstat), BIND(posix_set_file_times,posix_set_file_times),
	BIND(posix_seek,posix_seek), BIND(posix_truncate,posix_truncate), BIND(posix_disk_space,posix_disk_space),
	BIND(posix_set_read_only,posix_set_read_only), BIND(posix_make_directory,posix_make_directory),
	BIND(posix_directory_open,posix_directory_open), BIND(posix_directory_next,posix_directory_next),
	BIND(posix_directory_close,posix_directory_close), BIND(posix_find_entry_case_insensitive,posix_find_entry),
	BIND(posix_local_ipv4_address,posix_local_ipv4), BIND(posix_random_bytes,posix_random_bytes),
	BIND(posix_resolve_ipv4,posix_resolve_ipv4), BIND(posix_command_line_argument,posix_command_line),
	BIND(posix_process_id,posix_process_id), BIND(posix_register_url_scheme,posix_register_url),
	BIND(posix_discord_connect,posix_discord_connect), BIND(posix_discord_close,posix_discord_close),
	BIND(posix_discord_read,posix_discord_read), BIND(posix_discord_write,posix_discord_write),
	BIND(posix_socket,posix_socket), BIND(posix_socket_close,posix_socket_close), BIND(posix_socket_listen,posix_socket_listen),
	BIND(posix_socket_connect,posix_socket_connect), BIND(posix_socket_bind,posix_socket_bind),
	BIND(posix_socket_accept,posix_socket_accept), BIND(posix_socket_send,posix_socket_send),
	BIND(posix_socket_sendto,posix_socket_sendto), BIND(posix_socket_recv,posix_socket_recv),
	BIND(posix_socket_recvfrom,posix_socket_recvfrom), BIND(posix_socket_shutdown,posix_socket_shutdown),
	BIND(posix_socket_set_nonblocking,posix_socket_nonblocking), BIND(posix_socket_set_nodelay,posix_socket_nodelay),
	BIND(posix_socket_last_error,posix_socket_last_error), BIND(posix_socket_bytes_available,posix_socket_bytes_available),
	BIND(posix_socket_setsockopt,posix_socket_setsockopt), BIND(posix_socket_getsockopt,posix_socket_getsockopt),
	BIND(posix_socket_getsockname,posix_socket_getsockname), BIND(posix_socket_getpeername,posix_socket_getpeername),
	BIND(posix_socket_select,posix_socket_select),
	BIND(posix_upnp_forward_udp,posix_upnp_forward_udp), BIND(posix_upnp_stop_forwarding_udp,posix_upnp_stop_forwarding_udp)
};

mac_guest_import_bridge mac_macos_posix_host_import_bridge(const char *name)
{
	if (!name) return NULL;
	for (size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++) if (!strcmp(name,entries[i].name)) return entries[i].bridge;
	return NULL;
}
