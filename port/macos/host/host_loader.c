/*
HOST_LOADER.C

Loads the guest image: a statically linked ELF executable whose segments are
copied to the guest addresses it was linked at (host_guest_base plus them).
The native build's image is AArch64 code (64-bit ELF, built from arm64_32
code); the x86-64 build's is x32 code (32-bit ELF, x86-64 machine); see
tools/macos_build.py. The image starts with a struct halo_guest_header
naming its import table, which is filled with the host functions of the
same names (their thunks: tools/macos_host_thunks.py).
*/

#include "host.h"

#include <string.h>
#include <sys/mman.h>
#ifdef __aarch64__
#include <libkern/OSCacheControl.h>
#endif

/* the parts of <elf.h> this needs (macOS has none) */
typedef struct
{
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint32_t e_entry;
	uint32_t e_phoff;
	uint32_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} elf32_header;

typedef struct
{
	uint32_t p_type;
	uint32_t p_offset;
	uint32_t p_vaddr;
	uint32_t p_paddr;
	uint32_t p_filesz;
	uint32_t p_memsz;
	uint32_t p_flags;
	uint32_t p_align;
} elf32_segment;

typedef struct
{
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} elf64_header;

typedef struct
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} elf64_segment;

#define ELF_CLASS32 1
#define ELF_CLASS64 2
#define ELF_EXECUTABLE 2
#define ELF_MACHINE_X86_64 62
#define ELF_MACHINE_AARCH64 183
#define SEGMENT_LOAD 1
#define SEGMENT_EXECUTE 1

/* a loadable segment, whichever the class */
struct segment
{
	uint64_t address, file_size, memory_size, offset;
	int executable;
};

#define MAXIMUM_SEGMENTS 16

struct host_guest_image host_image;

static void missing_import(void)
{
	host_fatal("the guest called a host function that is not available");
}

/* the image's loadable segments; their count, or -1 */
static int image_segments(const void *file, size_t size, struct segment *segments)
{
	const unsigned char *ident = file;
	int count = 0, index;

	if (size < sizeof(elf64_header) || memcmp(ident, "\177ELF", 4))
		return -1;
#ifdef __aarch64__
	if (ident[4] == ELF_CLASS64 && ((const elf64_header *)file)->e_machine == ELF_MACHINE_AARCH64 &&
		((const elf64_header *)file)->e_type == ELF_EXECUTABLE)
	{
		const elf64_header *elf = file;
		const elf64_segment *list = (const elf64_segment *)((const char *)file + elf->e_phoff);

		for (index = 0; index < elf->e_phnum && count < MAXIMUM_SEGMENTS; index++)
		{
			if (list[index].p_type != SEGMENT_LOAD)
				continue;
			segments[count].address = list[index].p_vaddr;
			segments[count].file_size = list[index].p_filesz;
			segments[count].memory_size = list[index].p_memsz;
			segments[count].offset = list[index].p_offset;
			segments[count].executable = (list[index].p_flags & SEGMENT_EXECUTE) != 0;
			count++;
		}
		return count;
	}
	host_logf(HOST_LOG_ERROR, "the guest image is not an AArch64 executable (this is the native arm64 host)");
#else
	if (ident[4] == ELF_CLASS32 && ((const elf32_header *)file)->e_machine == ELF_MACHINE_X86_64 &&
		((const elf32_header *)file)->e_type == ELF_EXECUTABLE)
	{
		const elf32_header *elf = file;
		const elf32_segment *list = (const elf32_segment *)((const char *)file + elf->e_phoff);

		for (index = 0; index < elf->e_phnum && count < MAXIMUM_SEGMENTS; index++)
		{
			if (list[index].p_type != SEGMENT_LOAD)
				continue;
			segments[count].address = list[index].p_vaddr;
			segments[count].file_size = list[index].p_filesz;
			segments[count].memory_size = list[index].p_memsz;
			segments[count].offset = list[index].p_offset;
			segments[count].executable = (list[index].p_flags & SEGMENT_EXECUTE) != 0;
			count++;
		}
		return count;
	}
	host_logf(HOST_LOG_ERROR, "the guest image is not an x32 executable (this is the x86-64 host)");
#endif
	return -1;
}

int host_load_image(const void *file, size_t size)
{
	struct segment segments[MAXIMUM_SEGMENTS];
	int count = image_segments(file, size, segments);
	size_t page = host_guest_page_size();
	uint64_t low = ~0ULL, high = 0;
	const struct halo_guest_header *header;
	uint64_t *table;
	const char *name;
	uint32_t import_count;
	int index, missing = 0;
	void *host_low;

	if (count <= 0)
		return -1;
	for (index = 0; index < count; index++)
	{
		if (segments[index].address < low)
			low = segments[index].address;
		if (segments[index].address + segments[index].memory_size > high)
			high = segments[index].address + segments[index].memory_size;
	}
	low &= ~(uint64_t)(page - 1);
	high = (high + page - 1) & ~(uint64_t)(page - 1);
	if (low != HALO_GUEST_IMAGE_BASE || high > 0x100000000ULL)
	{
		host_logf(HOST_LOG_ERROR, "the guest image spans %llx-%llx", (unsigned long long)low, (unsigned long long)high);
		return -1;
	}
	if (host_memory_initialize((uint32_t)low, (uint32_t)(high - low)) != 0)
		return -1;
	host_low = GUEST(void *, low);
	if (mmap(host_low, high - low, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != host_low)
		return -1;
	for (index = 0; index < count; index++)
	{
		if (segments[index].offset + segments[index].file_size > size)
			return -1;
		memcpy(GUEST(void *, segments[index].address), (const char *)file + segments[index].offset,
			segments[index].file_size);
	}

	header = GUEST(const struct halo_guest_header *, low);
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		return -1;
	}
	host_image.header = header;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;
	host_profile_load_symbols(file, size);

	table = GUEST(uint64_t *, header->import_table);
	name = GUEST(const char *, header->import_names);
	import_count = *GUEST(const uint32_t *, header->import_count);
	for (index = 0; index < (int)import_count; index++)
	{
		void *function = host_resolve_import(name);

		if (!function && !strncmp(name, "hostgl_", 7))
			function = host_gl_resolve(name + 7);
		if (!function)
		{
			host_logf(HOST_LOG_WARN, "guest import %s is not available", name);
			function = (void *)missing_import;
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx, %u imports (%d unavailable)",
		(unsigned long long)low, (unsigned long long)high, import_count, missing);

	/* code becomes read-only and executable */
	for (index = 0; index < count; index++)
	{
		uint64_t start = segments[index].address & ~(uint64_t)(page - 1);
		uint64_t end = (segments[index].address + segments[index].memory_size + page - 1) & ~(uint64_t)(page - 1);

		if (!segments[index].executable)
			continue;
		mprotect(GUEST(void *, start), end - start, PROT_READ | PROT_EXEC);
#ifdef __aarch64__
		sys_icache_invalidate(GUEST(void *, start), end - start);
#endif
	}
	return 0;
}
