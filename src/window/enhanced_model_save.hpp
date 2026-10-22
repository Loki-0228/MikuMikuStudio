#pragma once

namespace mikumikustudio {
class MMDApp;

// Trusted host-only validation after flush/close and immediately before the
// atomic file replacement. It must not allocate, pump messages or call plugins.
struct EnhancedPmdPublishCheck {
    void* context = nullptr;
    bool (*current)(void*) noexcept = nullptr;
    // A guarded caller cancels its history reservation before showing errors.
    bool deferErrors = false;
};
enum class EnhancedPmdWriteResult { Written, Refused, SourceChanged };

// Serialize the captured object, never a later UI selection. This function
// publishes only the file; the caller owns model lifetime and source-path
// publication. A written file is external output, not an undo-owned resource.
EnhancedPmdWriteResult WriteEnhancedModelPmd(MMDApp*, unsigned char* object,
    const wchar_t* path, const EnhancedPmdPublishCheck& = {});
}
