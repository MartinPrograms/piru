#include "interrupts/macros.S"

// Functions defined by this file
.global ThreadTrampoline
.type ThreadTrampoline, @function

.global stub_ISRStub_252
.type stub_ISRStub_252, @function
// End of locally defined functions

// External functions
.global ThreadExit
.type ThreadExit, @function

.global HandleIrq
.type HandleIrq, @function
// End of external functions

.text

ThreadTrampoline:
    mov rdi, r13
    call r12
    call ThreadExit
.LHang:
    cli
    hlt
    jmp .LHang

// Threading timer interrupt
stub_ISRStub_252:
    SAVE_REGS

    mov rdi, rsp
    call HandleIrq
    mov rsp, rax

    RESTORE_REGS
    iretq