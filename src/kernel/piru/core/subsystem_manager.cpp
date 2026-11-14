#include "subsystem_manager.hpp"
#include "subsystem_manager.hpp"
#include <freestanding_reflection/fs_meta.h>

#include "logging.hpp"
#include "subsystems.hpp"

namespace {
    void SubsystemCycleDetected(); // never defined, deliberately
    void SubsystemMissingConstructor(); // never defined, deliberately
    void SubsystemDependencyNotInStorage(); // never defined, deliberately

    consteval bool IsSubsystemConstructor(std::meta::info info) {
        for (auto annotation : std::meta::annotations_of(info)) {
            if (std::meta::type_of(annotation) == ^^decltype(piru::subsystems::subsystem_constructor_tag)) {
                return true;
            }
        }
        return false;
    }

    consteval bool GetConstructorOf(std::meta::info info, std::meta::info& out) {
        for (auto ctor: std::meta::members_of(info, std::meta::access_context::unprivileged())) {
            if (std::meta::is_constructor(ctor) && IsSubsystemConstructor(ctor)) {
                out = ctor;
                return true;
            }
        }

        return false;
    }

    consteval std::vector<std::meta::info> DependenciesOf(std::meta::info type) {
        std::vector<std::meta::info> deps;

        for (auto ctor : std::meta::members_of(type, std::meta::access_context::unprivileged())) {
            if (std::meta::is_constructor(ctor) && IsSubsystemConstructor(ctor)) {
                for (auto param : std::meta::parameters_of(ctor)) {
                    deps.push_back(std::meta::remove_reference(std::meta::type_of(param)));
                }
            }
        }

        return deps;
    }

    consteval bool Contains(const std::vector<std::meta::info>& v, std::meta::info x) {
        for (auto e : v) {
            if (e == x) return true;
        }
        return false;
    }

    consteval void RemoveFrom(std::vector<std::meta::info>& v, std::meta::info x) {
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i] == x) {
                v.erase(i);
                return;
            }
        }
    }

    consteval void VisitNode(std::meta::info node, const std::vector<std::meta::info>& nodes,
                              std::vector<std::meta::info>& visiting, std::vector<std::meta::info>& visited,
                              std::vector<std::meta::info>& result) {
        if (Contains(visited, node)) return;
        if (Contains(visiting, node)) {
            SubsystemCycleDetected();
        }

        visiting.push_back(node);
        for (auto dep : DependenciesOf(node)) {
            if (Contains(nodes, dep)) {
                VisitNode(dep, nodes, visiting, visited, result);
            }
        }

        RemoveFrom(visiting, node);

        visited.push_back(node);
        result.push_back(node);
    }

    consteval std::vector<std::meta::info> TopoSort(std::vector<std::meta::info> nodes) {
        std::vector<std::meta::info> result;
        std::vector<std::meta::info> visited;
        std::vector<std::meta::info> visiting;
        for (auto node : nodes) {
            VisitNode(node, nodes, visiting, visited, result);
        }

        return result;
    }

    consteval auto GetSubsystemTypes() {
        std::vector<std::meta::info> subsystems;
        template for (constexpr auto member : std::define_static_array(std::meta::members_of(^^piru::subsystems, std::meta::access_context::unprivileged()))) {
            if constexpr (std::meta::is_type(member) && !std::meta::is_concept(member)) {
                using T = typename[:member:];

                if constexpr (piru::subsystems::DeclaresSubsystem<T>) {
                    static_assert(piru::subsystems::IsSubsystem<T>, "Type declares SUBSYSTEM_NAME but does not satisfy IsSubsystem");
                    subsystems.push_back(member);
                }
            }
        }

        auto sorted= TopoSort(subsystems);
        return std::define_static_array(sorted);
    }

    constexpr auto SubsystemMetaList = GetSubsystemTypes();
    struct SubsystemStorage;

    consteval {
        std::vector<std::meta::info> specs;

        template for (constexpr auto system : SubsystemMetaList) {
            using T = typename[:system:];
            std::meta::data_member_options options;
            options.name = T::SUBSYSTEM_NAME;
            specs.push_back(std::meta::data_member_spec(^^T, options));
        }

        std::meta::define_aggregate(^^SubsystemStorage, specs);
    }

    template<typename T>
    consteval bool FindStorageMember(std::meta::info &out) {
        for (auto m : std::meta::nonstatic_data_members_of(^^SubsystemStorage, std::meta::access_context::unprivileged())) {
            if (std::meta::type_of(m) == ^^T) {
                out = m;
                return true;
            }
        }

        return false;
    }

    template <typename T>
    consteval std::meta::info FindCtorOf() {
        std::meta::info ctor;
        if (!GetConstructorOf(^^T, ctor)) {
            SubsystemMissingConstructor();
        }
        return ctor;
    }

    template <typename T>
    consteval std::meta::info FindSelfMember() {
        std::meta::info self;
        if (!FindStorageMember<T>(self)) {
            SubsystemDependencyNotInStorage();
        }
        return self;
    }

    template<typename T>
    consteval auto DepMembers() {
        constexpr auto ctor = FindCtorOf<T>();

        std::vector<std::meta::info> deps;
        template for (constexpr auto param : std::define_static_array(std::meta::parameters_of(ctor))) {
            using ParamT = typename[:std::meta::remove_reference(std::meta::type_of(param)):];
            deps.push_back(FindSelfMember<ParamT>());
        }
        return std::define_static_array(deps);
    }

    template <std::meta::info...>
    struct InfoPack {};

    consteval auto ToPack(std::span<const std::meta::info> v) {
        std::vector<std::meta::info> args;
        for (auto e : v) args.push_back(std::meta::reflect_constant(e));
        return substitute(^^InfoPack, args);
    }

    template <typename T, std::meta::info... Deps>
    void ConstructSubsystemImpl(SubsystemStorage& storage, InfoPack<Deps...>) {
        constexpr auto self = FindSelfMember<T>();
        ::new (std::addressof(storage.[:self:])) T(storage.[:Deps:]...);
    }

    template <typename T>
    constexpr void ConstructSubsystem(SubsystemStorage& storage) {
        ConstructSubsystemImpl<T>(storage, typename[:ToPack(DepMembers<T>()):]{});
    }

    template<typename T>
    constexpr void InitSubsystem(SubsystemStorage& storage) {
        auto& subsystem = storage.[:FindSelfMember<T>():];
        if (subsystem.Initialize() != piru::subsystems::Result::SUCCESS) {
            if constexpr (T::IS_REQUIRED) {
                PANIC("Required subsystem {} failed to initialize", piru::reflection::NameOf<T>());
            } else {
                LOG_WARNING("Optional subsystem {} failed to initialize", piru::reflection::NameOf<T>());
            }
        }
    }

    template<typename T>
    bool TestSubsystem(SubsystemStorage& storage) {
        auto& subsystem = storage.[:FindSelfMember<T>():];

        if constexpr (T::TEST_SUBSYSTEM)
            return subsystem.RunTests();

        return true;
    }
}

namespace piru::core {
    static uint8_t subsystemBuffer [sizeof(SubsystemStorage)] __attribute__((aligned(alignof(SubsystemStorage))));

    template<typename T>
    USED T* subsystem_manager::GetSubsystem() {
        auto& storage = *reinterpret_cast<SubsystemStorage*>(_storage);
        return &storage.[:FindSelfMember<T>():];
    }

    void subsystem_manager::Initialize() {
        _storage = reinterpret_cast<SubsystemStorage*>(subsystemBuffer);

        template for (constexpr auto system : SubsystemMetaList) {
            using T = typename[:system:];
            ConstructSubsystem<T>(*(SubsystemStorage*)_storage);
        }

        template for (constexpr auto system : SubsystemMetaList) {
            using T = typename[:system:];
            InitSubsystem<T>(*(SubsystemStorage*)_storage);
        }

        template for (constexpr auto system : SubsystemMetaList) {
            using T = typename[:system:];
            (void)this->GetSubsystem<T>();   // instantiate only, don't execute
        }
    }

    void subsystem_manager::Test() {
        template for (constexpr auto system : SubsystemMetaList) {
            using T = typename[:system:];
            if (!TestSubsystem<T>(*(SubsystemStorage*)_storage)) {
                PANIC("Subsystem {} failed self-test", piru::reflection::NameOf<T>());
            } else {
                LOG_DEBUG("Subsystem {} self-test success", piru::reflection::NameOf<T>());
            }
        }
    }
}
