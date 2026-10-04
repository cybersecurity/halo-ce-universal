.text
.globl _macos_enter_guest_stack
.p2align 2
_macos_enter_guest_stack:
 mov sp, x0
 mov w0, w1
 bl _host_run_guest_main
 brk #0
