#pragma once
#include "../model/project_model_path.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/legacy_mme.hpp"
#include "mikumikustudio/ported_funcs.hpp"

namespace mikumikustudio::project_relocation {
// Scene/load operations run on the host thread. Only the app retains a
// published owner. Deferred users keep a weak ticket to their original scene.
struct Owner {
    RootMappings roots;
    std::uint64_t generation = 0;
};

class Ticket {
    friend class LoadScope;
    std::weak_ptr<Owner> owner_;
public:
    static Ticket Capture(const MMDApp& app) {
        Ticket ticket;
        const auto& owner = app.m_relocationOwner;
        if (owner && owner->generation == app.SceneGeneration()) ticket.owner_ = owner;
        return ticket;
    }
    Result Infer(std::wstring_view absolute) const {
        if (const auto p = owner_.lock()) return p->roots.Infer(absolute);
        return {Status::NoMatch, {}};
    }
    project_model::Reference Locate(const char* path, const std::filesystem::path& directory) const {
        if (const auto p = owner_.lock()) return project_model::Locate(path, directory, p->roots);
        return project_model::Locate(path, directory, RootMappings{});
    }
    bool Confirm(const project_model::Reference& ref, std::wstring_view loaded) const {
        const auto p = owner_.lock();
        return p && ref.Confirm(p->roots, loaded);
    }
    bool Current(const MMDApp& app) const noexcept {
        const auto p = owner_.lock();
        return p && p == app.m_relocationOwner && p->generation == app.SceneGeneration();
    }
    bool ConfirmEffect(const MMDApp& app, std::wstring_view original, std::wstring_view loaded) const {
        const auto p = owner_.lock();
        if (!Current(app)) return false;
        auto proposed = p->roots;
        if (!proposed.AddConfirmedFile(original, loaded)) return false;
        p->roots = std::move(proposed);
        return true;
    }
};

// The process cache follows only the live host. Its optional bookkeeping
// cannot veto a native scene transition or a successful relocation ticket.
class CacheTransition {
    cache_manager::SceneToken token_;
public:
    CacheTransition() = default;
    CacheTransition(MMDApp& app, cache_manager::SceneOperation operation) noexcept { Accept(app, operation); }
    CacheTransition(const CacheTransition&) = delete;
    CacheTransition& operator=(const CacheTransition&) = delete;
    ~CacheTransition() { Fail(); }

    void Accept(MMDApp& app, cache_manager::SceneOperation operation) noexcept {
        if (&app == g_Block) token_ = cache_manager::AcceptScene(operation);
    }
    void Fail() noexcept {
        if (token_) cache_manager::FinishScene(token_, cache_manager::SceneOutcome::Failed);
        token_ = {};
    }
    void Complete(cache_manager::SceneOutcome outcome, const wchar_t* path = nullptr) noexcept {
        if (token_ && !cache_manager::FinishScene(token_, outcome, path)) Fail();
        token_ = {};
    }
};

class LoadScope {
public:
    enum class Picker { Model, Accessory };

private:
    MMDApp& app_;
    LoadScope* previous_;
    std::shared_ptr<Owner> prepared_;
    Ticket accepted_;
    std::filesystem::path cachePath_;
    CacheTransition cache_;
    wchar_t modelPickerDirectory_[kPathFieldCapacity]{};
    wchar_t accessoryPickerDirectory_[kPathFieldCapacity]{};

    wchar_t* PendingPickerDirectory(Picker picker) noexcept {
        return picker == Picker::Accessory ? accessoryPickerDirectory_ : modelPickerDirectory_;
    }

    void Discard() noexcept {
        cache_.Fail();
        if (const auto p = accepted_.owner_.lock(); p && p == app_.m_relocationOwner)
            app_.m_relocationOwner.reset();
        accepted_ = {};
    }
public:
    static bool RememberPickerDirectory(HWND main) noexcept {
        const UINT state = GetMenuState(GetMenu(main), 0x12D, MF_BYCOMMAND);
        return state != UINT(-1) && (state & MF_CHECKED);
    }

    static const wchar_t* PickerDirectory(MMDApp& app, Picker picker) noexcept {
        if (auto* scope = app.m_relocationLoad) {
            const auto* pending = scope->PendingPickerDirectory(picker);
            if (pending[0]) return pending;
        }
        return picker == Picker::Accessory ? app.DirAccs() : app.DirModel();
    }

    static void StagePickerDirectory(MMDApp& app, Picker picker, const wchar_t* selected) noexcept {
        auto* scope = app.m_relocationLoad;
        if (!scope || !scope->accepted_.Current(app)) return;
        ExtractDirFromPath(scope->PendingPickerDirectory(picker), selected, kPathFieldCapacity);
    }

    static std::filesystem::path FreezePath(const wchar_t* candidate) noexcept {
        try {
            if (candidate && *candidate) {
                std::error_code error;
                auto path = std::filesystem::absolute(candidate, error);
                if (!error) return path.lexically_normal();
            }
        } catch (...) {}
        return {}; // Cache attribution can still fail closed after a successful parse.
    }
    explicit LoadScope(MMDApp& app, std::filesystem::path path = {}) noexcept
        : app_(app), previous_(app.m_relocationLoad), cachePath_(std::move(path)) {
        app.m_relocationLoad = this;
    }
    LoadScope(const LoadScope&) = delete;
    LoadScope& operator=(const LoadScope&) = delete;
    ~LoadScope() {
        Discard();
        app_.m_relocationLoad = previous_;
    }

    template<class Parse> bool Run(Parse parse) {
        prepared_ = std::make_shared<Owner>(); // before irreversible replacement
        try {
            if (parse()) return true;
        } catch (...) {
            Discard(); // before shell recovery/reset/cancellation callbacks
            throw;
        }
        Discard();
        return false;
    }
    static void Activate(MMDApp& app) noexcept {
        auto* scope = app.m_relocationLoad;
        if (!scope || !scope->prepared_) return;
        scope->cache_.Accept(app, cache_manager::SceneOperation::Load);
        scope->prepared_->generation = app.SceneGeneration();
        app.m_relocationOwner = std::move(scope->prepared_);
        scope->accepted_ = Ticket::Capture(app);
    }
    static Ticket Accepted(const MMDApp& app) noexcept {
        return app.m_relocationLoad ? app.m_relocationLoad->accepted_ : Ticket{};
    }
    static const wchar_t* ProjectPath(const MMDApp& app) noexcept {
        return app.m_relocationLoad ? app.m_relocationLoad->cachePath_.c_str() : L"";
    }
    static void FailCurrent(MMDApp& app) noexcept {
        if (app.m_relocationLoad) app.m_relocationLoad->Discard();
    }
    [[nodiscard]] static CacheTransition ResetAccepted(MMDApp& app) noexcept {
        app.m_relocationOwner.reset();
        return CacheTransition(app, cache_manager::SceneOperation::Reset);
    }
    void Complete() noexcept {
        cache_.Complete(cache_manager::SceneOutcome::Loaded, cachePath_.c_str());
        const bool current = accepted_.Current(app_);
        if (current && modelPickerDirectory_[0]) CopyDirPathW(app_.DirModel(), modelPickerDirectory_);
        if (current && accessoryPickerDirectory_[0]) CopyDirPathW(app_.DirAccs(), accessoryPickerDirectory_);
        if (&app_ == g_Block && current) {
            if (const auto owner = accepted_.owner_.lock(); owner && LegacyMmeLoaded() && !cachePath_.empty())
                PublishEffectProjectRelocation(cachePath_.c_str(),
                    std::shared_ptr<const RootMappings>(owner, &owner->roots));
            // Legacy MME observes the open only after all recovery decisions and
            // the PMM commit. A cancelled project has no pending EMM to bind.
            try {
                NotifyEffectFileOpen(cachePath_.empty() ? app_.EnvFileName() : cachePath_.c_str());
            } catch (...) { /* Optional legacy notification follows the committed scene. */ }
        }
        accepted_ = {}; // leave relocation ownership with the app
    }
};
bool CanRecoverProjectEffect(MMDApp& app) noexcept;
bool RecoverProjectEffect(MMDApp& app, HWND owner) noexcept;
bool ConfirmRemainingProjectEffects(MMDApp& app, HWND owner) noexcept;
}
