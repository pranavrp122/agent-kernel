#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace ak {

// fixed capacity ring that overwrites the oldest entry and hands it back to the caller
template <typename T, std::size_t N>
class Ring {
public:
    std::optional<T> push(T value) {
        std::optional<T> evicted;
        if (count_ == N) {
            evicted = std::move(slots_[head_]);
            head_ = (head_ + 1) % N;
            --count_;
        }
        slots_[(head_ + count_) % N] = std::move(value);
        ++count_;
        return evicted;
    }

    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    void clear() { head_ = 0; count_ = 0; }

    // index 0 is the newest entry so callers can walk from most recent backwards
    const T& newest(std::size_t i) const {
        return slots_[(head_ + count_ - 1 - i) % N];
    }

private:
    std::array<T, N> slots_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

}
