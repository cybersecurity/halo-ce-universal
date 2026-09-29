#include "../guest_address.h"
#include "../host_services.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
	unsigned char guest_memory[512] = { 0 };
	char host_path[] = "/tmp/halo-host-services-XXXXXX";
	const char payload[] = "guest-buffer-io";
	uint32_t guest_timespec = 0x10000100u;
	uint32_t guest_timeval = 0x10000108u;
	int32_t seconds, subsecond;
	int fd, guest_fd;

	fd = mkstemp(host_path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(mac_guest_address_register(0x10000000u, guest_memory,
		sizeof(guest_memory)) == 0);
	strcpy((char *)guest_memory, host_path);
	memcpy(guest_memory + 0x40, payload, sizeof(payload));

	guest_fd = (int)mac_host_openat(AT_FDCWD, 0x10000000u, 2u | 0x00001000u, 0600u);
	assert(guest_fd >= 0);
	assert(mac_host_pwrite(guest_fd, 0x10000040u, sizeof(payload), 0) == (int64_t)sizeof(payload));
	assert(mac_host_pread(guest_fd, 0x10000080u, sizeof(payload), 0) == (int64_t)sizeof(payload));
	assert(memcmp(guest_memory + 0x80, payload, sizeof(payload)) == 0);
	assert(mac_host_read(guest_fd, 0x10000FFFu, 8) == -EFAULT);
	assert(mac_host_openat(AT_FDCWD, 0x10000000u, 2u | 0x00040000u, 0600u) == -EINVAL);
	assert(mac_host_close(guest_fd) == 0);

	assert(mac_host_clock_gettime(1, guest_timespec) == 0);
	memcpy(&seconds, guest_memory + 0x100, sizeof(seconds));
	memcpy(&subsecond, guest_memory + 0x104, sizeof(subsecond));
	assert(seconds > 0 && subsecond >= 0 && subsecond < 1000000000);
	assert(mac_host_gettimeofday(guest_timeval) == 0);
	memcpy(&seconds, guest_memory + 0x108, sizeof(seconds));
	assert(seconds > 0);
	assert(mac_host_clock_gettime(99, guest_timespec) == -EINVAL);
	/* The time64 record fully replaces stale seconds/nanoseconds high words. */
	memset(guest_memory + 0x140, 0xCA, 16);
	assert(mac_host_clock_gettime64(1, 0x10000140u) == 0);
	int64_t seconds64, nanoseconds64;
	memcpy(&seconds64, guest_memory + 0x140, 8);
	memcpy(&nanoseconds64, guest_memory + 0x148, 8);
	assert(seconds64 > 0 && nanoseconds64 >= 0 && nanoseconds64 < 1000000000);
	assert(mac_host_clock_gettime64(99, 0x10000140u) == -EINVAL);
	assert(mac_host_clock_gettime64(1, 0x100001F8u) == -EFAULT);
	assert(mac_host_clock_gettime64(1, 0) == -EFAULT);
	assert(mac_host_thread_id() != 0);
	mac_host_set_guest_tp(0x12345678u);
	assert(mac_host_get_guest_tp() == 0x12345678u);
	assert(unlink(host_path) == 0);
	assert(mac_guest_address_unregister(0x10000000u) == 0);
	return 0;
}
