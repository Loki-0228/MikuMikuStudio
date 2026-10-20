#pragma once
#include "ui_registry.hpp"

namespace mikumikustudio::plugins {

// Native presentation of one registered property. All scene/transaction work
// belongs to UiRegistry's trusted property-input broker.
class NativePropertyInput {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativePropertyInput(UiRegistry&, std::string elementId, HWND parent,
        unsigned controlId, std::function<void(bool)> nativeWork, Log = {});
    ~NativePropertyInput();
    HWND Window() const;
    void Refresh(uint32_t dpi);
    // Transactional DATA readback for the current native presentation. The
    // callback checks the queue's original target before/after preparation and
    // application. This never lays out or rebuilds native fields.
    MmsResult Readback(const std::function<MmsResult()>& revalidate, uint64_t& sourceRevision);
    void Cancel() noexcept;
    // Called before a layout batch hides the wrapper, so focus loss cannot
    // turn hiding or folding into an edit submission.
    void LayoutVisible(bool) noexcept;
};

}
