#include "heap_allocator.hpp"
#include "heap_allocator.hpp"

#include "kernel.hpp"
#include "memory/physical_memory_manager.hpp"
#include <new>

#include "logging.hpp"

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

    heap_allocator::slab *heap_allocator::slab_allocator::AllocateSlab(size_t sizeClass) {
        const auto pageState = piru::kernel::GetInstance().GetCpu().pageState;
        const auto pmm = &piru::kernel::GetInstance().data.pmm;
        const auto paging = &piru::kernel::GetInstance().data.paging;

        auto page = (void*)pmm->Allocate(1);
        auto virt = paging->FindAvailableVirtualAddressRangeKernel(1);
        paging->MapPage(pageState, (uint64_t)virt.start, (uint64_t)page, memory::paging::Flags::ReadWrite | memory::paging::Flags::NoExecute);

        auto current = _memory;
        while (current->IsFull()) {
            if (current->next)
                current = current->next;

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

    void heap_allocator::slab_allocator::FreeSlab(slab *slab) {
        const auto pageState = piru::kernel::GetInstance().GetCpu().pageState;
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
            auto pageState = kernel::GetInstance().GetCpu().pageState;
            auto pmm = &kernel::GetInstance().data.pmm;
            auto paging = &kernel::GetInstance().data.paging;

            auto pageCount = math::AlignUp(math::AlignUp(size, alignment), system::PageSize) / system::PageSize;
            auto phys = pmm->Allocate(pageCount);
            auto virt = paging->FindAvailableVirtualAddressRangeKernel(pageCount);
            paging->MapRange(pageState, virt.start, (uint64_t)phys, pageCount, piru::memory::paging::Flags::ReadWrite | memory::paging::Flags::NoExecute);

            threading::spinlock::scoped_lock lock(_bigAllocationsLock, true);
            auto currentAllocationTracker = _bigAllocationTracker;
            while (currentAllocationTracker && currentAllocationTracker->IsFull()) {
                if (currentAllocationTracker->next) {
                    currentAllocationTracker = currentAllocationTracker->next;
                } else {
                    currentAllocationTracker->next = _slabAllocator.AllocateSlab(sizeof(big_allocation));
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
                bucket->next = _slabAllocator.AllocateSlab(bucket->sizeClass);
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

            auto pageState = kernel::GetInstance().GetCpu().pageState;
            auto pmm = &kernel::GetInstance().data.pmm;
            auto paging = &kernel::GetInstance().data.paging;

            auto pageCount = math::AlignUp(math::AlignUp(size, alignment), system::PageSize) / system::PageSize;

            auto phys = paging->GetPhysicalAddress(pageState, (uint64_t)allocation->addr);
            paging->UnmapRange(pageState, (uint64_t)allocation->addr, pageCount);
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

    heap_allocator::state::state(string_view name) : _name(name) {
        _bigAllocationTracker = _slabAllocator.AllocateSlab(sizeof(big_allocation));
        _smallAllocations[0] = _slabAllocator.AllocateSlab(16);
        _smallAllocations[1] = _slabAllocator.AllocateSlab(32);
        _smallAllocations[2] = _slabAllocator.AllocateSlab(64);
        _smallAllocations[3] = _slabAllocator.AllocateSlab(128);
        _smallAllocations[4] = _slabAllocator.AllocateSlab(256);
        _smallAllocations[5] = _slabAllocator.AllocateSlab(512);
        _smallAllocations[6] = _slabAllocator.AllocateSlab(1024);
        _smallAllocations[7] = _slabAllocator.AllocateSlab(2048);

        _totalUsedSystemMemory += sizeof(state) + (sizeof(big_allocation) + sizeof(slab) * 8) + (system::PageSize * 9); // state + big allocation tracker + 8 small allocation slabs + 9 pages for the slabs
    }

    heap_allocator::state::~state() {
        auto current = _bigAllocations;
        while (current) {
            auto next = current->next;
            Free(current->addr, current->size, current->alignment);
            current = next;
        }

        auto deleteSlabFn = [](slab_allocator* allocator, slab* slab) {
            while (slab) {
                auto next = slab->next;
                allocator->FreeSlab(slab);
                slab = next;
            }
        };

        deleteSlabFn(&_slabAllocator, _bigAllocationTracker);
        deleteSlabFn(&_slabAllocator, _smallAllocations[0]);
        deleteSlabFn(&_slabAllocator, _smallAllocations[1]);
        deleteSlabFn(&_slabAllocator, _smallAllocations[2]);
        deleteSlabFn(&_slabAllocator, _smallAllocations[3]);
        deleteSlabFn(&_slabAllocator, _smallAllocations[4]);
        deleteSlabFn(&_slabAllocator, _smallAllocations[5]);
        deleteSlabFn(&_slabAllocator, _smallAllocations[6]);
        deleteSlabFn(&_slabAllocator, _smallAllocations[7]);

        _totalAllocatedMemory = 0;
        _totalUsedSystemMemory = 0;
        _totalSmallAllocations = 0;
        _totalSmallAllocationSize = 0;
        _totalBigAllocations = 0;
        _totalBigAllocationSize = 0;
    }
} // state

namespace piru::subsystems {
    heap_allocator::heap_allocator() {

    }

    Result heap_allocator::Initialize() {
        return Result::SUCCESS;
    }

    heap_allocator::state * heap_allocator::CreateState(string_view name) {
        auto s = _heapState.Allocate(sizeof(state), alignof(state));
        auto st = :: new (s) state(name);

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

