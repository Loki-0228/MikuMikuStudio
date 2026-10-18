#include "manager.hpp"
#include "ui_preferences_file.hpp"
#include "edit_store.hpp"
#include <atomic>
#include <mutex>

namespace mikudancestudio::plugins {
struct Manager::Impl {
    struct Instance {
        Impl* host;
        UiClient ui;
        SceneClient scene;
        StorageClient storage;
        EditClient edit;
        RefreshClient refresh{nullptr, {}};
        std::atomic<bool> stopping{false};
        PluginStatus status;
        MmsHostV1 table{};
        MmsPluginApiV1 api{};
        void* instance = nullptr;
        LoadedPackage code;
        bool started = false, stopFailed = false;
        std::vector<std::string> dependencies;
    };
    struct Task {
        Instance* owner;
        MmsHandle handle;
        void (MMS_CALL *run)(void*);
        void (MMS_CALL *cancelled)(void*);
        void* context;
        bool cancel = false, running = false;
    };
    DWORD thread = GetCurrentThreadId();
    fs::path userRoot;
    Log log;
    UiRegistry registry;
    UiPreferencesFile preferences;
    MmsResult preferencesError = MMS_OK;
    StorageStore storage;
    std::shared_ptr<SceneStore> scene;
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
        void Poison() noexcept override { target.Poison(); }
    };
    // Destroy the store before its forwarding host, clients and storage.
    std::unique_ptr<EditBridge> editHost;
    std::unique_ptr<EditStore> edits;
    Instance* runningEditTask = nullptr;
    Instance* editCallOwner = nullptr;
    bool forwardingEdit = false, loadAttempted = false, notifyingEdit = false;
    std::function<void()> editPublished;
    std::vector<PluginStatus> failed;
    std::mutex mutex;
    std::map<MmsHandle, std::shared_ptr<Task>> tasks;
    uint64_t nextTask = 1, sequence = 1;
    HWND wake = nullptr;
    bool closing = false, draining = false, sending = false;
    unsigned active = 0;
    static constexpr UINT kDrain = WM_APP + 1;
    static constexpr UINT kRefreshDrain = WM_APP + 2;
    static constexpr UINT_PTR kTaskRetry = 0x4d4d5371;
    static constexpr UINT_PTR kRefreshRetry = 0x4d4d5372;
    std::function<bool()> refreshGuard;

    Impl(fs::path user, Log output, std::function<void()> changed, bool fallback, std::shared_ptr<SceneStore> sceneStore, bool unifiedSettings):
        userRoot(std::move(user)), log(std::move(output)), registry(std::move(changed),
            [this](UiClient& client, MmsResult result) { if (result) for (const auto& i : instances)
                if (&i->ui == &client) { CancelFailedEdit(*i); break; } }),
        preferences(userRoot / L"ui-layout.json", unifiedSettings), scene(std::move(sceneStore)), loader(fallback) {
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = Window; wc.lpszClassName = L"MMS.PluginAbi.Dispatch.V1";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("plugin dispatcher class");
        wake = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        if (!wake) throw std::runtime_error("plugin dispatcher window");
        registry.Refresh().SetWake([this] { return PostMessageW(wake, kRefreshDrain, 0, 0) != FALSE; });
    }
    ~Impl() { if (wake) DestroyWindow(wake); }
    bool EditOrigin(const Instance& i) const noexcept {
        if (GetCurrentThreadId() != thread || closing || i.stopping || notifyingEdit || storage.InCallback() || storage.Prepared()) return false;
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
            return Boundary([&] { auto& i = *static_cast<Instance*>(p); return i.host->EditCall(i,
                [&](auto& a) { return a.join(a.context, tx, participant); }); });
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
        if (active || registry.InCallback() || storage.InCallback() || storage.Prepared()) return MMS_E_BUSY;
        if (loadAttempted || closing || edits) return MMS_E_CONFLICT;
        auto bridge = std::make_unique<EditBridge>(*this, host);
        auto store = std::make_unique<EditStore>(*bridge, storage);
        editHost = std::move(bridge); edits = std::move(store); return MMS_OK;
    }
    MmsResult DetachEditHost() {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (active || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
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
        if (active || registry.InCallback() || closing) return MMS_E_BUSY;
        const auto result = preferences.Save(registry);
        if (result && result != preferencesError) Write("UI preferences save refused result=" + std::to_string(result));
        preferencesError = result; return result;
    }
    MmsResult Query(Instance& i, MmsString name, uint32_t major, uint32_t minor, uint32_t bytes, void* out) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        const auto id = String(name);
        if (id == "mms.ui") return Table(major, minor, bytes, out, registry.Api(i.ui));
        if (id == "mms.ui.surface") return Table(major, minor, bytes, out, registry.SurfaceApi(i.ui));
        if (id == "mms.ui.layout") return Table(major, minor, bytes, out, registry.LayoutApi(i.ui));
        if (id == "mms.ui.refresh") {
            if (closing || i.stopping.load()) return MMS_E_NOT_READY;
            return Table(major, minor, bytes, out, registry.Refresh().Api(i.refresh));
        }
        if (id == "mms.scene" && scene) return Table(major, minor, bytes, out, scene->Api(i.scene));
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
                if (String(service) == "mms.serializer") return i.host->storage.Register(i.storage,id,table,bytes,out);
                return MMS_E_UNSUPPORTED;
            });
        };
        h.unregisterProvider = [](void* p, MmsHandle h) { return Boundary([&] { auto& i = *static_cast<Instance*>(p); return i.host->storage.Unregister(i.storage,h); }); };
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
                auto h = host.nextTask++; auto t = std::make_shared<Task>(Task{&i, h, run, cancel, context}); host.tasks.emplace(h, t);
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
        if (GetCurrentThreadId() != thread || draining || closing || active || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
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
            if (tasks.empty()) KillTimer(wake, kTaskRetry);
            else failed = !SetTimer(wake, kTaskRetry, 16, nullptr);
        }
        if (failed) Write("UI task retry timer unavailable; pending tasks retained");
    }
    void Drain() {
        if (GetCurrentThreadId() != thread || draining || active || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
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
            auto r = Boundary([&] { (cancel ? t->cancelled : t->run)(t->context); return MMS_OK; });
            runningEditTask = nullptr;
            --active; if (r) { CancelFailedEdit(*t->owner); Write(t->owner->ui.id + ": queued callback threw an exception"); }
            { std::lock_guard<std::mutex> lock(mutex); tasks.erase(t->handle); }
        }
        std::lock_guard<std::mutex> lock(mutex); if (!tasks.empty()) PostMessageW(wake, kDrain, 0, 0);
    }
    void Event(uint32_t kind, MmsBytes payload = {}, uint32_t version = 0) {
        if (GetCurrentThreadId() != thread || sending || active || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
            (edits && edits->InCallback())) return;
        sending = true; struct Guard { bool& v; ~Guard() { v = false; } } guard{sending};
        MmsEventV1 event{}; event.h = Header<MmsEventV1>(); event.kind = kind; event.sequence = sequence++;
        event.payload = payload; event.payloadVersion = version;
        for (const auto& i : instances) if (i->started && !i->stopping) {
            ++active; auto r = Boundary([&] { i->api.onEvent(i->instance, &event); return MMS_OK; }); --active;
            if (r) Write(i->ui.id + ": event callback failed");
        }
    }
    bool StopOne(Instance& i) {
        i.stopping = true; i.ui.stopping = true; i.scene.stopping = true; i.storage.stopping = true; i.edit.stopping = true; i.status.stage = "stopping";
        registry.Refresh().Retire(i.refresh);
        if (edits && !edits->Retire(i.edit)) return false;
        if (i.stopFailed) return false;
        { std::lock_guard<std::mutex> lock(mutex); for (auto& [id, task] : tasks) if (task->owner == &i) task->cancel = true; }
        Drain();
        MmsResult r = MMS_OK;
        if (i.instance) { ++active; r = Boundary([&] { return i.api.stop(i.instance); }); --active; }
        if (r != MMS_OK) {
            if (r != MMS_E_BUSY) { i.stopFailed = true; i.status.error = "stop returned " + std::to_string(r); Write(i.ui.id + ": " + i.status.error); }
            return false;
        }
        { std::lock_guard<std::mutex> lock(mutex); for (const auto& [id, task] : tasks) if (task->owner == &i) return false; }
        if (registry.HasLeases(i.ui) || registry.Refresh().HasLeases(i.refresh) || (scene && scene->HasLeases(i.scene))) return false;
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
        if (GetCurrentThreadId() != thread || active || registry.InCallback() || storage.InCallback() || storage.Prepared() ||
            (edits && edits->InCallback())) return false;
        if (!closing) { Event(MMS_EVENT_UI_CLOSING); SavePreferences(); closing = true; }
        KillTimer(wake, kRefreshRetry);
        for (const auto& i : instances) { i->stopping = true; i->ui.stopping = true; i->scene.stopping = true; registry.Refresh().Retire(i->refresh); }
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
            auto instance = std::make_unique<Instance>(); auto& i = *instance; i.host = this; i.ui = {&registry, m.id}; i.status = {m.id, "validated", {}, m.package};
            i.storage.store = &storage; i.storage.id = m.id;
            i.edit.store = edits.get(); i.edit.id = m.id;
            i.refresh.queue = &registry.Refresh(); i.refresh.owner = m.id;
            for (const auto& dep : m.dependencies) i.dependencies.push_back(dep.id);
            bool ready = std::all_of(i.dependencies.begin(), i.dependencies.end(), [&](const auto& d) { return started.count(d) != 0; });
            if (!ready) { i.status.stage = "failed"; i.status.error = "dependency failed to start"; failed.push_back(i.status); Write(m.id + ": " + i.status.error); continue; }
            try {
                for (const auto& required : m.services) {
                    alignas(8) unsigned char table[256]{};
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
Manager::Manager(fs::path user, Log log, std::function<void()> changed, bool fallback, std::shared_ptr<SceneStore> scene, bool unifiedSettings): impl_(std::make_unique<Impl>(std::move(user), std::move(log), std::move(changed), fallback, std::move(scene), unifiedSettings)) {}
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
MmsResult Manager::AttachEditHost(EditHost& host) noexcept { return Boundary([&] { return impl_->AttachEditHost(host); }); }
MmsResult Manager::DetachEditHost() noexcept { return Boundary([&] { return impl_->DetachEditHost(); }); }
void Manager::SetEditPublished(std::function<void()> callback) {
    if (GetCurrentThreadId() != impl_->thread || ExecutingCallback()) throw std::invalid_argument("edit observer configuration boundary");
    impl_->editPublished = std::move(callback);
}
bool Manager::CanInvokeEdit() const noexcept { return impl_->CanInvokeEdit(); }
void Manager::Drain() { impl_->Drain(); }
bool Manager::Stop() { return Boundary([&] { return impl_->Stop() ? MMS_OK : MMS_E_BUSY; }) == MMS_OK; }
void Manager::Event(uint32_t kind, MmsBytes payload, uint32_t version) { impl_->Event(kind,payload,version); }
UiRegistry& Manager::Ui() { return impl_->registry; }
MmsResult Manager::SaveUiPreferences() { return impl_->SavePreferences(); }
StorageStore& Manager::Storage() { return impl_->storage; }
bool Manager::ExecutingCallback() const { return impl_->active != 0 || impl_->notifyingEdit || impl_->registry.InCallback() || impl_->storage.InCallback() ||
    (impl_->edits && impl_->edits->InCallback()); }
bool Manager::InCallback() const { return ExecutingCallback() || impl_->storage.Prepared(); }
std::vector<PluginStatus> Manager::Status() const { auto result = impl_->failed; for (const auto& i : impl_->instances) result.push_back(i->status); return result; }
}
