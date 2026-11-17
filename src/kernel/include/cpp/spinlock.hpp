#ifndef PIRU_SPINLOCK_HPP
#define PIRU_SPINLOCK_HPP

namespace piru::threading {
    class spinlock {
    public:
        using relax_hook = void(*)();

        spinlock();

        spinlock(const spinlock&) = delete;
        spinlock &operator=(const spinlock&) = delete;

        bool lock(bool stopInterrupts);
        void unlock(bool restoreInterrupts);

        bool try_lock();

        class scoped_lock {
        public:
            scoped_lock(spinlock &lock, bool stopInterrupts);
            ~scoped_lock();

            scoped_lock(const scoped_lock &) = delete;
            scoped_lock &operator=(const scoped_lock &) = delete;
            scoped_lock(scoped_lock &&) = delete;
            scoped_lock &operator=(scoped_lock &&) = delete;
            scoped_lock() = delete;
            scoped_lock &operator=(spinlock &) = delete;

        private:
            spinlock &_lock;
            bool _reenableInterrupts;
        };

        static bool AreInterruptsEnabled();
        static void SetRelaxHook(relax_hook hook); /// must NOT take a lock. Otherwise a lock will wait on a lock, which waits on this lock which then waits!

    private:
        volatile bool _locked;
    };
}

#endif //PIRU_SPINLOCK_HPP
