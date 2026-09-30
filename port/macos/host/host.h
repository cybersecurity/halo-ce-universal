/*
HOST.H

Internals of the macOS port's host executable. See port/macos/README.md for
the design and port/android/include/halo_android_abi.h for the guest
contract, which the macOS port shares with the Android port.

The host comes in two builds (tools/macos_build.py):

- native (Apple silicon): an arm64 executable running the game compiled as
  arm64_32 code, as on Android, whose memory accesses are rebased onto a
  4 GB-aligned region of the host's address space
  (tools/macos_arm64_rebase.py): arm64 macOS processes cannot map the low
  4 GB;
- x86-64 (Intel Macs, or Rosetta 2): an executable linked with a small
  __PAGEZERO, whose low 4 GB is the guest's region itself, running the game
  compiled as x32 code.

A guest address is a 32-bit offset into the region; the host address of it
is host_guest_base plus the offset (GUEST), 0 for the x86-64 build. The
thunks between the guest's imports and the host functions translate
pointer arguments (tools/macos_host_thunks.py).
*/

#ifndef __HALO_MACOS_HOST_H
#define __HALO_MACOS_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_android_abi.h"

/* ---------- logging (stderr and <data>/host.txt) */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
void host_log(int priority, const char *text);
void host_exit(int code) __attribute__((noreturn));
int host_errno(void);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* the folder holding the executable (and maps/, config.toml, saves) */
extern char host_data_root[1024];
/* the path the guest sees as /proc/self/exe */
extern char host_executable_path[1024];

/* ---------- guest addresses */

extern uint64_t host_guest_base;
/* a guest address as a host pointer */
#define GUEST(type, value) ((type)(uintptr_t)(host_guest_base + (uint32_t)(value)))
/* a host pointer into the guest's region as a guest address */
#define GUEST_ADDRESS(pointer) ((uint32_t)((uintptr_t)(pointer) - host_guest_base))

/* host pointers the guest holds as small handles (host_main.c) */
uint32_t host_handle_new(void *pointer);
void *host_handle_get(uint32_t handle);
void host_handle_release(uint32_t handle);

/* ---------- errno and flag translation (host_syscall.c) */

/* the Linux errno value for a macOS one */
int host_linux_errno(int value);

/* ---------- guest memory (host_memory.c)

All memory the guest can address lies below 4 GB. The host reserves one
region there at start-up, before anything else can map into it, and hands
out the Xbox window, the image's range and pages for everything else (the
guest's malloc arenas, thread stacks, anonymous mappings) from it. */

int host_memory_reserve(void);
/* the image's range, in guest addresses */
int host_memory_initialize(uint32_t image_base, uint32_t image_size);
/* page-granular allocations in the region, as host pointers */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
/* the host's page size (16 KB on Apple silicon, 4 KB on x86-64) */
size_t host_guest_page_size(void);
/* 1 if the guest range [address, address + size) was handed out or is one
of the fixed ranges */
int host_low_owns(uint64_t address, uint64_t size);
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);
void host_install_signal_handlers(void);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
};

extern struct host_guest_image host_image;

int host_load_image(const void *elf, size_t size);

/* ---------- entering guest code (host_thread.c) */

uint32_t host_get_tp(void);
void host_set_tp(uint32_t thread);
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
/* runs the guest's __guest_start on the calling thread (the process's main
thread, which Cocoa needs for the window and events) on a stack in guest
memory; does not return */
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));

void host_debug_thread_started(void);
void host_debug_thread_exited(void);

/* ---------- profiling (host_profile.c, HALO_PROFILE=1) */

void host_profile_load_symbols(const void *elf, size_t size);
void host_profile_start(void);
void host_profile_write(void);

/* ---------- import table (generated host_import_table.c) */

void *host_resolve_import(const char *name);

/* ---------- SDL / GL (host_sdl.c, host_gl.c) */

void *host_gl_resolve(const char *name);

#endif
