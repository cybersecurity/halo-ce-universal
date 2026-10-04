/* the guest image (tools/linux_arm64_build.py), inside the host executable;
host_main.c loads it */

	.section .rodata.halo_guest_image, "a"
	.balign 16
	.globl halo_guest_image_start
	.globl halo_guest_image_end
halo_guest_image_start:
	.incbin GUEST_IMAGE
halo_guest_image_end:

	.section .note.GNU-stack, "", %progbits
