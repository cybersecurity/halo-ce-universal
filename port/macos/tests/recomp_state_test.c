#include "recomp_state.h"
#define RECOMP_GENERATED_CODE 1
#include <recomp_types.h>

#include <assert.h>
#include <stdint.h>

int main(void)
{
	assert(mac_recomp_state_set_image((ptrdiff_t)0x12345000, 0x10000, 0x90000) == 0);
	assert(g_xbox_mem_offset == (ptrdiff_t)0x12345000);
	assert(g_xbox_code_lo == 0x10000 && g_xbox_code_hi == 0x90000);
	assert(mac_recomp_state_set_image(0, 0x20000, 0x10000) != 0);

	g_eax = 1; g_esp = 2; g_fp_stack[3] = 4; g_xmm2.u[1] = 5;
	g_mm7.q = 6; g_fs_base = 7; g_df = 1;
	mac_recomp_state_initialize_thread(0x7fff0000, 0x700000);
	assert(g_eax == 0 && g_esp == 0x7fff0000 && g_fs_base == 0x700000);
	assert(g_fp_stack[3] == 0.0 && g_xmm2.u[1] == 0 && g_mm7.q == 0);
	assert(g_df == 0 && g_fp_control_word == 0x037f && g_fp_empty_mask == 0xff);
	assert(g_mxcsr == UINT32_C(0x1f80));
	g_mxcsr = UINT32_C(0x5f80);
	mac_recomp_state_initialize_thread(0x7fff0000, 0x700000);
	assert(g_mxcsr == UINT32_C(0x1f80));
	return 0;
}
