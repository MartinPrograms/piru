#ifndef PIRU_SCHEDULER_HPP
#define PIRU_SCHEDULER_HPP

#include "subsystem.hpp"
#include "heap_allocator.hpp"
#include <cpp/list.hpp>

namespace piru::subsystems {
    struct scheduler {
        using callback = void(*)(uint64_t, void*);

        struct task {
            uint64_t id;
            uint64_t absolute_time;
            void *data;
            callback cb;
            uint64_t periodic;
        };

        static constexpr const char* SUBSYSTEM_NAME = "scheduler";
        static constexpr bool IS_REQUIRED = true;

        [[=subsystem_constructor_tag]] scheduler(heap_allocator& allocator);

        uint64_t Add(callback cb, void* data, uint64_t delayInMs, bool periodic);
        void Remove(uint64_t id);

        Result Initialize();

        void Run();

    private:
        heap_allocator& _allocator;
        threading::spinlock _lock;
        list<task> _tasks;

        void BubbleUp(size_t index);
        void BubbleDown(size_t index);

        uint64_t GetNewId() {
            threading::spinlock::scoped_lock lock(_lock, true);
            static uint64_t idCounter = 0;
            return ++idCounter;
        }

        uint64_t GetCurrentTime();
        void HardwareSleep(uint64_t durationInMs);
    };
}

#endif //PIRU_SCHEDULER_HPP
