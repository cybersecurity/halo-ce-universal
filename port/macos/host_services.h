/* Native Darwin side of the source-built guest's non-graphics host boundary.
 * Guest addresses are 32-bit Xbox VAs; buffer parameters below are always
 * guest VAs, never truncated host pointers. */

#ifndef HALO_MACOS_HOST_SERVICES_H
#define HALO_MACOS_HOST_SERVICES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mac_host_log(int priority, const char *message);
void mac_host_logf(int priority, const char *format, ...);
void mac_host_abort(const char *message) __attribute__((noreturn));
void mac_host_exit(int status) __attribute__((noreturn));
int mac_host_errno(void);
int mac_host_linux_errno(int darwin_errno);
uint64_t mac_host_thread_id(void);
uint32_t mac_host_get_guest_tp(void);
void mac_host_set_guest_tp(uint32_t value);

/* Guest Linux clock IDs and 32-bit time layouts. Results use Linux syscall
 * convention: 0 on success, otherwise a negative errno value. */
int64_t mac_host_clock_gettime(int guest_clock_id, uint32_t guest_timespec_va);
/* Hybrid guest syscall 403 uses the Linux 16-byte kernel time64 record;
 * legacy syscall 113 retains its eight-byte time32 layout. */
int64_t mac_host_clock_gettime64(int guest_clock_id, uint32_t guest_timespec_va);
int64_t mac_host_clock_getres(int guest_clock_id, uint32_t guest_timespec_va);
int64_t mac_host_gettimeofday(uint32_t guest_timeval_va);

/* File calls accept Linux open flag values and checked guest pointers. */
int64_t mac_host_openat(int directory_fd, uint32_t guest_path_va,
	uint32_t linux_flags, uint32_t mode);
int64_t mac_host_read(int fd, uint32_t guest_buffer_va, uint32_t size);
int64_t mac_host_write(int fd, uint32_t guest_buffer_va, uint32_t size);
int64_t mac_host_pread(int fd, uint32_t guest_buffer_va, uint32_t size,
	int64_t offset);
int64_t mac_host_pwrite(int fd, uint32_t guest_buffer_va, uint32_t size,
	int64_t offset);
int64_t mac_host_lseek(int fd, int64_t offset, int whence);
int64_t mac_host_close(int fd);

#ifdef __cplusplus
}
#endif

#endif
