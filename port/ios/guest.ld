/* Guest offsets remain Xbox-sized; the host maps these above 4 GB. */
ENTRY(__guest_header)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x88000000;
    .guest_header : { KEEP(*(.guest_header)) } :code
    .text : { *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) } :code
    . = ALIGN(0x4000);
    __guest_code_end = .;
    .data : { *(.data .data.*) } :data
    .guest_init : {
        __guest_init_start = .;
        KEEP(*(.guest_init))
        __guest_init_end = .;
    } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    . = ALIGN(0x4000);
    __guest_image_end = .;
    /DISCARD/ : { *(.comment) *(.note.*) *(.guest_fini) *(.eh_frame) }
}
