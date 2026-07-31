#ifndef PIRU_PAGING_HPP
#define PIRU_PAGING_HPP

#include <definitions.hpp>

#include "physical_memory_manager.hpp"
#include "cpp/spinlock.hpp"

namespace piru::memory {
    class paging {
    private:
        struct PT { uint64_t entries[512]; } ALIGNED (system::PageSize);
        struct PD { uint64_t entries[512]; } ALIGNED (system::PageSize);
        struct PDP { uint64_t entries[512]; } ALIGNED (system::PageSize);
        struct PML4 { uint64_t entries[512]; } ALIGNED (system::PageSize);
    public:
        enum class Flags : uint64_t {
            Present = 1 << 0,
            ReadWrite = 1 << 1,
            User = 1 << 2,
            WriteThrough = 1 << 3,
            CacheDisable = 1 << 4,
            Accessed = 1 << 5,
            Dirty = 1 << 6,
            HugePage = 1 << 7,
            Global = 1 << 8,
            NoExecute = 1ULL << 63,
        };

        struct state {
            PML4 *pml4{};
            threading::spinlock lock{};
        };

        struct available_virtual_address_range {
            uint64_t start;
            uint64_t end;
        };

        explicit paging(physical_memory_manager& pmm);

        void Initialize();
        void SwitchToKernelPageTable();
        void SwitchToPageTable(state *state);

        void MapPage(state *state, uint64_t vaddr, uint64_t paddr, Flags flags);
        void UnmapPage(state *state, uint64_t vaddr);

        uint64_t GetPhysicalAddress(state *state, uint64_t vaddr);
        [[nodiscard]] bool IsMapped(state *state, uint64_t vaddr);

        available_virtual_address_range FindAvailableVirtualAddressRange(state *state, uintptr_t start, uintptr_t end, size_t pageCount);
        available_virtual_address_range FindAvailableVirtualAddressRangeKernel(size_t count);

    private:
        threading::spinlock _lock{};
        physical_memory_manager& _pmm;
        state _kernelState{};

        static constexpr uint64_t PointerMask = 0x000FFFFFFFFFF000; // Mask to get the address portion of a page table entry
        static constexpr uint64_t FlagsMask = 0xFFF0000000000FFF; // Mask to get the flags portion of a page table entry

        void ReserveHigherHalfPML4Slots(state *state, uint64_t higherHalf) const;
        void CopyExistingPageTableToNew(state *state, uint64_t higherHalf);
        void DeepCopyPageTables(uint32_t level, uintptr_t srcPhys, uintptr_t dstPhys, uint64_t higherHalf);
        static bool IsTableEmpty(const uint64_t *table);
        static inline void ExtractPageTableIndices(uint64_t vaddr, uint32_t& pml4Index, uint32_t& pdpIndex, uint32_t& pdIndex, uint32_t& ptIndex, uint32_t& pageIndex);
    };

    constexpr paging::Flags operator|(paging::Flags a, paging::Flags b) {
        return static_cast<paging::Flags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
    }

    constexpr paging::Flags operator&(paging::Flags a, paging::Flags b) {
        return static_cast<paging::Flags>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
    }
}

#endif //PIRU_PAGING_HPP
