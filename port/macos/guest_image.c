#include "guest_image.h"

#include "guest_address.h"
#include "elf32_format.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define GUEST_SPACE_SIZE UINT64_C(0x100000000)
#define IMPORT_BASE UINT32_C(0x70000000)

static int fail(char *out, size_t cap, const char *message)
{
	if (out && cap)
		snprintf(out, cap, "%s", message);
	return -1;
}

static int bytes_fit(size_t total, uint64_t offset, uint64_t length)
{
	return offset <= total && length <= (uint64_t)total - offset;
}

static int guest_span(uint32_t address, uint32_t length)
{
	return (uint64_t)address + length <= GUEST_SPACE_SIZE;
}

int mac_guest_image_load(const void *elf_bytes, size_t elf_size,
	struct mac_guest_image *image_out, char *error_out, size_t error_capacity)
{
	const unsigned char *bytes = elf_bytes;
	const struct halo_elf32_ehdr *header;
	const struct halo_elf32_phdr *programs;
	void *arena;
	uint32_t low = UINT32_MAX, high = 0;
	uint32_t code_low = UINT32_MAX, code_high = 0;
	uint32_t guest_cursor;
	uint32_t tls_va = 0;
	size_t tls_file = 0, tls_memory = 0, tls_alignment = 1;
	size_t page_size;
	int map_protection;

	if (!bytes || !image_out || elf_size < sizeof(struct halo_elf32_ehdr))
		return fail(error_out, error_capacity, "truncated ELF header");
	header = (const struct halo_elf32_ehdr *)bytes;
	if (memcmp(header->ident, "\177ELF", 4) != 0 ||
		header->ident[4] != HALO_ELFCLASS32 ||
		header->ident[5] != HALO_ELFDATA2LSB ||
		header->machine != HALO_EM_386 || header->type != HALO_ET_EXEC ||
		header->phentsize != sizeof(struct halo_elf32_phdr) || header->phnum == 0)
		return fail(error_out, error_capacity, "expected little-endian ELF32/i386 ET_EXEC");
	if (!bytes_fit(elf_size, header->phoff,
		(uint64_t)header->phnum * sizeof(struct halo_elf32_phdr)))
		return fail(error_out, error_capacity, "program header table outside ELF");
	programs = (const struct halo_elf32_phdr *)(bytes + header->phoff);
	for (unsigned i = 0; i < header->phnum; ++i)
	{
		const struct halo_elf32_phdr *ph = &programs[i];
		uint64_t end;
		if (ph->type == HALO_PT_TLS)
		{
			if (tls_memory || ph->filesz > ph->memsz ||
				!bytes_fit(elf_size, ph->offset, ph->filesz))
				return fail(error_out, error_capacity, "invalid PT_TLS segment");
			tls_va = ph->vaddr;
			tls_file = ph->filesz;
			tls_memory = ph->memsz;
			tls_alignment = ph->align ? ph->align : 1;
			continue;
		}
		if (ph->type != HALO_PT_LOAD || !ph->memsz)
			continue;
		if (ph->filesz > ph->memsz ||
			!bytes_fit(elf_size, ph->offset, ph->filesz) ||
			!guest_span(ph->vaddr, ph->memsz))
			return fail(error_out, error_capacity, "invalid PT_LOAD span");
		end = (uint64_t)ph->vaddr + ph->memsz;
		/* Import trampolines are intercepted by address before guest memory. */
		if (ph->vaddr >= IMPORT_BASE)
			continue;
		if (ph->vaddr < low) low = ph->vaddr;
		if (end > high) high = (uint32_t)end;
		if (ph->flags & HALO_PF_X)
		{
			if (ph->vaddr < code_low) code_low = ph->vaddr;
			if (end > code_high) code_high = (uint32_t)end;
		}
	}
	if (low == UINT32_MAX || high <= low || code_low == UINT32_MAX ||
		code_high <= code_low || header->entry < code_low || header->entry >= code_high)
		return fail(error_out, error_capacity, "ELF has no loadable entry image");

	/* All ELF segment pages start writable for relocation-free copy/zero fill.
	 * Guest code is mechanically translated, so native execution never branches
	 * into this data arena. */
	arena = mac_guest_address_reserve(0, (size_t)GUEST_SPACE_SIZE);
	if (!arena)
		return fail(error_out, error_capacity, "unable to reserve 4 GiB guest VA arena");
	if (mac_guest_address_commit(low, (size_t)(high - low),
		PROT_READ | PROT_WRITE) != 0)
	{
		mac_guest_address_reset();
		return fail(error_out, error_capacity, "unable to commit guest ELF image pages");
	}
	for (unsigned i = 0; i < header->phnum; ++i)
	{
		const struct halo_elf32_phdr *ph = &programs[i];
		void *destination;
		if (ph->type != HALO_PT_LOAD || !ph->memsz || ph->vaddr >= IMPORT_BASE)
			continue;
		destination = mac_guest_address_resolve(ph->vaddr, ph->memsz);
		if (!destination)
		{
			mac_guest_address_reset();
			return fail(error_out, error_capacity, "PT_LOAD resolves outside guest arena");
		}
		memcpy(destination, bytes + ph->offset, ph->filesz);
		if (ph->memsz > ph->filesz)
			memset((unsigned char *)destination + ph->filesz, 0,
				ph->memsz - ph->filesz);
	}
	/* Restore a conservative image-wide protection derived from segment flags.
	 * The source image contains code and mutable globals in nearby segments, and
	 * Darwin rounds mprotect to 16 KiB pages; therefore use the union. */
	map_protection = 0;
	for (unsigned i = 0; i < header->phnum; ++i)
	{
		const struct halo_elf32_phdr *ph = &programs[i];
		if (ph->type != HALO_PT_LOAD || !ph->memsz || ph->vaddr >= IMPORT_BASE)
			continue;
		if (ph->flags & (HALO_PF_R | HALO_PF_X)) map_protection |= PROT_READ;
		if (ph->flags & HALO_PF_W) map_protection |= PROT_WRITE;
		/* Guest code bytes are data to the AOT host. Do not request native
		 * executable arena pages; this also avoids macOS W^X/JIT entitlements. */
	}
	page_size = mac_guest_address_host_page_size();
	guest_cursor = low & ~((uint32_t)page_size - 1u);
	if (!page_size || mac_guest_address_protect(guest_cursor,
		(size_t)(((uint64_t)high + page_size - 1) / page_size * page_size - guest_cursor),
		map_protection) != 0)
	{
		mac_guest_address_reset();
		return fail(error_out, error_capacity, "unable to finalize guest image protection");
	}
	if (mac_guest_address_linear_offset(0, arena, &image_out->memory_offset) != 0)
	{
		mac_guest_address_reset();
		return fail(error_out, error_capacity, "guest arena has no linear memory offset");
	}
	image_out->arena_host_base = arena;
	image_out->entry_va = header->entry;
	image_out->code_lo_va = code_low;
	image_out->code_hi_va = code_high;
	image_out->image_end_va = high;
	image_out->tls_image_va = tls_va;
	image_out->tls_file_size = tls_file;
	image_out->tls_memory_size = tls_memory;
	image_out->tls_alignment = tls_alignment;
	if (error_out && error_capacity) error_out[0] = '\0';
	return 0;
}

static int find_symbol_of_type(const void *elf_bytes, size_t elf_size,
	const char *name, uint32_t *guest_va_out, unsigned type, uint32_t object_size)
{
	const unsigned char *bytes = elf_bytes;
	const struct halo_elf32_ehdr *header = elf_bytes;
	const struct halo_elf32_shdr *sections;
	uint32_t found_va = 0;
	int found = 0;
	if (!bytes || !name || !guest_va_out || elf_size < sizeof(*header) ||
		memcmp(header->ident, "\177ELF", 4) != 0 ||
		header->ident[4] != HALO_ELFCLASS32 ||
		header->shentsize != sizeof(struct halo_elf32_shdr) || !header->shnum ||
		!bytes_fit(elf_size, header->shoff,
			(uint64_t)header->shnum * sizeof(struct halo_elf32_shdr)))
		return -1;
	sections = (const struct halo_elf32_shdr *)(bytes + header->shoff);
	for (unsigned section_index = 0; section_index < header->shnum; ++section_index)
	{
		const struct halo_elf32_shdr *symbols = &sections[section_index];
		const struct halo_elf32_shdr *strings;
		const struct halo_elf32_sym *entries;
		if (symbols->type != HALO_SHT_SYMTAB ||
			symbols->entsize != sizeof(struct halo_elf32_sym) ||
			symbols->link >= header->shnum ||
			!bytes_fit(elf_size, symbols->offset, symbols->size))
			continue;
		strings = &sections[symbols->link];
		if (strings->type != HALO_SHT_STRTAB ||
			!bytes_fit(elf_size, strings->offset, strings->size))
			continue;
		entries = (const struct halo_elf32_sym *)(bytes + symbols->offset);
		for (size_t index = 0; index < symbols->size / sizeof(*entries); ++index)
		{
			const struct halo_elf32_sym *entry = &entries[index];
			const char *symbol_name;
			size_t remaining;
			if (entry->shndx == HALO_SHN_UNDEF ||
				(entry->info & 0x0f) != type || entry->name >= strings->size)
				continue;
			symbol_name = (const char *)(bytes + strings->offset + entry->name);
			remaining = strings->size - entry->name;
			if (memchr(symbol_name, '\0', remaining) && strcmp(symbol_name, name) == 0)
			{
				if (type == HALO_STT_OBJECT)
				{
					int in_load_segment = 0;
					if (header->phentsize == sizeof(struct halo_elf32_phdr) &&
						bytes_fit(elf_size, header->phoff, (uint64_t)header->phnum * sizeof(struct halo_elf32_phdr)))
					{
						const struct halo_elf32_phdr *segments = (const void *)(bytes + header->phoff);
						for (unsigned p = 0; p < header->phnum; ++p)
							if (segments[p].type == HALO_PT_LOAD && (segments[p].flags & HALO_PF_R) &&
								entry->value >= segments[p].vaddr &&
								(uint64_t)entry->value + object_size <= (uint64_t)segments[p].vaddr + segments[p].memsz)
								in_load_segment = 1;
					}
					if (found || !object_size || entry->size != object_size ||
						entry->shndx >= header->shnum || !entry->value ||
						entry->value >= IMPORT_BASE || !in_load_segment ||
						!mac_guest_address_resolve(entry->value, object_size))
						return -1;
					found = 1;
					found_va = entry->value;
					continue;
				}
				*guest_va_out = entry->value;
				return 0;
			}
		}
	}
	if (found) { *guest_va_out = found_va; return 0; }
	return -1;
}

int mac_guest_image_find_symbol(const void *elf_bytes, size_t elf_size,
	const char *name, uint32_t *guest_va_out)
{
	return find_symbol_of_type(elf_bytes, elf_size, name, guest_va_out, HALO_STT_FUNC, 0);
}

int mac_guest_image_find_object(const void *elf_bytes, size_t elf_size,
	const char *name, uint32_t object_size, uint32_t *guest_va_out)
{
	return find_symbol_of_type(elf_bytes, elf_size, name, guest_va_out, HALO_STT_OBJECT, object_size);
}
