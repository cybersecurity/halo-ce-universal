/*
HOST.H

Internals of the Android port's host library (libmain.so). See
port/android/README.md for the overall design and
port/android/include/halo_android_abi.h for the guest contract.
*/

#ifndef __HALO_ANDROID_HOST_H
#define __HALO_ANDROID_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_android_abi.h"

/* ---------- logging (logcat tag "halo") */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
/* guest services also used inside the host (host_main.c) */
void host_exit(int code) __attribute__((noreturn));
int host_errno(void);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- guest memory (host_memory.c)

All memory the guest can address lies below 4 GB. The host reserves the
Xbox window and the image's range at start-up, and hands out pages for
everything else (the guest's malloc arenas, thread stacks, anonymous
mappings) from pools of address space it reserves below 4 GB on demand. */

/* reserves the fixed ranges; returns 0 on success */
int host_memory_initialize(uint32_t image_base, uint32_t image_size);
/* page-granular allocations below 4 GB; NULL on failure */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
/* 1 if [address, address + size) was handed out by host_low_map or is one of
the fixed ranges */
int host_low_owns(uintptr_t address, size_t size);
/* the guest's mmap/munmap/mprotect/madvise/mremap (host_syscall.c) */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
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
/* config.toml's debug.sample_seconds: seconds between samples of the guest
threads, as text */
void host_debug_start_sampler(const char *setting);

/* ---------- import table (host_imports.c) */

/* the host function for an import name, or NULL */
void *host_resolve_import(const char *name);

/* ---------- SDL / GL (host_sdl.c, host_gl.c) */

/* SDL objects are 64-bit pointers, which the guest cannot hold: it gets
small handles into a table instead (host_sdl.c; port/linux/arm64's
host_desktop.c uses it too) */
enum host_sdl_handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_gamepad,
	_handle_audio,
	_handle_renderer,
};

/* the object's handle (the same one again for an object that has one), or
0 for NULL or a full table */
uint32_t host_sdl_handle_new(int type, void *object);
/* the object of a handle of that type, or NULL */
void *host_sdl_handle_get(uint32_t handle, int type);
void host_sdl_handle_free(uint32_t handle);

void *host_gl_resolve(const char *name);

#endif
