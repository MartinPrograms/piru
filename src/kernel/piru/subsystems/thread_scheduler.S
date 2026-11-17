#include "interrupts/macros.S"

// Functions defined by this file
.global ThreadTrampoline
.type ThreadTrampoline, @function // void ThreadTrampoline()

.global SwitchContext
.type SwitchContext, @function // void SwitchContext(uint64_t* prev, uint64_t next)
// End of locally defined functions

// External functions
.global ThreadStart
.type ThreadStart, @function // void ThreadStart
// End of external functions

.text

ThreadTrampoline:
    mov rdi, r12
    mov rsi, r13
    and rsp, -16
    call ThreadStart // C++ [[noreturn]]
    ud2

SwitchContext:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret