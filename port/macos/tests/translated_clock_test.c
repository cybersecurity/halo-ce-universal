#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "guest_image.h"
#include "guest_address.h"
#include "recomp_state.h"
#include "host_syscall.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

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

static int64_t monotonic_microseconds(void)
{
	struct timespec value;
	assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
	return (int64_t)value.tv_sec * 1000000 + value.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	FILE *file = fopen(argv[1], "rb");
	assert(file);
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);
	void *bytes = malloc((size_t)size);
	assert(fread(bytes, 1, (size_t)size, file) == (size_t)size);
	fclose(file);
	struct mac_guest_image image;
	char error[256];
	assert(mac_guest_image_load(bytes, (size_t)size, &image, error, sizeof(error)) == 0);
	assert(mac_guest_address_commit(0x60000000, 0x10000, PROT_READ | PROT_WRITE) == 0);
	assert(mac_recomp_state_set_image(image.memory_offset, image.code_lo_va, image.code_hi_va) == 0);
	int64_t previous = 0;
	for (unsigned index = 0; index < 8; ++index)
	{
		/* Stale stack bytes exposed the original short time32 write. */
		memset((void *)XBOX_PTR(0x60000000), 0xCA, 0x10000);
		mac_recomp_state_initialize_thread(0x6000f000, 0x60002000);
		MEM32(g_esp) = 0;
		MEM32(g_esp + 4) = 0x60000100;
		int64_t before = monotonic_microseconds();
		HALO_QPC_FUNCTION();
		int64_t after = monotonic_microseconds();
		int64_t counter = SMEM64(0x60000100);
		assert(g_eax == 1 && g_esp == 0x6000f008);
		assert(counter >= before && counter <= after && counter >= previous);
		previous = counter;
	}
	mac_recomp_state_initialize_thread(0x6000f000, 0x60002000);
	MEM32(g_esp) = 0;
	MEM32(g_esp + 4) = 0x60000100;
	HALO_QPF_FUNCTION();
	assert(g_eax == 1 && SMEM64(0x60000100) == 1000000);
	printf("actual translated QueryPerformanceCounter: 8 plausible monotonic samples; frequency1000000\n");
	mac_guest_address_reset();
	free(bytes);
	return 0;
}
