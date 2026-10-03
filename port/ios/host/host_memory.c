/* Reserve an aligned arena and emulate Xbox pages on Apple's 16 KB pages. */
#include "ios_host.h"
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 0x4000u
#define POOL_START 0x01000000u
#define POOL_END 0x78000000u
#define POOL_PAGES ((POOL_END-POOL_START)/PAGE)
#define WINDOW_PAGES (HALO_GUEST_WINDOW_SIZE/PAGE)
uintptr_t host_arena;
static unsigned char allocated[POOL_PAGES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t serial = 1;
static uint32_t generations[WINDOW_PAGES];
static volatile unsigned char protected_pages[WINDOW_PAGES];
static unsigned char watched_pages[WINDOW_PAGES];
static unsigned page_locks[WINDOW_PAGES];
static int watch_enabled;
static struct sigaction old_bus, old_segv;

static size_t rounded(size_t n) { return (n + PAGE - 1) & ~(size_t)(PAGE-1); }
static int in_window(uint32_t p, uint64_t n) {
    return p >= HALO_GUEST_WINDOW_BASE && (uint64_t)p+n <= (uint64_t)HALO_GUEST_WINDOW_BASE+HALO_GUEST_WINDOW_SIZE;
}

int host_memory_initialize(uint32_t image_base, uint32_t image_size) {
    (void)image_base; (void)image_size;
    vm_address_t arena = 0;
    kern_return_t result = vm_map(mach_task_self(), &arena, 0x100000000ull, 0xffffffffull,
        VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS || (arena & 0xffffffffull)) {
        host_logf(HOST_LOG_ERROR,"cannot reserve aligned guest arena: %d",result); return -1;
    }
    host_arena = arena;
    if (mprotect(host_pointer(HALO_GUEST_WINDOW_BASE),HALO_GUEST_WINDOW_SIZE,PROT_READ|PROT_WRITE)) return -1;
    host_logf(HOST_LOG_INFO,"guest arena at %llx, native pages %d",(unsigned long long)arena,getpagesize());
    return 0;
}

void *host_low_map(size_t size, int protection) {
    size_t pages=rounded(size)/PAGE, run=0;
    if (!pages || pages>POOL_PAGES) return NULL;
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<POOL_PAGES;i++) {
        run = allocated[i] ? 0 : run+1;
        if (run == pages) {
            size_t first=i+1-pages;
            void *p=host_pointer(POOL_START+first*PAGE);
            if (mmap(p,pages*PAGE,protection & ~PROT_EXEC,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)!=p) break;
            memset(allocated+first,1,pages);
            pthread_mutex_unlock(&lock); return p;
        }
    }
    pthread_mutex_unlock(&lock); return NULL;
}
void host_low_unmap(void *address,size_t size) {
    uint32_t p=guest_pointer(address);
    size_t pages=rounded(size)/PAGE;
    if ((p&(PAGE-1)) || p<POOL_START || (uint64_t)p+pages*PAGE>POOL_END) return;
    pthread_mutex_lock(&lock);
    mprotect(address,pages*PAGE,PROT_NONE);
    madvise(address,pages*PAGE,MADV_DONTNEED);
    memset(allocated+(p-POOL_START)/PAGE,0,pages);
    pthread_mutex_unlock(&lock);
}
int host_low_owns(uintptr_t p,size_t n) {
    return host_arena && p>=host_arena && p-host_arena+n<=0x100000000ull;
}

/* Linux mmap constants are deliberately decoded here instead of using Darwin's. */
long host_guest_mmap(uint64_t address,uint64_t size,int protection,int flags,int fd,int64_t offset) {
    if (!size || size>0x70000000ull || address+size>0x100000000ull) return -22;
    void *p;
    if (flags & (0x10|0x100000)) {
        if (!in_window((uint32_t)address,size)) return -22;
        p=host_pointer(address);
        uint32_t first=(uint32_t)address & ~(PAGE-1);
        size_t length=rounded(address+size-first);
        if (mprotect(host_pointer(first),length,PROT_READ|PROT_WRITE)) return -12;
        memset(p,0,size);
    } else {
        p=host_low_map(size,PROT_READ|PROT_WRITE);
        if (!p) return -12;
    }
    if (!(flags&0x20) && fd>=0) {
        ssize_t n=pread(fd,p,size,offset);
        if (n<0) return -5;
    }
    /* Xbox 4 KB allocations can share an Apple page: protection of the
       physical window is handled by the coarser write tracker. */
    if (!in_window(guest_pointer(p),size) && mprotect(p,rounded(size),protection & ~PROT_EXEC)) return -12;
    return guest_pointer(p);
}
long host_guest_munmap(uint64_t address,uint64_t size) {
    if (address+size>0x100000000ull) return -22;
    if (in_window(address,size)) return 0;
    host_low_unmap(host_pointer(address),size); return 0;
}
long host_guest_mprotect(uint64_t address,uint64_t size,int protection) {
    if (address+size>0x100000000ull || !size) return -22;
    uint32_t first=(uint32_t)address & ~(PAGE-1);
    if(in_window((uint32_t)address,size) && !(protection&PROT_WRITE)) {
        /* Xbox allocations/protection boundaries are 4 KB. Rounding a read-only
           buffer outwards also protects a neighboring writable zlib buffer.
           Enforce read-only only on complete 16 KB pages; boundary pages stay
           writable. VirtualProtect still records exact Xbox permissions in
           xbox_memory.c. Texture write tracking independently covers them. */
        uint64_t end=address+size;
        if(address!=first && mprotect(host_pointer(first),PAGE,PROT_READ|PROT_WRITE))return -12;
        if((end&(PAGE-1)) && mprotect(host_pointer(end&~(uint64_t)(PAGE-1)),PAGE,PROT_READ|PROT_WRITE))return -12;
        uint64_t inner_start=rounded(address),inner_end=end&~(uint64_t)(PAGE-1);
        return inner_end>inner_start && mprotect(host_pointer(inner_start),inner_end-inner_start,protection&~PROT_EXEC) ? -12:0;
    }
    return mprotect(host_pointer(first),rounded(address+size-first),protection & ~PROT_EXEC) ? -12 : 0;
}

/* A streaming worker can fault while the render thread protects the same
   16 KB page. Serialize the flag and VM change together. This lock never
   encloses guest-memory accesses, so a synchronous fault cannot reenter it. */
static void page_lock(unsigned page) {
    while(__atomic_exchange_n(&page_locks[page],1,__ATOMIC_ACQUIRE)) {}
}
static void page_unlock(unsigned page) { __atomic_store_n(&page_locks[page],0,__ATOMIC_RELEASE); }
static void mark_written(unsigned page) {
    if(mprotect(host_pointer(HALO_GUEST_WINDOW_BASE+page*PAGE),PAGE,PROT_READ|PROT_WRITE))_exit(128+SIGBUS);
    __atomic_store_n(&generations[page],__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED),__ATOMIC_RELEASE);
    protected_pages[page]=0;
}
static void fault(int sig,siginfo_t *info,void *context) {
    uintptr_t p=(uintptr_t)info->si_addr;
    if (host_low_owns(p,1) && in_window((uint32_t)p,1)) {
        unsigned page=((uint32_t)p-HALO_GUEST_WINDOW_BASE)/PAGE;
        page_lock(page);
        /* A second writer may already have repaired this page after our fault
           was queued. Remember that it was tracked even when now writable. */
        if (watched_pages[page]) { mark_written(page);page_unlock(page);return; }
        page_unlock(page);
    }
    struct sigaction *old=sig==SIGBUS?&old_bus:&old_segv;
    sigaction(sig,old,NULL);
    if ((old->sa_flags&SA_SIGINFO) && old->sa_sigaction) old->sa_sigaction(sig,info,context);
    else if (old->sa_handler!=SIG_DFL && old->sa_handler!=SIG_IGN) old->sa_handler(sig);
}
void host_install_signal_handlers(void) {
    struct sigaction action={0}; action.sa_sigaction=fault;
    action.sa_flags=SA_SIGINFO|SA_ONSTACK; sigemptyset(&action.sa_mask);
    sigaction(SIGBUS,&action,&old_bus);sigaction(SIGSEGV,&action,&old_segv);
}
void host_memory_watch_initialize(void) { watch_enabled=1; }
void host_memory_watch_protect(uint32_t address,uint32_t size) {
    if (!watch_enabled || !size || !in_window(address,size)) return;
    unsigned first=(address-HALO_GUEST_WINDOW_BASE)/PAGE;
    unsigned last=(address+size-1-HALO_GUEST_WINDOW_BASE)/PAGE;
    for(unsigned i=first;i<=last;i++) {
        page_lock(i);
        if(!protected_pages[i]) {
            watched_pages[i]=1;
            if(!mprotect(host_pointer(HALO_GUEST_WINDOW_BASE+i*PAGE),PAGE,PROT_READ))protected_pages[i]=1;
        }
        page_unlock(i);
    }
}
uint32_t host_memory_watch_serial(void) { return __atomic_load_n(&serial,__ATOMIC_RELAXED); }
uint32_t host_memory_watch_generation(uint32_t address,uint32_t size) {
    if (!size || !in_window(address,size)) return 0;
    uint32_t result=0;
    for(unsigned i=(address-HALO_GUEST_WINDOW_BASE)/PAGE;i<=(address+size-1-HALO_GUEST_WINDOW_BASE)/PAGE;i++) {
        uint32_t generation=__atomic_load_n(&generations[i],__ATOMIC_ACQUIRE);
        if(generation>result)result=generation;
    }
    return result;
}
void host_memory_watch_prepare_write(uint32_t address,uint32_t size) {
    if (!size || !in_window(address,size)) return;
    for(unsigned i=(address-HALO_GUEST_WINDOW_BASE)/PAGE;i<=(address+size-1-HALO_GUEST_WINDOW_BASE)/PAGE;i++) {
        page_lock(i);if(protected_pages[i])mark_written(i);page_unlock(i);
    }
}
void host_memory_watch_forget(uint32_t address,uint32_t size) {
    if (!size || !in_window(address,size)) return;
    for(unsigned i=(address-HALO_GUEST_WINDOW_BASE)/PAGE;i<=(address+size-1-HALO_GUEST_WINDOW_BASE)/PAGE;i++) {
        page_lock(i);mark_written(i);page_unlock(i);
    }
}
