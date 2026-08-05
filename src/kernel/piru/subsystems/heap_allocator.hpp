#ifndef PIRU_HEAP_ALLOCATOR_HPP
#define PIRU_HEAP_ALLOCATOR_HPP

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

        struct slab {
            slab *next;
            void *addr;
            size_t sizeClass;
            size_t maxElementCount;
            uint64_t allocationMap[4]; // 256 entries, enough for a size class of 16.

            [[nodiscard]] bool IsFree(size_t index) const {
                for (size_t i = 0; i < 4; i++) {
                    if (i * 64 + index >= maxElementCount) break;
                    if (allocationMap[i] & (1ULL << index)) {
                        return false;
                    }
                }

                return true;
            }

            [[nodiscard]] bool IsEmpty() const {
                for (size_t i = 0; i < maxElementCount; i++) {
                    if (!IsFree(i)) {
                        return false;
                    }
                }
                return true;
            }

            [[nodiscard]] bool IsFull() const {
                for (size_t i = 0; i < maxElementCount; i++) {
                    if (IsFree(i)) {
                        return false;
                    }
                }

                return true;
            }

            [[nodiscard]] bool Has(void* ptr) const {
                size_t index = ((uintptr_t)ptr - (uintptr_t)addr) / sizeClass;
                return index < maxElementCount;
            }

            void Set(size_t index, bool allocated) {
                for (size_t i = 0; i < 4; i++) {
                    if (i * 64 + index >= maxElementCount) break;
                    if (allocated) {
                        allocationMap[i] |= (1ULL << index);
                    } else {
                        allocationMap[i] &= ~(1ULL << index);
                    }
                }
            }

            void* Allocate() {
                for (size_t i = 0; i < maxElementCount; i++) {
                    if (IsFree(i)) {
                        Set(i, true);
                        return (void*)((uintptr_t)addr + i * sizeClass);
                    }
                }

                return nullptr;
            }

            void Give(void *ptr) {
                size_t index = ((uintptr_t)ptr - (uintptr_t)addr) / sizeClass;
                Set(index, false);
            }
        };

        struct slab_allocator {
            slab_allocator();
            ~slab_allocator();

            static memory::paging::available_virtual_address_range GetVirtualAddress(
                memory::paging::state *pageState, bool higherHalf,
                memory::paging *paging);

            slab *AllocateSlab(size_t sizeClass, memory::paging::state *pageState, bool higherHalf);
            void FreeSlab(slab *slab, memory::paging::state *pageState);

        private:
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

        [[=subsystems::subsystem_constructor_tag]] heap_allocator();

        Result Initialize();

        state* CreateState(string_view name, piru::memory::paging::state* pageState, bool higherHalf);
        void DestroyState(state* state);

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
