#include <cpp/spinlock.hpp>
#include <definitions.hpp>

bool AreInterruptsEnabled() {
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

namespace piru::threading {
    spinlock::spinlock() : _locked(false) {
        __atomic_store_n(&_locked, false, __ATOMIC_RELEASE);
    }

    bool spinlock::lock(bool stopInterrupts) {
        bool hadInterruptsEnabled = AreInterruptsEnabled();
        if (stopInterrupts) {
            asm volatile ("cli" ::: "memory");
        }

        bool expected;
        do {
            while (__atomic_load_n(&_locked, __ATOMIC_RELAXED))
                asm volatile ("pause");
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

    spinlock::scoped_lock::scoped_lock(spinlock &lock, bool stopInterrupts) : _lock(lock) {
        _reenableInterrupts = _lock.lock(stopInterrupts);
    }

    spinlock::scoped_lock::~scoped_lock() {
        _lock.unlock(_reenableInterrupts);
    }
}
