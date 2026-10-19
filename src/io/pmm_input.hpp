#pragma once

#include "pmm_io_common.hpp"
#include <optional>
#include <utility>

namespace mikumikustudio::pmm_io {
// Own exactly one CRT input descriptor. Readers only borrow Descriptor(); every
// close, including an abort inside a native body, consumes this same owner.
// CheckedReads scopes, like their existing TLS implementation, nest in LIFO
// order. The input and its optional scope never move or allocate.
class Input final {
    int descriptor_;
    std::optional<CheckedReads> checked_;
public:
    explicit Input(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~Input() noexcept { Close(); }
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    Input(Input&&) = delete;
    Input& operator=(Input&&) = delete;

    int Descriptor() const noexcept { return descriptor_; }
    bool EnableCheckedReads() noexcept {
        if (descriptor_ < 0) return false;
        if (!checked_) checked_.emplace(descriptor_);
        return true;
    }
    void Close() noexcept {
        // The old integer can be reused during the success/error tail. Never
        // consult the CRT to infer ownership or put it back after closing.
        const int descriptor = std::exchange(descriptor_, -1);
        // End the PMM read context before a different file can reuse its number.
        checked_.reset();
        if (descriptor >= 0) _close(descriptor);
    }
};
}
