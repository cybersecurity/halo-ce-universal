#include "guest_address.h"
#include "guest_call.h"
#include "host_syscall.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TEST_BASE UINT32_C(0x61000000)
#define TEST_SIZE (64u * 1024u)

struct guest_timespec32
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec32
{
	uint32_t base;
	uint32_t length;
};

static void interrupted_sleep(int signal_number) { (void)signal_number; }
static void *interrupt_sleeper(void *opaque)
{
	pthread_t *target = opaque;
	struct timespec delay = {0, 2000000};
	nanosleep(&delay, NULL);
	assert(pthread_kill(*target, SIGUSR1) == 0);
	return NULL;
}

static uint32_t futex_word_va = TEST_BASE + 0x1000;
static volatile int32_t *futex_word;
static int64_t waiter_result;

struct vm_test_state
{
	uint64_t observed_offset;
	uint32_t mapped_address;
};

static int64_t test_vm_map(void *context, uint32_t address, uint32_t size,
	int protection, uint32_t flags, int fd, uint64_t offset,
	uint32_t *mapped_guest_va_out)
{
	struct vm_test_state *state = context;
	(void)address;
	(void)size;
	(void)protection;
	(void)flags;
	(void)fd;
	state->observed_offset = offset;
	*mapped_guest_va_out = state->mapped_address;
	return 0;
}

static int64_t test_vm_unmap(void *context, uint32_t address, uint32_t size)
{
	(void)context;
	(void)address;
	(void)size;
	return 0;
}

static int64_t test_vm_protect(void *context, uint32_t address, uint32_t size,
	int protection)
{
	(void)context;
	(void)address;
	(void)size;
	(void)protection;
	return 0;
}

static void *futex_waiter(void *unused)
{
	uint64_t arguments[6] = { futex_word_va, 0, 0, 0, 0, 0 };
	(void)unused;
	waiter_result = mac_host_arm64_32_syscall(98, arguments);
	return NULL;
}

int main(void)
{
	void *memory = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	char *guest_memory;
	uint64_t arguments[6] = { 0 };
	uint64_t syscall_slots[7] = { 0 };
	uint8_t call_stack[4 + 7 * 8] = { 0 };
	uint32_t guest_esp = TEST_BASE + 0x200;
	int fd;
	char template[] = "/tmp/halo-host-syscall-XXXXXX";
	char payload[] = "native-guest-io";
	char readback[sizeof(payload)] = { 0 };
	struct guest_iovec32 guest_iov[2];
	struct guest_timespec32 *timespec;
	pthread_t thread;
	struct vm_test_state vm_state = { 0, TEST_BASE + 0x2000 };
	struct mac_guest_vm_ops vm_ops = {
		&vm_state, test_vm_map, test_vm_unmap, test_vm_protect
	};

	assert(memory != MAP_FAILED);
	assert(mac_guest_address_register(TEST_BASE, memory, TEST_SIZE) == 0);
	guest_memory = memory;
	futex_word = (int32_t *)(guest_memory + 0x1000);

	/* The checked cdecl reader consumes the i386 long-long syscall frame. */
	{
		uint32_t return_va = UINT32_C(0x12345678);
		memcpy(call_stack, &return_va, sizeof(return_va));
	}
	syscall_slots[0] = 172; /* arm64_32 getpid */
	memcpy(call_stack + 4, syscall_slots, sizeof(syscall_slots));
	memcpy(guest_memory + 0x200, call_stack, sizeof(call_stack));
	assert(mac_host_arm64_32_syscall_from_stack(guest_esp) == (int64_t)getpid());
	assert(mac_guest_stack_arg64(guest_esp, 0, &syscall_slots[0]) == 0);
	assert(syscall_slots[0] == 172);

	/* arm64_32 syscall IDs retain checked ILP32 guest path/data spans. */
	fd = mkstemp(template);
	assert(fd >= 0);
	close(fd);
	unlink(template);
	memcpy(guest_memory + 0x400, template, strlen(template) + 1);
	arguments[0] = (uint32_t)-100; /* AT_FDCWD */
	arguments[1] = TEST_BASE + 0x400;
	arguments[2] = 2u | 0100u; /* O_RDWR | O_CREAT */
	arguments[3] = 0600;
	fd = (int)mac_host_arm64_32_syscall(56, arguments);
	assert(fd >= 0);
	memcpy(guest_memory + 0x800, payload, sizeof(payload));
	guest_iov[0] = (struct guest_iovec32){ TEST_BASE + 0x800, 6 };
	guest_iov[1] = (struct guest_iovec32){ TEST_BASE + 0x806,
		(uint32_t)sizeof(payload) - 6 };
	memcpy(guest_memory + 0xB00, guest_iov, sizeof(guest_iov));
	arguments[0] = (uint32_t)fd;
	arguments[1] = TEST_BASE + 0xB00;
	arguments[2] = 2;
	assert(mac_host_arm64_32_syscall(66, arguments) == (int64_t)sizeof(payload));
	assert(mac_host_arm64_32_syscall(62, (uint64_t[6]){ (uint32_t)fd, 0, 0 }) == 0);
	arguments[1] = TEST_BASE + 0x900;
	arguments[2] = sizeof(payload);
	assert(mac_host_arm64_32_syscall(63, arguments) == (int64_t)sizeof(payload));
	/* readv validates every destination before consuming any file bytes. */
	assert(mac_host_arm64_32_syscall(62, (uint64_t[6]){ (uint32_t)fd, 0, 0 }) == 0);
	memset(guest_memory + 0xC00, 0xAA, sizeof(payload));
	guest_iov[0] = (struct guest_iovec32){ TEST_BASE + 0xC00, 6 };
	guest_iov[1] = (struct guest_iovec32){ TEST_BASE + TEST_SIZE, 4 };
	memcpy(guest_memory + 0xB00, guest_iov, sizeof(guest_iov));
	assert(mac_host_arm64_32_syscall(65, (uint64_t[6]){ (uint32_t)fd, TEST_BASE + 0xB00, 2 }) == -14);
	assert((unsigned char)guest_memory[0xC00] == 0xAA);
	assert(mac_host_arm64_32_syscall(62, (uint64_t[6]){ (uint32_t)fd, 0, 1 }) == 0);
	guest_iov[1] = (struct guest_iovec32){ TEST_BASE + 0xC06, (uint32_t)sizeof(payload) - 6 };
	memcpy(guest_memory + 0xB00, guest_iov, sizeof(guest_iov));
	assert(mac_host_arm64_32_syscall(65, (uint64_t[6]){ (uint32_t)fd, TEST_BASE + 0xB00, 2 }) == sizeof(payload));
	assert(memcmp(guest_memory + 0xC00, payload, sizeof(payload)) == 0);
	assert(mac_host_arm64_32_syscall(65, (uint64_t[6]){ (uint32_t)fd, 0, UINT32_MAX }) == -22);
	guest_iov[0].length = UINT32_C(0x80000000);
	memcpy(guest_memory + 0xB00, guest_iov, sizeof(guest_iov));
	assert(mac_host_arm64_32_syscall(65, (uint64_t[6]){ (uint32_t)fd, TEST_BASE + 0xB00, 1 }) == -22);
	guest_iov[0] = (struct guest_iovec32){ TEST_BASE + 0x800, 6 };
	memcpy(readback, guest_memory + 0x900, sizeof(readback));
	assert(memcmp(readback, payload, sizeof(payload)) == 0);
	/* Positioned I/O uses arm64_32 syscall IDs with 64-bit byte offsets,
	 * while buffer VAs and the pointed-to data remain guest32. */
	memset(guest_memory + 0xA80, 0, sizeof(payload));
	arguments[0] = (uint32_t)fd;
	arguments[1] = TEST_BASE + 0x800;
	arguments[2] = sizeof(payload);
	arguments[3] = UINT64_C(0x100000020);
	assert(mac_host_arm64_32_syscall(68, arguments) == (int64_t)sizeof(payload));
	arguments[1] = TEST_BASE + 0xA80;
	assert(mac_host_arm64_32_syscall(67, arguments) == (int64_t)sizeof(payload));
	assert(memcmp(guest_memory + 0xA80, payload, sizeof(payload)) == 0);
	arguments[1] = TEST_BASE + TEST_SIZE - 1;
	arguments[2] = 2;
	assert(mac_host_arm64_32_syscall(68, arguments) == -14); /* bad guest span */
	arguments[1] = TEST_BASE + 0x800;
	arguments[2] = sizeof(payload);
	arguments[3] = UINT64_MAX;
	assert(mac_host_arm64_32_syscall(67, arguments) == -22); /* invalid offset */
	guest_iov[1] = (struct guest_iovec32){ TEST_BASE + TEST_SIZE - 1, 8 };
	memcpy(guest_memory + 0xB00, guest_iov, sizeof(guest_iov));
	arguments[1] = TEST_BASE + 0xB00;
	assert(mac_host_arm64_32_syscall(66, arguments) == -14); /* invalid late span: no partial write */
	assert(mac_host_arm64_32_syscall(66, (uint64_t[6]){ UINT32_MAX,
		TEST_BASE + TEST_SIZE, 0 }) == -9); /* count zero still checks fd */
	assert(mac_host_arm64_32_syscall(57, (uint64_t[6]){ (uint32_t)fd }) == 0);
	unlink(template);

	/* A clock result is marshalled into the guest's 32-bit timespec layout. */
	arguments[0] = 1; /* CLOCK_MONOTONIC */
	arguments[1] = TEST_BASE + 0xA00;
	assert(mac_host_arm64_32_syscall(113, arguments) == 0);
	timespec = (struct guest_timespec32 *)(guest_memory + 0xA00);
	assert(timespec->seconds >= 0 && timespec->nanoseconds >= 0 &&
		timespec->nanoseconds < 1000000000);
	/* Hybrid syscall 403 selects the complete 16-byte Linux time64 record. */
	memset(guest_memory + 0xA20, 0xCA, 16);
	arguments[1] = TEST_BASE + 0xA20;
	assert(mac_host_arm64_32_syscall(403, arguments) == 0);
	int64_t seconds64, nanoseconds64;
	memcpy(&seconds64, guest_memory + 0xA20, 8);
	memcpy(&nanoseconds64, guest_memory + 0xA28, 8);
	assert(seconds64 >= 0 && nanoseconds64 >= 0 && nanoseconds64 < 1000000000);
	arguments[1] = TEST_BASE + TEST_SIZE - 8;
	assert(mac_host_arm64_32_syscall(403, arguments) == -14);
	/* Legacy115 reads8 bytes; time64 extension407 reads16. Both sleep
	 * until real absolute deadlines rather than reporting dummy success. */
	struct timespec before, after;
	clock_gettime(CLOCK_MONOTONIC, &before);
	int64_t deadline64[2] = {before.tv_sec, before.tv_nsec + 20000000};
	if (deadline64[1] >= 1000000000) { ++deadline64[0]; deadline64[1] -= 1000000000; }
	memcpy(guest_memory + 0xA40, deadline64, sizeof(deadline64));
	memset(guest_memory + 0xA60, 0xCA, 16);
	assert(mac_host_arm64_32_syscall(407, (uint64_t[6]){1, 1, TEST_BASE + 0xA40, TEST_BASE + 0xA60}) == 0);
	clock_gettime(CLOCK_MONOTONIC, &after);
	assert((after.tv_sec - before.tv_sec) * INT64_C(1000000000) + after.tv_nsec - before.tv_nsec >= 20000000);
	assert((unsigned char)guest_memory[0xA60] == 0xCA); /* absolute rem untouched */
	struct guest_timespec32 span32 = {0, 2000000};
	memcpy(guest_memory + 0xA40, &span32, sizeof(span32));
	assert(mac_host_arm64_32_syscall(115, (uint64_t[6]){1, 0, TEST_BASE + 0xA40, 0}) == 0);
	assert(mac_host_arm64_32_syscall(407, (uint64_t[6]){1, 0, TEST_BASE + TEST_SIZE - 8, 0}) == -14);
	assert(mac_host_arm64_32_syscall(407, (uint64_t[6]){3, 0, TEST_BASE + 0xA40, 0}) == -22);
	deadline64[0] = INT64_C(0x100000000); deadline64[1] = 1000000000;
	memcpy(guest_memory + 0xA40, deadline64, sizeof(deadline64));
	assert(mac_host_arm64_32_syscall(407, (uint64_t[6]){1, 0, TEST_BASE + 0xA40, 0}) == -22);
	deadline64[0] = -1; deadline64[1] = 0;
	memcpy(guest_memory + 0xA40, deadline64, sizeof(deadline64));
	assert(mac_host_arm64_32_syscall(407, (uint64_t[6]){1, 0, TEST_BASE + 0xA40, 0}) == -22);
	deadline64[0] = 0; deadline64[1] = -1;
	memcpy(guest_memory + 0xA40, deadline64, sizeof(deadline64));
	assert(mac_host_arm64_32_syscall(407, (uint64_t[6]){1, 0, TEST_BASE + 0xA40, 0}) == -22);
	assert(mac_host_arm64_32_syscall(115, (uint64_t[6]){1, 2, TEST_BASE + 0xA40, 0}) == -22);
	struct sigaction action = {0}, previous;
	action.sa_handler = interrupted_sleep;
	sigemptyset(&action.sa_mask);
	assert(sigaction(SIGUSR1, &action, &previous) == 0);
	pthread_t target = pthread_self(), interrupter;
	deadline64[0] = 0; deadline64[1] = 20000000;
	memcpy(guest_memory + 0xA40, deadline64, sizeof(deadline64));
	assert(pthread_create(&interrupter, NULL, interrupt_sleeper, &target) == 0);
	assert(mac_host_arm64_32_syscall(407, (uint64_t[6]){1, 0, TEST_BASE + 0xA40, TEST_BASE + 0xA60}) == -4);
	assert(pthread_join(interrupter, NULL) == 0);
	int64_t remaining64[2]; memcpy(remaining64, guest_memory + 0xA60, sizeof(remaining64));
	assert(remaining64[0] == 0 && remaining64[1] > 0 && remaining64[1] < 20000000);
	assert(sigaction(SIGUSR1, &previous, NULL) == 0);
	/* mmap is the arm64_32 number 222 and its offset is in bytes, not pages. */
	assert(mac_guest_vm_ops_install(&vm_ops) == 0);
	arguments[0] = 0;
	arguments[1] = 4096;
	arguments[2] = 3;
	arguments[3] = 2;
	arguments[4] = (uint32_t)-1;
	arguments[5] = 12345;
	assert(mac_host_arm64_32_syscall(222, arguments) == vm_state.mapped_address);
	assert(vm_state.observed_offset == 12345);

	/* Futex wake is address-specific and the wait uses the expected word. */
	*futex_word = 0;
	assert(pthread_create(&thread, NULL, futex_waiter, NULL) == 0);
	{
		struct timespec pause = { 0, 10000000 };
		nanosleep(&pause, NULL);
	}
	*futex_word = 1;
	arguments[0] = futex_word_va;
	arguments[1] = 1; /* FUTEX_WAKE */
	arguments[2] = 1;
	assert(mac_host_arm64_32_syscall(98, arguments) == 1);
	pthread_join(thread, NULL);
	assert(waiter_result == 0);

	assert(mac_host_arm64_32_syscall(9999, arguments) == -38); /* ENOSYS */
	arguments[0] = TEST_BASE + TEST_SIZE - 4;
	arguments[1] = 0xFFFFFFFFu;
	assert(mac_host_arm64_32_syscall(63, arguments) == -14); /* EFAULT */

	mac_guest_address_reset();
	puts("host_syscall_test: ok");
	return 0;
}
