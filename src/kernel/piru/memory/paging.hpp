#ifndef PIRU_PAGING_HPP
#define PIRU_PAGING_HPP

#include <definitions.hpp>

#include "physical_memory_manager.hpp"
#include "cpp/spinlock.hpp"
#include <atomic>

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
        [[nodiscard]] state CreatePageState();
        // Frees the lower-half page tables (PML4 slots 0-255) and the PML4 itself. Leaf frames are NOT freed,
        // whoever mapped them owns them. The shared higher half (256-511) is never touched.
        // The state must not be active (in CR3) on any CPU.
        void DestroyPageState(state *state);

        void MapPage(state *state, uint64_t vaddr, uint64_t paddr, Flags flags);
        void UnmapPage(state *state, uint64_t vaddr);

        void MapRange(state *state, uint64_t vaddrStart, uint64_t paddrStart, size_t pageCount, Flags flags);
        void UnmapRange(state *state, uint64_t vaddrStart, size_t pageCount);

        uint64_t GetPhysicalAddress(state *state, uint64_t vaddr);
        [[nodiscard]] bool IsMapped(state *state, uint64_t vaddr);

        available_virtual_address_range FindAvailableVirtualAddressRange(state *state, uintptr_t start, uintptr_t end, size_t pageCount, size_t align = 0);
        available_virtual_address_range FindAvailableVirtualAddressRangeKernel(size_t count, size_t align = 0);

        // Finds a free range AND maps it while holding _findRangeLock, so two CPUs can never claim the same range.
        // Prefer these over FindAvailable* + MapRange, which races (the range is only "taken" once it is mapped).
        available_virtual_address_range MapAnywhere(state *state, uintptr_t start, uintptr_t end, uint64_t paddrStart, size_t pageCount, Flags flags, size_t align = 0);
        available_virtual_address_range MapAnywhereKernel(uint64_t paddrStart, size_t pageCount, Flags flags, size_t align = 0);

        void Shootdown(uint64_t start, size_t pageCount);

        static void InvalidateTlb(uint64_t vaddr);
        static void ReloadCR3();
        static void IRQShootdown();
        void HandleShootdown();

        void EnableShootdown();

    private:
        threading::spinlock _lock{};
        threading::spinlock _shootdownLock{};
        threading::spinlock _findRangeLock{};
        physical_memory_manager& _pmm;
        state _kernelState{};
        struct {
            std::atomic<uint64_t> gen; uint64_t start; size_t pageCount; std::atomic<int> pending;
        } _shootdownRequest{};
        bool _shootdownEnabled;
        int _shootdownCount; // amount pending.
        std::atomic<uint64_t> _shootdownGenNumber = 1;

        static constexpr uint64_t PointerMask = 0x000FFFFFFFFFF000; // Mask to get the address portion of a page table entry
        static constexpr uint64_t FlagsMask = 0xFFF0000000000FFF; // Mask to get the flags portion of a page table entry

        void ReserveHigherHalfPML4Slots(state *state, uint64_t higherHalf) const;
        void CopyExistingPageTableToNew(state *state, uint64_t higherHalf);
        void DeepCopyPageTables(uint32_t level, uintptr_t srcPhys, uintptr_t dstPhys, uint64_t higherHalf);
        threading::spinlock & LockFor(state * state, uint64_t vaddr);
        available_virtual_address_range FindAvailableVirtualAddressRangeUnlocked(state *state, uintptr_t start, uintptr_t end, size_t pageCount, size_t align);
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
