#include "paging.hpp"

#include "kernel.hpp"
#include "logging.hpp"
#include "boot/limine_definitions.hpp"

namespace piru::memory {
    paging::paging(physical_memory_manager& pmm) : _pmm(pmm) { }

    void paging::Initialize() {
        uintptr_t pml4Phys = (uintptr_t)_pmm.Allocate(1);
        if (!pml4Phys)
            PANIC("Failed to allocate memory for kernel PML4!");

        uint64_t higherHalf = boot::limine::hhdm_request.response->offset;
        memset((void*)(pml4Phys + higherHalf), 0, sizeof(PML4));

        _kernelState.pml4 = (PML4*)pml4Phys;

        CopyExistingPageTableToNew(&_kernelState, higherHalf);
        ReserveHigherHalfPML4Slots(&_kernelState, higherHalf);

        LOG_INFO("Kernel page table initialized at physical address: {}", (void*)pml4Phys);
    }

    void paging::SwitchToKernelPageTable() {
        SwitchToPageTable(&_kernelState);
    }

    void paging::SwitchToPageTable(state *state) {
        asm volatile("mov %0, %%cr3" :: "r"(reinterpret_cast<uintptr_t>(state->pml4)) : "memory");
        kernel::GetInstance().GetCpu().pageState = state;
    }

    paging::state paging::CreatePageState() {
        auto pml4Phys = (uintptr_t)_pmm.Allocate(1);
        if (!pml4Phys)
            PANIC("Failed to allocate memory for PML4!");

        uint64_t higherHalf = boot::limine::hhdm_request.response->offset;
        memset((void*)(pml4Phys + higherHalf), 0, sizeof(PML4));

        auto pml4State = reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(pml4Phys) + higherHalf);
        auto kernelState = reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(_kernelState.pml4) + higherHalf);

        for (size_t i = 256; i < 512; i++) {
            if (!(kernelState[i] & (uint64_t)Flags::Present))
                PANIC("Page entry not valid, should not happen unless CreatePageState ran BEFORE Initialize");

            pml4State[i] = kernelState[i];
        }

        return {(PML4*)pml4Phys};
    }

    void paging::DestroyPageState(state *state) {
        if (!state || !state->pml4)
            PANIC("DestroyPageState: state has no PML4");
        if (state == &_kernelState)
            PANIC("DestroyPageState: refusing to destroy the kernel page state");
        if (kernel::GetInstance().GetCpu().pageState == state)
            PANIC("DestroyPageState: state is active on this CPU, switch away first");

        const uint64_t hhdm = boot::limine::hhdm_request.response->offset;
        const auto present = static_cast<uint64_t>(Flags::Present);
        const auto huge = static_cast<uint64_t>(Flags::HugePage);
        auto table = [hhdm](uintptr_t phys) { return reinterpret_cast<uint64_t*>(phys + hhdm); };

        threading::spinlock::scoped_lock lock(state->lock, true);

        auto* pml4 = table(reinterpret_cast<uintptr_t>(state->pml4));
        for (size_t i = 0; i < 256; i++) { // lower half only, 256-511 is shared with every other state
            if (!(pml4[i] & present)) continue;
            uintptr_t pdpPhys = pml4[i] & PointerMask;
            auto* pdp = table(pdpPhys);

            for (size_t j = 0; j < 512; j++) {
                if (!(pdp[j] & present)) continue;
                if (pdp[j] & huge) PANIC("DestroyPageState: 1 GiB pages are not supported");
                uintptr_t pdPhys = pdp[j] & PointerMask;
                auto* pd = table(pdPhys);

                for (size_t k = 0; k < 512; k++) {
                    if (!(pd[k] & present)) continue;
                    if (pd[k] & huge) PANIC("DestroyPageState: 2 MiB pages are not supported");
                    _pmm.Free(reinterpret_cast<void*>(pd[k] & PointerMask), 1); // PT, its leaf frames belong to the mapper
                }
                _pmm.Free(reinterpret_cast<void*>(pdPhys), 1);
            }
            _pmm.Free(reinterpret_cast<void*>(pdpPhys), 1);
            pml4[i] = 0;
        }

        _pmm.Free(reinterpret_cast<void*>(state->pml4), 1);
        state->pml4 = nullptr;
    }

    void paging::MapPage(state *state, uint64_t vaddr, uint64_t paddr, Flags flags) {
        if (vaddr & (system::PageSize - 1)) PANIC("Virtual address is not page-aligned!");
        if (paddr & (system::PageSize - 1)) PANIC("Physical address is not page-aligned!");

        threading::spinlock::scoped_lock lock(LockFor(state, vaddr), true);

        auto offset = boot::limine::hhdm_request.response->offset;

        uint64_t entryFlags = static_cast<uint64_t>(flags) | static_cast<uint64_t>(Flags::Present);
        auto directoryFlags = static_cast<uint64_t>(Flags::Present | Flags::ReadWrite | Flags::User);

        uint32_t pml4Index, pdpIndex, pdIndex, ptIndex, pageOffset;
        ExtractPageTableIndices(vaddr, pml4Index, pdpIndex, pdIndex, ptIndex, pageOffset);

        PML4* pml4 = reinterpret_cast<PML4 *>(reinterpret_cast<uint64_t>(state->pml4) + offset);
        if (!pml4->entries[pml4Index]) {
            // Allocate a new PDP table
            PDP* newPdp = reinterpret_cast<PDP *>(_pmm.Allocate(1));
            if (!newPdp) PANIC("Failed to allocate memory for new PDP table!");
            __builtin_memset(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(newPdp) + offset), 0, sizeof(PDP)); // Clear the new table
            pml4->entries[pml4Index] = reinterpret_cast<uint64_t>(newPdp) | directoryFlags;
        }

        uintptr_t pdpPhysicalAddress = pml4->entries[pml4Index] & PointerMask;
        PDP* pdp = reinterpret_cast<PDP *>(reinterpret_cast<uint64_t>(pdpPhysicalAddress) + offset);
        if (!pdp->entries[pdpIndex]) {
            // Allocate a new PD table
            PD* newPd = reinterpret_cast<PD *>(_pmm.Allocate(1));
            if (!newPd) PANIC("Failed to allocate memory for new PD table!");
            __builtin_memset(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(newPd) + offset), 0, sizeof(PD)); // Clear the new table
            pdp->entries[pdpIndex] = reinterpret_cast<uint64_t>(newPd) | directoryFlags;
        }

        uintptr_t pdPhysicalAddress = pdp->entries[pdpIndex] & PointerMask;
        PD* pd = reinterpret_cast<PD *>(reinterpret_cast<uint64_t>(pdPhysicalAddress) + offset);
        if (!pd->entries[pdIndex]) {
            // Allocate a new PT table
            PT* newPt = reinterpret_cast<PT *>(_pmm.Allocate(1));
            if (!newPt) PANIC("Failed to allocate memory for new PT table!");
            __builtin_memset(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(newPt) + offset), 0, sizeof(PT)); // Clear the new table
            pd->entries[pdIndex] = reinterpret_cast<uint64_t>(newPt) | directoryFlags;
        }

        uintptr_t ptPhysicalAddress = pd->entries[pdIndex] & PointerMask;
        PT* pt = reinterpret_cast<PT *>(reinterpret_cast<uint64_t>(ptPhysicalAddress) + offset);
        if (pt->entries[ptIndex] & static_cast<uint64_t>(Flags::Present)) {
            LOG_ERROR("Virtual address {} is already mapped to physical address {} with flags {}", vaddr, pt->entries[ptIndex] & PointerMask, pt->entries[ptIndex] & FlagsMask);
            PANIC("Virtual address is already mapped");
        }

        pt->entries[ptIndex] = (paddr & PointerMask) | entryFlags;
        asm volatile("invlpg (%0)" ::"r"(vaddr) : "memory"); // Invalidate the TLB entry for this page to ensure the new mapping is used immediately
    }

    void paging::UnmapPage(state *state, uint64_t vaddr) {
        uint32_t pml4Index, pdpIndex, pdIndex, ptIndex, pageOffset;
        ExtractPageTableIndices(vaddr, pml4Index, pdpIndex, pdIndex, ptIndex, pageOffset);

        PML4* pml4{nullptr};
        PDP* pdp{nullptr};
        PD* pd{nullptr};
        PT* pt{nullptr};

        uintptr_t pdpPhysicalAddress{0};
        uintptr_t pdPhysicalAddress{0};
        uintptr_t ptPhysicalAddress{0};

        bool shouldFreePt{false};
        bool shouldFreePd{false};
        bool shouldFreePdp{false};

        {
            threading::spinlock::scoped_lock lock(LockFor(state, vaddr), true);

            // Reverse of the mapping process, but we also need to check if the page tables are present at each level and panic if we try to unmap an address that isn't mapped
            auto offset = boot::limine::hhdm_request.response->offset;

            pml4 = reinterpret_cast<PML4 *>(reinterpret_cast<uint64_t>(state->pml4) + offset);
            if (!(pml4->entries[pml4Index] & static_cast<uint64_t>(Flags::Present))) PANIC("Attempted to unmap a virtual address that is not mapped (PML4 entry not present)!");

            pdpPhysicalAddress = pml4->entries[pml4Index] & PointerMask;
            pdp = reinterpret_cast<PDP *>(reinterpret_cast<uint64_t>(pdpPhysicalAddress) + offset);
            if (!(pdp->entries[pdpIndex] & static_cast<uint64_t>(Flags::Present))) PANIC("Attempted to unmap a virtual address that is not mapped (PDP entry not present)!");

            pdPhysicalAddress = pdp->entries[pdpIndex] & PointerMask;
            pd = reinterpret_cast<PD *>(reinterpret_cast<uint64_t>(pdPhysicalAddress) + offset);
            if (!(pd->entries[pdIndex] & static_cast<uint64_t>(Flags::Present))) PANIC("Attempted to unmap a virtual address that is not mapped (PD entry not present)!");

            ptPhysicalAddress = pd->entries[pdIndex] & PointerMask;
            pt = reinterpret_cast<PT *>(reinterpret_cast<uint64_t>(ptPhysicalAddress) + offset);
            if (!(pt->entries[ptIndex] & static_cast<uint64_t>(Flags::Present))) PANIC("Attempted to unmap a virtual address that is not mapped (PT entry not present)!");

            pt->entries[ptIndex] = 0; // Clear the entry to unmap the page

            shouldFreePt = IsTableEmpty(reinterpret_cast<uint64_t*>(pt));
            if (shouldFreePt)
                pd->entries[pdIndex] = 0;

            shouldFreePd = IsTableEmpty(reinterpret_cast<uint64_t*>(pd));
            if (shouldFreePd)
                pdp->entries[pdpIndex] = 0;

            shouldFreePdp = IsTableEmpty(reinterpret_cast<uint64_t*>(pdp));
            if (shouldFreePdp && pml4Index < 256)
                pml4->entries[pml4Index] = 0;

            asm volatile("invlpg (%0)" ::"r"(vaddr) : "memory"); // Invalidate the TLB entry for this page to ensure the unmapping takes effect immediately
        }

        if (vaddr >= system::EndOfCanonical && _shootdownEnabled) {
            Shootdown(vaddr, 1);
        }

        // Check if PT is now empty, and if so, free it and clear the PD entry
        if (shouldFreePt) {
            _pmm.Free((void*)ptPhysicalAddress, 1);

            // Check if PD is now empty, and if so, free it and clear the PDP entry
            if (shouldFreePd) {
                _pmm.Free((void*)pdPhysicalAddress, 1);

                // Check if PDP is now empty, and if so, free it and clear the PML4 entry
                // Do not free kernel page tables.
                if (shouldFreePdp && pml4Index < 256) {
                    _pmm.Free((void*)pdpPhysicalAddress, 1);
                }
            }
        }
    }

    void paging::MapRange(state *state, uint64_t vaddrStart, uint64_t paddrStart, size_t pageCount, Flags flags) {
        for (size_t i = 0; i < pageCount; ++i) {
            MapPage(state, vaddrStart + i * system::PageSize, paddrStart + i * system::PageSize, flags);
        }
    }

    void paging::UnmapRange(state *state, uint64_t vaddrStart, size_t pageCount) {
        for (size_t i = 0; i < pageCount; ++i) {
            UnmapPage(state, vaddrStart + i * system::PageSize);
        }
    }

    uint64_t paging::GetPhysicalAddress(state *state, uint64_t vaddr) {
        threading::spinlock::scoped_lock lock(LockFor(state, vaddr), true);

        uint32_t pml4Index, pdpIndex, pdIndex, ptIndex, pageOffset;
        ExtractPageTableIndices(vaddr, pml4Index, pdpIndex, pdIndex, ptIndex, pageOffset);

        auto offset = boot::limine::hhdm_request.response->offset;

        auto pml4 = reinterpret_cast<PML4 *>(reinterpret_cast<uint64_t>(state->pml4) + offset);
        if (!(pml4->entries[pml4Index] & static_cast<uint64_t>(Flags::Present))) return 0;

        uintptr_t pdpPhysicalAddress = pml4->entries[pml4Index] & PointerMask;
        auto pdp = reinterpret_cast<PDP *>(reinterpret_cast<uint64_t>(pdpPhysicalAddress) + offset);
        if (!(pdp->entries[pdpIndex] & static_cast<uint64_t>(Flags::Present))) return 0;

        uintptr_t pdPhysicalAddress = pdp->entries[pdpIndex] & PointerMask;
        auto pd = reinterpret_cast<PD *>(reinterpret_cast<uint64_t>(pdPhysicalAddress) + offset);
        if (!(pd->entries[pdIndex] & static_cast<uint64_t>(Flags::Present))) return 0;

        uintptr_t ptPhysicalAddress = pd->entries[pdIndex] & PointerMask;
        auto pt = reinterpret_cast<PT *>(reinterpret_cast<uint64_t>(ptPhysicalAddress) + offset);
        if (!(pt->entries[ptIndex] & static_cast<uint64_t>(Flags::Present))) return 0;

        uintptr_t pagePhysicalAddress = pt->entries[ptIndex] & PointerMask;
        return pagePhysicalAddress | pageOffset;
    }

    bool paging::IsMapped(state *state, uint64_t vaddr) {
        return (GetPhysicalAddress(state, vaddr) != 0);
    }

    paging::available_virtual_address_range paging::FindAvailableVirtualAddressRange(state *state, uintptr_t start,
                                                                uintptr_t end, size_t pageCount, size_t align) {
        threading::spinlock::scoped_lock lock (_findRangeLock, true);
        return FindAvailableVirtualAddressRangeUnlocked(state, start, end, pageCount, align);
    }

    // Lock order: _findRangeLock -> paging _lock / state->lock -> pmm lock. Never take _findRangeLock while holding those.
    paging::available_virtual_address_range paging::MapAnywhere(state *state, uintptr_t start, uintptr_t end,
                                                                uint64_t paddrStart, size_t pageCount, Flags flags, size_t align) {
        threading::spinlock::scoped_lock lock (_findRangeLock, true);
        auto range = FindAvailableVirtualAddressRangeUnlocked(state, start, end, pageCount, align);
        MapRange(state, range.start, paddrStart, pageCount, flags);
        return range;
    }

    paging::available_virtual_address_range paging::MapAnywhereKernel(uint64_t paddrStart, size_t pageCount, Flags flags, size_t align) {
        uintptr_t start = 0xFFFFFFFF80000000ULL;
        uintptr_t end = 0xFFFFFFFFFFFFF000ULL;
        // Higher half is shared between all page tables, so mapping through _kernelState is visible everywhere.
        return MapAnywhere(&_kernelState, start, end, paddrStart, pageCount, flags, align);
    }

    paging::available_virtual_address_range paging::FindAvailableVirtualAddressRangeUnlocked(state *state, uintptr_t start,
                                                                uintptr_t end, size_t pageCount, size_t alignv) {
        auto align = math::Max(alignv, system::PageSize);

        if (pageCount == 0) {
            PANIC("Why are you calling FindAvailableVirtualAddressRangeUnlocked without any pages?");
        }

        if (align % system::PageSize != 0) {
            PANIC("Align is not aligned to page-size");
        }

        uintptr_t current = math::AlignUp(start, align);
        size_t foundPages = 0;

        while (current < end) {
            if (!IsMapped(state, current)) {
                foundPages++;
                if (foundPages == pageCount) {
                    uintptr_t rangeStart = current - (pageCount - 1) * system::PageSize;
                    return { rangeStart, rangeStart + (pageCount * system::PageSize) };
                }

                current += system::PageSize;
            } else {
                foundPages = 0;
                current += system::PageSize;
                current = math::AlignUp(current, align);
            }
        }

        PANIC("Failed to find available virtual address range!");
    }

    paging::available_virtual_address_range paging::FindAvailableVirtualAddressRangeKernel(size_t count, size_t align) {
        uintptr_t start = 0xFFFFFFFF80000000ULL;
        uintptr_t end = 0xFFFFFFFFFFFFF000ULL;
        return FindAvailableVirtualAddressRange(&_kernelState, start, end, count, align);
    }

    void paging::Shootdown(uint64_t start, size_t pageCount) {
        auto flags = threading::spinlock::AreInterruptsEnabled();
        asm volatile ("cli" ::: "memory");

        while (!_shootdownLock.try_lock()) {
            HandleShootdown();
            asm ("pause");
        }

        auto newGen = _shootdownGenNumber.fetch_add(1);

        _shootdownRequest.start = start;
        _shootdownRequest.pageCount = pageCount;
        _shootdownRequest.pending = _shootdownCount;
        _shootdownRequest.gen.store(newGen, std::memory_order::release);

        // Ping all other CPUs
        kernel::GetInstance().GetCpu().lastShootdownGen = newGen;
        kernel::GetInstance().GetCpu().apic.SendIPIAllExcludingSelf(interrupts::idt::PAGING_TLB_SHOOTDOWN);

        while (_shootdownRequest.pending.load() != 0) {
            // wait for all CPUs to register...
            asm ("pause");
        }

        // Should be done here
        _shootdownLock.unlock(flags);
    }

    void paging::InvalidateTlb(uint64_t vaddr) {
        asm volatile ("invlpg (%0)" ::"r"(vaddr) : "memory");
    }

    void paging::ReloadCR3() {
        asm volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    }

    void paging::IRQShootdown() {
        kernel::GetInstance().data.paging.HandleShootdown();
    }

    void paging::HandleShootdown() {
        if (threading::spinlock::AreInterruptsEnabled()) PANIC("HandleShootdown with IF=1");

        auto &state = kernel::GetInstance().GetCpu();
        auto gen = _shootdownRequest.gen.load(std::memory_order::acquire);

        if (state.lastShootdownGen == gen)
            return;

        for (size_t i = 0; i < _shootdownRequest.pageCount; i++)
            InvalidateTlb(_shootdownRequest.start + (i * system::PageSize));

        state.lastShootdownGen = gen;
        _shootdownRequest.pending.fetch_sub(1);
    }

    void paging::EnableShootdown() {
        _shootdownEnabled = true;
        _shootdownCount = (int)kernel::GetInstance().GetCpuCount() - 1; // minus 1 for the sender
    }

    void paging::ReserveHigherHalfPML4Slots(state *state, uint64_t higherHalf) const {
        auto *pml4 = reinterpret_cast<PML4 *>(reinterpret_cast<uintptr_t>(state->pml4) + higherHalf);
        auto flags = static_cast<uint64_t>(Flags::Present | Flags::ReadWrite);

        for (int i = 256; i < 512; i++) { // canonical higher half
            if (pml4->entries[i] & static_cast<uint64_t>(Flags::Present)) continue;

            uintptr_t pdpPhys = (uintptr_t)_pmm.Allocate(1);
            if (!pdpPhys) PANIC("Failed to allocate memory for reserved PDP table!");
            __builtin_memset(reinterpret_cast<void *>(pdpPhys + higherHalf), 0, sizeof(PDP));
            pml4->entries[i] = pdpPhys | flags;
        }
    }

    void paging::CopyExistingPageTableToNew(state *state, uint64_t higherHalf) {
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));

        auto *liminePml4 = reinterpret_cast<PML4 *>(cr3 + higherHalf);
        auto *newPml4 = reinterpret_cast<PML4 *>(reinterpret_cast<uintptr_t>(state->pml4) + higherHalf);

        for (int i = 0; i < 512; i++) {
            if (!(liminePml4->entries[i] & static_cast<uint64_t>(Flags::Present))) continue;

            uint64_t srcPhys = liminePml4->entries[i] & PointerMask;
            uintptr_t dstPhys = (uintptr_t) _pmm.Allocate(1);
            if (!dstPhys)
                PANIC("Failed to allocate memory for page table during deep copy!");

            DeepCopyPageTables(3, srcPhys, dstPhys, higherHalf);
            newPml4->entries[i] = (liminePml4->entries[i] & FlagsMask) | dstPhys;
        }
    }

    void paging::DeepCopyPageTables(uint32_t level, uintptr_t srcPhys, uintptr_t dstPhys, uint64_t higherHalf) {
        auto srcTable = reinterpret_cast<uint64_t*>(srcPhys + higherHalf);
        auto dstTable = reinterpret_cast<uint64_t*>(dstPhys + higherHalf);

        for (int i = 0; i < 512; i++) {
            if (!(srcTable[i] & static_cast<uint64_t>(Flags::Present))) continue;

            // If the entry is a huge page, we can copy it directly, since huge pages are leaf entries.
            if ((level == 3 || level == 2) && (srcTable[i] & static_cast<uint64_t>(Flags::HugePage))) {
                dstTable[i] = srcTable[i];
                continue;
            }

            // If level is 1, we are at the PT level and can copy the entries directly
            if (level == 1) {
                dstTable[i] = srcTable[i];
                continue;
            }

            // Otherwise, allocate a new table for the next level
            uintptr_t newTablePhys = (uintptr_t)_pmm.Allocate(1);
            memset(reinterpret_cast<void*>(newTablePhys + higherHalf), 0, 4096);

            // Link the new table into the destination hierarchy
            uint64_t flags = srcTable[i] & FlagsMask;
            dstTable[i] = newTablePhys | flags;

            uintptr_t nextSrcPhys = srcTable[i] & PointerMask;
            uintptr_t nextDstPhys = newTablePhys;
            DeepCopyPageTables(level - 1, nextSrcPhys, nextDstPhys, higherHalf);
        }
    }

    threading::spinlock & paging::LockFor(state *state, uint64_t vaddr) {
        return (vaddr >= system::EndOfCanonical) ? _lock : state->lock;
    }

    bool paging::IsTableEmpty(const uint64_t *table) {
        for (size_t i = 0; i < 512; i++) {
            if (table[i] & static_cast<uint64_t>(Flags::Present)) return false;
        }
        return true;
    }

    void paging::ExtractPageTableIndices(uint64_t vaddr, uint32_t &pml4Index, uint32_t &pdpIndex, uint32_t &pdIndex,
                                         uint32_t &ptIndex, uint32_t &pageIndex) {
        pml4Index = (vaddr >> 39) & 0x1FF;
        pdpIndex = (vaddr >> 30) & 0x1FF;
        pdIndex = (vaddr >> 21) & 0x1FF;
        ptIndex = (vaddr >> 12) & 0x1FF;
        pageIndex = vaddr & 0xFFF; // lower 12 bits are the offset within the page
    }
}
