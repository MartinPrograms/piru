#ifndef PIRU_SUBSYSTEM_MANAGER_HPP
#define PIRU_SUBSYSTEM_MANAGER_HPP

#include <definitions.hpp>

namespace piru::core {
    class subsystem_manager {
    public:
        void Initialize();

        template<typename T>
        USED T* GetSubsystem();

    private:
        void* _storage{};
    };
}

#endif //PIRU_SUBSYSTEM_MANAGER_HPP
