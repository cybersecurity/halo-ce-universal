#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "guest_image.h"
#include "guest_address.h"
#include "recomp_state.h"
#include "host_syscall.h"
#include <unistd.h>
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
recomp_func_t recomp_lookup_manual(uint32_t va)
{
	if (va == HALO_STDIO_SEEK_VA) return HALO_STDIO_SEEK;
	if (va == HALO_STDIO_READ_VA) return HALO_STDIO_READ;
	return NULL;
}
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
	const char config[] = "fullscreen=false\n";
	char path[] = "/tmp/halo-stdio-test-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);
	assert(write(fd, config, sizeof(config) - 1) == sizeof(config) - 1);
	/* Construct the real i386 musl FILE layout around a native descriptor.
	 * The callbacks execute actual translated __stdio_seek/__stdio_read. */
	uint32_t fp = 0x60001000;
	memset((void *)XBOX_PTR(fp), 0, 128);
	MEM32(fp + 60) = fd;
	MEM32(fp + 40) = HALO_STDIO_SEEK_VA;
	MEM32(fp + 76) = UINT32_MAX; /* unlocked FILE */
	prepare_call();
	MEM32(g_esp + 4) = fp;
	MEM32(g_esp + 8) = 0;
	MEM32(g_esp + 12) = SEEK_END;
	HALO_FSEEK();
	assert(g_eax == 0);
	prepare_call();
	MEM32(g_esp + 4) = fp;
	HALO_FTELL();
	assert(g_eax == sizeof(config) - 1);
	MEM32(fp + 32) = HALO_STDIO_READ_VA;
	MEM32(fp + 44) = 0x60004000;
	MEM32(fp + 48) = 128; /* nonzero buffering selects readv */
	prepare_call();
	MEM32(g_esp + 4) = fp;
	MEM32(g_esp + 8) = 0;
	MEM32(g_esp + 12) = SEEK_SET;
	HALO_FSEEK();
	assert(g_eax == 0);
	prepare_call();
	MEM32(g_esp + 4) = 0x60005000;
	MEM32(g_esp + 8) = 1;
	MEM32(g_esp + 12) = sizeof(config) - 1;
	MEM32(g_esp + 16) = fp;
	HALO_FREAD();
	assert(g_eax == sizeof(config) - 1);
	assert(memcmp((void *)XBOX_PTR(0x60005000), config, sizeof(config) - 1) == 0);
	prepare_call();
	MEM32(g_esp + 4) = 0x60005000;
	MEM32(g_esp + 8) = 1;
	MEM32(g_esp + 12) = 1;
	MEM32(g_esp + 16) = fp;
	HALO_FREAD();
	assert(g_eax == 0); /* EOF follows the exact-length read. */
	puts("actual translated fseek/ftell/fread: configuration bytes and EOF match");
	close(fd);
	unlink(path);
	mac_guest_address_reset();
	free(bytes);
	return 0;
}
