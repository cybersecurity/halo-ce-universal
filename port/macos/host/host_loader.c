/* Loader for the locally compiled rebased ARM game image. */
#include "host.h"
#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <string.h>
#include <sys/mman.h>
typedef struct {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf;
typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} Segment;
struct host_guest_image host_image;
static int load_failure(int line) {
    host_logf(HOST_LOG_ERROR, "Loader failure at line %d (errno %d)", line, errno);
    return -1;
}
static int extent(uint64_t a, uint64_t n, uint64_t size) { return a <= size && n <= size - a; }
int host_load_image(const void *data, size_t size) {
    const Elf *elf = data;
    uint64_t low = UINT64_MAX, high = 0;
    if (size < sizeof(*elf) || memcmp(elf->ident, "\177ELF\2\1", 6) || elf->machine != 183 ||
        elf->type != 2 || elf->phentsize != sizeof(Segment) ||
        !extent(elf->phoff, (uint64_t)elf->phnum * sizeof(Segment), size))
        return load_failure(__LINE__);
    const Segment *segments = (const void *)((const char *)data + elf->phoff);
    for (unsigned i = 0; i < elf->phnum; i++)
        if (segments[i].type == 1) {
            const Segment *s = segments + i;
            if (s->filesz > s->memsz || !extent(s->offset, s->filesz, size) ||
                !extent(s->vaddr, s->memsz, UINT64_C(0x100000000)))
                return load_failure(__LINE__);
            if (s->vaddr < low)
                low = s->vaddr;
            if (s->vaddr + s->memsz > high)
                high = s->vaddr + s->memsz;
        }
    high = (high + HALO_MACOS_PAGE - 1) & ~(uint64_t)(HALO_MACOS_PAGE - 1);
    if (low != HALO_GUEST_IMAGE_BASE || high <= low || high - low > 0x10000000)
        return load_failure(__LINE__);
    if (host_memory_initialize((uint32_t)low, (uint32_t)(high - low)))
        return load_failure(__LINE__);
    for (unsigned i = 0; i < elf->phnum; i++)
        if (segments[i].type == 1)
            memcpy(guest_pointer(segments[i].vaddr), (const char *)data + segments[i].offset,
                   segments[i].filesz);
    const struct halo_guest_header *header = guest_pointer(low);
    if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION ||
        header->image_end > high)
        return load_failure(__LINE__);
    host_image = (struct host_guest_image){header, (uint32_t)low, (uint32_t)high};
    if (header->import_count < low || !extent(header->import_count, 4, high))
        return load_failure(__LINE__);
    uint32_t count = *(uint32_t *)guest_pointer(header->import_count);
    if (count > 4096 || header->import_table < low ||
        !extent(header->import_table, (uint64_t)count * 8, high) || header->import_names < low ||
        header->import_names >= high)
        return load_failure(__LINE__);
    uint64_t *table = guest_pointer(header->import_table);
    const char *name = guest_pointer(header->import_names);
    const char *end = guest_pointer(high);
    for (uint32_t i = 0; i < count; i++) {
        if (name >= end || !memchr(name, 0, (size_t)(end - name)))
            return load_failure(__LINE__);
        void *fn = host_resolve_import(name);
        if (!fn && !strncmp(name, "hostgl_", 7))
            fn = host_gl_resolve(name + 7);
        if (!fn) {
            host_logf(HOST_LOG_ERROR, "missing import: %s", name);
            return load_failure(__LINE__);
        }
        table[i] = (uintptr_t)fn;
        name += strlen(name) + 1;
    }
    for (unsigned i = 0; i < elf->phnum; i++)
        if (segments[i].type == 1 && (segments[i].flags & 1)) {
            uint64_t start = segments[i].vaddr & ~(uint64_t)(HALO_MACOS_PAGE - 1);
            uint64_t end = (segments[i].vaddr + segments[i].memsz + HALO_MACOS_PAGE - 1) &
                           ~(uint64_t)(HALO_MACOS_PAGE - 1);
            sys_icache_invalidate(guest_pointer(start), end - start);
            if (mprotect(guest_pointer(start), end - start, PROT_READ | PROT_EXEC))
                return load_failure(__LINE__);
        }
    host_logf(HOST_LOG_INFO, "Loaded ARM image %08x-%08x with %u host imports", host_image.base,
              host_image.end, count);
    return 0;
}
