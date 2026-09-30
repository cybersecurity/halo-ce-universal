/*
GUEST.LD

Layout of the macOS guest image (tools/macos_build.py): linked to run at
0x20000000 (HALO_GUEST_IMAGE_BASE there), below 2 GB so x86-64 code can
address it with sign-extended 32-bit immediates, with the image header
first. The host copies the loadable segments to these addresses and makes
the code read-only (port/macos/host/host_loader.c). The constructors
(.init_array) become the header's init array, which __guest_start runs.
*/

ENTRY(__guest_header)

SECTIONS
{
	. = 0x20000000;
	.guest_header : { KEEP(*(.guest_header)) }
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(0x10000);
	.data : { *(.data .data.*) }
	.guest_init : {
		__guest_init_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array .ctors .ctors.*))
		__guest_init_end = .;
	}
	.bss : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(0x1000);
	__guest_image_end = .;
	/DISCARD/ : { *(.comment) *(.note.*) *(.fini_array*) *(.dtors*) *(.eh_frame) }
}
