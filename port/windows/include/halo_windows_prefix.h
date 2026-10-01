/*
HALO_WINDOWS_PREFIX.H

Force-included ahead of every game unit and every Xbox-facing platform unit
in the native Windows build (clang -include); the Windows-facing units
(port/windows/src/win32_*.c) never see it. The Microsoft target already
provides most of the MSVC environment the game was written for, so this is
much shorter than the Linux build's halo_linux_prefix.h.
*/

#ifndef __HALO_WINDOWS_PREFIX_H
#define __HALO_WINDOWS_PREFIX_H

#if !defined(_M_IX86)
#error the Windows port targets 32-bit x86: game data structures assume 32-bit pointers
#endif

#define HALO_WINDOWS 1

/* ---------- pointers inside Xbox data (the 64-bit macOS build's; no-ops
here, as in the Linux build: the shared sources name them) */

#include "../../../source/cseries/xbox_address.h"

/* ---------- XDK architecture selection */

#define _X86_ 1
#define _STDCALL_SUPPORTED 1
#define _USE_MATH_DEFINES

/* ---------- MSVC inline semantics

Clang's Microsoft target gives C `__inline` functions MSVC's COMDAT linkage
already. Where MSVC left a copy that clang inlines away, the build supplies
one: port/linux/game/msvc_comdat.c for header inlines, generated wrappers for
the few defined in .c files (tools/windows_build.py). */

/* ---------- MSVC intrinsics

Clang predeclares the MSVC _Interlocked* builtins with prototypes that
conflict with the XDK's WINAPI declarations; route the XDK names to the
platform layer instead (as on Linux: port/linux/src/xbox_kernel.c). */

#define _InterlockedCompareExchange halo_linux_InterlockedCompareExchange
#define _InterlockedDecrement halo_linux_InterlockedDecrement
#define _InterlockedExchange halo_linux_InterlockedExchange
#define _InterlockedExchangeAdd halo_linux_InterlockedExchangeAdd
#define _InterlockedIncrement halo_linux_InterlockedIncrement

/* ---------- structured exception handling

Only the top-level crash handler in main() uses SEH, and clang does not
catch a fault raised in the same function as the __try. The guarded block
always runs and the handler is compiled out; the platform layer reports
crashes instead (port/windows/src/win32_memory_watch.c). */

#define __try if (1)
#define __except(filter) else if (0)
#define __finally
#define __leave

/* ---------- multiplayer session limits of the native builds */

#include "../../linux/include/halo_port_limits.h"

/* the Xbox Winsock headers' fd_set, for game units and the platform's XNet
alike (the Windows-facing units never see this header) */
#define FD_SETSIZE HALO_PORT_FD_SETSIZE

/* ---------- Xbox functions named like Windows functions */

#include "halo_windows_api_names.h"

/* ---------- Winsock and source fixups shared with the Linux build */

#ifndef HALO_LINUX_PLATFORM_LAYER
/* the game's own strnlen (source/cseries/cseries.c), which the C runtime
also defines (see crt/string.h) */
#define strnlen halo_game_strnlen
#include "../../linux/include/halo_linux_winsock_names.h"
#include "../../linux/include/halo_linux_source_fixups.h"
#endif

#include <stddef.h>

#endif /* __HALO_WINDOWS_PREFIX_H */
