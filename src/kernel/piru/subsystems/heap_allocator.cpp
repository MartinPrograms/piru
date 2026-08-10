#include "heap_allocator.hpp"
#include "heap_allocator.hpp"

#include "kernel.hpp"
#include "memory/physical_memory_manager.hpp"
#include <new>

#include "logging.hpp"

namespace {
    piru::subsystems::heap_allocator::state *_cppNewState = nullptr;

    struct AllocationHeader {
        size_t totalSize;
        uint32_t magic;
    };

    static constexpr uint32_t ALLOC_MAGIC = 0xDEADC0DE;
    static constexpr size_t HEADER_SIZE = (sizeof(AllocationHeader) + 15) & ~15; // Aligned to 16 bytes.

    uintptr_t SizedNewAllocate(size_t bytes) {
        size_t actualBytes = bytes + HEADER_SIZE;

        uintptr_t raw = (uintptr_t)_cppNewState->Allocate(actualBytes, 16);
        if (!raw)
            return 0;

        auto *header = reinterpret_cast<AllocationHeader*>(raw);
        header->totalSize = actualBytes;
        header->magic = ALLOC_MAGIC;
        return (uintptr_t)(raw + HEADER_SIZE);
    }

    void FreeWithHeader(void* ptr) {
        if (!ptr) return;

        uintptr_t rawAddr = reinterpret_cast<uintptr_t>(ptr) - HEADER_SIZE;
        auto *header = reinterpret_cast<AllocationHeader*>(rawAddr);
        if (header->magic != ALLOC_MAGIC)
            PANIC("Invalid magic number in allocation header. Possible double free or memory corruption.");

        header->magic = 0; // Invalidate the magic number to prevent double free.
        _cppNewState->Free((void*)rawAddr, header->totalSize);
    }
}

void* operator new (size_t size){
    return reinterpret_cast<void*>(SizedNewAllocate(size));
}

void* operator new[](size_t size){
    return reinterpret_cast<void*>(SizedNewAllocate(size));
}

void* operator new (size_t size, std::align_val_t alignment) {
    return reinterpret_cast<void*>(SizedNewAllocate(size));
}

void operator delete(void* ptr, size_t size) noexcept {
    FreeWithHeader(ptr);
}

void operator delete(void* ptr, std::align_val_t alignment) noexcept {
    FreeWithHeader(ptr);
}

void operator delete(void* ptr) noexcept {
    FreeWithHeader(ptr);
}

void operator delete[](void* ptr) noexcept {
    FreeWithHeader(ptr);
}

void operator delete[](void* ptr, size_t size) noexcept {
    FreeWithHeader(ptr);
}

namespace piru::subsystems {
    heap_allocator::slab_allocator::slab_allocator() {
        const auto pageState = piru::kernel::GetInstance().GetCpu().pageState;
        const auto pmm = &piru::kernel::GetInstance().data.pmm;
        const auto paging = &piru::kernel::GetInstance().data.paging;

        auto page = pmm->Allocate(1);
        auto virt = paging->FindAvailableVirtualAddressRangeKernel(1);
        paging->MapPage(pageState, (uint64_t)virt.start, (uint64_t)page, memory::paging::Flags::ReadWrite | memory::paging::Flags::NoExecute);

        _memory = (slab*)virt.start;
        _memory->addr = (void*)virt.start; // we allocate this slab on the same memory we used for this.
        _memory->sizeClass = math::PowerOf2(sizeof(slab));
        _memory->maxElementCount = (system::PageSize / _memory->sizeClass);
        _memory->allocationMap[0] = 0b1; // we allocate the first one, since that is this instance.
        _memory->next = nullptr;
    }

    heap_allocator::slab_allocator::~slab_allocator() {
        const auto pageState = piru::kernel::GetInstance().GetCpu().pageState;
        const auto pmm = &piru::kernel::GetInstance().data.pmm;
        const auto paging = &piru::kernel::GetInstance().data.paging;

        auto current = _memory->next;
        while (current) {
            auto page = (void*)paging->GetPhysicalAddress(pageState, (uint64_t)current);
            paging->UnmapPage(pageState, (uint64_t)current);
            pmm->Free(page, 1);

            current->addr = nullptr;

            current->allocationMap[0] = UINT64_MAX;
            current->allocationMap[1] = UINT64_MAX;
            current->allocationMap[2] = UINT64_MAX;
            current->allocationMap[3] = UINT64_MAX;

            current->sizeClass = 0;
            current->maxElementCount = 0;
            current->next = nullptr;

            current = current->next;
        }
    }

    memory::paging::available_virtual_address_range heap_allocator::slab_allocator::GetVirtualAddress(memory::paging::state *pageState, bool higherHalf, memory::paging * const paging, size_t pageCount) {
        memory::paging::available_virtual_address_range virt{};
        if (higherHalf)
            virt = paging->FindAvailableVirtualAddressRangeKernel(pageCount);
        else
            virt = paging->FindAvailableVirtualAddressRange(pageState, 1 * constants::MiB, system::EndOfCanonical, pageCount);
        return virt;
    }

    heap_allocator::slab *heap_allocator::slab_allocator::AllocateSlab(size_t sizeClass, memory::paging::state *pageState, bool higherHalf) {
        const auto pmm = &piru::kernel::GetInstance().data.pmm;
        const auto paging = &piru::kernel::GetInstance().data.paging;

        auto page = (void*)pmm->Allocate(1);
        memory::paging::available_virtual_address_range virt = GetVirtualAddress(pageState, higherHalf, paging, 1);

        paging->MapPage(pageState, (uint64_t)virt.start, (uint64_t)page, memory::paging::Flags::ReadWrite | memory::paging::Flags::NoExecute);

        auto current = _memory;
        while (current->IsFull()) {
            if (current->next)
                current = current->next;

            // This one remains on the kernel address range, since the slab headers, are kernel data.
            auto newPage = pmm->Allocate(1);
            auto newVirt = paging->FindAvailableVirtualAddressRangeKernel(1);
            paging->MapPage(pageState, (uint64_t)virt.start, (uint64_t)newPage, memory::paging::Flags::ReadWrite | memory::paging::Flags::NoExecute);

            auto newSlab = (slab*)newVirt.start;
            newSlab->addr = (void*)newVirt.start;
            newSlab->sizeClass = math::PowerOf2(sizeof(slab));
            newSlab->maxElementCount = (system::PageSize / newSlab->sizeClass);
            newSlab->allocationMap[0] = 0b1; // we allocate the first one, since that is this instance.
            newSlab->next = nullptr;
            current->next = newSlab;
            current = newSlab;
        }

        auto s = (slab*)current->Allocate();
        s->addr = (void*)virt.start;

        s->allocationMap[0] = 0; // mark all as free
        s->allocationMap[1] = 0;
        s->allocationMap[2] = 0;
        s->allocationMap[3] = 0;

        s->sizeClass = sizeClass;
        s->maxElementCount = (system::PageSize / sizeClass);
        s->next = nullptr;

        return s;
    }

    void heap_allocator::slab_allocator::FreeSlab(slab *slab, memory::paging::state *pageState) {
        const auto pmm = &piru::kernel::GetInstance().data.pmm;
        const auto paging = &piru::kernel::GetInstance().data.paging;

        auto phys = paging->GetPhysicalAddress(pageState, (uint64_t)slab->addr);
        paging->UnmapPage(pageState, (uint64_t)slab->addr);
        pmm->Free((void*)phys, 1);

        slab->addr = nullptr;

        slab->allocationMap[0] = UINT64_MAX;
        slab->allocationMap[1] = UINT64_MAX;
        slab->allocationMap[2] = UINT64_MAX;
        slab->allocationMap[3] = UINT64_MAX;

        slab->sizeClass = 0;
        slab->maxElementCount = 0;
        slab->next = nullptr;

        auto current = _memory;
        while (!current->Has(slab)) {
            threading::spinlock::scoped_lock lock(_lock, true);
            current->Give(slab);
        }
    }
} // slab_allocator

namespace piru::subsystems {
    static size_t BucketFromSize(size_t size, size_t alignment) {
        auto realSize = math::AlignUp(size, alignment); // if we need 8 bytes, aligned to 32, we just need a 32 byte allocation!

        if (realSize > 1024)
            return 7;
        if (realSize > 512)
            return 6;
        if (realSize > 256)
            return 5;
        if (realSize > 128)
            return 4;
        if (realSize > 64)
            return 3;
        if (realSize > 32)
            return 2;
        if (realSize > 16)
            return 1;

        return 0;
    }

    void* heap_allocator::state::Allocate(size_t size, size_t alignment) {
        if (math::AlignUp(size, alignment) > 2048) {
            auto pmm = &kernel::GetInstance().data.pmm;
            auto paging = &kernel::GetInstance().data.paging;

            auto pageCount = math::AlignUp(math::AlignUp(size, alignment), system::PageSize) / system::PageSize;
            auto phys = pmm->Allocate(pageCount);
            auto virt = slab_allocator::GetVirtualAddress(_pageState, _higherHalf, paging, pageCount);
            paging->MapRange(_pageState, virt.start, (uint64_t)phys, pageCount, piru::memory::paging::Flags::ReadWrite | memory::paging::Flags::NoExecute);

            threading::spinlock::scoped_lock lock(_bigAllocationsLock, true);
            auto currentAllocationTracker = _bigAllocationTracker;
            while (currentAllocationTracker && currentAllocationTracker->IsFull()) {
                if (currentAllocationTracker->next) {
                    currentAllocationTracker = currentAllocationTracker->next;
                } else {
                    currentAllocationTracker->next = _slabAllocator.AllocateSlab(sizeof(big_allocation), _pageState, true); // big_allocation metadata is ALWAYS in higher half. Its kernel data.
                    threading::spinlock::scoped_lock statsLock(_statsLock, true);
                    _totalUsedSystemMemory += system::PageSize;
                    currentAllocationTracker = currentAllocationTracker->next;
                }
            }

            auto bigAlloc = (big_allocation*)currentAllocationTracker->Allocate();
            bigAlloc->addr = (void*)virt.start;
            bigAlloc->size = size;
            bigAlloc->alignment = alignment;
            bigAlloc->next = _bigAllocations;
            _bigAllocations = bigAlloc;

            threading::spinlock::scoped_lock statsLock(_statsLock, true);
            _totalBigAllocations++;
            _totalBigAllocationSize += size;

            _totalAllocatedMemory += size;
            _totalUsedSystemMemory += pageCount * system::PageSize;

            return (void*)virt.start;
        }

        auto bucket = _smallAllocations[BucketFromSize(size, alignment)];
        threading::spinlock::scoped_lock lock(_smallAllocationsLock[BucketFromSize(size, alignment)], true);

        while (bucket->IsFull()) {
            if (bucket->next) {
                bucket = bucket->next;
            } else {
                bucket->next = _slabAllocator.AllocateSlab(bucket->sizeClass, _pageState, _higherHalf);
                threading::spinlock::scoped_lock statsLock(_statsLock, true);
                _totalUsedSystemMemory += system::PageSize;
                bucket = bucket->next;
            }
        }

        threading::spinlock::scoped_lock statsLock(_statsLock, true);
        _totalSmallAllocations++;
        _totalSmallAllocationSize += size;

        _totalAllocatedMemory += size;

        return bucket->Allocate();
    }

    void * heap_allocator::state::Reallocate(void *ptr, size_t oldSize, size_t newSize, size_t alignment) {
        auto newMemory = Allocate(newSize, alignment);
        memcpy(newMemory, ptr, math::Min(oldSize, newSize));
        Free(ptr, oldSize, alignment);

        return newMemory;
    }

    void heap_allocator::state::Free(void *ptr, size_t size, size_t alignment) {
        if (math::AlignUp(size, alignment) > 2048) {
            auto allocation = _bigAllocations;
            while (allocation && allocation->addr != ptr) {
                allocation = allocation->next;
            }

            if (!allocation) {
                LOG_ERROR("Failed to free pointer {} of size {}: not found in big allocations!", ptr, size);
                return;
            }

            if (allocation->size != size) {
                LOG_ERROR("Failed to free pointer {} of size {}: size mismatch (expected {}, got {})!", ptr, size, allocation->size, size);
                return;
            }

            auto pmm = &kernel::GetInstance().data.pmm;
            auto paging = &kernel::GetInstance().data.paging;

            auto pageCount = math::AlignUp(math::AlignUp(size, alignment), system::PageSize) / system::PageSize;

            auto phys = paging->GetPhysicalAddress(_pageState, (uint64_t)allocation->addr);
            paging->UnmapRange(_pageState, (uint64_t)allocation->addr, pageCount);
            pmm->Free((void*)phys, pageCount);

            // Remove from linked list
            threading::spinlock::scoped_lock lock(_bigAllocationsLock, true);
            if (allocation == _bigAllocations) {
                _bigAllocations = allocation->next;
            } else {
                auto prev = _bigAllocations;
                while (prev && prev->next != allocation) {
                    prev = prev->next;
                }
                if (prev) {
                    prev->next = allocation->next;
                }
            }

            allocation->next = nullptr;
            allocation->addr = nullptr;
            allocation->size = 0;
            auto currentAllocationTracker = _bigAllocationTracker;

            while (currentAllocationTracker && !currentAllocationTracker->Has(allocation)) {
                currentAllocationTracker = currentAllocationTracker->next;
            }

            if (!currentAllocationTracker) {
                LOG_ERROR("Failed to free pointer {} of size {}: allocation tracker not found!", ptr, size);
                LOG_ERROR("This should never happen, if it did uhh wow...");
                return;
            }

            threading::spinlock::scoped_lock statsLock(_statsLock, true);
            _totalBigAllocations--;
            _totalBigAllocationSize -= size;
            _totalAllocatedMemory -= size;
            _totalUsedSystemMemory -= pageCount * system::PageSize;

            currentAllocationTracker->Give(allocation);
            return;
        }

        auto bucket = _smallAllocations[BucketFromSize(size, alignment)];
        threading::spinlock::scoped_lock lock(_smallAllocationsLock[BucketFromSize(size, alignment)], true);

        while (bucket && !bucket->Has(ptr)) {
            if (bucket->next) {
                bucket = bucket->next;
            } else {
                // This should never happen, as we should always find the pointer in one of the slabs.
                LOG_ERROR("Failed to free pointer {} of size {}: not found in any slab!", ptr, size);
                return;
            }
        }

        threading::spinlock::scoped_lock statsLock(_statsLock, true);
        _totalSmallAllocations--;
        _totalSmallAllocationSize -= size;
        _totalAllocatedMemory -= size;

        bucket->Give(ptr);
    }

    heap_allocator::state::statistics heap_allocator::state::GetStats() {
        threading::spinlock::scoped_lock statsLock(_statsLock, true);

        return {
            .totalAllocatedMemory = _totalAllocatedMemory,
            .totalUsedSystemMemory = _totalUsedSystemMemory,
            .totalSmallAllocations = _totalSmallAllocations,
            .totalSmallAllocationSize = _totalSmallAllocationSize,
            .totalBigAllocations = _totalBigAllocations,
            .totalBigAllocationSize = _totalBigAllocationSize,
        };
    }

    heap_allocator::state::state(string_view name, memory::paging::state* pageState, bool higherHalf) : _name(name), _pageState(pageState), _higherHalf(higherHalf) {
        _bigAllocationTracker = _slabAllocator.AllocateSlab(sizeof(big_allocation), _pageState, true);
        _smallAllocations[0] = _slabAllocator.AllocateSlab(16, _pageState, _higherHalf);
        _smallAllocations[1] = _slabAllocator.AllocateSlab(32, _pageState, _higherHalf);
        _smallAllocations[2] = _slabAllocator.AllocateSlab(64, _pageState, _higherHalf);
        _smallAllocations[3] = _slabAllocator.AllocateSlab(128, _pageState, _higherHalf);
        _smallAllocations[4] = _slabAllocator.AllocateSlab(256, _pageState, _higherHalf);
        _smallAllocations[5] = _slabAllocator.AllocateSlab(512, _pageState, _higherHalf);
        _smallAllocations[6] = _slabAllocator.AllocateSlab(1024, _pageState, _higherHalf);
        _smallAllocations[7] = _slabAllocator.AllocateSlab(2048, _pageState, _higherHalf);

        _totalUsedSystemMemory += sizeof(state) + (sizeof(big_allocation) + sizeof(slab) * 8) + (system::PageSize * 9); // state + big allocation tracker + 8 small allocation slabs + 9 pages for the slabs
    }

    heap_allocator::state::~state() {
        auto current = _bigAllocations;
        while (current) {
            auto next = current->next;
            Free(current->addr, current->size, current->alignment);
            current = next;
        }

        auto deleteSlabFn = [](slab_allocator* allocator, slab* slab, memory::paging::state* pageState) {
            while (slab) {
                auto next = slab->next;
                allocator->FreeSlab(slab, pageState);
                slab = next;
            }
        };

        deleteSlabFn(&_slabAllocator, _bigAllocationTracker, _pageState);
        deleteSlabFn(&_slabAllocator, _smallAllocations[0], _pageState);
        deleteSlabFn(&_slabAllocator, _smallAllocations[1], _pageState);
        deleteSlabFn(&_slabAllocator, _smallAllocations[2], _pageState);
        deleteSlabFn(&_slabAllocator, _smallAllocations[3], _pageState);
        deleteSlabFn(&_slabAllocator, _smallAllocations[4], _pageState);
        deleteSlabFn(&_slabAllocator, _smallAllocations[5], _pageState);
        deleteSlabFn(&_slabAllocator, _smallAllocations[6], _pageState);
        deleteSlabFn(&_slabAllocator, _smallAllocations[7], _pageState);

        _totalAllocatedMemory = 0;
        _totalUsedSystemMemory = 0;
        _totalSmallAllocations = 0;
        _totalSmallAllocationSize = 0;
        _totalBigAllocations = 0;
        _totalBigAllocationSize = 0;
    }
} // state

namespace piru::subsystems {
    heap_allocator::heap_allocator() : _heapState("kernel_heap_allocator", kernel::GetInstance().GetCpu().pageState, true) {
        _states = nullptr;
    }

    Result heap_allocator::Initialize() {
        _cppNewState = CreateState("kernel_cpp_new", kernel::GetInstance().GetCpu().pageState, true);

        return Result::SUCCESS;
    }

    heap_allocator::state * heap_allocator::CreateState(string_view name, memory::paging::state* pageState, bool higherHalf) {
        auto s = _heapState.Allocate(sizeof(state), alignof(state));
        auto st = :: new (s) state(name, pageState, higherHalf);

        auto tracker = (state_list*)_heapState.Allocate(sizeof(state_list));
        tracker->value = st;
        tracker->next = _states;
        _states = tracker;

        return st;
    }

    void heap_allocator::DestroyState(state *s) {
        auto prev = (state_list*)nullptr;
        auto current = _states;
        while (current && current->value != s) {
            prev = current;
            current = current->next;
        }

        if (!current) {
            LOG_ERROR("Failed to destroy state {}: not found in state list!", s->_name);
            return;
        }

        s->~state();
        _heapState.Free(s, sizeof(state));

        if (prev) {
            prev->next = current->next;
        } else {
            _states = current->next;
        }

        _heapState.Free(current, sizeof(state_list));
    }
} // heap_allocator
