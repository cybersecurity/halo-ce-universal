/* Stress the exact render/streaming race on the host's native page size. */
#include "ios_host.h"
#include "guest_host.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sched.h>
#include <sys/mman.h>

void host_logf(int level,const char *format,...) {
    (void)level;va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);fputc('\n',stderr);va_end(ap);
}
static unsigned finished, started;
static void *writer(void *arg) {
    volatile unsigned *p=host_pointer(0x80010000+(uintptr_t)arg*4);
    __atomic_add_fetch(&started,1,__ATOMIC_RELEASE);
    unsigned n=0;while(!__atomic_load_n(&finished,__ATOMIC_RELAXED))*p=++n;
    return NULL;
}
int main(void) {
    if(host_memory_initialize(0,0))return 1;
    host_install_signal_handlers();host_memory_watch_initialize();
    /* Reproduce the map inflater's writable 4 KB tail beside a read-only
       buffer. The boundary Apple page must remain writable. */
    if(host_guest_mprotect(0x80020000,0x6000,PROT_READ))return 4;
    *(volatile unsigned *)host_pointer(0x80026014)=42;
    pthread_t threads[4];
    for(uintptr_t i=0;i<4;i++)pthread_create(&threads[i],NULL,writer,(void *)i);
    while(__atomic_load_n(&started,__ATOMIC_ACQUIRE)<4)sched_yield();
    for(int i=0;i<40000;i++){host_memory_watch_protect(0x80010000,4096);if(!(i%10))sched_yield();}
    __atomic_store_n(&finished,1,__ATOMIC_RELAXED);
    for(int i=0;i<4;i++)pthread_join(threads[i],NULL);
    unsigned generation=host_memory_watch_generation(0x80010000,4096);
    if(!generation)return 2;
    host_memory_watch_prepare_write(0x80010000,4096);
    unsigned *p=host_low_map(16384,PROT_READ|PROT_WRITE);*p=1234;
    host_low_unmap(p,16384);p=host_low_map(16384,PROT_READ|PROT_WRITE);
    if(*p)return 3;
    printf("PASS: concurrent page tracking, generation %u; reused mmap zeroed\n",generation);
    return 0;
}
