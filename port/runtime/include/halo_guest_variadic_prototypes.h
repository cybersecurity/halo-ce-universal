/*
HALO_ILP32_VARIADIC_PROTOTYPES.H

Force-included by the iOS build (tools/ios_guest_build.py) into the game
files that call these variadic functions without a prototype in scope.
x86 passes every argument on the stack either way, but the iOS guest's
calling convention (Darwin's arm64) passes variadic arguments on the stack
and the others in registers, so such a call must see the real prototype.
tools/guest_abi_check.py finds the calls.
*/

#ifndef __HALO_ILP32_VARIADIC_PROTOTYPES_H
#define __HALO_ILP32_VARIADIC_PROTOTYPES_H

union real_argb_color;

void error(short priority, const char *format, ...);
void console_printf(unsigned char clear, char const *format, ...);
void terminal_printf(union real_argb_color const *color, char const *format, ...);

#endif
