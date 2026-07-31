#ifndef PIRU_REFLECTION_HPP
#define PIRU_REFLECTION_HPP

#include "string_view.hpp"
#include <freestanding_reflection/fs_meta.h>

/// Wrappers around a few meta calls to make it easier to work with
namespace piru::reflection {
    template<typename E>
    struct enum_entry {
        E val;
        piru::string_view name;
    };

    template<typename E>
    requires (std::is_enum_v<E>)
    constexpr auto& Entries() {
        static constexpr auto entries = [] {
            static constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^E));
            std::array<enum_entry<E>, enumerators.size()> result{};
            size_t i = 0;
            template for (constexpr auto e : enumerators) {
                constexpr auto id = std::meta::identifier_of(e);
                constexpr auto staticName = std::define_static_string(id);
                result[i++] = enum_entry<E>{[:e:], piru::string_view(staticName, id.size())};
            }
            return result;
        }();
        return entries;
    }

    template<typename E>
    requires (std::is_enum_v<E>)
    constexpr piru::string_view EnumeratorName(E value) {
        for (auto& entry : Entries<E>()) {
            if (entry.val == value) return entry.name;
        }
        return "<unknown>";
    }

    template<typename E>
    requires (std::is_enum_v<E>)
    constexpr E NameToEnumerator(piru::string_view name) {
        for (auto& entry : Entries<E>()) {
            if (entry.name == name) return entry.val;
        }
        return E{};
    }

    template<typename T>
    constexpr piru::string_view NameOf() {
        constexpr auto name= std::meta::identifier_of(^^T);
        return {name.data(), name.size()};
    }
}

#endif //PIRU_REFLECTION_HPP
