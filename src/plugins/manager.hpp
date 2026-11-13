#pragma once
#include "ui_registry.hpp"
#include "loader_win32.hpp"
#include "render_service.hpp"
#include "scene_store.hpp"
#include "storage_store.hpp"
#include <list>

namespace mikumikustudio::plugins {
// UI-thread-only notification staging. A reservation allocates before the
// associated edit commits; publishing it never allocates or drops an item.
class PropertyNotificationQueue {
public:
    using Item = std::pair<std::string, uint64_t>;
    class Reservation {
        friend class PropertyNotificationQueue;
        PropertyNotificationQueue* owner_ = nullptr;
        std::list<Item> staged_;
    public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        ~Reservation() { if (owner_) --owner_->reserved_; }
    };
    MmsResult Prepare(const std::string& id, Reservation& out) {
        if (out.owner_ || id.empty()) return MMS_E_ARGUMENT;
        if (pending_.size() + reserved_ >= 64) return MMS_E_BUSY;
        out.staged_.emplace_back(id, 0);
        out.owner_ = this; ++reserved_;
        return MMS_OK;
    }
    void Publish(Reservation& item, uint64_t revision) noexcept {
        if (item.owner_ != this) return;
        item.staged_.front().second = revision;
        pending_.splice(pending_.end(), item.staged_);
        item.owner_ = nullptr; --reserved_;
    }
    bool Empty() const noexcept { return pending_.empty(); }
    const Item& Front() const noexcept { return pending_.front(); }
    void Pop() noexcept { pending_.pop_front(); }
private:
    std::list<Item> pending_;
    size_t reserved_ = 0;
};
class EditHost;
class EvaluationStore;
struct PluginStatus { std::string id, stage, error; fs::path package; };
class Manager {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Manager(fs::path userRoot, Log log, std::function<void()> uiChanged = {}, bool forceFallback = false,
        std::shared_ptr<SceneStore> scene = {}, bool unifiedSettings = false,
        std::shared_ptr<EvaluationStore> evaluations = {});
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
    // UI-thread safe boundary; false leaves pending notifications for retry.
    bool FlushPropertyNotifications() noexcept;
    // Pure host invalidation uses the same safe dispatch boundary, without
    // entering the plugin task/edit origin or refreshing the complete UI.
    void SetRefreshGuard(std::function<bool()>);
    void AdvanceRefreshEpoch();
    // Trusted SharedHistory publication signal. Records a scalar revision and
    // posts the existing refresh dispatcher; never reads plugins or edits UI.
    void NotifyHistoryPublication(uint64_t) noexcept;
    // Internal host-integration device notifications; not part of the C ABI.
    // RecordDevice* are UI-thread scalar updates: no dispatch, no allocation. A
    // rejected generation changes nothing. Repeated loss state with the same
    // generation is ignored, so a burst of loss/restore keeps only the latest
    // loss and the current availability. FlushDeviceEvents dispatches at the
    // same safe boundary as Event and additionally requires Render to have no
    // lease drain pending. False leaves the batch pending for a retry; a device
    // state change between callbacks abandons the stale remainder.
    void RecordDeviceLost(uint64_t) noexcept;
    void RecordDeviceRestored(uint64_t) noexcept;
    bool DeviceEventsPending() const noexcept;
    bool FlushDeviceEvents() noexcept;
    bool Stop();
    void Event(uint32_t kind, MmsBytes payload = {}, uint32_t version = 0,
               MmsObjectRef object = {});
    UiRegistry& Ui();
    MmsResult SaveUiPreferences();
    StorageStore& Storage();
    RenderService& Render();
    bool InCallback() const;
    // Excludes an idle prepared project load, which shutdown must cancel.
    bool ExecutingCallback() const;
    std::vector<PluginStatus> Status() const;
};
}
