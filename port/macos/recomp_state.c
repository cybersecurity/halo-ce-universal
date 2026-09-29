#include "recomp_state.h"

#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"

#include <string.h>

ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo;
uint32_t g_xbox_code_hi;

RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;
RECOMP_TLS uint32_t g_mxcsr;
RECOMP_TLS uint32_t g_fs_base;
RECOMP_TLS uint32_t g_seh_ebp;
RECOMP_TLS int g_df;
RECOMP_TLS uint16_t g_fp_control_word;
RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_status_word;
RECOMP_TLS uint8_t g_fp_empty_mask;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;

volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;

int mac_recomp_state_set_image(ptrdiff_t memory_offset, uint32_t code_lo,
	uint32_t code_hi)
{
	if (code_hi <= code_lo)
		return -1;
	g_xbox_mem_offset = memory_offset;
	g_xbox_code_lo = code_lo;
	g_xbox_code_hi = code_hi;
	return 0;
}

void mac_recomp_state_initialize_thread(uint32_t guest_esp, uint32_t fs_base)
{
	g_eax = g_ecx = g_edx = 0;
	g_esp = guest_esp;
	g_ebx = g_esi = g_edi = g_ebp = 0;
	memset(g_fp_stack, 0, sizeof(g_fp_stack));
	g_fp_top = 0;
	g_fs_base = fs_base;
	g_seh_ebp = 0;
	g_df = 0;
	g_fp_control_word = UINT16_C(0x037f);
	g_mxcsr = UINT32_C(0x1f80);
	g_fp_cmp = 0;
	g_fp_status_word = 0;
	g_fp_empty_mask = UINT8_C(0xff);
	memset(&g_xmm0, 0, sizeof(g_xmm0));
	memset(&g_xmm1, 0, sizeof(g_xmm1));
	memset(&g_xmm2, 0, sizeof(g_xmm2));
	memset(&g_xmm3, 0, sizeof(g_xmm3));
	memset(&g_xmm4, 0, sizeof(g_xmm4));
	memset(&g_xmm5, 0, sizeof(g_xmm5));
	memset(&g_xmm6, 0, sizeof(g_xmm6));
	memset(&g_xmm7, 0, sizeof(g_xmm7));
	memset(&g_mm0, 0, sizeof(g_mm0));
	memset(&g_mm1, 0, sizeof(g_mm1));
	memset(&g_mm2, 0, sizeof(g_mm2));
	memset(&g_mm3, 0, sizeof(g_mm3));
	memset(&g_mm4, 0, sizeof(g_mm4));
	memset(&g_mm5, 0, sizeof(g_mm5));
	memset(&g_mm6, 0, sizeof(g_mm6));
	memset(&g_mm7, 0, sizeof(g_mm7));
}
