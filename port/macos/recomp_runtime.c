#include "host_services.h"
#include "gl_token_registry.h"
#include "guest_address.h"
#include "guest_call.h"

#include "recomp_types.h"

#include <stdatomic.h>

/* The source ELF contains the complete guest function image; this AOT target
 * has no separate Xbox kernel-thunk range. Imported Android host functions
 * are resolved by the exact manifest aliases before this fallback. */
recomp_func_t recomp_lookup_kernel(uint32_t guest_va)
{
	/* GL procedure tokens live in the FE100000 namespace. The AOT guest holds
	 * these 32-bit tokens in its function tables; they resolve to typed guest
	 * cdecl wrappers, never to native 64-bit GL pointers. */
	if (guest_va >= UINT32_C(0xFE100000))
		return (recomp_func_t)mac_macos_gl_wrapper_for_token(guest_va);
	return NULL;
}

void recomp_icall_fail_log(uint32_t guest_va)
{
	static _Atomic unsigned reports;
	unsigned report = atomic_fetch_add_explicit(&reports, 1, memory_order_relaxed);
	if (report < 32)
		mac_host_logf(3, "unresolved guest indirect call target 0x%08x (icalls=%llu)",
			guest_va, (unsigned long long)g_icall_count);
	mac_host_abort("unresolved in-range translated indirect call");
}

void recomp_icall_not_code_log(uint32_t guest_va)
{
	static _Atomic unsigned reports;
	unsigned report = atomic_fetch_add_explicit(&reports, 1, memory_order_relaxed);
	if (report < 32)
		mac_host_logf(3, "guest indirect target outside code/import ranges: 0x%08x",
			guest_va);
	mac_host_abort("invalid guest indirect target");
}

void recomp_unsupported_instruction(uint32_t guest_va)
{
	mac_host_logf(3, "AOT guest reached unsupported instruction at 0x%08x", guest_va);
	mac_host_logf(3, "guest registers esp=0x%08x ebp=0x%08x eax=0x%08x ecx=0x%08x edx=0x%08x",
		g_esp, g_ebp, g_eax, g_ecx, g_edx);
	uint32_t frame = g_ebp;
	for (unsigned index = 0; index < 16 && frame; ++index)
	{
		uint32_t words[2];
		if (mac_guest_read(frame, words, sizeof(words)) != 0)
			break;
		mac_host_logf(3, "guest frame %u fp=0x%08x return=0x%08x parent=0x%08x",
			index, frame, words[1], words[0]);
		uint32_t arguments[6];
		if (mac_guest_read(frame + 8, arguments, sizeof(arguments)) == 0)
		{
			mac_host_logf(3, "guest frame %u words=%08x %08x %08x %08x %08x %08x",
				index, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5]);
			if (guest_va == 0x003e2a23 && index == 1)
				for (unsigned arg = 0; arg < 2; ++arg)
				{
					uint32_t bytes[4];
					if (arguments[arg] >= 8 && mac_guest_read(arguments[arg] - 8, bytes, sizeof(bytes)) == 0)
						mac_host_logf(3, "malloc guard arg%u va=%08x surrounding=%08x %08x %08x %08x",
							arg, arguments[arg], bytes[0], bytes[1], bytes[2], bytes[3]);
				}
		}
		if (words[0] <= frame || words[0] - frame > 1024 * 1024)
			break;
		frame = words[0];
	}
	mac_host_abort("translated guest instruction is unsupported");
}
