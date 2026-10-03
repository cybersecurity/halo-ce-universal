/* Execute the compiler-translated probe from signed pages on an Apple ARM64 host. */
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "guest_image.h"
extern unsigned long long host_guest_on_stack(uintptr_t,unsigned,uintptr_t,void *);
int main(void) {
    vm_address_t arena=0;
    int result=vm_map(mach_task_self(),&arena,0x100000000ull,0xffffffffull,VM_FLAGS_ANYWHERE,
        MACH_PORT_NULL,0,FALSE,VM_PROT_NONE,VM_PROT_ALL,VM_INHERIT_NONE);
    if(result){fprintf(stderr,"reserve: %d\n",result);return 1;}
    vm_address_t code=arena+0x88000000;vm_prot_t current,maximum;
    result=vm_remap(mach_task_self(),&code,IOS_GUEST_CODE_SIZE,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,
        mach_task_self(),(vm_address_t)halo_guest_code,FALSE,&current,&maximum,VM_INHERIT_COPY);
    if(result){fprintf(stderr,"signed alias: %d\n",result);return 1;}
    uintptr_t data=arena+IOS_GUEST_DATA_ADDRESS;
    if(mprotect((void *)data,(IOS_GUEST_IMAGE_END-IOS_GUEST_DATA_ADDRESS+0x3fff)&~0x3fff,PROT_READ|PROT_WRITE))return 2;
    memcpy((void *)data,halo_guest_data,IOS_GUEST_DATA_SIZE);
    uintptr_t stack=arena+0x1000000;
    if(mprotect((void *)stack,0x100000,PROT_READ|PROT_WRITE))return 3;
    uint32_t entry=((const uint32_t *)code)[1];
    result=host_guest_on_stack(arena+entry,0,arena,(void *)(stack+0x100000));
    uint32_t pointer_entry=((const uint32_t *)code)[2];
    uint64_t raw=host_guest_on_stack(arena+pointer_entry,0,arena,(void *)(stack+0x100000));
    if(raw>=0x100000000ull || raw<0x88000000)result|=32;
    printf("ILP32 guest probe: %s (failure bits %d, arena %lx)\n",result?"FAIL":"PASS",result,arena);
    return result;
}
