#include "limine_definitions.hpp"

extern "C" void main();

#define REQUEST __attribute__((used, section(".limine_requests")))

namespace piru::boot::limine {
    REQUEST volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(6);

    REQUEST volatile limine_entry_point_request entry_point = {
        .id = LIMINE_ENTRY_POINT_REQUEST_ID,
        .revision = 0,
        .response = nullptr,
        .entry = main
    };

    REQUEST volatile limine_flanterm_fb_init_params_request flanterm_fb_init_params = {
        .id = LIMINE_FLANTERM_FB_INIT_PARAMS_REQUEST_ID,
        .revision = 0,
        .response = nullptr
    };

    REQUEST volatile limine_framebuffer_request framebuffer_request = {
        .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
        .revision = 0,
        .response = nullptr
    };

    REQUEST volatile limine_date_at_boot_request date_at_boot_request = {
        .id = LIMINE_DATE_AT_BOOT_REQUEST_ID,
        .revision = 0,
        .response = nullptr
    };

    REQUEST volatile limine_hhdm_request hhdm_request = {
        .id = LIMINE_HHDM_REQUEST_ID,
        .revision = 0,
        .response = nullptr
    };

    REQUEST volatile limine_stack_size_request stack_size_request = {
        .id = LIMINE_STACK_SIZE_REQUEST_ID,
        .revision = 0,
        .response = nullptr,
        .stack_size = 0x8000 // 32kib stack size
    };

    REQUEST volatile limine_rsdp_request rsdp_request = {
        .id = LIMINE_RSDP_REQUEST_ID,
        .revision = 0,
        .response = nullptr
    };

    REQUEST volatile limine_mp_request mp_request = {
        .id = LIMINE_MP_REQUEST_ID,
        .revision = 0,
        .response = nullptr,
        .flags = 0
    };

    REQUEST volatile limine_memmap_request memmap_request = {
        .id = LIMINE_MEMMAP_REQUEST_ID,
        .revision = 0,
        .response = nullptr
    };

    __attribute__((used, section(".limine_requests_start")))
    volatile uint64_t requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

    __attribute__((used, section(".limine_requests_end")))
    volatile uint64_t requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;
}