#ifdef __x86_64__
#define __ASSEMBLY__
#include "asm_define.h"
#define RPAL_TASK_DONE $0x0
#define RPAL_TASK_BLOCKED $0x1

.text
.globl rpal_ret_critical
.type rpal_ret_critical,@function
.align 16

//void rpal_ret_critical(volatile rpal_epoll_context_t *ectx, rpal_call_info_t *rci)

rpal_ret_critical:
    mov     RPAL_TASK_BLOCKED, %eax
    mov     RPAL_TASK_DONE, %ecx
    lock cmpxchg   %ecx, ECTX_G_STATUS(%rdi)
ret_begin:
    jne 2f
    movq    ECTX_PKRU(%rdi), %rax
    xor     %edx, %edx
    .byte 0x0f,0x01,0xef
    movq    RCI_APP_TLS_BASE(%rsi), %rax
    wrfsbase %rax
ret_end:
    movq    RCI_APP_FCTX(%rsi), %rdi
    call    jump_fcontext@plt
2:
    ret

.globl rpal_get_critical_addr
.type rpal_get_critical_addr,@function
.align 16
rpal_get_critical_addr:
    leaq    ret_begin(%rip), %rax
    movq    %rax, RET_BEGIN(%rdi)
    leaq    ret_end(%rip), %rax
    movq    %rax, RET_END(%rdi)
    ret

.size rpal_ret_critical,.-rpal_ret_critical

/* Mark that we don't need executable stack. */
.section .note.GNU-stack,"",%progbits
#endif
