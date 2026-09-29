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
 const uint16_t deltas[] = { 0, 1, 2, 256 };
 for (unsigned arm = 0; arm < 4; ++arm)
  for (unsigned short_ticks = 0; short_ticks < 2; ++short_ticks)
  {
   mac_recomp_state_initialize_thread(0x6000f000, 0x60002000);
   g_ebp = 0x13579; g_esi = 0x24680; g_edi = 0x35791;
   MEM8(0x60000200) = (uint8_t)(4 + arm); /* playback_end, each time-delta arm */
   MEM16(0x60000201) = deltas[arm];
   MEM32(0x60000100) = (uint32_t)((int32_t)deltas[arm] - short_ticks);
   MEM32(0x60000110) = 0x60000200;
   MEM32(g_esp) = 0; MEM32(g_esp + 4) = 0;
   MEM32(g_esp + 8) = 0x60000300;
   MEM32(g_esp + 12) = 0x60000100;
   MEM32(g_esp + 16) = 0x60000110;
   HALO_EVENT_FUNCTION();
   assert((g_eax & 255) == short_ticks);
   assert(g_esp == 0x6000f004 && g_ebp == 0x13579);
   assert(g_esi == 0x24680 && g_edi == 0x35791);
   assert(MEM32(0x60000110) == 0x60000200);
   assert(MEM32(0x60000100) == (uint32_t)((int32_t)deltas[arm] - short_ticks));
  }
 printf("actual event stream switch: all4arms/8cases preserve sameframe, ticks, stream, booleanresult\n");
	mac_guest_address_reset();
	free(bytes);
	return 0;
}
