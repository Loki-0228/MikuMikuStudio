#include "native_texture_reload.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "history_runtime.hpp"
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_resource_budget.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/texture_lookup.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include <commctrl.h>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
struct ReleaseCom { template<class T> void operator()(T* p) const noexcept { if (p) p->Release(); } };
template<class T> using Com = std::unique_ptr<T, ReleaseCom>;
template<class T> Com<T> Retain(T* p) { if (p) p->AddRef(); return Com<T>(p); }
thread_local const MMDApp* publication = nullptr;
thread_local bool* closing = nullptr;
bool Ready(MMDApp& app, bool replay) noexcept {
    const bool own = replay && (publication == &app || NativeObjectPublicationAuthorized(&app));
    return (!closing || !*closing) && (!bake::UiLocked() || own) && (!app.SceneMutationInProgress() || own) &&
        !SceneSaveBlocksEdits() && !NativeObjectUiPending(&app) && !GetCapture() && !app.PlaybackActive() &&
        !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending && !app.RecordingWindow() &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.GravitySettingDialog() &&
        !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen &&
        !NativeModelOutlineDialogBlocks(app) && app.MainWindow() && IsWindow(app.MainWindow()) &&
        GetWindowThreadProcessId(app.MainWindow(), nullptr) == GetCurrentThreadId();
}
struct WindowGuard {
    std::array<HWND, 2> windows{}; std::array<bool, 2> close{};
    bool requested = false; bool* previous = closing;
    static constexpr UINT_PTR id = 0x4d54524c;
    static LRESULT CALLBACK Filter(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& s = *reinterpret_cast<WindowGuard*>(data);
        if (msg == WM_NCDESTROY) s.requested = true;
        if (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE)) {
            s.requested = true; for (unsigned i = 0; i < s.windows.size(); ++i) if (s.windows[i] == h) s.close[i] = true; return 0;
        }
        if (msg == WM_COMMAND || msg == WM_NOTIFY || msg == WM_HSCROLL || msg == WM_VSCROLL ||
            (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)) return 0;
        return DefSubclassProc(h, msg, wp, lp);
    }
    explicit WindowGuard(MMDApp& app) {
        windows = {app.MainWindow(), app.FloatingWindow() == app.MainWindow() ? nullptr : app.FloatingWindow()}; closing = &requested;
        for (auto h : windows) if (h && !SetWindowSubclass(h, Filter, id, reinterpret_cast<DWORD_PTR>(this))) {
            Release(); throw std::runtime_error("texture reload owner guard");
        }
    }
    void Release() noexcept {
        closing = previous;
        for (unsigned i = 0; i < windows.size(); ++i) if (windows[i] && IsWindow(windows[i])) {
            RemoveWindowSubclass(windows[i], Filter, id); if (close[i]) PostMessageW(windows[i], WM_CLOSE, 0, 0);
        }
    }
    ~WindowGuard() { Release(); }
};
struct Authorization {
    const MMDApp* previous;
    explicit Authorization(MMDApp& app) : previous(publication) { publication = &app; }
    ~Authorization() { publication = previous; }
};
std::wstring CurrentDirectory() {
    std::array<wchar_t, 32768> text{}; const auto length = GetCurrentDirectoryW(DWORD(text.size()), text.data());
    if (!length || length >= text.size()) throw std::runtime_error("texture reload current directory");
    return std::wstring(text.data(), length);
}
std::wstring AbsolutePath(const std::wstring& key) {
    std::array<wchar_t, 32768> text{}; const auto length = GetFullPathNameW(key.c_str(), DWORD(text.size()), text.data(), nullptr);
    if (!length || length >= text.size()) throw std::runtime_error("texture reload path");
    return std::wstring(text.data(), length);
}
bool Slash(wchar_t c) noexcept { return c == L'\\' || c == L'/'; }
std::wstring ReloadPath(const std::wstring& key, const std::wstring& exe) {
    // The old menu first changed CWD to ExeDir through InitToonTextures.
    // Resolve the same names without changing the process-wide CWD. Drive-
    // relative names on another drive keep Windows' existing per-drive rule.
    if (key.size() > 1 && Slash(key[0]) && Slash(key[1])) return AbsolutePath(key);
    const auto sameDrive = [&](wchar_t a, wchar_t b) {
        return (a >= L'a' && a <= L'z' ? a - L'a' + L'A' : a) == (b >= L'a' && b <= L'z' ? b - L'a' + L'A' : b);
    };
    if (key.size() > 1 && key[1] == L':') {
        if ((key.size() > 2 && Slash(key[2])) || exe.size() < 2 || exe[1] != L':' || !sameDrive(key[0], exe[0])) return AbsolutePath(key);
        return AbsolutePath(exe + (Slash(exe.back()) ? L"" : L"\\") + key.substr(2));
    }
    if (Slash(key[0])) {
        if (exe.size() > 1 && exe[1] == L':') return AbsolutePath(exe.substr(0, 2) + key);
        if (exe.size() > 2 && Slash(exe[0]) && Slash(exe[1])) {
            const auto server = exe.find_first_of(L"\\/", 2);
            const auto share = server == std::wstring::npos ? server : exe.find_first_of(L"\\/", server + 1);
            if (server != std::wstring::npos) return AbsolutePath(exe.substr(0, share) + key);
        }
        throw std::runtime_error("texture reload rooted path");
    }
    return AbsolutePath(exe + (Slash(exe.back()) ? L"" : L"\\") + key);
}
bool DirectoryCurrent(const std::wstring& expected) noexcept {
    std::array<wchar_t, 32768> text{}; const auto length = GetCurrentDirectoryW(DWORD(text.size()), text.data());
    return length == expected.size() && length < text.size() && !std::wmemcmp(text.data(), expected.c_str(), length + 1);
}
void MeasureTexture(IUnknown* object, IDirect3DDevice9* device, resource_budget::Collector& out) {
    if (!object) return; // Optional null cache entries/before slots are real states.
    import_texture_cache::MeasureTextureResources(object, out);
    IDirect3DTexture9* rawTexture = nullptr; HRESULT query;
    try { query = object->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&rawTexture)); }
    catch (...) { if (rawTexture) rawTexture->Release(); throw; }
    Com<IDirect3DTexture9> texture(rawTexture);
    if (FAILED(query) || !texture) { out.MissingResource(resource_budget::ExternalRenderer); return; }
    IDirect3DDevice9* rawDevice = nullptr; HRESULT result;
    try { result = texture->GetDevice(&rawDevice); }
    catch (...) { if (rawDevice) rawDevice->Release(); throw; }
    Com<IDirect3DDevice9> actual(rawDevice);
    if (FAILED(result) || !actual) { out.MissingResource(resource_budget::RenderQuery); return; }
    if (actual.get() != device) {
        // COM permits distinct non-IUnknown interface pointers for the same
        // object. Validate that identity rather than rejecting a tear-off.
        const auto identity = [](IDirect3DDevice9* value, HRESULT& query) {
            IUnknown* raw = nullptr;
            try { query = value->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&raw)); }
            catch (...) { if (raw) raw->Release(); throw; }
            return Com<IUnknown>(raw);
        };
        HRESULT expectedQuery, actualQuery;
        auto expected = identity(device, expectedQuery);
        auto observed = identity(actual.get(), actualQuery);
        if (FAILED(expectedQuery) || FAILED(actualQuery) || !expected || expected.get() != observed.get())
            out.MissingResource(resource_budget::RenderQuery);
    }
}
}

struct NativeTextureReloadPlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    struct Slot {
        size_t index = 0; void* name = nullptr;
        std::wstring key, loadPath;
        Com<IUnknown> before, after;
        std::int32_t beforeTag = 0, afterTag = 0;
    };
    MMDApp& app; SharedHistory* const history;
    D3DRenderer* const renderer; Com<IDirect3DDevice9> device;
    const uint64_t scene, initialBinding;
    uint64_t initialRevision = 0, serial = 1, bytes = 0;
    std::wstring directory;
    std::array<wchar_t, 256> exeDirectory{};
    TextureReloadLoaders loaders;
    std::array<texture_reload::Texture, 11> beforeShared;
    texture_reload::PreparedSharedToon afterShared;
    std::array<float, 30> beforeEdge{};
    std::vector<Slot> slots;
    bool prepared = false, taken = false, applied = false, published = false;
    explicit State(MMDApp& value, TextureReloadLoaders functions)
        : app(value), history(BoundNativePoseHistory(&value)), renderer(value.Renderer()),
          device(Retain(renderer ? renderer->device : nullptr)), scene(value.SceneGeneration()),
          initialBinding(BoundNativePoseGeneration(&value)), loaders(functions) {}
    bool Source(bool forward, bool initial) const noexcept {
        if (!Ready(app, true) || BoundNativePoseHistory(&app) != history || app.SceneGeneration() != scene || app.Renderer() != renderer ||
            !renderer || renderer->device != device.get() || std::memcmp(app.ExeDir(), exeDirectory.data(), sizeof(exeDirectory)) ||
            (initial && BoundNativePoseGeneration(&app) != initialBinding)) return false;
        if (initial && (!DirectoryCurrent(directory) ||
            (slots.size() < 10000 && renderer->resourcePool[slots.size()].heapBuffer))) return false;
        const auto& edge = forward ? beforeEdge : afterShared.edge;
        if (std::memcmp(app.state.toonEdgeTable, edge.data(), sizeof(app.state.toonEdgeTable))) return false;
        for (unsigned i = 0; i < 11; ++i) {
            auto* expected = forward ? beforeShared[i].get() : afterShared.textures[i].get();
            if (app.ToonTexture(i) != expected) return false;
        }
        for (const auto& slot : slots) {
            const auto& live = renderer->resourcePool[slot.index];
            if (live.heapBuffer != slot.name || !live.heapBuffer ||
                live.comObject != (forward ? slot.before.get() : slot.after.get()) || live.tag != (forward ? slot.beforeTag : slot.afterTag) ||
                std::wmemcmp(static_cast<const wchar_t*>(live.heapBuffer), slot.key.c_str(), slot.key.size() + 1)) return false;
        }
        return true;
    }
    bool InitialCurrent() const noexcept {
        if (!Source(true, true)) return false;
        uint64_t revision = 0; return !history || (history->Revision(revision) == MMS_OK && revision == initialRevision);
    }
    void Measure(resource_budget::Collector& out) const {
        out.Array(this, 1); out.Add(this, 1, sizeof(State) + 4 * sizeof(void*)); out.String(directory); out.Vector(slots);
        for (const auto& texture : beforeShared) MeasureTexture(texture.get(), device.get(), out);
        for (const auto& texture : afterShared.textures) MeasureTexture(texture.get(), device.get(), out);
        for (const auto& slot : slots) {
            out.String(slot.key); out.String(slot.loadPath);
            MeasureTexture(slot.before.get(), device.get(), out); MeasureTexture(slot.after.get(), device.get(), out);
        }
    }
    bool SameResources() const noexcept {
        if (std::memcmp(beforeEdge.data(), afterShared.edge.data(), sizeof(beforeEdge))) return false;
        for (unsigned i = 0; i < 11; ++i) if (beforeShared[i].get() != afterShared.textures[i].get()) return false;
        for (const auto& slot : slots) if (slot.before.get() != slot.after.get() || slot.beforeTag != slot.afterTag) return false;
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> source; bool forward, consumed = false;
        const uint64_t serial, binding;
        std::array<texture_reload::Texture, 11> shared;
        std::vector<Com<IUnknown>> cache;
        mutable resource_budget::Collector resources; uint64_t bytes = 0;
        Candidate(std::shared_ptr<State> state, bool direction)
            : source(std::move(state)), forward(direction), serial(source->serial), binding(BoundNativePoseGeneration(&source->app)) {
            cache.reserve(source->slots.size());
            for (unsigned i = 0; i < 11; ++i) {
                auto* p = forward ? source->afterShared.textures[i].get() : source->beforeShared[i].get(); if (p) p->AddRef(); shared[i].reset(p);
            }
            for (const auto& slot : source->slots) cache.push_back(Retain(forward ? slot.after.get() : slot.before.get()));
        }
        bool Metadata() const noexcept {
            return !consumed && serial == source->serial && forward != source->applied &&
                binding == BoundNativePoseGeneration(&source->app) && source->Source(forward, !source->published);
        }
        bool IsCurrent() const noexcept override {
            return Metadata() && resources.Covers([&](auto& collector) { source->Measure(collector); }) && Metadata();
        }
        void Publish() noexcept override {
            if (!Metadata()) std::terminate();
            // Every cache-owned reference is exchanged with a pre-retained
            // reference. Displaced references survive in Candidate until its
            // later destruction; no AddRef/Release or driver calls occur here.
            for (unsigned i = 0; i < 11; ++i) {
                auto* next = shared[i].release(); auto* previous = source->app.ToonTexture(i);
                source->app.ToonTexture(i) = next; shared[i].reset(previous);
            }
            for (size_t i = 0; i < source->slots.size(); ++i) {
                const auto& slot = source->slots[i]; auto& live = source->renderer->resourcePool[slot.index];
                auto* next = cache[i].release(); auto* previous = live.comObject;
                live.comObject = next; cache[i].reset(previous); live.tag = forward ? slot.afterTag : slot.beforeTag;
            }
            const auto& edge = forward ? source->afterShared.edge : source->beforeEdge;
            std::memcpy(source->app.state.toonEdgeTable, edge.data(), sizeof(source->app.state.toonEdgeTable));
            InvalidateTextureLookup(); source->applied = forward; source->published = true; ++source->serial; consumed = true;
        }
        bool TargetRetainedBytes(uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        return Boundary([&]() -> MmsResult {
            if (!prepared || !taken || forward == applied || serial == UINT64_MAX || !Source(forward, !published) ||
                (!published && !InitialCurrent())) return MMS_E_STALE;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            Measure(candidate->resources); if (!candidate->resources.Result().Complete()) return MMS_E_NOT_READY;
            candidate->bytes = candidate->resources.Result().totalBytes;
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    uint64_t RetainedBytes() const noexcept override { return bytes; }
    bool RequiresPhysicsReset() const noexcept override { return false; }
    // Renderer-only participant: frozen publication effects are exactly a
    // drawing request. This is never derived from the command label or physics.
    HistoryEffects Effects() const noexcept override { return HistoryEffects::Viewport; }
};

MmsResult NativeTextureReloadPlan::Begin(MMDApp& app, const TextureReloadLoaders& loaders) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        if (!Ready(app, false) || g_Block != &app) return MMS_E_BUSY;
        // Permission is shared with the real production journal even when
        // this command has no migration binding and will publish unrecorded.
        // Let that gate retain its CanInvokeEdit authorization; a blanket
        // InCallback check here would reject authorized host callbacks too.
        auto* permissionHistory = BoundNativePoseHistory(&app);
        if (!permissionHistory) permissionHistory = NativeHistory(&app);
        if (permissionHistory) {
            if (const auto r = permissionHistory->CheckNativeEdit()) return r;
        } else if (InCallback()) return MMS_E_BUSY;
        if (!app.Renderer() || !app.Renderer()->device) return MMS_E_NOT_READY;
        auto state = std::make_shared<State>(app, loaders);
        if (state->history) {
            if (const auto r = state->history->Revision(state->initialRevision)) return r;
        }
        state->directory = CurrentDirectory();
        std::memcpy(state->exeDirectory.data(), app.ExeDir(), sizeof(state->exeDirectory));
        const auto exeLength = wcsnlen_s(state->exeDirectory.data(), state->exeDirectory.size());
        if (!exeLength || exeLength == state->exeDirectory.size()) return MMS_E_NOT_READY;
        const auto exe = AbsolutePath(std::wstring(state->exeDirectory.data(), exeLength));
        std::memcpy(state->beforeEdge.data(), app.state.toonEdgeTable, sizeof(app.state.toonEdgeTable));
        size_t used = 0; while (used < 10000 && state->renderer->resourcePool[used].heapBuffer) ++used;
        state->slots.reserve(used);
        for (size_t i = 0; i < used; ++i) {
            const auto& live = state->renderer->resourcePool[i]; const auto* path = static_cast<const wchar_t*>(live.heapBuffer);
            const auto length = wcsnlen_s(path, 32768); if (!length || length == 32768) return MMS_E_NOT_READY;
            State::Slot slot; slot.index = i; slot.name = live.heapBuffer; slot.key.assign(path, length); slot.loadPath = ReloadPath(slot.key, exe);
            slot.beforeTag = live.tag; state->slots.push_back(std::move(slot));
        }
        for (unsigned i = 0; i < 11; ++i) { auto* p = app.ToonTexture(i); if (p) p->AddRef(); state->beforeShared[i].reset(p); }
        for (auto& slot : state->slots) slot.before = Retain(state->renderer->resourcePool[slot.index].comObject);
        if (!state->InitialCurrent()) return MMS_E_STALE;
        state_ = std::move(state); return MMS_OK;
    });
}
MmsResult NativeTextureReloadPlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto& state = *state_; if (state.taken) return MMS_E_BUSY;
        if (!state.InitialCurrent()) return MMS_E_STALE;
        if (!state.prepared) {
            texture_reload::PreparedSharedToon shared; std::vector<texture_reload::PreparedCacheTexture> cache; cache.reserve(state.slots.size());
            const auto load = state.loaders.shared ? state.loaders.shared : texture_reload::PrepareSharedToon;
            if (const auto r = load(state.app, shared)) return r;
            if (!state.InitialCurrent()) return MMS_E_STALE;
            for (unsigned i = 0; i < 11; ++i) if (!shared.textures[i]) return MMS_E_NOT_READY;
            for (float value : shared.edge) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
            for (const auto& slot : state.slots) {
                texture_reload::PreparedCacheTexture texture;
                texture.tag = slot.beforeTag;
                if (const auto r = texture_reload::PrepareCacheTexture(state.device.get(), slot.loadPath.c_str(), state.loaders.cache, texture)) return r;
                if (!state.InitialCurrent()) return MMS_E_STALE; cache.push_back(std::move(texture));
            }
            state.afterShared = std::move(shared);
            for (size_t i = 0; i < cache.size(); ++i) { state.slots[i].after.reset(cache[i].texture.release()); state.slots[i].afterTag = cache[i].tag; }
            state.prepared = true;
        }
        resource_budget::Collector resources; state.Measure(resources);
        if (!resources.Result().Complete()) return MMS_E_NOT_READY;
        if (!state.InitialCurrent()) return MMS_E_STALE;
        state.bytes = resources.Result().totalBytes; state.taken = true;
        if (state.SameResources()) { out.reset(); return MMS_OK; }
        out = state_; return MMS_OK;
    });
}
bool TryReloadNativeTextures(MMDApp* app) noexcept {
    if (!app) return false;
    static thread_local bool active = false; if (active) return true;
    const auto result = Boundary([&]() -> MmsResult {
        if (!Ready(*app, false) || g_Block != app) return MMS_E_BUSY;
        struct Active { bool& value; explicit Active(bool& v) : value(v) { value = true; } ~Active() { value = false; } } operation(active);
        WindowGuard windows(*app); NativeTextureReloadPlan plan; if (const auto r = plan.Begin(*app)) return r;
        auto* history = BoundNativePoseHistory(app); std::shared_ptr<HistoryChange> change;
        {
            Authorization authorize(*app); bake::ScopedUiInputGate input;
            if (const auto r = plan.Take(change)) return r;
            if (!change) return MMS_OK;
            if (history) {
                std::unique_ptr<SharedHistory::NativeTransaction> token;
                if (const auto r = history->PrepareNative("Reload textures", change, token, SharedHistory::NativeIntent::ExplicitCommit)) return r;
                if (const auto r = history->CommitNative(std::move(token))) return r;
            } else {
                std::unique_ptr<PreparedHistoryChange> candidate;
                if (const auto r = change->Prepare(true, candidate)) return r;
                if (!candidate || !candidate->IsCurrent()) return MMS_E_STALE; candidate->Publish();
            }
        }
        // The bound commit has already released its transaction token. Publish
        // its frozen effects through the real helper: the Viewport mask only
        // refreshes the history controls and requests drawing. Unbound
        // publication records nothing, so it only requests drawing. Neither
        // path reads pose/scene data, marks the project dirty, resets physics or
        // toggles windows, and a UI failure cannot undo the resource publication
        // or add a second history node.
        if (history) {
            PublishBoundHistoryEffects(*app, *history, history->LastPublicationEffects(),
                history->LastPublicationRequiresPhysicsReset());
        } else {
            Boundary([&]() -> MmsResult { updates::RequestViewport(app, false); return MMS_OK; });
        }
        return MMS_OK;
    });
    if (result) runtime_log::Write("PLUGIN_ABI texture reload refused: %d", result);
    return true;
}
}
#endif
