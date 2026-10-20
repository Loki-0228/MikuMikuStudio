#pragma once
#include "ui_property_value.hpp"
#include <cstddef>

namespace mikumikustudio::plugins {

// A private input session. Raw text, including incomplete IME/numeric input,
// never reaches a plugin or the scene until Prepare succeeds. Host refreshes
// must preserve an active draft; Begin is an explicit start/restart operation.
class UiPropertyDraft {
public:
    // UTF-8 bytes in one default input field. BYTES uses two hexadecimal
    // characters per byte; larger values require a plugin-provided editor.
    static constexpr size_t kMaxFieldBytes = 1024u * 1024u;
    MmsResult Begin(const MmsValue& canonical, uint64_t sourceRevision) noexcept;
    MmsResult SetField(size_t index, std::string_view text) noexcept;
    void SetComposing(bool composing) noexcept;
    MmsResult Prepare(const MmsValue& current, uint64_t sourceRevision,
        UiPropertyValue& output, bool& changed) const noexcept;
    void Cancel() noexcept;

    bool Active() const noexcept { return sourceRevision_ != 0; }
    bool Composing() const noexcept { return composing_; }
    bool Dirty() const noexcept { return Active() && fields_ != initialFields_; }
    uint64_t SourceRevision() const noexcept { return sourceRevision_; }
    const std::vector<std::string>& Fields() const noexcept { return fields_; }

private:
    UiPropertyValue baseline_;
    std::vector<std::string> initialFields_, fields_;
    uint64_t sourceRevision_ = 0;
    bool composing_ = false;
};

}
