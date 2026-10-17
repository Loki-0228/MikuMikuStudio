#pragma once
#include "ui_registry.hpp"
#include "loader_win32.hpp"
#include "scene_store.hpp"
#include "storage_store.hpp"

namespace mikudancestudio::plugins {
class EditHost;
struct PluginStatus { std::string id, stage, error; fs::path package; };
class Manager {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Manager(fs::path userRoot, Log log, std::function<void()> uiChanged = {}, bool forceFallback = false,
        std::shared_ptr<SceneStore> scene = {});
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    void Load(const fs::path& pluginsRoot);
    // Explicit opt-in before the first Load. The external host must outlive
    // this manager or a completed DetachEditHost; no service is enabled by
    // the ordinary constructor. Cached plugin tables remain safe after detach.
    MmsResult AttachEditHost(EditHost&) noexcept;
    // UI-thread lifecycle boundary, never from a callback. Cancels pending
    // edits before disconnecting. Any failure leaves the host attached. A
    // failing cancellation still releases all tokens and poisons the host;
    // a later safe retry can disconnect the now-empty store.
    MmsResult DetachEditHost() noexcept;
    // UI-thread configuration outside callbacks. Runs after a successful edit
    // commit/undo/redo has released its publication gate. A throwing observer
    // is logged; it cannot roll back the already committed journal.
    void SetEditPublished(std::function<void()>);
    // True only while an approved edit call enters the attached host. Keep
    // InCallback() checks for scene/project operations; this narrowly permits
    // the edit gate, not arbitrary work from a plugin callback.
    bool CanInvokeEdit() const noexcept;
    void Drain();
    // Pure host invalidation uses the same safe dispatch boundary, without
    // entering the plugin task/edit origin or refreshing the complete UI.
    void SetRefreshGuard(std::function<bool()>);
    void AdvanceRefreshEpoch();
    bool Stop();
    void Event(uint32_t kind, MmsBytes payload = {}, uint32_t version = 0);
    UiRegistry& Ui();
    MmsResult SaveUiPreferences();
    StorageStore& Storage();
    bool InCallback() const;
    // Excludes an idle prepared project load, which shutdown must cancel.
    bool ExecutingCallback() const;
    std::vector<PluginStatus> Status() const;
};
}
