#include "../guest_address.h"
#include "../guest_image.h"
#include "../elf32_format.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

union fixture
{
	max_align_t align;
	unsigned char bytes[512];
};

int main(void)
{
	union fixture fixture = { 0 };
	struct halo_elf32_ehdr *eh = (struct halo_elf32_ehdr *)fixture.bytes;
	struct halo_elf32_phdr *ph;
	struct halo_elf32_shdr *sh;
	struct halo_elf32_sym *sym;
	struct mac_guest_image image;
	char error[160];
	const unsigned char file_data[] = { 0x90, 0xC3, 0x12, 0x34 };
	unsigned char *loaded;

	memcpy(eh->ident, "\177ELF", 4);
	eh->ident[4] = HALO_ELFCLASS32;
	eh->ident[5] = HALO_ELFDATA2LSB;
	eh->ident[6] = HALO_EVCURRENT;
	eh->type = HALO_ET_EXEC;
	eh->machine = HALO_EM_386;
	eh->version = HALO_EVCURRENT;
	eh->entry = 0x10000;
	eh->phoff = sizeof(*eh);
	eh->ehsize = sizeof(*eh);
	eh->phentsize = sizeof(struct halo_elf32_phdr);
	eh->phnum = 2;
	eh->shoff = 280;
	eh->shentsize = sizeof(struct halo_elf32_shdr);
	eh->shnum = 3;
	ph = (struct halo_elf32_phdr *)(fixture.bytes + eh->phoff);
	ph[0].type = HALO_PT_LOAD;
	ph[0].offset = 256;
	ph[0].vaddr = 0x10000;
	ph[0].filesz = sizeof(file_data);
	ph[0].memsz = 64;
	ph[0].flags = HALO_PF_R | HALO_PF_X;
	ph[0].align = 4096;
	ph[1].type = HALO_PT_TLS;
	ph[1].offset = 260;
	ph[1].vaddr = 0x10004;
	ph[1].filesz = 2;
	ph[1].memsz = 8;
	ph[1].align = 4;
	sh = (struct halo_elf32_shdr *)(fixture.bytes + eh->shoff);
	sh[1].type = HALO_SHT_SYMTAB;
	sh[1].offset = 400;
	sh[1].size = 2 * sizeof(struct halo_elf32_sym);
	sh[1].link = 2;
	sh[1].entsize = sizeof(struct halo_elf32_sym);
	sh[2].type = HALO_SHT_STRTAB;
	sh[2].offset = 440;
	sh[2].size = 16;
	sym = (struct halo_elf32_sym *)(fixture.bytes + sh[1].offset);
	sym[1].name = 1;
	sym[1].value = 0x10000;
	sym[1].size = 64;
	sym[1].info = HALO_STT_FUNC;
	sym[1].shndx = 1;
	memcpy(fixture.bytes + sh[2].offset, "\0entry\0unused\0", 14);
	memcpy(fixture.bytes + 256, file_data, sizeof(file_data));

	if (mac_guest_image_load(fixture.bytes, sizeof(fixture.bytes),
		&image, error, sizeof(error)) != 0)
	{
		fprintf(stderr, "load failed: %s\n", error);
		return 1;
	}
	assert(image.entry_va == 0x10000);
	assert(image.code_lo_va == 0x10000 && image.code_hi_va == 0x10040);
	assert(image.image_end_va == 0x10040);
	assert(image.tls_image_va == 0x10004);
	assert(image.tls_file_size == 2 && image.tls_memory_size == 8);
	uint32_t symbol_va = 0;
	assert(mac_guest_image_find_symbol(fixture.bytes, sizeof(fixture.bytes),
		"entry", &symbol_va) == 0 && symbol_va == 0x10000);
	assert(mac_guest_image_find_symbol(fixture.bytes, sizeof(fixture.bytes),
		"missing", &symbol_va) != 0);
	assert(mac_guest_image_find_object(fixture.bytes, sizeof(fixture.bytes), "entry", 4, &symbol_va) != 0);
	sym[1].info = HALO_STT_OBJECT;
	sym[1].size = 4;
	assert(mac_guest_image_find_object(fixture.bytes, sizeof(fixture.bytes), "entry", 4, &symbol_va) == 0 && symbol_va == 0x10000);
	assert(mac_guest_image_find_symbol(fixture.bytes, sizeof(fixture.bytes), "entry", &symbol_va) != 0);
	assert(mac_guest_image_find_object(fixture.bytes, sizeof(fixture.bytes), "entry", 8, &symbol_va) != 0);
	sym[1].value = 0x9000;
	assert(mac_guest_image_find_object(fixture.bytes, sizeof(fixture.bytes), "entry", 4, &symbol_va) != 0);
	sym[1].value = 0x10000;
	sym[0] = sym[1];
	assert(mac_guest_image_find_object(fixture.bytes, sizeof(fixture.bytes), "entry", 4, &symbol_va) != 0);
	memset(&sym[0], 0, sizeof(sym[0]));
	loaded = mac_guest_address_resolve(0x10000, 64);
	assert(loaded);
	assert(memcmp(loaded, file_data, sizeof(file_data)) == 0);
	for (size_t i = sizeof(file_data); i < 64; ++i)
		assert(loaded[i] == 0);
	assert(image.memory_offset > 0);
	mac_guest_address_reset();

	eh->entry = 0x9000;
	assert(mac_guest_image_load(fixture.bytes, sizeof(fixture.bytes),
		&image, error, sizeof(error)) != 0);
	assert(strstr(error, "entry image") != NULL);
	puts("ELF32 guest image loader: PASS");
	return 0;
}
