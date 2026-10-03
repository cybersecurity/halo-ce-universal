/* Alias signed code pages into the arena; writable game data has its own pages. */
#include "ios_host.h"
#include "guest_image.h"
#include <mach/mach.h>
#include <string.h>
#include <sys/mman.h>
struct host_guest_image host_image;
static void missing_import(void) { host_fatal("an unavailable guest import was called"); }
int host_load_image(const void *unused,size_t ignored) {
    (void)unused;(void)ignored;
    if(host_memory_initialize(HALO_GUEST_IMAGE_BASE,IOS_GUEST_IMAGE_END-HALO_GUEST_IMAGE_BASE))return -1;
    vm_address_t code=(uintptr_t)host_pointer(HALO_GUEST_IMAGE_BASE);
    vm_prot_t current=0,maximum=0;
    kern_return_t result=vm_remap(mach_task_self(),&code,IOS_GUEST_CODE_SIZE,0,
        VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,mach_task_self(),(vm_address_t)halo_guest_code,
        FALSE,&current,&maximum,VM_INHERIT_COPY);
    if(result || !(current&VM_PROT_EXECUTE) || (current&VM_PROT_WRITE)) {
        host_logf(HOST_LOG_ERROR,"cannot alias signed guest code: %d (protection %d)",result,current);return -1;
    }
    void *data=host_pointer(IOS_GUEST_DATA_ADDRESS);
    if(mprotect(data,(IOS_GUEST_IMAGE_END-IOS_GUEST_DATA_ADDRESS+0x3fff)&~0x3fff,PROT_READ|PROT_WRITE))return -1;
    memcpy(data,halo_guest_data,IOS_GUEST_DATA_SIZE);
    const struct halo_guest_header *header=host_pointer(HALO_GUEST_IMAGE_BASE);
    if(header->magic!=HALO_GUEST_MAGIC || header->abi_version!=HALO_GUEST_ABI_VERSION)return -1;
    host_image=(struct host_guest_image){header,HALO_GUEST_IMAGE_BASE,IOS_GUEST_IMAGE_END};
    uint64_t *table=host_pointer(header->import_table);
    const char *name=host_pointer(header->import_names);
    uint32_t count=*(const uint32_t *)host_pointer(header->import_count);
    if(count>4096)return -1;
    unsigned missing=0;
    for(unsigned i=0;i<count;i++) {
        void *function=host_resolve_import(name);
        if(!function){host_logf(HOST_LOG_WARN,"unavailable import: %s",name);function=missing_import;missing++;}
        table[i]=(uintptr_t)function;name+=strlen(name)+1;
    }
    host_logf(HOST_LOG_INFO,"loaded guest image, %u imports (%u unavailable)",count,missing);
    return 0;
}
