#include "framebuffer_console.hpp"

#include "logging.hpp"
#include "boot/limine_definitions.hpp"
#include "flanterm_backends/fb.h"

namespace piru::io {
    void framebuffer_console::Initialize() {
        auto fbResponse = boot::limine::framebuffer_request.response;
        if (!fbResponse || fbResponse->framebuffer_count == 0)
            PANIC("No valid framebuffer available");

        _framebuffer = fbResponse->framebuffers[0];

        auto fbInit = boot::limine::flanterm_fb_init_params.response;
        if (!fbInit || fbInit->entry_count == 0)
            PANIC("No valid flanterm framebuffer init params available");

        auto entry = fbInit->entries[0];
        _flantermContext = flanterm_fb_init(
            nullptr,
            nullptr,
            reinterpret_cast<uint32_t *>(_framebuffer->address),
            _framebuffer->width,
            _framebuffer->height,
            _framebuffer->pitch,
            _framebuffer->red_mask_size,
            _framebuffer->red_mask_shift,
            _framebuffer->green_mask_size,
            _framebuffer->green_mask_shift,
            _framebuffer->blue_mask_size,
            _framebuffer->blue_mask_shift,
            entry->canvas,
            entry->ansi_colours,
            entry->ansi_bright_colours,
            &entry->default_bg,
            &entry->default_fg,
            &entry->default_bg_bright,
            &entry->default_fg_bright,
            entry->font,
            entry->font_width,
            entry->font_height,
            entry->font_spacing,
            entry->font_scale_x,
            entry->font_scale_y,
            entry->margin,
            entry->rotation,
            true
        );

        if (!_flantermContext)
            PANIC("Failed to initialize flanterm framebuffer context");

        _initialized = true;

        Write(ANSI::EscapeCodes::ClearScreen);
        Write("Framebuffer console initialized\n\r");
    }

    void framebuffer_console::Write(const uint8_t *data, size_t count) {
        if (!_initialized)
            return;
        threading::spinlock::scoped_lock lock(_lock, true);
        flanterm_write(_flantermContext, (const char*)data, count);
    }

    void framebuffer_console::Write(string_view data) {
        Write(reinterpret_cast<const uint8_t *>(data.Data()), data.Size());
    }
} // piru::io