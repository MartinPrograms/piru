#ifndef PIRU_LIMINE_DEFINITIONS_HPP
#define PIRU_LIMINE_DEFINITIONS_HPP

#include "limine.h"

namespace piru::boot::limine {
    extern volatile uint64_t base_revision[];
    extern volatile limine_entry_point_request entry_point;
    extern volatile limine_flanterm_fb_init_params_request flanterm_fb_init_params;
    extern volatile limine_framebuffer_request framebuffer_request;
    extern volatile limine_date_at_boot_request date_at_boot_request;
    extern volatile limine_hhdm_request hhdm_request;
    extern volatile limine_stack_size_request stack_size_request;
    extern volatile limine_rsdp_request rsdp_request;
    extern volatile limine_mp_request mp_request;
    extern volatile limine_memmap_request memmap_request;
}

#endif //PIRU_LIMINE_DEFINITIONS_HPP
