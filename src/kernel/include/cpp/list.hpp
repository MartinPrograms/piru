#ifndef PIRU_LIST_HPP
#define PIRU_LIST_HPP

#include <cstddef>
#include <cstdint>
#include <cpp/math.hpp>

namespace piru {
    // Depends on implementations of new/delete.
    template<typename T>
    class list {
    public:
        using predicate = bool(*)(const T&, void* data);
        constexpr static size_t npos = (size_t)-1;

        list() : _memory(nullptr), _count(0), _capacity(0) {}

        list(size_t capacity) {
            _capacity = capacity;
            Resize(_capacity);
        }

        ~list () {
            delete[] _memory;
        }

        list(const list&) = delete;
        list(list&&) = delete;
        list operator=(const list&) = delete;
        list operator=(list&&) = delete;

        void Add(const T& item) {
            if (_count >= _capacity) {
                Resize();
            }
            _memory[_count++] = item;
        }

        void Remove(const T& item) {
            for (size_t i = 0; i < _count; i++) {
                if (_memory[i] == item) {
                    for (size_t j = i; j < _count - 1; j++) {
                        _memory[j] = _memory[j + 1];
                    }
                    _count--;
                    return;
                }
            }
        }

        void Remove(size_t index) {
            if (index >= _count || index == npos) {
                return;
            }

            for (size_t i = index; i < _count - 1; i++) {
                _memory[i] = _memory[i + 1];
            }
            _count--;
        }

        [[nodiscard]] size_t Count() const {
            return _count;
        }

        [[nodiscard]] T& operator[](size_t index) {
            return _memory[index];
        }

        [[nodiscard]] const T& operator[](size_t index) const {
            return _memory[index];
        }

        [[nodiscard]] T* Data() {
            return _memory;
        }

        // Predicates:
        size_t IndexOf(predicate pred, void* data) const {
            for (size_t i = 0; i < _count; i++) {
                if (pred(_memory[i], data)) {
                    return i;
                }
            }

            return npos;
        }

        [[nodiscard]] T* Find(predicate pred, void* data) const {
            for (size_t i = 0; i < _count; i++) {
                if (pred(_memory[i], data)) {
                    return &_memory[i];
                }
            }
            return nullptr;
        }

        void Resize(size_t size) {
            auto newMemory = new T[size];
            for (size_t i = 0; i < _count; i++) {
                newMemory[i] = _memory[i];
            }

            delete[] _memory;
            _memory = newMemory;
            _capacity = size;
        }

        T* begin() { return _memory; }
        const T* begin() const { return _memory; }

        T* end() { return _memory + _count; }
        const T* end() const { return _memory + _count; }

    private:
        T* _memory;
        size_t _count;
        size_t _capacity;


        void Resize() {
            Resize(math::Max(_capacity * 2, _capacity + 1));
        }
    };
}

#endif //PIRU_LIST_HPP
