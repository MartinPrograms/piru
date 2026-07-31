#ifndef PIRU_FMT_HPP
#define PIRU_FMT_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "reflection.hpp"
#include <freestanding_reflection/fs_meta.h>

#include "stream.hpp"

namespace piru {
    class fmt {
    public:
        using out_stream = stream::ostream<char>;

    private:
        static void writeString(out_stream out, string_view value);
        static void writeChar(out_stream out, char value);
        static void writeBool(out_stream out, bool value);
        static void writeInt(out_stream out, uint64_t value, uint64_t base);
        static void writePointer(out_stream out, const void* value);
        static void writeDouble(out_stream out, double value);

        template<std::meta::info Info>
        constexpr static void writeNamespaceImpl(out_stream out) {
            if constexpr (std::meta::has_parent(Info)) {
                constexpr auto parent = std::meta::parent_of(Info);

                // Stop recursion at the global namespace
                if constexpr (parent != ^^::) {
                    writeNamespaceImpl<parent>(out);
                    writeString(out, "::");
                }
            }

            constexpr auto name = std::meta::identifier_of(Info);
            writeString(out, string_view(name.data(), name.size()));
        }

        template<typename T>
        constexpr static void writeNamespace(out_stream out) {
            if (!std::meta::is_namespace_member(^^T)) return;
            writeNamespaceImpl<std::meta::parent_of(^^T)>(out);
            writeString(out, "::");
        }

        template<typename T>
        static void writeAggregate(out_stream out, const T& value) {
            writeString(out, reflection::NameOf<T>());
            writeString(out, " { ");
            [[maybe_unused]] bool first = true;
            template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
                if (!first) writeString(out, ", ");
                first = false;
                constexpr auto name = std::meta::identifier_of(member);
                writeString(out, string_view(name.data(), name.size()));
                writeString(out, ": ");
                writeArg(out, value.[:member:]);
            }
            writeString(out, " }");
        }

        template<typename T>
        static void writeClassMembers(out_stream out, const T& value, bool& first) {
            template for (constexpr auto base : std::define_static_array(std::meta::bases_of(^^T, std::meta::access_context::unchecked()))) {
                using BaseType = [:std::meta::type_of(base):];
                writeClassMembers<BaseType>(out, value, first);
            }

            template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()))) {
                if (!first) writeString(out, ", ");
                first = false;

                constexpr auto name = std::meta::identifier_of(member);
                writeString(out, string_view(name.data(), name.size()));
                writeString(out, ": ");
                writeArg(out, value.[:member:]);
            }
        }

        template<typename T>
        static void writeClass(out_stream out, const T& value) {
            writeString(out, reflection::NameOf<T>());
            writeString(out, " { ");
            [[maybe_unused]] bool first = true;
            writeClassMembers<T>(out, value, first);
            writeString(out, " }");
        }

        // Fallback for non primitives
        template<typename T>
        static void writeType(out_stream out, T value) {
            writeNamespace<T>(out);
            if constexpr (std::is_aggregate_v<T>) {
                writeAggregate(out, value);
            } else if constexpr (std::is_class_v<T>) {
                writeClass(out, value);
            } else if constexpr (std::is_enum_v<T> && !std::is_aggregate_v<T>) {
                writeString(out, reflection::NameOf<T>());
                writeString(out, "::");
                writeString(out, reflection::EnumeratorName(value));
            }  else {
                writeString(out, reflection::NameOf<T>());
            }
        }

        template <typename T>
        static void writeArg(out_stream out, T value) {
            if constexpr (std::is_same_v<T, bool>) {
                writeBool(out, value);
            } else if constexpr (std::is_same_v<T, char>) {
                writeChar(out, value);
            } else if constexpr (std::is_same_v<T, string_view>) {
                writeString(out, value);
            } else if constexpr (std::is_integral_v<T> && !std::is_signed_v<T>) {
                writeInt(out, static_cast<uint64_t>(value), 10);
            } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
                if (value < 0) {
                    writeChar(out, '-');
                    writeInt(out, (uint64_t)(-(uint64_t)value), 10);
                } else {
                    writeInt(out, (uint64_t)value, 10);
                }
            } else if constexpr (std::is_floating_point_v<T>) {
                writeDouble(out, static_cast<double>(value));
            } else if constexpr (std::is_pointer_v<T>) {
                writePointer(out, value);
            } else {
                writeType(out, value);
            }
        }

        template<typename T, typename... Rest>
        static void formatImpl(out_stream out, string_view fmt, T&& first, Rest&&... rest) {
            size_t i = 0;
            while (i < fmt.Size()) {
                if (fmt.Data()[i] == '{' && i + 1 < fmt.Size() && fmt.Data()[i + 1] == '}') {
                    writeArg(out, std::forward<T>(first));
                    formatImpl(out, string_view(fmt.Data() + i + 2, fmt.Size() - i - 2), std::forward<Rest>(rest)...);
                    return;
                }

                // Check for {{}}
                if (fmt.Data()[i] == '{' && i + 1 < fmt.Size() && fmt.Data()[i + 1] == '{') {
                    writeChar(out, '{');
                    i += 2;
                    continue;
                }

                if (fmt.Data()[i] == '}' && i + 1 < fmt.Size() && fmt.Data()[i + 1] == '}') {
                    writeChar(out, '}');
                    i += 2;
                    continue;
                }

                writeChar(out, fmt.Data()[i]);
                ++i;
            }
        }

        static void formatImpl(out_stream out, string_view fmt) {
            writeString(out, fmt);
        }

    public:
        template<typename... Args>
        static void format(out_stream out, string_view fmt, Args&&... args) {
            formatImpl(out, fmt, std::forward<Args>(args)...);
        }
    };
}

#endif //PIRU_FMT_HPP
