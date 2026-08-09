#ifndef PIRU_FRAMEBUFFER_HPP
#define PIRU_FRAMEBUFFER_HPP

#include <definitions.hpp>

#include <flanterm.h>
#include "boot/limine.h"
#include "cpp/spinlock.hpp"
#include "cpp/string_view.hpp"

namespace piru::io {
    class framebuffer_console {
    public:
        void Initialize();
        void Write(const uint8_t* data, size_t count);
        void Write(string_view data);

        [[nodiscard]] limine_framebuffer* GetFramebuffer() {
            return _framebuffer;
        }

    private:
        threading::spinlock _lock{};
        limine_framebuffer *_framebuffer{};
        flanterm_context *_flantermContext{};
        bool _initialized{false};
    };
}

#endif //PIRU_FRAMEBUFFER_HPP
