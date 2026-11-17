#include <cpp/spinlock.hpp>
#include <definitions.hpp>
#include <atomic>

namespace {
    std::atomic<piru::threading::spinlock::relax_hook> _hook = nullptr;
}

namespace piru::threading {

    spinlock::spinlock() : _locked(false) {
        __atomic_store_n(&_locked, false, __ATOMIC_RELEASE);
    }

    bool spinlock::lock(bool stopInterrupts) {
        bool hadInterruptsEnabled = AreInterruptsEnabled();
        if (stopInterrupts) {
            asm volatile ("cli" ::: "memory");
        }
        bool irqOff = stopInterrupts || !hadInterruptsEnabled;
        auto hook = irqOff ? _hook.load(std::memory_order_relaxed) : nullptr;

        bool expected;
        do {
            while (__atomic_load_n(&_locked, __ATOMIC_RELAXED)) {
                if (hook)
                    hook();

                asm volatile ("pause");
            }
            expected = false;
        } while (!__atomic_compare_exchange_n(&_locked, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));

        return (stopInterrupts && hadInterruptsEnabled);
    }

    void spinlock::unlock(bool restoreInterrupts) {
        __atomic_store_n(&_locked, false, __ATOMIC_RELEASE);

        if (restoreInterrupts) {
            asm volatile ("sti" ::: "memory");
        }
    }

    bool spinlock::try_lock() {
        bool expected = false;
        return __atomic_compare_exchange_n(&_locked, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }

    spinlock::scoped_lock::scoped_lock(spinlock &lock, bool stopInterrupts) : _lock(lock) {
        _reenableInterrupts = _lock.lock(stopInterrupts);
    }

    spinlock::scoped_lock::~scoped_lock() {
        _lock.unlock(_reenableInterrupts);
    }

    bool spinlock::AreInterruptsEnabled() {
        uint64_t rflags;

        asm volatile (
            "pushfq\n\t"
            "pop %0"
            : "=rm" (rflags)
            :
            : "memory"
        );

        return (rflags & (1 << 9)) != 0;
    }

    void spinlock::SetRelaxHook(relax_hook hook) {
        _hook.store(hook);
    }
}
