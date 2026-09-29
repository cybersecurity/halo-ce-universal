#ifndef HALO_MACOS_ELF32_FORMAT_H
#define HALO_MACOS_ELF32_FORMAT_H

#include <stdint.h>

#define HALO_ELFCLASS32 1
#define HALO_ELFDATA2LSB 1
#define HALO_EVCURRENT 1
#define HALO_ET_EXEC 2
#define HALO_EM_386 3
#define HALO_PT_LOAD 1
#define HALO_PT_TLS 7
#define HALO_SHT_SYMTAB 2
#define HALO_SHT_STRTAB 3
#define HALO_STT_FUNC 2
#define HALO_STT_OBJECT 1
#define HALO_SHN_UNDEF 0
#define HALO_PF_X 1
#define HALO_PF_W 2
#define HALO_PF_R 4

struct halo_elf32_ehdr
{
	unsigned char ident[16];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint32_t entry;
	uint32_t phoff;
	uint32_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phentsize;
	uint16_t phnum;
	uint16_t shentsize;
	uint16_t shnum;
	uint16_t shstrndx;
};

struct halo_elf32_phdr
{
	uint32_t type;
	uint32_t offset;
	uint32_t vaddr;
	uint32_t paddr;
	uint32_t filesz;
	uint32_t memsz;
	uint32_t flags;
	uint32_t align;
};

struct halo_elf32_shdr
{
	uint32_t name;
	uint32_t type;
	uint32_t flags;
	uint32_t addr;
	uint32_t offset;
	uint32_t size;
	uint32_t link;
	uint32_t info;
	uint32_t addralign;
	uint32_t entsize;
};

struct halo_elf32_sym
{
	uint32_t name;
	uint32_t value;
	uint32_t size;
	unsigned char info;
	unsigned char other;
	uint16_t shndx;
};

#endif
