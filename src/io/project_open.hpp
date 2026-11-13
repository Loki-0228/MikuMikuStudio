#pragma once

#include <cstdint>

namespace mikumikustudio {
class MMDApp;
namespace pmm_io { class Input; }

// The confirmation and file picker can run commands while pumping messages.
// Keep the UI entry exclusive and bind its consent to the scene it was shown for.
class ProjectOpenUiScope {
public:
    explicit ProjectOpenUiScope(MMDApp*) noexcept;
    ~ProjectOpenUiScope();
    ProjectOpenUiScope(const ProjectOpenUiScope&) = delete;
    ProjectOpenUiScope& operator=(const ProjectOpenUiScope&) = delete;

    bool Prepare();
    bool ConfirmDiscard(bool topmost) const;
    bool CanContinue() const;

private:
    MMDApp* app_;
    std::uint64_t generation_ = 0;
    std::uint64_t revision_ = 0;
    bool recoveryPending_ = false;
};

// Internal status-returning bodies. The legacy void entry points remain intact.
// replaced becomes true only after the shared-history replacement boundary.
bool TryLoadSceneV1(MMDApp*, pmm_io::Input&, bool& replaced);
bool TryLoadSceneV2(MMDApp*, pmm_io::Input&, bool& replaced);
// Called after that boundary and before the first native scene release.
void AcceptProjectSceneReplacement(MMDApp*);

// Close input, recover a replaced scene and report the invalid source in the
// caller's language. False means the caller still owes the bounded reset retry.
bool ReportInvalidProjectLoad(MMDApp*, pmm_io::Input&, bool replaced, const wchar_t* path);

// Try the first reset after closing a rejected or cancelled project's input.
// A refusal or exception leaves the scene quarantined in the caller's language.
bool ResetAfterLoadFailure(MMDApp*) noexcept;

// Internal refusal signal shared by the four PMM loader files only. A native
// body throws it after the shared-history replacement boundary when
// the initial reset does not complete. restoreWindowSize marks the model-abort paths that still owe their
// post-reset window-size restore. It must never escape this module.
struct SceneResetRefused final {
    bool restoreWindowSize;
};

// One bounded in-module recovery for that refusal. The caller has already
// closed its input and released the loader workspace. Only an entry that
// prepared the plugin load transaction may cancel it (ownsLoadTransaction);
// a plain native body leaves that to ResetAppState's own gate. Retries the
// normal reset once, applies the deferred window-size restore only after a
// successful model-abort retry, preserves the UI language and logs a second
// refusal. Returns whether the scene is reset; it never rethrows the signal.
bool RecoverSceneResetRefusal(MMDApp*, bool ownsLoadTransaction,
                             bool restoreWindowSize);
}
