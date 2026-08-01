#ifndef PIRU_HEAP_ALLOCATOR_HPP
#define PIRU_HEAP_ALLOCATOR_HPP

#include "subsystem.hpp"

namespace piru::subsystems {
    struct heap_allocator {
        static constexpr const char* SUBSYSTEM_NAME = "heap_allocator";
        static constexpr bool IS_REQUIRED = true;

        [[=subsystems::subsystem_constructor_tag]] heap_allocator();

        Result Initialize();
    };

    struct dependent_on {
        static constexpr const char* SUBSYSTEM_NAME = "dependent_on";
        static constexpr bool IS_REQUIRED = true;

        [[=subsystems::subsystem_constructor_tag]] dependent_on(heap_allocator& heapAllocator);

        Result Initialize();

    private:
        heap_allocator& _heapAllocator;
    };
}

#endif //PIRU_HEAP_ALLOCATOR_HPP
