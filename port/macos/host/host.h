/* Native Darwin services for the rebased ILP32 ARM guest.
 * The shared wire ABI is in port/android/include/halo_android_abi.h.
 */

#ifndef __HALO_MACOS_HOST_H
#define __HALO_MACOS_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_android_abi.h"

#define HALO_MACOS_BIAS UINT64_C(0x10000000000)
#define HALO_ARENA_SIZE UINT64_C(0x100000000)
#define guest_code_pointer guest_pointer
#define HALO_MACOS_PAGE 16384u
static inline void *guest_pointer(uint64_t p) {
    return p ? (void *)(HALO_MACOS_BIAS | (uint32_t)p) : NULL;
}
int host_linux_errno(int value);
void host_install_signal_handlers(void);

/* ---------- logging */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
/* guest services also used inside the host (host_main.c) */
void host_exit(int code) __attribute__((noreturn));
int host_errno(void);
/* Queue an OS invite in this instance's save folder for the P2P thread. */
int host_invite_received(const char *text);
/* Discord launches the app first, then delivers its invite through RPC. */
int host_is_discord_launch_url(const char *text);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- guest memory (host_memory.c)

Guest pointers remain 32-bit offsets. The LLVM pass rebases dereferences
into a reserved 4 GB virtual arena above macOS's low-address guard. Physical
memory is committed on demand. Darwin allocations use 16 KB pages, with
separate 4 KB protection bookkeeping for Xbox allocations. */

/* reserves the fixed ranges; returns 0 on success */
int host_memory_initialize(uint32_t image_base, uint32_t image_size);
/* page-granular allocations in the guest arena; NULL on failure */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
/* 1 if [address, address + size) was handed out by host_low_map or is one of
the fixed ranges */
int host_low_owns(uintptr_t address, size_t size);
/* the guest's mmap/munmap/mprotect/madvise/mremap (host_syscall.c) */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd,
                     int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);
void host_memory_watch_protect(uint32_t address, uint32_t size);
uint32_t host_memory_watch_generation(uint32_t address, uint32_t size);
void host_memory_watch_prepare_write(uint32_t address, uint32_t size);
void host_memory_watch_forget(uint32_t address, uint32_t size);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image {
    const struct halo_guest_header *header;
    uint32_t base, end;
};

extern struct host_guest_image host_image;

/* maps the image from the ELF file in memory; returns 0 on success */
int host_load_image(const void *elf, size_t size);

/* ---------- entering guest code (host_thread.c) */

/* calls the guest function at address with up to four 32-bit arguments on
this thread, which must have been made by host_native_thread_create (giving
the thread a guest struct pthread first if it has none); returns the
guest's w0 */
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* starts a thread running function(argument) with its stack in guest
memory, so that it can call guest code; the stack is freed after it exits.
Returns 0 or an errno value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
/* runs the guest's __guest_start on the calling thread (one made by
host_native_thread_create); does not return */
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));

/* ---------- debugging (host_debug.c) */

void host_debug_thread_started(void);
void host_debug_thread_exited(void);
/* HALO_SAMPLE: seconds between samples of the guest threads, or NULL */
void host_debug_start_sampler(const char *setting);

/* ---------- import table (host_imports.c) */

/* the host function for an import name, or NULL */
void *host_resolve_import(const char *name);

/* ---------- SDL / GL (host_sdl.c, host_gl.c) */

void *host_gl_resolve(const char *name);

#endif
