#ifndef PIRU_SPINLOCK_HPP
#define PIRU_SPINLOCK_HPP

namespace piru::threading {
    class spinlock {
    public:
        spinlock();

        bool lock(bool stopInterrupts);
        void unlock(bool restoreInterrupts);

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

    private:
        volatile bool _locked;
    };
}

#endif //PIRU_SPINLOCK_HPP
