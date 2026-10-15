#include "manager.hpp"
#include <atomic>
#include <mutex>

namespace mikudancestudio::plugins {
struct Manager::Impl {
    struct Instance {
        Impl* host;
        UiClient ui;
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
    NativeLoader loader;
    std::vector<std::unique_ptr<Instance>> instances;
    std::vector<PluginStatus> failed;
    std::mutex mutex;
    std::map<MmsHandle, std::shared_ptr<Task>> tasks;
    uint64_t nextTask = 1, sequence = 1;
    HWND wake = nullptr;
    bool closing = false, draining = false, sending = false;
    unsigned active = 0;
    static constexpr UINT kDrain = WM_APP + 1;

    Impl(fs::path user, Log output, std::function<void()> changed, bool fallback):
        userRoot(std::move(user)), log(std::move(output)), registry(std::move(changed)), loader(fallback) {
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = Window; wc.lpszClassName = L"MMS.PluginAbi.Dispatch.V1";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("plugin dispatcher class");
        wake = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        if (!wake) throw std::runtime_error("plugin dispatcher window");
    }
    ~Impl() { if (wake) DestroyWindow(wake); }
    static LRESULT CALLBACK Window(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        if (self && message == kDrain) { Boundary([&] { self->Drain(); return MMS_OK; }); return 0; }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    void Write(const std::string& text) noexcept { try { if (log) log(text); } catch (...) {} }
    MmsResult Query(Instance& i, MmsString name, uint32_t major, uint32_t minor, uint32_t bytes, void* out) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        const auto id = String(name);
        if (id == "mms.ui") return Table(major, minor, bytes, out, registry.Api(i.ui));
        if (id == "mms.ui.surface") return Table(major, minor, bytes, out, registry.SurfaceApi(i.ui));
        if (id == "mms.ui.layout") return Table(major, minor, bytes, out, registry.LayoutApi(i.ui));
        return MMS_E_NOT_FOUND;
    }
    MmsHostV1 Host(Instance& i) {
        MmsHostV1 h{}; h.h = Header<MmsHostV1>(); h.context = &i;
        h.queryService = [](void* p, MmsString name, uint32_t ma, uint32_t mi, uint32_t n, void* out) {
            return Boundary([&] { auto& i = *static_cast<Instance*>(p); return i.host->Query(i, name, ma, mi, n, out); });
        };
        h.registerProvider = [](void* p, MmsString, MmsString, const void*, uint32_t, MmsHandle*) -> MmsResult {
            auto& i = *static_cast<Instance*>(p); if (GetCurrentThreadId() != i.host->thread) return MMS_E_THREAD;
            return i.stopping ? MMS_E_NOT_READY : MMS_E_UNSUPPORTED;
        };
        h.unregisterProvider = [](void* p, MmsHandle) -> MmsResult { auto& i = *static_cast<Instance*>(p); return GetCurrentThreadId() == i.host->thread ? MMS_E_NOT_FOUND : MMS_E_THREAD; };
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
    void Drain() {
        if (GetCurrentThreadId() != thread || draining || active || registry.InCallback()) return;
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
            auto r = Boundary([&] { (cancel ? t->cancelled : t->run)(t->context); return MMS_OK; });
            --active; if (r) Write(t->owner->ui.id + ": queued callback threw an exception");
            { std::lock_guard<std::mutex> lock(mutex); tasks.erase(t->handle); }
        }
        std::lock_guard<std::mutex> lock(mutex); if (!tasks.empty()) PostMessageW(wake, kDrain, 0, 0);
    }
    void Event(uint32_t kind) {
        if (GetCurrentThreadId() != thread || sending || active) return;
        sending = true; struct Guard { bool& v; ~Guard() { v = false; } } guard{sending};
        MmsEventV1 event{}; event.h = Header<MmsEventV1>(); event.kind = kind; event.sequence = sequence++;
        for (const auto& i : instances) if (i->started && !i->stopping) {
            ++active; auto r = Boundary([&] { i->api.onEvent(i->instance, &event); return MMS_OK; }); --active;
            if (r) Write(i->ui.id + ": event callback failed");
        }
    }
    bool StopOne(Instance& i) {
        i.stopping = true; i.ui.stopping = true; i.status.stage = "stopping";
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
        if (registry.HasLeases(i.ui)) return false;
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
        if (GetCurrentThreadId() != thread || active || registry.InCallback()) return false;
        if (!closing) { Event(MMS_EVENT_UI_CLOSING); closing = true; }
        for (const auto& i : instances) { i->stopping = true; i->ui.stopping = true; }
        bool complete = true; std::set<std::string> keep;
        for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
            auto& i = **it;
            if (keep.count(i.ui.id) || !StopOne(i)) { complete = false; for (const auto& d : i.dependencies) keep.insert(d); }
        }
        return complete;
    }
    void Load(const fs::path& root) {
        if (GetCurrentThreadId() != thread || closing || !instances.empty()) throw std::runtime_error("plugin manager already started or wrong thread");
        auto discovered = Discover(root); for (const auto& d : discovered.diagnostics) Write(d);
        auto order = Resolve(discovered, [&](const std::string& d) { Write(d); }); std::set<std::string> started;
        for (size_t index : order) {
            const auto& m = discovered.packages[index];
            auto instance = std::make_unique<Instance>(); auto& i = *instance; i.host = this; i.ui = {&registry, m.id}; i.status = {m.id, "validated", {}, m.package};
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
        Event(MMS_EVENT_UI_READY);
        PostMessageW(wake, kDrain, 0, 0);
    }
};
Manager::Manager(fs::path user, Log log, std::function<void()> changed, bool fallback): impl_(std::make_unique<Impl>(std::move(user), std::move(log), std::move(changed), fallback)) {}
Manager::~Manager() {
    if (!impl_) return;
    if (Boundary([&] { return impl_->Stop() ? MMS_OK : MMS_E_BUSY; }) != MMS_OK) {
        for (const auto& i : impl_->instances) i->code.Pin();
        impl_->Write("plugin stop is pending; retaining manager, instances and DLLs until process exit");
        // A busy worker may still use its host context. Keep that context and
        // its dispatch window alive, not just the DLL's code pages.
        impl_.release();
    }
}
void Manager::Load(const fs::path& root) { impl_->Load(root); }
void Manager::Drain() { impl_->Drain(); }
bool Manager::Stop() { return Boundary([&] { return impl_->Stop() ? MMS_OK : MMS_E_BUSY; }) == MMS_OK; }
void Manager::Event(uint32_t kind) { impl_->Event(kind); }
UiRegistry& Manager::Ui() { return impl_->registry; }
bool Manager::InCallback() const { return impl_->active != 0 || impl_->registry.InCallback(); }
std::vector<PluginStatus> Manager::Status() const { auto result = impl_->failed; for (const auto& i : impl_->instances) result.push_back(i->status); return result; }
}
