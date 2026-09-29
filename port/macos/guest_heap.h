#ifndef HALO_MACOS_GUEST_HEAP_H
#define HALO_MACOS_GUEST_HEAP_H

#include <stdint.h>

/* Installs the shared stack/mmap allocator into the linker-free guest VA
 * interval [align_up(image_end, host_page), 0x70000000). The image loader must
 * already own one linear 4 GiB address reservation. Anonymous
 * MAP_FIXED_NOREPLACE records ranges inside that arena, and later MAP_FIXED
 * may overlay only those explicitly replaceable reservations (used by the
 * Xbox contiguous-memory window at 0x80000000).
 * Darwin commits/protects at host-page granularity; the guest ABI still uses
 * 4KB pages. In the replaceable Xbox window, subpage data is tracked and a
 * host page receives the union of its subpage permissions, so adjacent live
 * blocks survive a 4KB free. macOS cannot enforce independent permissions
 * within one 16KB host page. File-backed mappings and executable guest
 * mappings fail explicitly. */
int mac_guest_heap_install(uint32_t image_end_va);

#endif
