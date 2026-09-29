#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "guest_image.h"
#include "guest_address.h"
#include "guest_call.h"
#include "recomp_state.h"
#include "host_syscall.h"
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

static void unexpected(uint32_t va)
{
	fprintf(stderr, "unexpected guest call %08x\n", va);
	abort();
}
void recomp_unsupported_instruction(uint32_t va) { unexpected(va); }
void recomp_icall_fail_log(uint32_t va) { unexpected(va); }
void recomp_icall_not_code_log(uint32_t va) { unexpected(va); }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return NULL; }
void host_get_tp(void) { g_eax = 0x60002000; g_esp += 4; }
void host_syscall(void)
{
	int64_t result = mac_host_arm64_32_syscall_from_stack(g_esp);
	g_eax = (uint32_t)result;
	g_edx = (uint32_t)((uint64_t)result >> 32);
	g_esp += 4;
}

static void prepare_call(void)
{
	mac_recomp_state_initialize_thread(0x6000f000, 0x60002000);
	MEM32(g_esp) = 0;
}

static int64_t host_ns(void)
{
	struct timespec time; assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
	return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}
static void caught_signal(int number) { (void)number; }
static void *interrupt(void *opaque)
{
	struct timespec delay = {0, 2000000}; nanosleep(&delay, NULL);
	assert(pthread_kill(*(pthread_t *)opaque, SIGUSR1) == 0); return NULL;
}
struct musl_timespec { int64_t seconds; int32_t nanos, padding; };
_Static_assert(sizeof(struct musl_timespec) == 16, "musl time64 layout");
int main(int argc, char **argv)
{
	assert(argc == 2);
	FILE *file = fopen(argv[1], "rb"); assert(file);
	assert(fseek(file, 0, SEEK_END) == 0); long size = ftell(file); rewind(file);
	void *bytes = malloc(size); assert(fread(bytes, 1, size, file) == size); fclose(file);
	struct mac_guest_image image; char error[256];
	assert(mac_guest_image_load(bytes, size, &image, error, sizeof(error)) == 0);
	assert(mac_guest_address_commit(0x60000000, 0x10000, PROT_READ | PROT_WRITE) == 0);
	assert(mac_recomp_state_set_image(image.memory_offset, image.code_lo_va, image.code_hi_va) == 0);
	int64_t before = host_ns(), target = before + 20000000;
	struct musl_timespec request = {target / 1000000000, target % 1000000000, (int32_t)0xCACACACA};
	assert(mac_guest_write(0x60003000, &request, sizeof(request)) == 0);
	memset((void *)XBOX_PTR(0x60003020), 0xCA, 16);
	prepare_call(); MEM32(g_esp + 4) = 1; MEM32(g_esp + 8) = 1;
	MEM32(g_esp + 12) = 0x60003000; MEM32(g_esp + 16) = 0x60003020;
	HALO_CLOCK_NANOSLEEP();
	assert(g_eax == 0 && host_ns() >= target);
	assert(MEM32(0x60003020) == UINT32_C(0xCACACACA));
	request.seconds = 0; request.nanos = 2000000;
	assert(mac_guest_write(0x60003000, &request, sizeof(request)) == 0);
	before = host_ns(); prepare_call(); MEM32(g_esp + 4) = 0x60003000; MEM32(g_esp + 8) = 0;
	HALO_NANOSLEEP_TIME64(); assert(g_eax == 0 && host_ns() - before >= 2000000);
	struct sigaction action = {0}, old; action.sa_handler = caught_signal; sigemptyset(&action.sa_mask);
	assert(sigaction(SIGUSR1, &action, &old) == 0);
	request.nanos = 20000000; assert(mac_guest_write(0x60003000, &request, sizeof(request)) == 0);
	pthread_t self = pthread_self(), sender; assert(pthread_create(&sender, NULL, interrupt, &self) == 0);
	prepare_call(); MEM32(g_esp + 4) = 1; MEM32(g_esp + 8) = 0;
	MEM32(g_esp + 12) = 0x60003000; MEM32(g_esp + 16) = 0x60003020;
	HALO_CLOCK_NANOSLEEP(); assert(g_eax == 4);
	assert(pthread_join(sender, NULL) == 0); assert(sigaction(SIGUSR1, &old, NULL) == 0);
	struct musl_timespec remaining; assert(mac_guest_read(0x60003020, &remaining, sizeof(remaining)) == 0);
	assert(remaining.seconds == 0 && remaining.nanos > 0 && remaining.nanos < 20000000);
	puts("actual translated absolute20ms, relative2ms and EINTR remaining pass");
	mac_guest_address_reset(); free(bytes); return 0;
}
