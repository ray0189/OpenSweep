// Bounded circular buffer. When full, push() overwrites the oldest element.
// Not thread-safe; callers synchronise externally.
#pragma once

#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace opensweep {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity) : data_(capacity) {
        if (capacity == 0) throw std::invalid_argument("RingBuffer capacity must be > 0");
    }

    std::size_t capacity() const noexcept { return data_.size(); }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    bool full() const noexcept { return size_ == data_.size(); }

    // Appends a value; returns true if an old element was overwritten.
    bool push(T value) {
        const std::size_t idx = (head_ + size_) % data_.size();
        if (full()) {
            data_[head_] = std::move(value);
            head_ = (head_ + 1) % data_.size();
            return true;
        }
        data_[idx] = std::move(value);
        ++size_;
        return false;
    }

    // Removes and returns the oldest element. Throws std::out_of_range when empty.
    T pop_front() {
        if (empty()) throw std::out_of_range("RingBuffer is empty");
        T v = std::move(data_[head_]);
        data_[head_] = T{};
        head_ = (head_ + 1) % data_.size();
        --size_;
        return v;
    }

    // Index 0 is the oldest element. Throws std::out_of_range when out of bounds.
    const T& at(std::size_t i) const {
        if (i >= size_) throw std::out_of_range("RingBuffer index out of range");
        return data_[(head_ + i) % data_.size()];
    }
    const T& operator[](std::size_t i) const { return at(i); }
    const T& front() const { return at(0); }
    const T& back() const {
        if (empty()) throw std::out_of_range("RingBuffer is empty");
        return at(size_ - 1);
    }

    void clear() noexcept(std::is_nothrow_default_constructible_v<T> &&
                          std::is_nothrow_move_assignable_v<T>) {
        for (auto& e : data_) e = T{};
        head_ = 0;
        size_ = 0;
    }

    // Copies the contents, oldest first.
    std::vector<T> to_vector() const {
        std::vector<T> out;
        out.reserve(size_);
        for (std::size_t i = 0; i < size_; ++i) out.push_back(data_[(head_ + i) % data_.size()]);
        return out;
    }

private:
    std::vector<T> data_;
    std::size_t head_ = 0;  // index of the oldest element
    std::size_t size_ = 0;
};

}  // namespace opensweep
