#ifdef __x86_64__
#define __ASSEMBLY__
#include "asm_define.h"

.text
.globl rpal_access_warpper
.type rpal_access_warpper,@function
.align 16

rpal_access_warpper:
    pushq  %r12
    pushq  %r13
    pushq  %r14
    pushq  %r15
    pushq  %rbx
    pushq  %rbp

    leaq -0x8(%rsp), %rsp
    stmxcsr  (%rsp)
    fnstcw   0x4(%rsp)

    pushq  %rsp         // Save rsp which may be unaligned.
    pushq  (%rsp)       // Save the original value again
    andq   $-16, %rsp   // Align stack to 16bytes - SysV AMD64 ABI.

    movq   %rsp, (%rdi)
    call   rpal_access@plt
retip:
    movq   8(%rsp), %rsp	// Restore the potentially unaligned stack
    ldmxcsr  (%rsp)
    fldcw    0x4(%rsp)
    leaq 0x8(%rsp), %rsp

    popq   %rbp
    popq   %rbx
    popq   %r15
    popq   %r14
    popq   %r13
    popq   %r12
    ret

.size rpal_access_warpper,.-rpal_access_warpper



.globl rpal_get_ret_rip
.type rpal_get_ret_rip, @function
.align 16
rpal_get_ret_rip:
    leaq retip(%rip), %rax
    ret

.size rpal_get_ret_rip,.-rpal_get_ret_rip

/* Mark that we don't need executable stack. */
.section .note.GNU-stack,"",%progbits
#endif
