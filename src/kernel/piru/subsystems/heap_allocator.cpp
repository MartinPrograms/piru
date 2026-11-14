#include "heap_allocator.hpp"
#include <cpp/math.hpp>
#include "kernel.hpp"
#include "memory/physical_memory_manager.hpp"
#include <new>
#include "logging.hpp"
#include "core/smp.hpp"

namespace {
    piru::subsystems::heap_allocator::state *_cppNewState = nullptr;

    struct AllocationHeader {
        size_t totalSize;
        size_t align;
        uint32_t magic;
    };

    constexpr uint32_t ALLOC_MAGIC = 0xDEADC0DE;
    constexpr size_t HEADER_SIZE = (sizeof(AllocationHeader) + 15) & ~15; // Aligned to 16 bytes.

    uintptr_t SizedNewAllocate(size_t bytes, size_t align = 0) {
        auto A = piru::math::Max(align, (size_t)16);
        size_t offset = piru::math::Max(HEADER_SIZE, A);
        size_t actualBytes = bytes + offset;

        auto raw = (uintptr_t)_cppNewState->Allocate(actualBytes, A);
        if (!raw)
            PANIC("Operator new failed!");

        uintptr_t user = raw + offset;

        auto *header = reinterpret_cast<AllocationHeader*>(user - HEADER_SIZE);
        header->totalSize = actualBytes;
        header->align = A;
        header->magic = ALLOC_MAGIC;
        return user;
    }

    void FreeWithHeader(void* ptr) {
        if (!ptr) return;

        uintptr_t headerPtr = reinterpret_cast<uintptr_t>(ptr) - HEADER_SIZE;
        auto *header = reinterpret_cast<AllocationHeader*>(headerPtr);
        if (header->magic != ALLOC_MAGIC)
            PANIC("Invalid magic number in allocation header. Possible double free or memory corruption.");

        auto A = piru::math::Max(header->align, (size_t)16);
        size_t offset = piru::math::Max(HEADER_SIZE, A);
        auto raw = (uintptr_t)ptr - offset;

        header->magic = 0; // Invalidate the magic number to prevent double free.
        _cppNewState->Free((void*)raw, header->totalSize, header->align);
    }
}

void* operator new (size_t size){
    return reinterpret_cast<void*>(SizedNewAllocate(size));
}

void* operator new[](size_t size){
    return reinterpret_cast<void*>(SizedNewAllocate(size));
}

void* operator new (size_t size, std::align_val_t alignment) {
    return reinterpret_cast<void*>(SizedNewAllocate(size, (size_t)alignment));
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
    heap_allocator::slab *heap_allocator::slab_allocator::InitHeaderPage(uintptr_t virt) {
        auto header = ::new ((void*)virt) slab{};
        header->addr = (void*)virt;
        header->sizeClass = math::PowerOf2(sizeof(slab));
        header->maxElementCount = (system::PageSize / header->sizeClass);
        header->allocationMap[0] = 0b1; // slot 0 is this header itself.
        header->allocatedCount = 1;
        header->next = nullptr;
        return header;
    }

    heap_allocator::slab_allocator::slab_allocator() {
        const auto pmm = &piru::kernel::GetInstance().data.pmm;
        const auto paging = &piru::kernel::GetInstance().data.paging;

        auto page = pmm->Allocate(1);
        if (!page)
            PANIC("slab_allocator: out of physical memory for header page!");

        auto virt = paging->MapAnywhereKernel((uint64_t)page, 1, memory::paging::Flags::ReadWrite | memory::paging::Flags::NoExecute);
        _memory = InitHeaderPage(virt.start);
    }

    heap_allocator::slab_allocator::~slab_allocator() {
        const auto pageState = piru::kernel::GetInstance().GetCpu().pageState;
        const auto pmm = &piru::kernel::GetInstance().data.pmm;
        const auto paging = &piru::kernel::GetInstance().data.paging;

        // Header pages are only freed here; the data slabs they describe are freed by ~state via FreeSlab.
        auto current = _memory;
        while (current) {
            auto next = current->next; // read BEFORE unmapping, the header lives on the page we're about to unmap.
            auto virt = (uint64_t)current;
            auto phys = (void*)paging->GetPhysicalAddress(pageState, virt);
            paging->UnmapPage(pageState, virt);
            pmm->Free(phys, 1);
            current = next;
        }
        _memory = nullptr;
    }

    memory::paging::available_virtual_address_range heap_allocator::slab_allocator::MapPages(memory::paging::state *pageState, bool higherHalf, memory::paging * const paging, uint64_t phys, size_t pageCount, size_t align) {
        constexpr auto flags = memory::paging::Flags::ReadWrite | memory::paging::Flags::NoExecute;
        if (higherHalf)
            return paging->MapAnywhereKernel(phys, pageCount, flags, align);
        return paging->MapAnywhere(pageState, 1 * constants::MiB, system::EndOfCanonical, phys, pageCount, flags, align);
    }

    // Lock order: bucket lock -> _lock -> paging/pmm locks.
    heap_allocator::slab *heap_allocator::slab_allocator::AllocateSlab(size_t sizeClass, memory::paging::state *pageState, bool higherHalf) {
        const auto pmm = &piru::kernel::GetInstance().data.pmm;
        const auto paging = &piru::kernel::GetInstance().data.paging;

        auto page = pmm->Allocate(1);
        if (!page)
            PANIC("slab_allocator: out of physical memory for slab!");
        auto virt = MapPages(pageState, higherHalf, paging, (uint64_t)page, 1, 0);

        slab *s;
        {
            threading::spinlock::scoped_lock lock(_lock, true);

            auto current = _memory;
            while (current->IsFull()) {
                if (current->next) {
                    current = current->next;
                    continue;
                }

                // Every header page is full: add a new one. Headers are kernel data, so always higher half.
                auto newPage = pmm->Allocate(1);
                if (!newPage)
                    PANIC("slab_allocator: out of physical memory for header page!");
                auto newVirt = paging->MapAnywhereKernel((uint64_t)newPage, 1, memory::paging::Flags::ReadWrite | memory::paging::Flags::NoExecute, 0);

                current->next = InitHeaderPage(newVirt.start);
                current = current->next;
            }

            s = ::new (current->Allocate()) slab{};
        }

        s->addr = (void*)virt.start;
        s->sizeClass = sizeClass;
        s->maxElementCount = (system::PageSize / sizeClass);
        s->allocatedCount = 0;
        s->next = nullptr;

        return s;
    }

    void heap_allocator::slab_allocator::FreeSlab(slab *slab, memory::paging::state *pageState) {
        const auto pmm = &piru::kernel::GetInstance().data.pmm;
        const auto paging = &piru::kernel::GetInstance().data.paging;

        auto addr = (uint64_t)slab->addr; // read before Give: after that the header slot may be reused.

        {
            threading::spinlock::scoped_lock lock(_lock, true);

            auto current = _memory;
            while (current && !current->Has(slab))
                current = current->next;

            if (!current)
                PANIC("slab_allocator::FreeSlab: slab header not owned by this allocator!");
            if (!current->Give(slab))
                PANIC("slab_allocator::FreeSlab: double free of slab header!");
        }

        // Unmap outside of _lock: UnmapPage may shoot down, which must not run while holding a cli lock.
        auto phys = paging->GetPhysicalAddress(pageState, addr);
        paging->UnmapPage(pageState, addr);
        pmm->Free((void*)phys, 1);
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

    bool heap_allocator::state::statistics::operator==(const statistics &other) const {
        if ((this->totalAllocatedMemory != other.totalAllocatedMemory) ||
            (this->totalBigAllocations != other.totalBigAllocations) ||
            (this->totalBigAllocationSize != other.totalBigAllocationSize) ||
            (this->totalSmallAllocations != other.totalSmallAllocations) ||
            (this->totalSmallAllocationSize != other.totalSmallAllocationSize) ||
            (this->totalUsedSystemMemory != other.totalUsedSystemMemory))
                return false;

        return true;
    }

    void* heap_allocator::state::Allocate(size_t size, size_t alignment) {
        if (math::AlignUp(size, alignment) > 2048) {
            auto pmm = &kernel::GetInstance().data.pmm;
            auto paging = &kernel::GetInstance().data.paging;

            auto pageCount = math::AlignUp(size, system::PageSize) / system::PageSize;
            auto phys = pmm->Allocate(pageCount);
            if (!phys)
                return nullptr;

            auto virt = slab_allocator::MapPages(_pageState, _higherHalf, paging, (uint64_t)phys, pageCount, alignment);

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
            void* toUnmapStart = 0;
            size_t toUnmapPageCount = 0;

            {
                threading::spinlock::scoped_lock lock(_bigAllocationsLock, true);

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

                auto pageCount = math::AlignUp(size, system::PageSize) / system::PageSize;

                toUnmapStart = allocation->addr;
                toUnmapPageCount = pageCount;

                // Remove from linked list
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

                if (!currentAllocationTracker->Give(allocation))
                    PANIC("heap_allocator: double free of big allocation tracker entry!");
            }

            auto pmm = &kernel::GetInstance().data.pmm;
            auto paging = &kernel::GetInstance().data.paging;
            auto phys = paging->GetPhysicalAddress(_pageState, (uint64_t)toUnmapStart);
            paging->UnmapRange(_pageState, (uint64_t)toUnmapStart, toUnmapPageCount);
            pmm->Free((void*)phys, toUnmapPageCount);

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

        if (!bucket->Give(ptr))
            PANIC("heap_allocator: double free of small allocation!");
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

}

// Tests (generated by AI)
namespace {
    using namespace piru;
    using heap_state = subsystems::heap_allocator::state;

    constexpr size_t MaxLoggedFailures = 8;

    size_t _failures = 0;
    string_view _currentTest = "";

    void Fail(string_view what, void* ptr, uint64_t expected, uint64_t got) {
        if (_failures++ < MaxLoggedFailures)
            LOG_ERROR("[heap:{}] {} at {} (expected {}, got {})", _currentTest, what, ptr, (void*)expected, (void*)got);
    }

    uint64_t XorShift(uint64_t& s) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }

    uint64_t PatternFor(uint64_t seed, size_t i) {
        return (seed * 0x9E3779B97F4A7C15ULL) ^ (i * 0xBF58476D1CE4E5B9ULL);
    }

    // Writes a pattern over the whole allocation (8 byte words, then a byte tail).
    void FillPattern(void* ptr, size_t size, uint64_t seed) {
        auto words = static_cast<volatile uint64_t*>(ptr);
        size_t n = size / 8;
        for (size_t i = 0; i < n; i++) words[i] = PatternFor(seed, i);
        auto bytes = static_cast<volatile uint8_t*>(ptr);
        for (size_t i = n * 8; i < size; i++) bytes[i] = static_cast<uint8_t>(PatternFor(seed, i));
    }

    // Returns false (and records a failure) on the first mismatching word/byte.
    bool CheckPattern(void* ptr, size_t size, uint64_t seed, string_view what) {
        auto words = static_cast<volatile uint64_t*>(ptr);
        size_t n = size / 8;
        for (size_t i = 0; i < n; i++) {
            if (words[i] != PatternFor(seed, i)) { Fail(what, (void*)&words[i], PatternFor(seed, i), words[i]); return false; }
        }
        auto bytes = static_cast<volatile uint8_t*>(ptr);
        for (size_t i = n * 8; i < size; i++) {
            if (bytes[i] != static_cast<uint8_t>(PatternFor(seed, i))) { Fail(what, (void*)&bytes[i], static_cast<uint8_t>(PatternFor(seed, i)), bytes[i]); return false; }
        }
        return true;
    }

    // Allocate, check alignment, fill, verify, free.
    void* AllocChecked(heap_state* s, size_t size, size_t alignment, uint64_t seed) {
        void* p = s->Allocate(size, alignment);
        if (!p) { Fail("Allocate returned null", nullptr, size, 0); return nullptr; }
        if (reinterpret_cast<uintptr_t>(p) % alignment != 0) Fail("misaligned allocation", p, alignment, reinterpret_cast<uintptr_t>(p) % alignment);
        FillPattern(p, size, seed);
        return p;
    }

    // Only the allocation counters must return to their old values; totalUsedSystemMemory may grow,
    // because empty slabs are (by design, for now) never given back.
    bool SameAllocationStats(const heap_state::statistics& a, const heap_state::statistics& b) {
        return a.totalAllocatedMemory == b.totalAllocatedMemory &&
               a.totalSmallAllocations == b.totalSmallAllocations &&
               a.totalSmallAllocationSize == b.totalSmallAllocationSize &&
               a.totalBigAllocations == b.totalBigAllocations &&
               a.totalBigAllocationSize == b.totalBigAllocationSize;
    }

    constexpr size_t TestSizes[] = { 1, 8, 15, 16, 17, 31, 32, 33, 64, 65, 100, 128, 129, 256, 257,
                                     512, 513, 1000, 1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097, 3 * 4096 + 5, 64 * 1024 };

    // ---------------------------------------------------------------------------------
    // Every size class and every boundary: alignment, full write/read-back, free.
    // ---------------------------------------------------------------------------------
    void TestSizesAndBoundaries(heap_state* s) {
        void* ptrs[sizeof(TestSizes) / sizeof(TestSizes[0])]{};
        for (size_t i = 0; i < sizeof(TestSizes) / sizeof(TestSizes[0]); i++)
            ptrs[i] = AllocChecked(s, TestSizes[i], 16, 1000 + i);

        // Verify only after everything is allocated: catches one allocation overwriting another.
        for (size_t i = 0; i < sizeof(TestSizes) / sizeof(TestSizes[0]); i++) {
            if (!ptrs[i]) continue;
            CheckPattern(ptrs[i], TestSizes[i], 1000 + i, "pattern corrupted");
            s->Free(ptrs[i], TestSizes[i], 16);
        }
    }

    // ---------------------------------------------------------------------------------
    // Many objects per size class, each with its own pattern: catches overlapping hand-outs.
    // ---------------------------------------------------------------------------------
    void TestNoOverlap(heap_state* s) {
        constexpr size_t PerClass = 300; // > one slab for 16 byte objects (256 per slab)
        static void* ptrs[PerClass];

        for (size_t size = 16; size <= 2048; size *= 2) {
            size_t count = (size >= 1024) ? 40 : PerClass;
            for (size_t i = 0; i < count; i++) ptrs[i] = AllocChecked(s, size, 16, size * 100000 + i);

            for (size_t i = 0; i < count; i++) {
                for (size_t j = i + 1; j < count; j++) {
                    if (ptrs[i] == ptrs[j]) Fail("same pointer handed out twice", ptrs[i], 0, 0);
                }
            }

            for (size_t i = 0; i < count; i++) {
                if (!ptrs[i]) continue;
                CheckPattern(ptrs[i], size, size * 100000 + i, "overlap: pattern overwritten");
                s->Free(ptrs[i], size, 16);
            }
        }
    }

    // ---------------------------------------------------------------------------------
    // A freed slot is handed out again (bitmap and allocatedCount stay consistent).
    // ---------------------------------------------------------------------------------
    void TestReuse(heap_state* s) {
        void* a = s->Allocate(64, 16);
        void* b = s->Allocate(64, 16);
        s->Free(a, 64, 16);
        void* c = s->Allocate(64, 16);
        if (c != a) Fail("freed slot not reused (lowest free slot first)", c, reinterpret_cast<uintptr_t>(a), reinterpret_cast<uintptr_t>(c));
        s->Free(b, 64, 16);
        s->Free(c, 64, 16);
    }

    // ---------------------------------------------------------------------------------
    // More than 63 slabs: forces a second slab-header page (InitHeaderPage + chaining).
    // ---------------------------------------------------------------------------------
    void TestHeaderPageFill(heap_state* s) {
        constexpr size_t Count = 160; // 2 x 2048 per slab -> 80 slabs
        static void* ptrs[Count];
        for (size_t i = 0; i < Count; i++) ptrs[i] = AllocChecked(s, 2048, 16, 7000 + i);
        for (size_t i = 0; i < Count; i++) {
            if (!ptrs[i]) continue;
            CheckPattern(ptrs[i], 2048, 7000 + i, "header fill: pattern corrupted");
            s->Free(ptrs[i], 2048, 16);
        }
    }

    // ---------------------------------------------------------------------------------
    // Many big allocations, freed in random order; freed pages must be unmapped.
    // ---------------------------------------------------------------------------------
    void TestBig(heap_state* s, memory::paging::state* pageState) {
        constexpr size_t Count = 48;
        static void* ptrs[Count];
        static size_t sizes[Count];
        uint64_t rng = 0xC0FFEE;
        auto& paging = kernel::GetInstance().data.paging;

        for (size_t i = 0; i < Count; i++) {
            sizes[i] = 2049 + XorShift(rng) % (5 * system::PageSize);
            ptrs[i] = AllocChecked(s, sizes[i], 16, 9000 + i);
        }

        // Fisher-Yates shuffle of the free order.
        size_t order[Count];
        for (size_t i = 0; i < Count; i++) order[i] = i;
        for (size_t i = Count - 1; i > 0; i--) {
            size_t j = XorShift(rng) % (i + 1);
            size_t t = order[i]; order[i] = order[j]; order[j] = t;
        }

        for (size_t n = 0; n < Count; n++) {
            size_t i = order[n];
            if (!ptrs[i]) continue;
            CheckPattern(ptrs[i], sizes[i], 9000 + i, "big: pattern corrupted");
            s->Free(ptrs[i], sizes[i], 16);

            size_t pages = math::AlignUp(sizes[i], system::PageSize) / system::PageSize;
            for (size_t p = 0; p < pages; p++) {
                uint64_t vaddr = reinterpret_cast<uint64_t>(ptrs[i]) + p * system::PageSize;
                if (paging.IsMapped(pageState, vaddr)) Fail("big: page still mapped after Free", (void*)vaddr, 0, 1);
            }
        }
    }

    // ---------------------------------------------------------------------------------
    // SMP stress: every CPU randomly allocates / verifies / frees in its own slots, same heap state.
    // ---------------------------------------------------------------------------------
    struct parallel_ctx {
        heap_state* state;
        std::atomic<size_t> failures{0};
        std::atomic<size_t> operations{0};
    };

    constexpr size_t ParallelSlots = 48;
    constexpr size_t ParallelIterations = 3000;

    void ParallelJob(void* arg) {
        auto* ctx = static_cast<parallel_ctx*>(arg);
        auto& k = kernel::GetInstance();
        const size_t cpu = k.GetCpuIndex(interrupts::gdt::GetCpuId());

        // TEMP DEBUG: phys recorded at allocation, to detect remapping.
        struct slot { void* ptr; size_t size; uint64_t seed; uint64_t phys; };
        slot slots[ParallelSlots]{};
        uint64_t rng = 0x1234567ULL + cpu * 0x9E3779B97F4A7C15ULL;
        auto& paging = k.data.paging;
        auto* pageState = k.GetCpu().pageState;

        // TEMP DEBUG: PatternFor(seed, i) = seed*C1 ^ i*C2, C1 odd -> invertible mod 2^64, so a foreign
        // word can be decoded back to the seed (= cpu << 32 ^ iteration) that wrote it.
        constexpr uint64_t C1 = 0x9E3779B97F4A7C15ULL;
        constexpr uint64_t C1Inv = [] { uint64_t x = C1; for (int n = 0; n < 6; n++) x *= 2 - C1 * x; return x; }();

        auto verify = [&](slot& sl) {
            auto words = static_cast<volatile uint64_t*>(sl.ptr);
            const size_t count = sl.size / 8;
            size_t bad = 0, first = 0;
            uint64_t got = 0;
            for (size_t i = 0; i < count; i++) {
                if (words[i] != PatternFor(sl.seed, i)) {
                    if (bad++ == 0) { first = i; got = words[i]; }
                }
            }
            if (!bad) return;
            if (ctx->failures.fetch_add(1) >= 6) return; // keep it on one screen: 6 failures x 2 lines

            // Line 1: where, how much.
            LOG_ERROR("[par] cpu{} {} sz{} bad {}/{}w first w{}", cpu, sl.ptr, sl.size, bad, count, first);

            // Line 2: expected/got, who wrote it (if it decodes to a pattern), and phys only if it moved.
            uint64_t srcSeed = (got ^ (first * 0xBF58476D1CE4E5B9ULL)) * C1Inv;
            uint64_t srcCpu = srcSeed >> 32;
            const uint64_t addr = reinterpret_cast<uint64_t>(sl.ptr) + first * 8;
            const uint64_t physNow = paging.GetPhysicalAddress(pageState, addr);
            const uint64_t physThen = sl.phys + first * 8;
            const bool isPattern = srcCpu < k.GetCpuCount();
            const bool moved = physNow != physThen;

            if (isPattern && moved)
                LOG_ERROR("  got {} =cpu{} it{} PHYS {}->{}", (void*)got, srcCpu, srcSeed & 0xFFFFFFFF, (void*)physThen, (void*)physNow);
            else if (isPattern)
                LOG_ERROR("  got {} =cpu{} it{} (phys same)", (void*)got, srcCpu, srcSeed & 0xFFFFFFFF);
            else if (moved)
                LOG_ERROR("  got {} exp {} PHYS {}->{}", (void*)got, (void*)PatternFor(sl.seed, first), (void*)physThen, (void*)physNow);
            else
                LOG_ERROR("  got {} exp {} (phys same)", (void*)got, (void*)PatternFor(sl.seed, first));
        };

        for (size_t it = 0; it < ParallelIterations; it++) {
            auto& sl = slots[XorShift(rng) % ParallelSlots];
            if (sl.ptr) {
                verify(sl);
                ctx->state->Free(sl.ptr, sl.size, 16);
                sl.ptr = nullptr;
            } else {
                uint64_t r = XorShift(rng);
                sl.size = (r % 16 == 0) ? 2049 + (r >> 8) % (3 * system::PageSize) : 1 + (r >> 8) % 2048;
                sl.seed = (static_cast<uint64_t>(cpu) << 32) ^ it;
                sl.ptr = ctx->state->Allocate(sl.size, 16);
                if (!sl.ptr) { ctx->failures.fetch_add(1); continue; }
                sl.phys = paging.GetPhysicalAddress(pageState, reinterpret_cast<uint64_t>(sl.ptr)); // TEMP DEBUG
                auto words = static_cast<volatile uint64_t*>(sl.ptr);
                for (size_t i = 0; i < sl.size / 8; i++) words[i] = PatternFor(sl.seed, i);
            }
            ctx->operations.fetch_add(1, std::memory_order_relaxed);
        }

        for (auto& sl : slots) {
            if (!sl.ptr) continue;
            verify(sl);
            ctx->state->Free(sl.ptr, sl.size, 16);
        }
    }

    void TestParallel(heap_state* s) {
        static parallel_ctx ctx{};
        ctx.state = s;
        ctx.failures.store(0);
        ctx.operations.store(0);

        core::smp::CallOnAll(ParallelJob, &ctx, /*wait*/ true, /*self*/ true);

        LOG_DEBUG("[heap:parallel] {} operations on {} CPUs", ctx.operations.load(), kernel::GetInstance().GetCpuCount());
        _failures += ctx.failures.load();
    }

    // ---------------------------------------------------------------------------------
    // Lower-half heap state: this CPU switches to the test page state so the memory can be touched.
    // ---------------------------------------------------------------------------------
    void TestLowerHalfContents(heap_state* s, memory::paging::state* pageState) {
        auto& paging = kernel::GetInstance().data.paging;
        auto* previous = kernel::GetInstance().GetCpu().pageState;

        paging.SwitchToPageTable(pageState);
        TestSizesAndBoundaries(s);
        TestReuse(s);
        paging.SwitchToPageTable(previous);
    }

    // ---------------------------------------------------------------------------------
    // Aligned operator new / delete: alignment, header layout, no overflow into a neighbour,
    // and Free hitting the same bucket / big path as Allocate (allocation stats return to baseline).
    // ---------------------------------------------------------------------------------
    void TestAlignedNew() {
        constexpr size_t Alignments[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
        constexpr size_t Sizes[] = { 1, 15, 16, 100, 500, 1000, 2000, 3000, 5000 };

        const auto statsBefore = _cppNewState->GetStats();
        uint64_t seed = 0xA11C0DE;

        for (size_t align : Alignments) {
            for (size_t n : Sizes) {
                seed++;
                void* p = ::operator new(n, std::align_val_t{align});
                auto user = reinterpret_cast<uintptr_t>(p);

                if (user % align != 0)
                    Fail("aligned new: misaligned pointer", p, align, user % align);

                // Header sits directly in front of the user pointer and must describe a block big enough
                // for header + padding + n bytes. Catches allocating bytes + HEADER_SIZE instead of bytes + offset.
                auto* header = reinterpret_cast<AllocationHeader*>(user - HEADER_SIZE);
                const size_t offset = math::Max(HEADER_SIZE, math::Max(align, (size_t)16));
                if (header->magic != ALLOC_MAGIC)
                    Fail("aligned new: bad header magic", p, ALLOC_MAGIC, header->magic);
                if (header->totalSize < n + offset)
                    Fail("aligned new: block too small for padding + data", p, n + offset, header->totalSize);

                // Neighbour from the same state and the same size class: written first, so an overflow
                // of p's data would corrupt it.
                void* neighbour = _cppNewState->Allocate(header->totalSize, header->align);
                FillPattern(neighbour, header->totalSize, seed ^ 0xFFFF);
                FillPattern(p, n, seed);

                CheckPattern(p, n, seed, "aligned new: data corrupted");
                CheckPattern(neighbour, header->totalSize, seed ^ 0xFFFF, "aligned new: neighbour overwritten");

                _cppNewState->Free(neighbour, header->totalSize, header->align);
                ::operator delete(p, std::align_val_t{align});
            }
        }

        // If delete used a different alignment than new, Free looks in the wrong bucket / path,
        // logs an error and leaves the counters raised.
        if (!SameAllocationStats(statsBefore, _cppNewState->GetStats()))
            Fail("aligned new: allocation stats not back to baseline", nullptr, statsBefore.totalAllocatedMemory, _cppNewState->GetStats().totalAllocatedMemory);
    }

    template<typename Fn>
    bool RunTest(string_view name, Fn&& fn) {
        _currentTest = name;
        size_t before = _failures;
        fn();
        size_t failed = _failures - before;
        if (failed == 0) LOG_INFO("[heap:{}] PASSED", name);
        else LOG_ERROR("[heap:{}] FAILED with {} failure(s)", name, failed);
        return failed == 0;
    }
}

namespace piru::subsystems {
    bool heap_allocator::RunTests() {
        auto& k = kernel::GetInstance();
        auto& paging = k.data.paging;
        auto& pmm = k.data.pmm;
        _failures = 0;
        bool passed = true;

        // Warm-up: CreateState allocates the state + its list entry from _heapState. The first time that may
        // grow _heapState by a slab, which is never returned. Do one create/destroy first so that growth is not
        // counted as a leak below.
        DestroyState(CreateState("heap_test_warmup", k.GetCpu().pageState, true));

        // --- Higher-half state: all content tests, including SMP --------------------------------
        {
            const size_t freeBefore = pmm.GetFreePageCount();
            auto* s = CreateState("heap_test", k.GetCpu().pageState, true);
            const auto statsBefore = s->GetStats();

            passed &= RunTest("sizes", [&] { TestSizesAndBoundaries(s); });
            passed &= RunTest("no_overlap", [&] { TestNoOverlap(s); });
            passed &= RunTest("reuse", [&] { TestReuse(s); });
            passed &= RunTest("header_page_fill", [&] { TestHeaderPageFill(s); });
            passed &= RunTest("big", [&] { TestBig(s, k.GetCpu().pageState); });
            passed &= RunTest("parallel", [&] { TestParallel(s); });

            passed &= RunTest("stats_balanced", [&] {
                auto after = s->GetStats();
                if (!SameAllocationStats(statsBefore, after)) {
                    Fail("allocation stats not back to baseline (allocated bytes)", nullptr, statsBefore.totalAllocatedMemory, after.totalAllocatedMemory);
                    Fail("small allocation count", nullptr, statsBefore.totalSmallAllocations, after.totalSmallAllocations);
                    Fail("big allocation count", nullptr, statsBefore.totalBigAllocations, after.totalBigAllocations);
                }
            });

            DestroyState(s);
        }

        // --- Lower-half state on its own page state: content + lifecycle --------------------------
        {
            const size_t freeBefore = pmm.GetFreePageCount();
            static memory::paging::state pageState = paging.CreatePageState();

            passed &= RunTest("lower_half", [&] {
                auto* s = CreateState("heap_test_lower", &pageState, false);
                TestLowerHalfContents(s, &pageState);
                DestroyState(s);
            });

            passed &= RunTest("lifecycle", [&] {
                for (size_t round = 0; round < 10; round++) {
                    auto* s = CreateState("heap_test_cycle", &pageState, false);
                    void* small[16];
                    for (size_t i = 0; i < 16; i++) small[i] = s->Allocate(16 << (i % 8), 16);
                    void* big = s->Allocate(3 * system::PageSize, 16);
                    for (size_t i = 0; i < 8; i++) s->Free(small[i], 16 << (i % 8), 16); // free half, DestroyState cleans the rest
                    (void)big;
                    DestroyState(s);
                }
            });

            paging.DestroyPageState(&pageState);
            passed &= RunTest("no_leak_lower_half", [&] {
                const size_t freeAfter = pmm.GetFreePageCount();
                if (freeAfter != freeBefore) Fail("physical pages not returned after DestroyState + DestroyPageState", nullptr, freeBefore, freeAfter);
            });
        }

        passed &= RunTest("aligned_new", [&] { TestAlignedNew(); });

        if (passed) LOG_INFO("[heap tests] ALL PASSED");
        else LOG_ERROR("[heap tests] FAILED");
        return passed;
    }
} // heap_allocator
