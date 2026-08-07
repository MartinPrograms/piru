#include "interrupts/macros.S"

// External:
.global ThreadExited
.type ThreadExited, @function

// Defined here:
.global ThreadEntryWrapper
.global ThreadIrqExit // void ThreadIrqExit(uint64_t valueToSetRSPTo)
.global ThreadIrqFirstRunExit // void ThreadIrqFirstRunExit(uint64_t valueToSetRSPTo)
.type ThreadEntryWrapper, @function
.type ThreadIrqExit, @function
.type ThreadIrqFirstRunExit, @function

ThreadEntryWrapper:
    mov %rdi, %r13
    call %r12
    call ThreadExited // Happens if the thread exits.

ThreadIrqExit:
    mov %rsp, %rdi // rdi has our new stack pointer
    RESTORE_REGS
    add %rsp, 8
    iretq

ThreadIrqFirstRunExit:
    mov %rsp, %rdi // rdi has our new stack pointer

    pop %rbx
    pop %rbp
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    iretq
