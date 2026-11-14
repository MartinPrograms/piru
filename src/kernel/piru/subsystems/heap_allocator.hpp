#ifndef PIRU_HEAP_ALLOCATOR_HPP
#define PIRU_HEAP_ALLOCATOR_HPP

#include <bit>

#include "subsystem.hpp"
#include "cpp/string_view.hpp"
#include "cpp/spinlock.hpp"
#include "memory/paging.hpp"
#include "memory/physical_memory_manager.hpp"

namespace piru::subsystems {
    struct heap_allocator {
    private:
        struct big_allocation {
            void *addr;
            big_allocation *next;
            size_t size;
            size_t alignment;
        };

        struct ALIGNED(32) slab {
            slab *next{};
            void *addr{};
            uint16_t sizeClass{};
            uint16_t maxElementCount{};
            uint16_t allocatedCount{};
            uint64_t allocationMap[4]{}; // 256 entries, enough for a size class of 16.

            [[nodiscard]] bool IsFree(size_t index) const {
                return !(allocationMap[index / 64] & (1ULL << (index % 64)));
            }

            [[nodiscard]] bool IsEmpty() const {
                return allocatedCount == 0;
            }

            [[nodiscard]] bool IsFull() const {
                return allocatedCount == maxElementCount;
            }

            [[nodiscard]] bool Has(void* ptr) const {
                auto p = static_cast<std::byte*>(ptr);
                auto max = static_cast<std::byte*>(addr) + (maxElementCount * sizeClass);
                if (p < addr || p >= max)
                    return false;

                size_t offset = p - static_cast<std::byte*>(addr);
                return (offset % sizeClass) == 0;
            }

            void Set(size_t index, bool allocated) {
                size_t chunk = index / 64;
                uint64_t mask = 1ULL << (index % 64);
                if (allocated) {
                    allocationMap[chunk] |= mask;
                } else {
                    allocationMap[chunk] &= ~mask;
                }
            }

            void* Allocate() {
                if (allocatedCount == maxElementCount) return nullptr;

                for (size_t i = 0; i < 4; i++) {
                    uint64_t freeBits = ~allocationMap[i];
                    if (i == 3 && (maxElementCount % 64) != 0) {
                        freeBits &= (1ULL << (maxElementCount % 64)) - 1;
                    }

                    if (freeBits) {
                        size_t bit = std::countr_zero(freeBits);
                        size_t index = i * 64 + bit;
                        Set(index, true);
                        allocatedCount++;
                        return static_cast<std::byte*>(addr) + (index * sizeClass);
                    }
                }

                return nullptr;
            }

            // Returns false if ptr is not an element of this slab (out of range / misaligned) or was already free (double free).
            [[nodiscard]] bool Give(void *ptr) {
                if (!Has(ptr))
                    return false;

                size_t offset = static_cast<std::byte*>(ptr) - static_cast<std::byte*>(addr);
                size_t index = offset / sizeClass;
                if (IsFree(index))
                    return false;

                Set(index, false);
                allocatedCount--;
                return true;
            }
        };

        struct slab_allocator {
            slab_allocator();
            ~slab_allocator();

            // Finds a free virtual range and maps phys there atomically (see paging::MapAnywhere).
            static memory::paging::available_virtual_address_range MapPages(
                memory::paging::state *pageState, bool higherHalf,
                memory::paging *paging, uint64_t phys, size_t pageCount, size_t align);

            slab *AllocateSlab(size_t sizeClass, memory::paging::state *pageState, bool higherHalf);
            void FreeSlab(slab *slab, memory::paging::state *pageState);

        private:
            static slab *InitHeaderPage(uintptr_t virt);

            slab *_memory;
            threading::spinlock _lock;
        };

    public:
        struct state {
            struct statistics {
                size_t totalAllocatedMemory;
                size_t totalUsedSystemMemory;
                size_t totalSmallAllocations;
                size_t totalSmallAllocationSize;
                size_t totalBigAllocations;
                size_t totalBigAllocationSize;

                bool operator==(const statistics & other) const;
            };

            void *Allocate(size_t size, size_t alignment = std::alignment_of_v<std::max_align_t>);
            void *Reallocate(void *ptr, size_t oldSize, size_t newSize, size_t alignment = std::alignment_of_v<std::max_align_t>);
            void Free(void *ptr, size_t size, size_t alignment = std::alignment_of_v<std::max_align_t>);
            statistics GetStats();

        private:
            friend heap_allocator;

            state(string_view name, memory::paging::state* pageState, bool higherHalf);
            ~state(); // destroys ALL allocated memory, for this pool/state

            string_view _name;
            memory::paging::state* _pageState;
            bool _higherHalf;

            slab_allocator _slabAllocator;
            slab *_smallAllocations[8]{}; // 16, 32, 64, 128, 256, 512, 1024, 2048
            threading::spinlock _smallAllocationsLock[8]{};

            big_allocation *_bigAllocations{}; // anything above 2048
            threading::spinlock _bigAllocationsLock{};
            slab *_bigAllocationTracker;

            threading::spinlock _statsLock;
            size_t _totalAllocatedMemory{}; // not aliased
            size_t _totalUsedSystemMemory{}; // including overhead and aliasing
            size_t _totalSmallAllocations{};
            size_t _totalSmallAllocationSize{};
            size_t _totalBigAllocations{};
            size_t _totalBigAllocationSize{};
        };

        static constexpr const char* SUBSYSTEM_NAME = "heap_allocator";
        static constexpr bool IS_REQUIRED = true;
        static constexpr bool TEST_SUBSYSTEM = true;

        [[=subsystems::subsystem_constructor_tag]] heap_allocator();

        Result Initialize();

        state* CreateState(string_view name, piru::memory::paging::state* pageState, bool higherHalf);
        void DestroyState(state* state);

        bool RunTests();

    private:
        state _heapState; // this is the heap, for allocating states, that get handed out.

        struct state_list {
            state_list *next;
            state *value;
        };

        state_list *_states{};
    };
}

#endif //PIRU_HEAP_ALLOCATOR_HPP
