.intel_syntax noprefix

/* Save all general-purpose registers (16 registers = 128 bytes) */
.macro SAVE_REGS
    push    r15
    push    r14
    push    r13
    push    r12
    push    r11
    push    r10
    push    r9
    push    r8
    push    rbp
    push    rdi
    push    rsi
    push    rdx
    push    rcx
    push    rbx
    push    rax
    push    rax /* Padding slot to keep 16-register size balance */
.endm

/* Restore registers in reverse order */
.macro RESTORE_REGS
    pop     rax
    pop     rax
    pop     rbx
    pop     rcx
    pop     rdx
    pop     rsi
    pop     rdi
    pop     rbp
    pop     r8
    pop     r9
    pop     r10
    pop     r11
    pop     r12
    pop     r13
    pop     r14
    pop     r15
.endm

#define SIZE_OF_REGS 128

.macro mac_IRQStub vector
.global stub_ISRStub_\vector
.type stub_ISRStub_\vector, @function
stub_ISRStub_\vector:
    push    0                   // dummy error code
    SAVE_REGS

    mov     rdi, \vector
    mov     rsi, [rsp + SIZE_OF_REGS]
    mov     rdx, rsp

    mov     rbx, rsp
    and     rsp, -16
    call    IRQHandler
    mov     rsp, rbx

    RESTORE_REGS
    add     rsp, 8              // remove dummy error code
    iretq
.endm

.macro mac_ISRErrStub vector
.global stub_ISRStub_\vector
.type stub_ISRStub_\vector, @function
stub_ISRStub_\vector:
    SAVE_REGS

    mov     rdi, \vector
    mov     rsi, [rsp + SIZE_OF_REGS]
    mov     rdx, rsp

    mov     rbx, rsp
    and     rsp, -16
    call    ExceptionHandler
    mov     rsp, rbx

    RESTORE_REGS
    add     rsp, 8              // remove hardware error code
    iretq
.endm

.macro mac_ISRNoErrStub vector
.global stub_ISRStub_\vector
.type stub_ISRStub_\vector, @function
stub_ISRStub_\vector:
    push    0                   // dummy error code
    SAVE_REGS

    mov     rdi, \vector
    mov     rsi, [rsp + SIZE_OF_REGS]
    mov     rdx, rsp

    mov     rbx, rsp
    and     rsp, -16
    call    ExceptionHandler
    mov     rsp, rbx

    RESTORE_REGS
    add     rsp, 8              // remove dummy error code
    iretq
.endm