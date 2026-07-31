OUTPUT_FORMAT(elf64-x86-64)

PHDRS
{
    limine_requests PT_LOAD;
    text PT_LOAD;
    rodata PT_LOAD;
    data PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;
    _kernel_base = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    _kernel_end = .;

    STACK_SIZE = 0x4000; /* 16 KiB stack */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .stack : {
        . = ALIGN(16);
        _kernel_stack_bottom = .;
        . += STACK_SIZE;
        _kernel_stack_top = .;
    } :data

    FAULT_HANDLER_SIZE = 0x2000;
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .fault_handler_stack : {
        . = ALIGN(16);
        _fault_handler_stack_bottom = .;
        . += FAULT_HANDLER_SIZE;
        _fault_handler_stack_top = .;
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}