#include "heap_allocator.hpp"

#include "logging.hpp"

namespace piru::subsystems {
    heap_allocator::heap_allocator() {
        LOG_DEBUG("heap_allocator constructor called, with this at {}", (void*)this);
    }

    Result heap_allocator::Initialize() {
        LOG_DEBUG("heap_allocator Initialize called");
        return Result::SUCCESS;
    }

    dependent_on::dependent_on(heap_allocator &heapAllocator) : _heapAllocator(heapAllocator) {
        LOG_DEBUG("dependent_on constructor called, with this at {}, heapAllocator at {}", (void*)this, (void*)&heapAllocator);
    }

    Result dependent_on::Initialize() {
        LOG_DEBUG("dependent_on Initialize called, heapAllocator at {}", (void*)&_heapAllocator);
        return Result::NOT_IMPLEMENTED;
    }
}
