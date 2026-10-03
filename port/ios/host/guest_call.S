/* Preserve the host ABI while installing the guest arena register. */
.text
.p2align 2
.globl _host_guest_invoke
_host_guest_invoke:
    stp x27, x30, [sp, #-16]!
    mov x16, x0
    mov x27, x5
    mov w0, w1
    mov w1, w2
    mov w2, w3
    mov w3, w4
    blr x16
    ldp x27, x30, [sp], #16
    ret

/* Run the game on the UI thread, using an arena stack for its 32-bit locals. */
.globl _host_guest_on_stack
_host_guest_on_stack:
    stp x27, x30, [sp, #-32]!
    str x29, [sp, #16]
    mov x9, sp
    mov sp, x3
    stp x9, xzr, [sp, #-16]!
    mov x27, x2
    mov x16, x0
    mov w0, w1
    blr x16
    ldr x9, [sp]
    mov sp, x9
    ldr x29, [sp, #16]
    ldp x27, x30, [sp], #32
    ret
