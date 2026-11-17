#ifndef PIRU_SMP_HPP
#define PIRU_SMP_HPP

#include <definitions.hpp>
#include <cpp/ringbuffer.hpp>
#include "cpp/spinlock.hpp"
#include <atomic>

namespace piru::core {
    class smp {
    public:
        using CallFunction = void(*)(void* data);
        using WaitType = std::atomic<int>*;

        static void CallOnVector();
        static void CallOn(uint32_t cpu, CallFunction func, void* data, WaitType wait = nullptr);
        static void CallOnAll(CallFunction func, void* data, bool wait = false, bool self = false);

    private:
        struct QueuedOp {
            CallFunction func;
            void* data;
            WaitType wait; // decremented by one, on completion.
        };

        using SmpQueue = ringbuffer<QueuedOp, 32>;

        struct CpuQueue {
            SmpQueue queue{};
            threading::spinlock lock;
        };

        static CpuQueue _queues[system::MaxCpuCount];
        static std::atomic<bool> _draining[system::MaxCpuCount];

        static void DrainQueue(CpuQueue &queue, SmpQueue &copiedQueue);
    };
}

#endif //PIRU_SMP_HPP
