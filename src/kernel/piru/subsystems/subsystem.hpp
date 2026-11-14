#ifndef PIRU_SUBSYSTEM_HPP
#define PIRU_SUBSYSTEM_HPP

#include <definitions.hpp>
#include <concepts>

namespace piru::subsystems {
    enum class Result {
        SUCCESS,
        FAILURE,
        NOT_IMPLEMENTED
    };

    template<typename T>
    concept IsSubsystem = requires(T& t) {
        { T::SUBSYSTEM_NAME } -> std::convertible_to<const char*>;
        { T::IS_REQUIRED } -> std::convertible_to<bool>;
        { T::TEST_SUBSYSTEM } -> std::convertible_to<bool>;
        { t.Initialize() } -> std::same_as<Result>;
        { t.RunTests() } -> std::same_as<bool>;
    };

    template<typename T>
    concept DeclaresSubsystem = requires {
        { T::SUBSYSTEM_NAME } -> std::convertible_to<const char*>;
    };

    struct subsystem_constructor{};
    inline constexpr subsystem_constructor subsystem_constructor_tag{};

    /*
    struct example_subsystem {
        static constexpr const char* SUBSYSTEM_NAME = "example_subsystem";
        static constexpr bool IS_REQUIRED = true; // If true, and the init process fails it will crash the entire system

        // If you want to do dependency injection, you must get a reference to the dependency you want, and inject it
        [[=subsystem_constructor_tag]] example_subsystem(some_other_subsystem& injectedSubsystem);

        Result Initialize();

        // A testing function, if fails, runs a panic.
        // True = success, false = failure
        bool RunTests();

    private:
        some_other_subsystem& _injectedSubsystem;
    };
    */
}

#endif //PIRU_SUBSYSTEM_HPP
