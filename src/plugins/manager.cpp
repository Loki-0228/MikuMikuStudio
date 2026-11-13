#include "manager.hpp"
#include "mikumikustudio/legacy_mme_bootstrap.hpp"
#include "ui_preferences_file.hpp"
#include "edit_store.hpp"
#include "ui_refresh_binding.hpp"
#include "ui_property_refresh.hpp"
#include "render_image_store.hpp"
#include "evaluation_store.hpp"
#include <atomic>
#include <mutex>

namespace mikumikustudio::plugins {
struct Manager::Impl {
    struct Instance {
        Impl* host;
        UiClient ui;
        SceneClient scene;
        StorageClient storage;
        EditClient edit;
        RefreshClient refresh{nullptr, {}};
        RenderService::Client render;
        RenderImageStore::Client images;
        EvaluationStore::Client evaluation;
        std::atomic<bool> stopping{false};
        PluginStatus status;
        MmsHostV1 table{};
        MmsPluginApiV1 api{};
        void* instance = nullptr;
        LoadedPackage code;
        bool started = false, stopFailed = false, stopSucceeded = false;
        // Last device notification this plugin was given. RecordDevice* never
        // touches these; only FlushDeviceEvents advances a cursor, right before
        // the matching onEvent call, so an exception still counts as attempted.
        uint64_t deviceLossEpoch = 0, deviceRestoredGeneration = 0;
        std::vector<std::string> dependencies;
    };
    struct Task {
        Instance* owner;
        MmsHandle handle;
        void (MMS_CALL *run)(void*);
        void (MMS_CALL *cancelled)(void*);
        void* context;
        bool cancel = false, running = false;
        const RefreshClient* refreshCause = nullptr;
    };
    DWORD thread = GetCurrentThreadId();
    fs::path userRoot;
    Log log;
    std::function<void()> hostUiChanged;
    bool uiRegistryChanged = false;
    bool layoutChanged = false;
    uint64_t layoutRevision = 0;
    PropertyNotificationQueue pendingPropertyChanges;
    UiRegistry registry;
    RenderService render;
    RenderImageStore images;
    UiPreferencesFile preferences;
    MmsResult preferencesError = MMS_OK;
    StorageStore storage;
    std::shared_ptr<SceneStore> scene;
    std::shared_ptr<EvaluationStore> evaluations;
    NativeLoader loader;
    std::vector<std::unique_ptr<Instance>> instances;
    struct EditBridge final : EditHost {
        Impl& manager;
        EditHost& target;
        EditBridge(Impl& m, EditHost& h) : manager(m), target(h) {}
        void AttachStore(EditStore* s) override { target.AttachStore(s); }
        void DetachStore(EditStore* s) noexcept override { target.DetachStore(s); }
        MmsResult Revision(uint64_t& r) override { return target.Revision(r); }
        MmsResult Enter() override {
            manager.forwardingEdit = true;
            struct Reset { bool& value; ~Reset() { value = false; } } reset{manager.forwardingEdit};
            return target.Enter();
        }
        void Leave() noexcept override { target.Leave(); }
        MmsResult Reserve(std::shared_ptr<const EditRecord> r, std::unique_ptr<EditPublication>& p) override {
            return target.Reserve(std::move(r), p);
        }
        MmsResult History(bool redo) override {
            manager.forwardingEdit = true;
            struct Reset { bool& value; ~Reset() { value = false; } } reset{manager.forwardingEdit};
            return target.History(redo);
        }
        MmsResult WithParticipantCallback(MmsResult (*callback)(void*), void* context) override {
            return manager.registry.WithReadOnlyCallback(callback, context);
        }
        void Poison() noexcept override { target.Poison(); }
    };
    // Destroy the store before its forwarding host, clients and storage.
    std::unique_ptr<EditBridge> editHost;
    std::unique_ptr<EditStore> edits;
    Instance* runningEditTask = nullptr;
    Instance* editCallOwner = nullptr;
    struct StageGrant {
        Instance* owner;
        MmsHandle transaction;
        PropertyStageToken* token;
        const PropertyStageFrame* frame;
    };
    StageGrant* stageGrant = nullptr;
    struct InputRequest {
        Instance* owner;
        MmsHandle transaction;
        std::shared_ptr<const std::atomic<bool>> intent;
    };
    InputRequest* inputRequest = nullptr;
    bool forwardingEdit = false, loadAttempted = false, notifyingEdit = false;
    std::function<void()> editPublished;
    std::vector<PluginStatus> failed;
    std::mutex mutex;
    std::map<MmsHandle, std::shared_ptr<Task>> tasks;
    uint64_t nextTask = 1, sequence = 1;
    HWND wake = nullptr;
    bool closing = false, draining = false, sending = false;
    unsigned active = 0;
    // Device state notification bookkeeping. Recording only updates these
    // scalars; FlushDeviceEvents is the sole dispatcher. lossEpoch identifies one
    // accepted loss so each instance is notified once, while revision abandons a
    // batch whose remaining notifications went stale.
    uint64_t latestLossGeneration = 0, lossEpoch = 0, currentGeneration = 0;
    uint64_t deviceEventRevision = 0;
    bool deviceLost = false, deviceEventsPending = false;
    static constexpr UINT kDrain = WM_APP + 1;
    static constexpr UINT kRefreshDrain = WM_APP + 2;
    static constexpr UINT_PTR kTaskRetry = 0x4d4d5371;
    static constexpr UINT_PTR kRefreshRetry = 0x4d4d5372;
    std::function<bool()> refreshGuard;

    Impl(fs::path user, Log output, std::function<void()> changed, bool fallback, std::shared_ptr<SceneStore> sceneStore, bool unifiedSettings,
        std::shared_ptr<EvaluationStore> evaluationStore):
        userRoot(std::move(user)), log(std::move(output)), hostUiChanged(std::move(changed)),
        registry([this] { uiRegistryChanged = true; if (hostUiChanged) hostUiChanged(); },
            [this](UiClient& client, MmsResult result) { if (result) for (const auto& i : instances)
                if (&i->ui == &client) { CancelFailedEdit(*i); break; } }),
        preferences(userRoot / L"ui-layout.json", unifiedSettings), scene(std::move(sceneStore)), evaluations(std::move(evaluationStore)), loader(fallback) {
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = Window; wc.lpszClassName = L"MMS.PluginAbi.Dispatch.V1";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("plugin dispatcher class");
        wake = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        if (!wake) throw std::runtime_error("plugin dispatcher window");
        registry.SetLayoutChanged([this](uint64_t revision) { layoutChanged = true; layoutRevision = revision; });
        registry.Refresh().SetWake([this] { return PostMessageW(wake, kRefreshDrain, 0, 0) != FALSE; });
        registry.SetPropertyStageBridge([this](UiClient& client, MmsHandle transaction,
            const std::shared_ptr<PropertyRecord>& record, MmsObjectRef target, const MmsValue& value) {
            return StageProperty(client, transaction, record, target, value);
        });
        registry.SetPropertyInputBridges([this](UiClient& client, uint64_t& revision) {
            return InputReadRevision(client, revision);
        }, [this](UiClient& client, const PropertyInputSnapshot& snapshot, const MmsValue& value) {
            return InputEdit(client, snapshot, value);
        });
        // The manager's edit bridge reserves and publishes its own deferred
        // property event. Do not also install the registry's optional direct
        // observer, which would deliver the same successful edit twice.
    }
    ~Impl() { if (wake) DestroyWindow(wake); }
    bool EditOrigin(const Instance& i) const noexcept {
        if (GetCurrentThreadId() != thread || closing || i.stopping || render.InCallback() || notifyingEdit || storage.InCallback() || storage.Prepared()) return false;
        return (active == 0 && registry.InvokingClient() == &i.ui) ||
            (active == 1 && runningEditTask == &i && !registry.InCallback());
    }
    bool CanInvokeEdit() const noexcept {
        return forwardingEdit && edits && editCallOwner && EditOrigin(*editCallOwner);
    }
    template<class F> MmsResult EditCall(Instance& i, F&& call) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (!edits || i.stopping || closing) return MMS_E_NOT_READY;
        if (editCallOwner || edits->InCallback() || !EditOrigin(i)) return MMS_E_BUSY;
        editCallOwner = &i;
        struct Reset { Instance*& value; ~Reset() { value = nullptr; } } reset{editCallOwner};
        auto api = edits->Api(i.edit);
        return call(api);
    }
    struct PropertyGuard final : EditStageGuard {
        UiRegistry& registry;
        std::shared_ptr<PropertyRecord> record;
        std::string id;
        std::shared_ptr<const std::atomic<bool>> intent;
        PropertyGuard(UiRegistry& ui, std::shared_ptr<PropertyRecord> property,
                std::shared_ptr<const std::atomic<bool>> requested = {}):
            registry(ui), record(std::move(property)), id(String(record->desc.info.id)), intent(std::move(requested)) {}
        bool IsCurrent() const noexcept override {
            return (!intent || intent->load(std::memory_order_acquire)) &&
                registry.IsPropertyCurrent(id, record.get(), record->owner);
        }
    };
    MmsResult StageProperty(UiClient& client, MmsHandle transaction,
        const std::shared_ptr<PropertyRecord>& record, MmsObjectRef target, const MmsValue& value) {
        auto owner = std::find_if(instances.begin(), instances.end(),
            [&](const auto& instance) { return &instance->ui == &client; });
        if (owner == instances.end()) return MMS_E_NOT_READY;
        auto& instance = **owner;
        return EditCall(instance, [&](auto&) {
            const auto intent = inputRequest && inputRequest->owner == &instance && inputRequest->transaction == transaction
                ? inputRequest->intent : std::shared_ptr<const std::atomic<bool>>{};
            auto guard = std::make_shared<PropertyGuard>(registry, record, intent);
            return edits->StageProperty(instance.edit, transaction, std::move(guard),
                [&](PropertyStageToken& token) {
                    return registry.WithPropertyStage(record, transaction, [&](const PropertyStageFrame& frame) {
                        StageGrant grant{&instance, transaction, &token, &frame};
                        auto* previous = stageGrant;
                        stageGrant = &grant;
                        struct Reset { StageGrant*& slot; StageGrant* prior; ~Reset() { slot = prior; } } reset{stageGrant, previous};
                        return record->desc.stage(record->desc.context, transaction, target, &value);
                    });
                });
        });
    }
    MmsResult JoinEdit(Instance& instance, MmsHandle transaction, const MmsTransactionParticipantV1* participant) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (!edits || instance.stopping || closing) return MMS_E_NOT_READY;
        if (stageGrant && stageGrant->owner == &instance && stageGrant->transaction == transaction &&
            editCallOwner == &instance && !notifyingEdit && !storage.InCallback() && !storage.Prepared() &&
            registry.IsPropertyStageCurrent(*stageGrant->frame))
            return edits->JoinPropertyStage(*stageGrant->token, instance.edit, participant);
        return EditCall(instance, [&](auto& api) { return api.join(api.context, transaction, participant); });
    }
    MmsResult InputReadRevision(UiClient& client, uint64_t& revision) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        const auto owner = std::find_if(instances.begin(), instances.end(),
            [&](const auto& instance) { return &instance->ui == &client; });
        if (owner == instances.end() || !edits || !editHost || closing || (*owner)->stopping || client.stopping)
            return MMS_E_NOT_READY;
        // Pure query only. Capturing a draft never acquires an edit gate or
        // grants the property read callback the command's edit authority.
        return editHost->Revision(revision);
    }
    MmsResult InputEdit(UiClient& client, const PropertyInputSnapshot& snapshot, const MmsValue& value) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        const auto owner = std::find_if(instances.begin(), instances.end(),
            [&](const auto& instance) { return &instance->ui == &client; });
        if (owner == instances.end() || !edits || closing || (*owner)->stopping) return MMS_E_NOT_READY;
        auto& instance = **owner;
        if (!EditOrigin(instance)) return MMS_E_BUSY;
        const auto intent = snapshot.intent;
        const auto intended = [&]() noexcept { return !intent || intent->load(std::memory_order_acquire); };
        auto api = EditApi(instance); auto ui = registry.Api(instance.ui);
        MmsHandle transaction = 0; bool committed = false;
        struct Cancel {
            MmsEditApiV1& api; MmsHandle& transaction; bool& committed;
            ~Cancel() { if (transaction && !committed) api.cancel(api.context, transaction); }
        } cancel{api, transaction, committed};
        const auto current = [&]() -> MmsResult {
            if (!intended() || !registry.IsPropertyInputCurrent(snapshot)) return MMS_E_STALE;
            PropertyInputSnapshot fresh;
            const auto captured = registry.CapturePropertyInput(snapshot.elementId, fresh);
            if (!intended()) return MMS_E_STALE;
            if (captured) return captured;
            if (!fresh.editable) return MMS_E_NOT_READY;
            return registry.IsPropertyInputCurrent(snapshot) && fresh.sceneRevision == snapshot.sceneRevision &&
                fresh.value.Equals(snapshot.value) ? MMS_OK : MMS_E_STALE;
        };
        if (const auto result = current()) return result;
        const auto& info = snapshot.property->desc.info;
        const auto label = info.label.bytes ? info.label : info.id;
        if (const auto result = api.begin(api.context, label, snapshot.sceneRevision, &transaction)) return result;
        if (const auto result = current()) return result;
        InputRequest request{&instance, transaction, intent};
        auto* previous = inputRequest; inputRequest = &request;
        struct ResetRequest {
            InputRequest*& value; InputRequest* previous;
            ~ResetRequest() { value = previous; }
        } resetRequest{inputRequest, previous};
        if (const auto result = ui.stageProperty(ui.context, transaction, Text(snapshot.propertyId), {}, &value)) return result;
        if (const auto result = current()) return result;
        if (const auto result = api.prepare(api.context, transaction)) return result;
        if (const auto result = current()) return result;
        PropertyNotificationQueue::Reservation notification;
        if (const auto result = pendingPropertyChanges.Prepare(snapshot.propertyId, notification)) return result;
        const auto result = api.commit(api.context, transaction);
        committed = result == MMS_OK;
        if (result == MMS_OK) {
            uint64_t revision = snapshot.sceneRevision;
            if (editHost) {
                uint64_t current = 0;
                if (Boundary([&] { return editHost->Revision(current); }) == MMS_OK && current) revision = current;
            }
            if (!revision) revision = registry.Revision();
            pendingPropertyChanges.Publish(notification, revision);
            WakePropertyChanges();
        }
        return result;
    }
    void CancelFailedEdit(Instance& i) noexcept {
        if (!edits) return;
        const auto result = Boundary([&] { return edits->CancelOwned(i.edit); });
        if (result) try { Write(i.ui.id + ": failed callback edit cancellation returned " + std::to_string(result)); } catch (...) {}
    }
    void EditPublished() noexcept {
        if (!editPublished) return;
        notifyingEdit = true;
        struct Reset { bool& value; ~Reset() { value = false; } } reset{notifyingEdit};
        if (Boundary([&] { editPublished(); return MMS_OK; }))
            try { Write("edit publication observer failed; committed history retained"); } catch (...) {}
    }
    MmsEditApiV1 EditApi(Instance& i) {
        MmsEditApiV1 api{Header<MmsEditApiV1>(), &i};
        api.begin = [](void* p, MmsString label, uint64_t revision, MmsHandle* out) {
            return Boundary([&] { auto& i = *static_cast<Instance*>(p); return i.host->EditCall(i,
                [&](auto& a) { return a.begin(a.context, label, revision, out); }); });
        };
        api.join = [](void* p, MmsHandle tx, const MmsTransactionParticipantV1* participant) {
            return Boundary([&] { auto& i = *static_cast<Instance*>(p); return i.host->JoinEdit(i, tx, participant); });
        };
        api.prepare = [](void* p, MmsHandle tx) {
            return Boundary([&] { auto& i = *static_cast<Instance*>(p); return i.host->EditCall(i,
                [&](auto& a) { return a.prepare(a.context, tx); }); });
        };
        api.commit = [](void* p, MmsHandle tx) {
            return Boundary([&] { auto& i = *static_cast<Instance*>(p); const auto result = i.host->EditCall(i,
                [&](auto& a) { return a.commit(a.context, tx); });
                if (!result) i.host->EditPublished(); return result; });
        };
        api.cancel = [](void* p, MmsHandle tx) {
            Boundary([&] { auto& i = *static_cast<Instance*>(p); return i.host->EditCall(i,
                [&](auto& a) { a.cancel(a.context, tx); return MMS_OK; }); });
        };
        api.undo = [](void* p) { return Boundary([&] { auto& i = *static_cast<Instance*>(p);
            const auto result = i.host->EditCall(i, [&](auto& a) { return a.undo(a.context); });
            if (!result) i.host->EditPublished(); return result; }); };
        api.redo = [](void* p) { return Boundary([&] { auto& i = *static_cast<Instance*>(p);
            const auto result = i.host->EditCall(i, [&](auto& a) { return a.redo(a.context); });
            if (!result) i.host->EditPublished(); return result; }); };
        return api;
    }
    MmsResult AttachEditHost(EditHost& host) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (active || render.InCallback() || registry.InCallback() || storage.InCallback() || storage.Prepared()) return MMS_E_BUSY;
        if (loadAttempted || closing || edits) return MMS_E_CONFLICT;
        auto bridge = std::make_unique<EditBridge>(*this, host);
        auto store = std::make_unique<EditStore>(*bridge, storage);
        editHost = std::move(bridge); edits = std::move(store); return MMS_OK;
    }
    MmsResult DetachEditHost() {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (active || render.InCallback() || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
            editCallOwner || (edits && edits->InCallback())) return MMS_E_BUSY;
        if (!edits) return MMS_OK;
        const auto result = edits->CancelPending();
        if (result) return result;
        for (const auto& i : instances) { i->edit.stopping = true; i->edit.store = nullptr; }
        edits.reset(); editHost.reset(); return result;
    }
    static LRESULT CALLBACK Window(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        if (self && (message == kDrain || (message == WM_TIMER && wp == kTaskRetry))) {
            KillTimer(hwnd, kTaskRetry);
            Boundary([&] { self->Drain(); return MMS_OK; });
            self->RetryTasks();
            return 0;
        }
        if (self && (message == kRefreshDrain || (message == WM_TIMER && wp == kRefreshRetry))) {
            KillTimer(hwnd, kRefreshRetry);
            const auto result = Boundary([&] { self->DrainRefresh(); return MMS_OK; });
            if (result) self->registry.Refresh().FailPending(result);
            if (!self->closing && self->registry.Refresh().Pending() && !SetTimer(hwnd, kRefreshRetry, 16, nullptr))
                self->registry.Refresh().FailPending(MMS_E_FAILED);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    void Write(const std::string& text) noexcept { try { if (log) log(text); } catch (...) {} }
    MmsResult SavePreferences() {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (active || render.InCallback() || registry.InCallback() || closing) return MMS_E_BUSY;
        const auto result = preferences.Save(registry);
        if (result && result != preferencesError) Write("UI preferences save refused result=" + std::to_string(result));
        preferencesError = result; return result;
    }
    MmsResult Query(Instance& i, MmsString name, uint32_t major, uint32_t minor, uint32_t bytes, void* out) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        const auto id = String(name);
        if (id == "mms.ui.catalog") {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            return Table(major, minor, bytes, out, registry.Catalog().Api(i.ui));
        }
        if (id == "mms.ui") return Table(major, minor, bytes, out, registry.Api(i.ui));
        if (id == "mms.ui.surface") return Table(major, minor, bytes, out, registry.SurfaceApi(i.ui));
        if (id == "mms.ui.layout.session") {
            if (major != 1 || minor > 2) return MMS_E_VERSION;
            if (minor == 2) {
                if (!out || bytes < sizeof(MmsUiLayoutSessionApiV1_2)) return MMS_E_ARGUMENT;
                const auto api = registry.LayoutSessionApi12(i.ui);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            if (!out || bytes < sizeof(MmsUiLayoutSessionApiV1)) return MMS_E_ARGUMENT;
            const auto api = registry.LayoutSessionApi(i.ui, minor);
            std::memcpy(out, &api, sizeof(api)); return MMS_OK;
        }
        if (id == "mms.ui.layout") {
            if (minor == 1) {
                if (major != 1) return MMS_E_VERSION;
                if (!out || bytes < sizeof(MmsUiLayoutApiV1_1)) return MMS_E_ARGUMENT;
                const auto api = registry.LayoutHistoryApi(i.ui);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            return Table(major, minor, bytes, out, registry.LayoutApi(i.ui));
        }
        if (id == "mms.ui.refresh") {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            return Table(major, minor, bytes, out, registry.Refresh().Api(i.refresh));
        }
        if (id == "mms.ui.refresh.bind") {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            return Table(major, minor, bytes, out, registry.Refresh().Bindings().Api(i.refresh));
        }
        if (id == "mms.ui.refresh.subscribe") {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            if (minor == 1) {
                if (!out || bytes < sizeof(MmsUiRefreshSubscriptionApiV1_1)) return MMS_E_ARGUMENT;
                if (major != 1) return MMS_E_VERSION;
                const auto api = registry.Refresh().Bindings().ScopedSubscriptionApi(i.refresh);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            return Table(major, minor, bytes, out, registry.Refresh().Bindings().SubscriptionApi(i.refresh));
        }
        if (id == "mms.ui.refresh.property") {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            if (minor == 1) {
                if (!out || bytes < sizeof(MmsUiPropertyRefreshApiV1_1)) return MMS_E_ARGUMENT;
                if (major != 1) return MMS_E_VERSION;
                const auto api = registry.Refresh().PropertyRefresh().ScopedApi(i.refresh);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            return Table(major, minor, bytes, out, registry.Refresh().PropertyRefresh().Api(i.refresh));
        }
        if (id == "mms.ui.refresh.dependencies") {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            return Table(major, minor, bytes, out, registry.Refresh().DependencyApi(i.refresh));
        }
        if (id == "mms.ui.refresh.state") {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            return Table(major, minor, bytes, out, registry.StateContentApi(i.ui));
        }
        if (id == "mms.render.image") {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            return Table(major, minor, bytes, out, images.Api(i.images));
        }
        if (id == "mms.render") {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            if (minor == 2) {
                if (major != 1) return MMS_E_VERSION;
                if (!out || bytes < sizeof(MmsRenderApiV1_2)) return MMS_E_ARGUMENT;
                const auto api = render.Api12(i.render);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            if (minor == 1) {
                if (major != 1) return MMS_E_VERSION;
                if (!out || bytes < sizeof(MmsRenderApiV1_1)) return MMS_E_ARGUMENT;
                const auto api = render.Api11(i.render);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            return Table(major, minor, bytes, out, render.Api(i.render));
        }
        if (id == "mms.scene.evaluate" && evaluations) {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            if (minor == 7) {
                if (major != 1) return MMS_E_VERSION;
                if (!out || bytes < sizeof(MmsEvaluationApiV1_7)) return MMS_E_ARGUMENT;
                const auto api = evaluations->Api17(i.evaluation);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            if (minor == 6) {
                if (major != 1) return MMS_E_VERSION;
                if (!out || bytes < sizeof(MmsEvaluationApiV1_6)) return MMS_E_ARGUMENT;
                const auto api = evaluations->Api16(i.evaluation);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            if (minor == 5) {
                if (major != 1) return MMS_E_VERSION;
                if (!out || bytes < sizeof(MmsEvaluationApiV1_5)) return MMS_E_ARGUMENT;
                const auto api = evaluations->Api15(i.evaluation);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            if (minor == 4) {
                if (major != 1) return MMS_E_VERSION;
                if (!out || bytes < sizeof(MmsEvaluationApiV1_4)) return MMS_E_ARGUMENT;
                const auto api = evaluations->Api14(i.evaluation);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            if (minor == 3) {
                if (major != 1) return MMS_E_VERSION;
                if (!out || bytes < sizeof(MmsEvaluationApiV1_3)) return MMS_E_ARGUMENT;
                const auto api = evaluations->Api13(i.evaluation);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            if (minor == 2) {
                if (major != 1) return MMS_E_VERSION;
                if (!out || bytes < sizeof(MmsEvaluationApiV1_2)) return MMS_E_ARGUMENT;
                const auto api = evaluations->Api12(i.evaluation);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            if (minor == 1) {
                if (major != 1) return MMS_E_VERSION;
                if (!out || bytes < sizeof(MmsEvaluationApiV1_1)) return MMS_E_ARGUMENT;
                const auto api = evaluations->Api11(i.evaluation);
                std::memcpy(out, &api, sizeof(api)); return MMS_OK;
            }
            return Table(major, minor, bytes, out, evaluations->Api(i.evaluation));
        }
        if (id == "mms.scene" && scene) {
            if (!out || bytes < sizeof(MmsSceneApiV1)) return MMS_E_ARGUMENT;
            if (major != 1 || minor > 0) return MMS_E_VERSION;
            MmsSceneApiV1 api{};
            if (const auto result = scene->Api(i.scene, api)) return result;
            std::memcpy(out, &api, sizeof(api)); return MMS_OK;
        }
        if (id == "mms.storage") return Table(major, minor, bytes, out, storage.Api(i.storage));
        if (id == "mms.edit" && edits) return Table(major, minor, bytes, out, EditApi(i));
        return MMS_E_NOT_FOUND;
    }
    MmsHostV1 Host(Instance& i) {
        MmsHostV1 h{}; h.h = Header<MmsHostV1>(); h.context = &i;
        h.queryService = [](void* p, MmsString name, uint32_t ma, uint32_t mi, uint32_t n, void* out) {
            return Boundary([&] { auto& i = *static_cast<Instance*>(p); return i.host->Query(i, name, ma, mi, n, out); });
        };
        h.registerProvider = [](void* p, MmsString service, MmsString id, const void* table, uint32_t bytes, MmsHandle* out) -> MmsResult {
            return Boundary([&]() -> MmsResult {
                auto& i = *static_cast<Instance*>(p); if (GetCurrentThreadId() != i.host->thread) return MMS_E_THREAD;
                if (i.stopping) return MMS_E_NOT_READY;
                if (String(service) == "mms.serializer") {
                    if (i.host->edits && i.host->edits->InCallback()) return MMS_E_BUSY;
                    return i.host->storage.Register(i.storage,id,table,bytes,out);
                }
                return MMS_E_UNSUPPORTED;
            });
        };
        h.unregisterProvider = [](void* p, MmsHandle h) { return Boundary([&]() -> MmsResult {
            auto& i = *static_cast<Instance*>(p);
            if (GetCurrentThreadId() != i.host->thread) return MMS_E_THREAD;
            if (i.host->edits && i.host->edits->InCallback()) return MMS_E_BUSY;
            return i.host->storage.Unregister(i.storage,h);
        }); };
        h.log = [](void* p, uint32_t severity, MmsString s) {
            Boundary([&] { auto& i = *static_cast<Instance*>(p); if (severity > 3) return MMS_E_ARGUMENT;
                i.host->Write(i.ui.id + " [" + std::to_string(severity) + "] " + String(s)); return MMS_OK; });
        };
        h.enqueueUi = [](void* p, void(MMS_CALL *run)(void*), void(MMS_CALL *cancel)(void*), void* context, MmsHandle* out) {
            return Boundary([&]() -> MmsResult {
                auto& i = *static_cast<Instance*>(p); auto& host = *i.host;
                if (!run || !cancel || !out) return MMS_E_ARGUMENT;
                std::lock_guard<std::mutex> lock(host.mutex);
                if (i.stopping.load()) return MMS_E_NOT_READY;
                if (host.tasks.size() >= 65536) return MMS_E_BUSY;
                auto h = host.nextTask++; auto t = std::make_shared<Task>(Task{&i, h, run, cancel, context});
                if (host.registry.Refresh().ReadOnlyCause(i.refresh)) t->refreshCause = &i.refresh;
                host.tasks.emplace(h, t);
                if (!PostMessageW(host.wake, kDrain, 0, 0)) { host.tasks.erase(h); return MMS_E_FAILED; }
                *out = h; return MMS_OK;
            });
        };
        h.cancelUi = [](void* p, MmsHandle handle) {
            return Boundary([&]() -> MmsResult {
                auto& i = *static_cast<Instance*>(p); auto& host = *i.host; std::lock_guard<std::mutex> lock(host.mutex);
                auto it = host.tasks.find(handle); if (it == host.tasks.end() || it->second->owner != &i) return MMS_E_NOT_FOUND;
                if (it->second->running) return MMS_E_BUSY;
                it->second->cancel = true; PostMessageW(host.wake, kDrain, 0, 0); return MMS_OK;
            });
        };
        return h;
    }
    void DrainRefresh() {
        if (GetCurrentThreadId() != thread || draining || closing || active || render.InCallback() || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
            (edits && edits->InCallback()) || (refreshGuard && !refreshGuard())) return;
        registry.Refresh().Drain();
    }
    void RetryTasks() noexcept {
        // A modal/read-only callback can consume every posted wake while the
        // mutation gate is closed. Retain a bounded, non-recursive retry until
        // callbacks and project preparation finish. Do not run plugin code here.
        bool failed = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (tasks.empty() && pendingPropertyChanges.Empty()) KillTimer(wake, kTaskRetry);
            else failed = !SetTimer(wake, kTaskRetry, 16, nullptr);
        }
        if (failed) Write("UI task retry timer unavailable; pending tasks retained");
    }
    void Drain() {
        if (GetCurrentThreadId() != thread || draining || active || render.InCallback() || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
            (edits && edits->InCallback())) return;
        draining = true;
        struct Guard { bool& b; ~Guard() { b = false; } } guard{draining};
        // Work scheduled by a callback remains queued for the next dispatch.
        // A fixed batch prevents a self-enqueueing plugin from starving input.
        std::vector<std::shared_ptr<Task>> batch;
        { std::lock_guard<std::mutex> lock(mutex); for (const auto& [id, t] : tasks) { if (batch.size() == 64) break; batch.push_back(t); } }
        for (const auto& t : batch) {
            bool cancel;
            { std::lock_guard<std::mutex> lock(mutex); t->running = true; cancel = t->cancel || t->owner->stopping; }
            ++active;
            runningEditTask = cancel ? nullptr : t->owner;
            const auto inherited = registry.Refresh().ExchangeInheritedReadOnlyCause(t->refreshCause);
            struct RefreshCauseGuard {
                UiRefreshQueue& queue; const RefreshClient* previous;
                ~RefreshCauseGuard() { queue.ExchangeInheritedReadOnlyCause(previous); }
            } refreshCause{registry.Refresh(), inherited};
            const auto call = [&] { return Boundary([&] { (cancel ? t->cancelled : t->run)(t->context); return MMS_OK; }); };
            auto r = t->refreshCause ? registry.WithReadOnlyCallback(call) : call();
            runningEditTask = nullptr;
            --active; if (r) { CancelFailedEdit(*t->owner); Write(t->owner->ui.id + ": queued callback threw an exception"); }
            { std::lock_guard<std::mutex> lock(mutex); tasks.erase(t->handle); }
        }
        {
            std::lock_guard<std::mutex> lock(mutex); if (!tasks.empty()) PostMessageW(wake, kDrain, 0, 0);
        }
        // Registry and layout mutations are coalesced here, outside the
        // registry callback, so a plugin onEvent sees the final revision.
        if (uiRegistryChanged) {
            uiRegistryChanged = false;
            MmsRevisionEventV1 event{Header<MmsRevisionEventV1>(), registry.Revision()};
            Event(MMS_EVENT_UI_REGISTRY_CHANGED, {&event, sizeof(event)}, 1);
        }
        if (layoutChanged) {
            layoutChanged = false;
            MmsRevisionEventV1 event{Header<MmsRevisionEventV1>(), layoutRevision};
            Event(MMS_EVENT_LAYOUT_CHANGED, {&event, sizeof(event)}, 1);
        }
        FlushPropertyChanges();
    }
    void Event(uint32_t kind, MmsBytes payload = {}, uint32_t version = 0,
               MmsObjectRef object = {}) {
        if (GetCurrentThreadId() != thread || sending || active || render.InCallback() || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
            (edits && edits->InCallback())) return;
        sending = true; struct Guard { bool& v; ~Guard() { v = false; } } guard{sending};
        MmsEventV1 event{}; event.h = Header<MmsEventV1>(); event.kind = kind; event.sequence = sequence++;
        event.payload = payload; event.payloadVersion = version; event.object = object;
        for (const auto& i : instances) if (i->started && !i->stopping) {
            ++active; auto r = Boundary([&] { i->api.onEvent(i->instance, &event); return MMS_OK; }); --active;
            if (r) try { Write(i->ui.id + ": event callback failed"); } catch (...) {}
        }
    }
    void RecordDeviceLost(uint64_t generation) noexcept {
        if (!generation) return;
        // A repeated report of the same loss must not create a second epoch.
        if (deviceLost && latestLossGeneration == generation) return;
        deviceLost = true;
        latestLossGeneration = generation;
        currentGeneration = generation;
        ++lossEpoch;
        ++deviceEventRevision;
        deviceEventsPending = true;
    }
    void RecordDeviceRestored(uint64_t generation) noexcept {
        // A restore without a recorded loss, or with generation zero, is not a
        // valid device sequence and creates no notification.
        if (!generation || !deviceLost) return;
        deviceLost = false;
        currentGeneration = generation;
        ++deviceEventRevision;
        deviceEventsPending = true;
    }
    bool DeviceEventsPending() const noexcept { return deviceEventsPending; }
    bool FlushDeviceEvents() noexcept {
        if (!deviceEventsPending) return true;
        // Same safe dispatch boundary as Event, plus Render must have no pending
        // lease drain: a plugin may only reacquire resources after the loss was
        // released outside a callback.
        if (GetCurrentThreadId() != thread || sending || active || render.InCallback() || registry.InCallback() ||
            storage.InCallback() || storage.Prepared() || (edits && edits->InCallback()) ||
            render.DeviceLossPending()) return false;
        sending = true;
        struct Guard { bool& value; ~Guard() { value = false; } } guard{sending};
        // Snapshot scalars only; the instance list is walked in place so a batch
        // never allocates. A RecordDevice* call from inside a callback bumps the
        // revision, abandoning the rest of this batch instead of delivering an
        // older RESTORED after a newer LOST.
        const uint64_t revision = deviceEventRevision;
        const uint64_t epoch = lossEpoch, lossGeneration = latestLossGeneration;
        const uint64_t generation = currentGeneration;
        const bool available = !deviceLost;
        for (const auto& instance : instances) {
            auto& i = *instance;
            if (!i.started || i.stopping || i.deviceLossEpoch == epoch) continue;
            if (deviceEventRevision != revision) return false;
            MmsDeviceEventV1 payload{Header<MmsDeviceEventV1>(), lossGeneration, 0, 0};
            MmsEventV1 event{};
            event.h = Header<MmsEventV1>();
            event.kind = MMS_EVENT_DEVICE_LOST;
            event.payloadVersion = 1;
            event.sequence = sequence++;
            event.payload = {&payload, sizeof(payload)};
            // Commit first: an exception still counts as attempted, and a later
            // flush must not repeat this epoch for the same plugin.
            i.deviceLossEpoch = epoch;
            ++active;
            const auto result = Boundary([&] { i.api.onEvent(i.instance, &event); return MMS_OK; });
            --active;
            // A log allocation failure must not interrupt delivery to the rest.
            if (result) try { Write(i.ui.id + ": device lost event callback failed"); } catch (...) {}
        }
        if (deviceEventRevision != revision) return false;
        if (!available) { deviceEventsPending = false; return true; }
        // Every instance was offered the loss above; only now deliver the restore
        // to the ones that have not seen this generation yet.
        for (const auto& instance : instances) {
            auto& i = *instance;
            if (!i.started || i.stopping || i.deviceRestoredGeneration == generation) continue;
            if (deviceEventRevision != revision) return false;
            MmsDeviceEventV1 payload{Header<MmsDeviceEventV1>(), generation, 0, 0};
            MmsEventV1 event{};
            event.h = Header<MmsEventV1>();
            event.kind = MMS_EVENT_DEVICE_RESTORED;
            event.payloadVersion = 1;
            event.sequence = sequence++;
            event.payload = {&payload, sizeof(payload)};
            i.deviceRestoredGeneration = generation;
            ++active;
            const auto result = Boundary([&] { i.api.onEvent(i.instance, &event); return MMS_OK; });
            --active;
            if (result) try { Write(i.ui.id + ": device restored event callback failed"); } catch (...) {}
        }
        if (deviceEventRevision != revision) return false;
        deviceEventsPending = false;
        return true;
    }
    void BroadcastPropertyChanged(const std::string& id, uint64_t revision) {
        if (id.empty() || !revision) return;
        MmsPropertyEventV1 payload{Header<MmsPropertyEventV1>(), {id.data(), id.size()}, {}, revision};
        Event(MMS_EVENT_PROPERTY_CHANGED, {&payload, sizeof(payload)}, 1);
    }
    // A declarative property edit commits inside a plugin callback, where Event
    // dispatch is illegal. Keep the change until Drain can broadcast it outside
    // the callback.
    void WakePropertyChanges() noexcept {
        if (pendingPropertyChanges.Empty()) return;
        if (!PostMessageW(wake, kDrain, 0, 0) && !SetTimer(wake, kTaskRetry, 16, nullptr))
            try { Write("property notification wake unavailable; pending changes retained"); } catch (...) {}
    }
    void FlushPropertyChanges() {
        if (pendingPropertyChanges.Empty() || GetCurrentThreadId() != thread || sending || active ||
            render.InCallback() || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
            (edits && edits->InCallback())) return;
        while (!pendingPropertyChanges.Empty()) {
            const auto& item = pendingPropertyChanges.Front();
            if (Boundary([&] { BroadcastPropertyChanged(item.first, item.second); return MMS_OK; }) != MMS_OK) {
                WakePropertyChanges(); return;
            }
            pendingPropertyChanges.Pop();
        }
    }
    bool StopOne(Instance& i) {
        i.stopping = true; i.ui.stopping = true; i.scene.stopping = true; i.storage.stopping = true; i.edit.stopping = true; i.status.stage = "stopping";
        registry.RevokeLayoutPreviews(i.ui);
        // Retire even clients with no callbacks in the dispatch list. Cached
        // service tables must not resurrect callbacks from stop/destroy.
        i.render.retired = true;
        render.DropClient(i.render.id);
        images.Retire(i.images);
        if (evaluations) evaluations->Retire(i.evaluation);
        registry.Refresh().Retire(i.refresh);
        if (edits && !edits->Retire(i.edit)) return false;
        if (i.stopFailed) return false;
        { std::lock_guard<std::mutex> lock(mutex); for (auto& [id, task] : tasks) if (task->owner == &i) task->cancel = true; }
        Drain();
        MmsResult r = MMS_OK;
        if (i.instance && !i.stopSucceeded) {
            ++active; r = Boundary([&] { return i.api.stop(i.instance); }); --active;
            if (r == MMS_OK) i.stopSucceeded = true;
        }
        if (r != MMS_OK) {
            if (r != MMS_E_BUSY) { i.stopFailed = true; i.status.error = "stop returned " + std::to_string(r); Write(i.ui.id + ": " + i.status.error); }
            return false;
        }
        { std::lock_guard<std::mutex> lock(mutex); for (const auto& [id, task] : tasks) if (task->owner == &i) return false; }
        if (images.HasLeases(i.images) || registry.HasLeases(i.ui) || registry.Refresh().HasLeases(i.refresh) || (scene && scene->HasLeases(i.scene))) return false;
        if (evaluations && evaluations->HasLeases(i.evaluation)) return false;
        if (!storage.Retire(i.storage)) return false;
        r = Boundary([&] { registry.Retire(i.ui); return MMS_OK; });
        if (r != MMS_OK || registry.HasLeases(i.ui)) {
            i.stopFailed = true; i.status.error = "UI retirement failed; code retained"; Write(i.ui.id + ": " + i.status.error); return false;
        }
        if (i.instance) { ++active; r = Boundary([&] { i.api.destroy(i.instance); return MMS_OK; }); --active; }
        if (r != MMS_OK) { i.stopFailed = true; i.status.error = "destroy failed; code retained"; return false; }
        i.instance = nullptr; i.started = false; i.status.stage = "stopped";
        // Destroy dependents before releasing any dependency library.
        while (!i.code.modules.empty()) i.code.modules.pop_back(); i.code.entry = nullptr; return true;
    }
    bool Stop() {
        if (GetCurrentThreadId() != thread || active || render.InCallback() || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
            (edits && edits->InCallback())) return false;
        if (!closing) {
            // Deliver committed property edits to every live subscriber before
            // UI_CLOSING and before any instance enters stopping. If dispatch
            // cannot complete, retain the manager and retry at a safe point.
            FlushPropertyChanges();
            if (!pendingPropertyChanges.Empty()) { WakePropertyChanges(); return false; }
            Event(MMS_EVENT_UI_CLOSING); SavePreferences(); closing = true;
        }
        KillTimer(wake, kRefreshRetry);
        for (const auto& i : instances) {
            i->stopping = true; i->ui.stopping = true; i->scene.stopping = true;
            registry.RevokeLayoutPreviews(i->ui);
            images.Retire(i->images);
            if (evaluations) evaluations->Retire(i->evaluation);
            registry.Refresh().Retire(i->refresh);
        }
        bool complete = true; std::set<std::string> keep;
        for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
            auto& i = **it;
            if (keep.count(i.ui.id) || !StopOne(i)) { complete = false; for (const auto& d : i.dependencies) keep.insert(d); }
        }
        return complete;
    }
    void Load(const fs::path& root) {
        if (GetCurrentThreadId() != thread || closing || !instances.empty()) throw std::runtime_error("plugin manager already started or wrong thread");
        loadAttempted = true;
        auto discovered = Discover(root); for (const auto& d : discovered.diagnostics) Write(d);
        auto order = Resolve(discovered, [&](const std::string& d) { Write(d); }); std::set<std::string> started;
        for (size_t index : order) {
            const auto& m = discovered.packages[index];
            if (m.kind == Manifest::Kind::LegacyMmdPlugin) continue;
            if (m.kind == Manifest::Kind::LegacyMme) {
                const auto& legacy = LegacyMmeStatus();
                const bool loaded = legacy.loaded && legacy.id == m.id;
                failed.push_back({m.id, loaded ? "legacy-loaded" : "failed",
                    loaded ? std::string{} : (legacy.error.empty() ? "legacy MME requires graphics startup and restart" : legacy.error), m.package});
                // Legacy packages do not supply native ABI services or a
                // hot-unload lifecycle. They cannot satisfy native dependencies.
                continue;
            }
            auto instance = std::make_unique<Instance>(); auto& i = *instance; i.host = this; i.ui = {&registry, m.id}; i.render.id = m.id; i.status = {m.id, "validated", {}, m.package};
            i.images.owner = &images;
            i.evaluation.owner = evaluations.get();
            i.storage.store = &storage; i.storage.id = m.id;
            i.edit.store = edits.get(); i.edit.id = m.id;
            i.refresh.queue = &registry.Refresh(); i.refresh.owner = m.id;
            for (const auto& dep : m.dependencies) i.dependencies.push_back(dep.id);
            bool ready = std::all_of(i.dependencies.begin(), i.dependencies.end(), [&](const auto& d) { return started.count(d) != 0; });
            if (!ready) { i.status.stage = "failed"; i.status.error = "dependency failed to start"; failed.push_back(i.status); Write(m.id + ": " + i.status.error); continue; }
            try {
                const auto paired = registry.Refresh().PropertyRefresh().BindClient(i.refresh, i.ui);
                if (paired != MMS_OK) throw std::runtime_error("property refresh client pairing failed result=" + std::to_string(paired));
                for (const auto& required : m.services) {
                    // The service preflight must fit every published table,
                    // including append-only evaluation 1.5 (288 bytes).
                    alignas(8) unsigned char table[4096]{};
                    auto r = Query(i, Text(required.id), required.major, required.minor, sizeof(table), table);
                    if (r) throw std::runtime_error("required service unavailable: " + required.id + " result=" + std::to_string(r));
                }
                i.code = loader.Load(m, [&](const std::string& d) { Write(d); }); i.status.stage = "loaded";
                auto get = reinterpret_cast<MmsPluginGetApiFn>(GetProcAddress(i.code.entry, "MMSPluginGetApi"));
                if (!get) throw std::runtime_error("missing MMSPluginGetApi");
                auto r = Boundary([&] { return get(m.major, m.minor, sizeof(i.api), &i.api); });
                if (r || !Valid(&i.api) || !i.api.create || !i.api.start || !i.api.onEvent || !i.api.stop || !i.api.destroy) throw std::runtime_error("invalid plugin API result=" + std::to_string(r));
                auto user = userRoot / Wide(m.id); fs::create_directories(user);
                std::string package = Utf8(m.package.native()), userPath = Utf8(fs::absolute(user).native());
                MmsCreateInfoV1 info{Header<MmsCreateInfoV1>(), Text(m.id), Text(package), Text(userPath), MMS_COMPUTE_CPU, 0};
                i.table = Host(i);
                ++active; r = Boundary([&] { return i.api.create(&i.table, &info, &i.instance); }); --active;
                if (r || !i.instance) {
                    // A failing create must release its own unpublished instance.
                    // If it violates the contract, retain code/context rather than
                    // calling a destroy function with an unvalidated instance.
                    if (i.instance) { i.stopFailed = true; i.stopping = true; i.ui.stopping = true; }
                    throw std::runtime_error("create failed result=" + std::to_string(r));
                }
                i.status.stage = "created";
                ++active; r = Boundary([&] { return i.api.start(i.instance); }); --active;
                if (r) throw std::runtime_error("start failed result=" + std::to_string(r));
                i.started = true; i.status.stage = "started"; started.insert(m.id); Write(m.id + ": started");
                instances.push_back(std::move(instance));
            } catch (const std::exception& e) {
                i.status.stage = "failed"; i.status.error = e.what(); Write(m.id + " " + Utf8(m.package.native()) + ": " + i.status.error);
                auto error = i.status.error;
                // create can accept host tasks before failing with a null
                // instance. Its host context still owns those callbacks.
                // Cancellation must finish while both context and DLL live.
                bool stopped = StopOne(i); i.status.error = error; i.status.stage = stopped ? "failed" : "stopping";
                instances.push_back(std::move(instance));
            }
        }
        const auto preferencesResult = preferences.Load(registry);
        if (preferencesResult) Write("UI preferences load refused; original file preserved result=" + std::to_string(preferencesResult));
        Event(MMS_EVENT_UI_READY);
        PostMessageW(wake, kDrain, 0, 0);
    }
};
Manager::Manager(fs::path user, Log log, std::function<void()> changed, bool fallback, std::shared_ptr<SceneStore> scene, bool unifiedSettings,
    std::shared_ptr<EvaluationStore> evaluations): impl_(std::make_unique<Impl>(std::move(user), std::move(log), std::move(changed), fallback, std::move(scene), unifiedSettings, std::move(evaluations))) {}
Manager::~Manager() {
    if (!impl_) return;
    const auto stopped = Boundary([&] { return impl_->Stop() ? MMS_OK : MMS_E_BUSY; });
    const auto detached = Boundary([&] { return impl_->DetachEditHost(); });
    if (stopped != MMS_OK || detached != MMS_OK) {
        for (const auto& i : impl_->instances) i->code.Pin();
        impl_->Write("plugin stop is pending; retaining manager, instances and DLLs until process exit");
        // A busy worker may still use its host context. Keep that context and
        // its dispatch window alive, not just the DLL's code pages.
        impl_.release();
    }
}
void Manager::Load(const fs::path& root) { impl_->Load(root); }
void Manager::SetRefreshGuard(std::function<bool()> guard) {
    if (GetCurrentThreadId() != impl_->thread || ExecutingCallback()) throw std::invalid_argument("refresh configuration boundary");
    impl_->refreshGuard = std::move(guard);
}
void Manager::AdvanceRefreshEpoch() {
    if (GetCurrentThreadId() != impl_->thread) throw std::invalid_argument("refresh epoch thread");
    impl_->registry.Refresh().AdvanceProjectEpoch();
}
void Manager::NotifyHistoryPublication(uint64_t revision) noexcept {
    auto& properties = impl_->registry.Refresh().PropertyRefresh();
    properties.NotifyHistory(revision);
    properties.HistoryWakeResult(PostMessageW(impl_->wake, Impl::kRefreshDrain, 0, 0) != FALSE);
}
void Manager::RecordDeviceLost(uint64_t generation) noexcept { impl_->RecordDeviceLost(generation); }
void Manager::RecordDeviceRestored(uint64_t generation) noexcept { impl_->RecordDeviceRestored(generation); }
bool Manager::DeviceEventsPending() const noexcept { return impl_->DeviceEventsPending(); }
bool Manager::FlushDeviceEvents() noexcept { return impl_->FlushDeviceEvents(); }
MmsResult Manager::AttachEditHost(EditHost& host) noexcept { return Boundary([&] { return impl_->AttachEditHost(host); }); }
MmsResult Manager::DetachEditHost() noexcept { return Boundary([&] { return impl_->DetachEditHost(); }); }
void Manager::SetEditPublished(std::function<void()> callback) {
    if (GetCurrentThreadId() != impl_->thread || ExecutingCallback()) throw std::invalid_argument("edit observer configuration boundary");
    impl_->editPublished = std::move(callback);
}
bool Manager::CanInvokeEdit() const noexcept { return impl_->CanInvokeEdit(); }
void Manager::Drain() { impl_->Drain(); }
bool Manager::FlushPropertyNotifications() noexcept {
    if (GetCurrentThreadId() != impl_->thread) return false;
    const auto result = Boundary([&] { impl_->FlushPropertyChanges(); return MMS_OK; });
    return result == MMS_OK && impl_->pendingPropertyChanges.Empty();
}
bool Manager::Stop() { return Boundary([&] { return impl_->Stop() ? MMS_OK : MMS_E_BUSY; }) == MMS_OK; }
void Manager::Event(uint32_t kind, MmsBytes payload, uint32_t version, MmsObjectRef object) {
    impl_->Event(kind,payload,version,object);
}
UiRegistry& Manager::Ui() { return impl_->registry; }
RenderService& Manager::Render() { return impl_->render; }
MmsResult Manager::SaveUiPreferences() { return impl_->SavePreferences(); }
StorageStore& Manager::Storage() { return impl_->storage; }
bool Manager::ExecutingCallback() const { return impl_->active != 0 || impl_->render.InCallback() || impl_->notifyingEdit || impl_->registry.InCallback() || impl_->storage.InCallback() ||
    (impl_->edits && impl_->edits->InCallback()); }
bool Manager::InCallback() const { return ExecutingCallback() || impl_->storage.Prepared(); }
std::vector<PluginStatus> Manager::Status() const { auto result = impl_->failed; for (const auto& i : impl_->instances) result.push_back(i->status); return result; }
}
