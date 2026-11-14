#include "physical_memory_manager.hpp"
#include <boot/limine_definitions.hpp>

#include <logging.hpp>

namespace {
    inline bool IsPageSet(uint64_t *bitmap, size_t pageIndex) {
        size_t bitmapIndex = pageIndex / 64;
        if (bitmap[bitmapIndex] == UINT64_MAX) return true;

        size_t bitIndex = pageIndex % 64;
        return (bitmap[bitmapIndex] & (1ULL << bitIndex)) != 0;
    }

    inline void SetPage(uint64_t *bitmap, size_t pageIndex, bool value) {
        size_t bitmapIndex = pageIndex / 64;
        size_t bitIndex = pageIndex % 64;
        if (value) {
            bitmap[bitmapIndex] |= (1ULL << bitIndex);
        } else {
            bitmap[bitmapIndex] &= ~(1ULL << bitIndex);
        }
    }
}

namespace piru::memory {
    void physical_memory_manager::Initialize() {
        auto memmap = boot::limine::memmap_request.response;
        if (!memmap)
            PANIC("Failed to get memory map from bootloader");

        struct {
            uintptr_t start;
            uintptr_t end;
        } validEntries[boot::limine::memmap_request.response->entry_count];
        size_t validEntryCount = 0;
        uintptr_t highestAddress = 0;

        for (size_t i = 0; i < memmap->entry_count; i++) {
            auto entry = memmap->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE && entry->base > constants::MiB) {
                validEntries[validEntryCount++] = { entry->base, entry->base + entry->length };
                highestAddress = math::Max(highestAddress, validEntries[validEntryCount - 1].end);
            }
        }

        highestAddress = math::AlignDown(highestAddress, system::PageSize);

        _totalPageCount = math::AlignDown(highestAddress, system::PageSize) / system::PageSize;

        uintptr_t bitmapSize = (_totalPageCount / 64) + 1;
        uintptr_t requiredSize = bitmapSize * 2 * sizeof(uint64_t);

        uintptr_t bitmapStart = 0;

        for (size_t i = 0; i < validEntryCount; i++) {
            auto start = math::AlignUp(validEntries[i].start, system::PageSize);
            auto end = math::AlignDown(validEntries[i].end, system::PageSize);
            auto size = (end > start) ? (end - start) : 0;

            if (size >= requiredSize) {
                bitmapStart = start + boot::limine::hhdm_request.response->offset;
                validEntries[i].start = start + requiredSize;
                break;
            }
        }

        _reserved = (uint64_t*)bitmapStart;
        _allocated = (uint64_t*)(bitmapStart + bitmapSize * sizeof(uint64_t));

        for (size_t page = 0; page < _totalPageCount; page++) {
            _reserved[page / 64] = UINT64_MAX; // Reserve everything
            _allocated[page / 64] = UINT64_MAX;
        }

        for (size_t i = 0; i < validEntryCount; i++) {
            auto start = math::AlignUp(validEntries[i].start, system::PageSize);
            auto end = math::AlignDown(validEntries[i].end, system::PageSize);

            for (uintptr_t addr = start; addr < end; addr += system::PageSize) {
                size_t pageIndex = addr / system::PageSize;
                SetPage(_reserved, pageIndex, false);
                SetPage(_allocated, pageIndex, false);
            }
        }

        for (size_t i = 0; i < (1 * constants::MiB) / system::PageSize; i++) {
            SetPage(_reserved, i, true);
        }

        auto bitmapBeginning = (uintptr_t)(bitmapStart - boot::limine::hhdm_request.response->offset) / system::PageSize;
        auto bitmapEnding = (uintptr_t)(math::AlignUp(bitmapStart + requiredSize, system::PageSize) - boot::limine::hhdm_request.response->offset) / system::PageSize;

        for (size_t i = bitmapBeginning; i < bitmapEnding; i++) {
            SetPage(_reserved, i, true);
        }

        for (size_t i = 0; i < _totalPageCount; i++) {
            if (!IsPageSet(_reserved, i)) {
                _availablePageCount++;
                SetPage(_allocated, i, false);
            }
        }

        LOG_INFO("Total pages: {} ({} MiB), free pages: {} ({} MiB)", _totalPageCount, (_totalPageCount * system::PageSize) / (1024 * 1024), _availablePageCount, (_availablePageCount * system::PageSize) / (1024 * 1024));
    }

    void *physical_memory_manager::Allocate(size_t pageCount) {
        threading::spinlock::scoped_lock lock(_lock, true);

        size_t currentPage = 0;
        size_t run = 0;

        while (currentPage < _totalPageCount) {
            if (IsPageSet(_reserved, currentPage) || IsPageSet(_allocated, currentPage)) {
                run = 0;
            } else {
                run++;
            }

            if (run == pageCount) {
                size_t startPage = currentPage - pageCount + 1;

                for (size_t i = startPage; i <= currentPage; i++) {
                    SetPage(_allocated, i, true);
                }

                _availablePageCount -= pageCount;
                return (void*)(startPage * system::PageSize);
            }

            currentPage++;
        }

        return nullptr;
    }

    void physical_memory_manager::Free(void *ptr, size_t pageCount) {
        threading::spinlock::scoped_lock lock(_lock, true);

        for (size_t i = 0; i < pageCount; i++) {
            size_t pageIndex = ((uintptr_t)ptr / system::PageSize) + i;
            if (IsPageSet(_reserved, pageIndex)) PANIC("Attempted to free a reserved page");
            if (!IsPageSet(_allocated, pageIndex)) PANIC("Attempted to free an already freed page");
            SetPage(_allocated, pageIndex, false);
        }

        _availablePageCount += pageCount;
    }

    size_t physical_memory_manager::GetFreePageCount() const {
        return _availablePageCount;
    }

    size_t physical_memory_manager::GetTotalPageCount() const {
        return _totalPageCount;
    }
} // memory