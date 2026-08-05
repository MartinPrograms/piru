#ifndef PIRU_SUBSYSTEM_MANAGER_HPP
#define PIRU_SUBSYSTEM_MANAGER_HPP

namespace piru::core {
    class subsystem_manager {
    public:
        void Initialize();

        template<typename T>
        T* GetSubsystem();

    private:
        void* _storage{};
    };
}

#endif //PIRU_SUBSYSTEM_MANAGER_HPP
