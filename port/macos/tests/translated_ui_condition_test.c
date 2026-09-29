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
static void unrelated_false(void) { g_eax = 0; g_esp += 4; }
recomp_func_t recomp_lookup_manual(uint32_t va)
{
 if (va == HALO_UI_CALLBACK_VA) return HALO_UI_CALLBACK;
 if (va == HALO_UI_OTHER_VA) return unrelated_false;
 return NULL;
}
int profile_count, name_opened, warnings, errors;
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
 const uint32_t indices[] = {0,101,102,UINT16_MAX};
 for(unsigned i=0;i<4;i++) {
  mac_recomp_state_initialize_thread(0x6000f000,0x60002000);
  MEM32(g_esp)=0; MEM32(g_esp+4)=indices[i];
  HALO_UI_CLASSIFIER();
  assert((g_eax&255)==(indices[i]==101));
  assert(g_esp==0x6000f004);
 }
 for(unsigned mode=0;mode<3;mode++) {
  profile_count=mode==1;name_opened=warnings=errors=0;
  mac_recomp_state_initialize_thread(0x6000f000,0x60002000);
  MEM8(0x60000300)=0;
  MEM32(g_esp)=0;MEM32(g_esp+4)=0x60000100;MEM32(g_esp+8)=0x60000200;
  MEM32(g_esp+12)=mode==2?0:101;MEM32(g_esp+16)=0x60000300;
  HALO_UI_INVOKER();
  assert((g_eax&255)==(mode==1));
  assert(name_opened==(mode==0) && warnings==(mode==2) && errors==0);
  assert(MEM8(0x60000300)==0 && g_esp==0x6000f004);
 }
 puts("actual translated UI callback101: zero profiles opens naming/FALSE; existing profile TRUE; unrelatedFALSE warns; classifier bounds pass");
	mac_guest_address_reset();
	free(bytes);
	return 0;
}
