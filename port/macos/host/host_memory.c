/* Native Darwin storage for the ILP32 guest, biased above the low 4 GB. */
#include "host.h"
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#define PAGE HALO_MACOS_PAGE
#define PAGES (UINT64_C(0x100000000) / PAGE)
static unsigned char used[PAGES], watched[PAGES];
static uint32_t generation[PAGES];
static uint32_t serial = 1;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
static int initialized;
/* Four Xbox 4 KB pages share one Darwin 16 KB page. Preserve the access
 * required by neighboring allocations when a single Xbox page changes. */
static unsigned char xbox_protection[HALO_GUEST_WINDOW_SIZE / 4096];
static int xbox_protect(uint32_t address, size_t size, int protection, int fresh) {
    if ((address & 4095) || !size ||
        (uint64_t)address + size > HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
        return -22;
    uint32_t first = (address - HALO_GUEST_WINDOW_BASE) / 4096;
    size_t count = (size + 4095) / 4096;
    uint32_t native_first = first & ~3u;
    size_t native_end = (first + count + 3) & ~(size_t)3;
    pthread_mutex_lock(&memory_lock);
    /* Updating one Xbox allocation can make its whole Darwin page writable.
       Invalidate watched neighbors before changing protection or zeroing data,
       so cached geometry and textures notice subsequent writes. */
    host_memory_watch_prepare_write(HALO_GUEST_WINDOW_BASE + native_first * 4096,
                                   (uint32_t)((native_end - native_first) * 4096));
    if (fresh && protection) {
        if (mprotect(guest_pointer(HALO_GUEST_WINDOW_BASE + native_first * 4096),
                     (native_end - native_first) * 4096, PROT_READ | PROT_WRITE)) {
            pthread_mutex_unlock(&memory_lock);
            return -host_linux_errno(errno);
        }
        memset(guest_pointer(address), 0, count * 4096);
    }
    memset(xbox_protection + first, protection, count);
    int result = 0;
    for (size_t i = native_first; i < native_end; i += 4) {
        int combined = xbox_protection[i] | xbox_protection[i + 1] | xbox_protection[i + 2] |
                       xbox_protection[i + 3];
        if (mprotect(guest_pointer(HALO_GUEST_WINDOW_BASE + i * 4096), PAGE, combined)) {
            result = -host_linux_errno(errno);
            break;
        }
    }
    pthread_mutex_unlock(&memory_lock);
    return result;
}
static size_t rounded(size_t n) { return (n + PAGE - 1) & ~(size_t)(PAGE - 1); }
static void occupy(uint32_t a, size_t n) { memset(used + a / PAGE, 1, rounded(n) / PAGE); }
int host_memory_initialize(uint32_t image_base, uint32_t image_size) {
    if (initialized)
        return 0;
    // Unlike MAP_FIXED, mach_vm_allocate with VM_FLAGS_FIXED never replaces
    // an existing mapping. Darwin may disregard mmap hints for a large arena.
    vm_address_t address = HALO_MACOS_BIAS;
    kern_return_t error =
        vm_allocate(mach_task_self(), &address, HALO_ARENA_SIZE, VM_FLAGS_FIXED);
    if (error != KERN_SUCCESS) {
        host_logf(HOST_LOG_ERROR, "Guest arena reservation: %s", mach_error_string(error));
        vm_address_t occupied = HALO_MACOS_BIAS;
        vm_size_t length = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object;
        if (vm_region_64(mach_task_self(), &occupied, &length, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &count, &object) == KERN_SUCCESS) {
            host_logf(HOST_LOG_ERROR, "Next occupied range: %llx-%llx",
                (unsigned long long)occupied, (unsigned long long)(occupied + length));
            if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        }
        return -1;
    }
    if (mprotect((void *)address, HALO_ARENA_SIZE, PROT_NONE))
        return -1;
    occupy(0, 0x1000000);
    occupy(HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE);
    occupy(image_base, image_size);
    if (mprotect(guest_pointer(HALO_GUEST_WINDOW_BASE), HALO_GUEST_WINDOW_SIZE,
                 PROT_READ | PROT_WRITE))
        return -1;
    if (mprotect(guest_pointer(image_base), rounded(image_size), PROT_READ | PROT_WRITE))
        return -1;
    initialized = 1;
    return 0;
}
void *host_low_map(size_t n, int protection) {
    if (!n || n > 0x40000000)
        return NULL;
    n = rounded(n);
    unsigned need = (unsigned)(n / PAGE), run = 0, start = 0;
    pthread_mutex_lock(&memory_lock);
    for (unsigned i = 0x1000000 / PAGE; i < HALO_GUEST_WINDOW_BASE / PAGE; i++) {
        if (used[i])
            run = 0;
        else {
            if (!run)
                start = i;
            if (++run == need) {
                void *p = guest_pointer(start * PAGE);
                /* MAP_FIXED is safe here: this is a free portion of our already reserved
                 * arena. A fresh anonymous map must contain zeros, including after reuse;
                 * Darwin's MADV_DONTNEED alone does not provide that guarantee. */
                if (mmap(p, n, protection, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) ==
                    MAP_FAILED) {
                    pthread_mutex_unlock(&memory_lock);
                    return NULL;
                }
                memset(used + start, 1, need);
                pthread_mutex_unlock(&memory_lock);
                return p;
            }
        }
    }
    pthread_mutex_unlock(&memory_lock);
    return NULL;
}
void host_low_unmap(void *p, size_t n) {
    uint32_t a = (uint32_t)(uintptr_t)p;
    if (!n || a < 0x1000000 || (uint64_t)a + n > HALO_GUEST_WINDOW_BASE)
        return;
    n = rounded(n);
    pthread_mutex_lock(&memory_lock);
    mprotect(p, n, PROT_NONE);
    madvise(p, n, MADV_DONTNEED);
    memset(used + a / PAGE, 0, n / PAGE);
    memset(watched + a / PAGE, 0, n / PAGE);
    pthread_mutex_unlock(&memory_lock);
}
int host_low_owns(uintptr_t a, size_t n) {
    return a >= HALO_MACOS_BIAS && a + n >= a && a + n <= HALO_MACOS_BIAS + HALO_ARENA_SIZE;
}
long host_guest_mmap(uint64_t a, uint64_t n, int prot, int flags, int fd, int64_t offset) {
    if (!n || n > 0x40000000)
        return -22;
    if (!(flags & 0x20) || fd != -1 || offset)
        return -38;
    if (flags & (0x10 | 0x100000)) {
        if (a < HALO_GUEST_WINDOW_BASE || a + n > HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
            return -22;
        if (!(flags & 0x100000)) {
            int result = xbox_protect((uint32_t)a, n, prot, 1);
            if (result)
                return result;
        }
        return (uint32_t)a;
    }
    void *p = host_low_map(n, prot);
    return p ? (long)(uint32_t)(uintptr_t)p : -12;
}
long host_guest_munmap(uint64_t a, uint64_t n) {
    if ((a & (PAGE - 1)) || a + n > UINT64_C(0x100000000))
        return -22;
    host_low_unmap(guest_pointer(a), n);
    return 0;
}
long host_guest_mprotect(uint64_t a, uint64_t n, int prot) {
    if (!n || a + n > UINT64_C(0x100000000))
        return -22;
    if (a >= HALO_GUEST_WINDOW_BASE && a + n <= HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
        return xbox_protect((uint32_t)a, n, prot, 0);
    uint32_t start = (uint32_t)a & ~(PAGE - 1);
    size_t size = rounded(a + n - start);
    return mprotect(guest_pointer(start), size, prot) ? -host_linux_errno(errno) : 0;
}
void host_memory_watch_initialize(void) {}
void host_memory_watch_protect(uint32_t a, uint32_t n) {
    if (!n || (uint64_t)a + n > UINT64_C(0x100000000))
        return;
    uint32_t first = a / PAGE, last = (a + n - 1) / PAGE;
    for (uint32_t i = first; i <= last; i++)
        if (!__atomic_load_n(watched + i, __ATOMIC_RELAXED)) {
            __atomic_store_n(watched + i, 1, __ATOMIC_RELEASE);
            mprotect(guest_pointer(i * PAGE), PAGE, PROT_READ);
        }
}
uint32_t host_memory_watch_generation(uint32_t a, uint32_t n) {
    uint32_t result = 0;
    if (!n || (uint64_t)a + n > UINT64_C(0x100000000))
        return 0;
    for (uint32_t i = a / PAGE; i <= (a + n - 1) / PAGE; i++) {
        uint32_t g = __atomic_load_n(generation + i, __ATOMIC_ACQUIRE);
        if (g > result)
            result = g;
    }
    return result;
}
uint32_t host_memory_watch_serial(void) { return __atomic_load_n(&serial, __ATOMIC_ACQUIRE); }
void host_memory_watch_prepare_write(uint32_t a, uint32_t n) {
    if (!n || (uint64_t)a + n > UINT64_C(0x100000000))
        return;
    for (uint32_t i = a / PAGE; i <= (a + n - 1) / PAGE; i++)
        if (__atomic_exchange_n(watched + i, 0, __ATOMIC_ACQ_REL)) {
            mprotect(guest_pointer(i * PAGE), PAGE, PROT_READ | PROT_WRITE);
            __atomic_store_n(generation + i, __atomic_add_fetch(&serial, 1, __ATOMIC_ACQ_REL),
                             __ATOMIC_RELEASE);
        }
}
void host_memory_watch_forget(uint32_t a, uint32_t n) {
    if (!n || (uint64_t)a + n > UINT64_C(0x100000000))
        return;
    /* Remapping invalidates cached data even if a previous write already
       removed the watch. Match the shared allocator's forget contract. */
    for (uint32_t i = a / PAGE; i <= (a + n - 1) / PAGE; i++) {
        if (__atomic_exchange_n(watched + i, 0, __ATOMIC_ACQ_REL))
            mprotect(guest_pointer(i * PAGE), PAGE, PROT_READ | PROT_WRITE);
        __atomic_store_n(generation + i, __atomic_add_fetch(&serial, 1, __ATOMIC_ACQ_REL),
                         __ATOMIC_RELEASE);
    }
}
static void fault(int signal, siginfo_t *info, void *context) {
    uintptr_t address = (uintptr_t)info->si_addr;
    if (address >= HALO_MACOS_BIAS && address < HALO_MACOS_BIAS + UINT64_C(0x100000000)) {
        uint32_t a = (uint32_t)address, index = a / PAGE;
        if (__atomic_exchange_n(watched + index, 0, __ATOMIC_ACQ_REL)) {
            mprotect(guest_pointer(index * PAGE), PAGE, PROT_READ | PROT_WRITE);
            __atomic_store_n(generation + index, __atomic_add_fetch(&serial, 1, __ATOMIC_ACQ_REL),
                             __ATOMIC_RELEASE);
            return;
        }
    }
    ucontext_t *u = context;
    uint32_t guest_pc = (uint32_t)u->uc_mcontext->__ss.__pc;
    fprintf(stderr, "Halo fault %d at %p; pc=%llx guest pc=%x\n", signal, info->si_addr,
            (unsigned long long)u->uc_mcontext->__ss.__pc, guest_pc);
    _exit(128 + signal);
}
void host_install_signal_handlers(void) {
    struct sigaction a = {0};
    a.sa_sigaction = fault;
    a.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &a, NULL);
    sigaction(SIGBUS, &a, NULL);
}
void host_debug_thread_started(void) {}
void host_debug_thread_exited(void) {}
void host_debug_start_sampler(const char *setting) { (void)setting; }
