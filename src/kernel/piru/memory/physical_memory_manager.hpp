#ifndef PIRU_PHYSICAL_MEMORY_MANAGER_HPP
#define PIRU_PHYSICAL_MEMORY_MANAGER_HPP

#include <definitions.hpp>
#include <cpp/spinlock.hpp>

namespace piru::memory {
    class physical_memory_manager {
    public:
        void Initialize();

        void *Allocate(size_t pageCount);
        void Free(void *ptr, size_t pageCount);

        [[nodiscard]] size_t GetFreePageCount() const;
        [[nodiscard]] size_t GetTotalPageCount() const;

    private:
        threading::spinlock _lock;
        uint64_t *_reserved{};
        uint64_t *_allocated{};
        size_t _totalPageCount{};
        size_t _availablePageCount{};
    };
} // memory

#endif //PIRU_PHYSICAL_MEMORY_MANAGER_HPP
