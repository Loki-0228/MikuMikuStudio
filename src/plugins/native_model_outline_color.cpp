#include "native_model_outline_color.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include <commdlg.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

namespace mikumikustudio { float ModelOutlineDistance(MMDApp*, unsigned char*, const float[16]); }
namespace mikumikustudio::plugins {
namespace {
using Color = std::array<std::int32_t, 3>;
constexpr DWORD bodyFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
constexpr DWORD edgeFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
struct Release {
    template<class T> void operator()(T* value) const noexcept { if (value) value->Release(); }
};
using Buffer = std::unique_ptr<IDirect3DVertexBuffer9, Release>;
thread_local const MMDApp* publication = nullptr;
thread_local bool* closing = nullptr;
struct WindowGuard {
    std::array<HWND, 2> windows{};
    std::array<bool, 2> close{};
    bool requested = false;
    bool* previous = closing;
    static constexpr UINT_PTR id = 0x4d4d5363;
    static LRESULT CALLBACK Filter(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& self = *reinterpret_cast<WindowGuard*>(data);
        if (message == WM_CLOSE || (message == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE)) {
            self.requested = true;
            for (unsigned i = 0; i < self.windows.size(); ++i) if (window == self.windows[i]) self.close[i] = true;
            return 0;
        }
        if (message == WM_COMMAND || message == WM_NOTIFY || message == WM_HSCROLL || message == WM_VSCROLL ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST) || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)) return 0;
        return DefSubclassProc(window, message, wp, lp);
    }
    explicit WindowGuard(MMDApp& app) {
        windows = {app.MainWindow(), app.FloatingWindow() == app.MainWindow() ? nullptr : app.FloatingWindow()};
        closing = &requested;
        for (auto window : windows) if (window && !SetWindowSubclass(window, Filter, id, reinterpret_cast<DWORD_PTR>(this))) {
            Release(); throw std::runtime_error("outline color window guard");
        }
    }
    void Release() noexcept {
        closing = previous;
        for (unsigned i = 0; i < windows.size(); ++i) if (windows[i] && IsWindow(windows[i])) {
            RemoveWindowSubclass(windows[i], Filter, id);
            if (close[i]) PostMessageW(windows[i], WM_CLOSE, 0, 0);
        }
    }
    ~WindowGuard() { Release(); }
};
bool Ready(MMDApp& app, bool replay) noexcept {
    const bool authorized = replay && (publication == &app || NativeObjectPublicationAuthorized(&app));
    return (!closing || !*closing) && (!bake::UiLocked() || authorized) && (!app.SceneMutationInProgress() || authorized) &&
        !SceneSaveBlocksEdits() && !NativeObjectUiPending(&app) && !GetCapture() &&
        !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.RecordingWindow() && !app.FrameRangeDialog() && !app.state.frameCopyDialog &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !NativeModelOutlineDialogBlocks(app) &&
        app.MainWindow() && GetWindowThreadProcessId(app.MainWindow(), nullptr) == GetCurrentThreadId();
}
Color ReadColor(const MMDApp& app) noexcept {
    return {app.state.modelOutlineColorRed, app.state.modelOutlineColorGreen, app.state.modelOutlineColorBlue};
}
void WriteColor(MMDApp& app, const Color& color) noexcept {
    app.state.modelOutlineColorRed = color[0]; app.state.modelOutlineColorGreen = color[1]; app.state.modelOutlineColorBlue = color[2];
}
MmsResult Result(HRESULT hr) noexcept {
    return hr == E_OUTOFMEMORY || hr == D3DERR_OUTOFVIDEOMEMORY ? MMS_E_MEMORY : MMS_E_FAILED;
}
bool Describe(IDirect3DVertexBuffer9* buffer, unsigned count, unsigned stride, DWORD fvf,
              bool readable, D3DVERTEXBUFFER_DESC& out) noexcept {
    if (!buffer || count > UINT_MAX / stride || FAILED(buffer->GetDesc(&out))) return false;
    return out.Type == D3DRTYPE_VERTEXBUFFER && out.Format == D3DFMT_VERTEXDATA &&
        out.FVF == fvf && out.Size == count * stride && out.Pool == D3DPOOL_MANAGED &&
        (!readable || !(out.Usage & (D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC)));
}
// This is the already-rendered view rotation. ResolveFrameFocus is read-only;
// composing its pivot translation makes the same frame-world matrix used by
// ModelOutlineDistance without installing a device transform or evaluating.
bool World(MMDApp& app, std::array<float, 16>& out) noexcept {
    float focus[3]{}; ResolveFrameFocus(&app, static_cast<int>(app.CameraReferenceMode()), focus);
    std::memcpy(out.data(), &app.ViewRotationTransform(), sizeof(out));
    for (float value : out) if (!std::isfinite(value)) return false;
    for (float value : focus) if (!std::isfinite(value)) return false;
    if (out[3] || out[7] || out[11] || out[15] != 1.f) return false;
    for (unsigned c = 0; c < 3; ++c)
        out[12 + c] = -focus[0] * out[c] - focus[1] * out[4 + c] - focus[2] * out[8 + c] + out[12 + c];
    return true;
}
bool Same(const void* a, const void* b, std::size_t bytes) noexcept { return !std::memcmp(a, b, bytes); }
std::uint64_t Saturate(std::uint64_t a, std::uint64_t b) noexcept { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }
}
struct NativeModelOutlineColorPlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    struct Entry {
        unsigned slot = 0;
        unsigned char* object = nullptr;
        MmsObjectRef initial{};
        std::shared_ptr<RetainedSceneObject> owner;
        unsigned count = 0;
        unsigned char physicsMode = 0;
        void* main = nullptr;
        void* edge = nullptr;
        mdl::PmdVertex* raw = nullptr;
        Buffer before, after;
        D3DVERTEXBUFFER_DESC description{};
    };
    MMDApp& app;
    SharedHistory* const history;
    const std::uint64_t binding, scene, revision;
    const Color before;
    Color after;
    std::vector<Entry> entries;
    std::uint64_t serial = 1, preparation = 0, bookkeeping = 0, resourceBytes = 0;
    bool staged = false, applied = false, published = false;
    State(MMDApp& a, SharedHistory& h, std::uint64_t rev)
        : app(a), history(&h), binding(BoundNativePoseGeneration(&a)), scene(a.SceneGeneration()),
          revision(rev), before(ReadColor(a)), after(before) {}
    bool Bound() const noexcept { return BoundNativePoseHistory(&app) == history; }
    bool Source(bool forward, bool initial) const noexcept {
        if (!Bound() || app.SceneGeneration() != scene || ReadColor(app) != (forward ? before : after)) return false;
        if (initial && BoundNativePoseGeneration(&app) != binding) return false;
        std::size_t index = 0;
        try {
            for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.ModelSlot(slot)) {
                if (index == entries.size()) return false;
                const auto& e = entries[index++];
                if (e.slot != slot || e.object != object || !e.owner->Present() || !SceneObjectPresent(object, e.initial.id)) return false;
                if (initial && SceneObjectIdentity(object).generation != e.initial.generation) return false;
                const auto& m = *mdl::Mdl(object);
                if (m.physicsMode != e.physicsMode || m.vertexCount != e.count) return false;
                if (e.physicsMode != 2 && m.vertexBuffer2 != (e.before ? (forward ? e.before.get() : e.after.get()) : e.edge)) return false;
                if (e.before && (m.vertexBuffer != e.main || m.rawVertices != e.raw)) return false;
            }
        } catch (...) { return false; }
        return index == entries.size();
    }
    bool InitialCurrent() const noexcept {
        if (!Source(true, true)) return false;
        std::uint64_t current = 0; return !history->Revision(current) && current == revision;
    }
    bool Inactive(void* target) const noexcept {
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.ModelSlot(slot))
            if (mdl::Mdl(object)->vertexBuffer2 == target || mdl::Mdl(object)->vertexBuffer == target) return false;
        return true;
    }
    std::uint64_t RecordBytes() const noexcept {
        return sizeof(State) + 4 * sizeof(void*) + entries.capacity() * sizeof(Entry);
    }
    struct Geometry {
        std::size_t entry = 0;
        Buffer body, handoff;
        std::vector<mdl::SkinnedVertexBase> vertices;
        std::vector<unsigned char> disabled;
        std::array<float, 16> world{};
        float distance = 0;
        bool Current(State& state) const noexcept {
            const auto& e = state.entries[entry];
            const auto& m = *mdl::Mdl(e.object);
            if (m.vertexBuffer != body.get() || m.rawVertices != e.raw || m.vertexCount != vertices.size()) return false;
            std::array<float, 16> current{};
            if (!World(state.app, current) || !Same(current.data(), world.data(), sizeof(world))) return false;
            const float liveDistance = ModelOutlineDistance(&state.app, e.object, current.data());
            if (!Same(&liveDistance, &distance, sizeof(distance))) return false;
            for (unsigned i = 0; i < e.count; ++i) if (m.rawVertices[i].edgeDisabled != disabled[i]) return false;
            void* bytes = nullptr;
            const HRESULT locked = body->Lock(0, static_cast<UINT>(vertices.size() * sizeof(vertices[0])), &bytes, D3DLOCK_READONLY);
            if (FAILED(locked)) return false;
            const bool same = bytes && Same(bytes, vertices.data(), vertices.size() * sizeof(vertices[0]));
            const HRESULT unlocked = body->Unlock();
            return same && SUCCEEDED(unlocked);
        }
    };
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> state;
        const bool forward, initial;
        const std::uint64_t serial, preparation, binding;
        std::vector<std::uint64_t> generations;
        std::vector<Geometry> geometry;
        mutable resource_budget::Collector bookkeeping;
        std::uint64_t bytes = 0;
        bool consumed = false;
        Candidate(std::shared_ptr<State> value, bool direction)
            : state(std::move(value)), forward(direction), initial(!state->published), serial(state->serial), preparation(state->preparation),
              binding(BoundNativePoseGeneration(&state->app)) {}
        bool Metadata() const noexcept {
            if (consumed || state->serial != serial || state->preparation != preparation || forward == state->applied || !state->Source(forward, initial) ||
                BoundNativePoseGeneration(&state->app) != binding || !Ready(state->app, true)) return false;
            try {
                for (std::size_t i = 0; i < state->entries.size(); ++i) {
                    const auto& e = state->entries[i];
                    if (SceneObjectIdentity(e.object).generation != generations[i] || !e.owner->CoversBookkeeping(bookkeeping)) return false;
                }
                for (const auto& g : geometry) if (!state->Inactive(g.handoff.get())) return false;
            } catch (...) { return false; }
            return true;
        }
        bool IsCurrent() const noexcept override {
            if (!Metadata()) return false;
            for (const auto& g : geometry) if (!g.Current(*state) || !Metadata()) return false;
            return true;
        }
        void Publish() noexcept override {
            // IsCurrent's checked, read-only D3D queries ran at the journal's
            // last failure boundary. Publication must make no driver calls.
            if (!Metadata()) std::terminate();
            for (auto& g : geometry) {
                auto& m = *mdl::Mdl(state->entries[g.entry].object);
                auto* previous = static_cast<IDirect3DVertexBuffer9*>(m.vertexBuffer2);
                m.vertexBuffer2 = g.handoff.release();
                g.handoff.reset(previous); // null -> owned old scene reference
            }
            WriteColor(state->app, forward ? state->after : state->before);
            state->applied = forward; state->published = true; ++state->serial; consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!staged || forward == applied || !Source(forward, !published) || (!published && !InitialCurrent())) return MMS_E_STALE;
            if (serial == UINT64_MAX || preparation == UINT64_MAX) return MMS_E_NOT_READY;
            // Candidates share the inactive retained buffers. A new attempt
            // may rewrite them before failing, so invalidate every older
            // candidate before any allocation or driver call can intervene.
            ++preparation;
            if (!Ready(app, true)) return MMS_E_BUSY;
            if (RetainedBytes() > history->MaxBytes()) return MMS_E_MEMORY;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward);
            next->generations.reserve(entries.size()); next->geometry.reserve(entries.size());
            for (const auto& e : entries) {
                next->generations.push_back(SceneObjectIdentity(e.object).generation);
                if (const auto status = e.owner->MeasureBookkeeping(next->bookkeeping)) return status;
            }
            if (!next->bookkeeping.Result().Complete()) return MMS_E_NOT_READY;
            for (std::size_t index = 0; index < entries.size(); ++index) {
                auto& e = entries[index]; if (!e.before) continue;
                Geometry g; g.entry = index;
                auto* body = static_cast<IDirect3DVertexBuffer9*>(mdl::Mdl(e.object)->vertexBuffer);
                D3DVERTEXBUFFER_DESC desc{};
                if (!Describe(body, e.count, sizeof(mdl::SkinnedVertexBase), bodyFvf, true, desc)) return MMS_E_NOT_READY;
                body->AddRef(); g.body.reset(body);
                g.vertices.resize(e.count); g.disabled.resize(e.count);
                if (!World(app, g.world)) return MMS_E_NOT_READY;
                g.distance = ModelOutlineDistance(&app, e.object, g.world.data());
                if (!std::isfinite(g.distance)) return MMS_E_ARGUMENT;
                void* bits = nullptr;
                HRESULT hr = body->Lock(0, desc.Size, &bits, D3DLOCK_READONLY);
                if (FAILED(hr)) return Result(hr);
                if (bits) std::memcpy(g.vertices.data(), bits, desc.Size);
                const HRESULT unlocked = body->Unlock();
                if (!bits || FAILED(unlocked)) return bits ? Result(unlocked) : MMS_E_FAILED;
                for (unsigned i = 0; i < e.count; ++i) {
                    g.disabled[i] = e.raw[i].edgeDisabled;
                    for (unsigned c = 0; c < 3; ++c)
                        if (!std::isfinite(g.vertices[i].position[c]) || !std::isfinite(g.vertices[i].normal[c])) return MMS_E_ARGUMENT;
                }
                if (!Source(forward, !published)) return MMS_E_STALE;
                if (!e.after) {
                    IDirect3DDevice9* rawDevice = nullptr;
                    hr = e.before->GetDevice(&rawDevice);
                    std::unique_ptr<IDirect3DDevice9, Release> device(rawDevice);
                    if (FAILED(hr) || !device) return FAILED(hr) ? Result(hr) : MMS_E_NOT_READY;
                    IDirect3DVertexBuffer9* created = nullptr;
                    hr = device->CreateVertexBuffer(e.description.Size, e.description.Usage, edgeFvf,
                        e.description.Pool, &created, nullptr);
                    Buffer owned(created);
                    if (FAILED(hr) || !owned) return FAILED(hr) ? Result(hr) : MMS_E_FAILED;
                    // The complete before resource, including arbitrary initial
                    // diffuse colors, is retained rather than read or guessed.
                    bits = nullptr; hr = owned->Lock(0, e.description.Size, &bits, 0);
                    if (FAILED(hr)) return Result(hr);
                    if (bits) {
                        const auto color = 0xff000000u | (std::uint32_t(after[0]) << 16) | (std::uint32_t(after[1]) << 8) | std::uint32_t(after[2]);
                        for (unsigned i = 0; i < e.count; ++i) std::memcpy(static_cast<unsigned char*>(bits) + i * sizeof(mdl::EdgeVertex) + 12, &color, 4);
                    }
                    const HRESULT done = owned->Unlock();
                    if (!bits || FAILED(done)) return bits ? Result(done) : MMS_E_FAILED;
                    e.after = std::move(owned);
                }
                auto* target = forward ? e.after.get() : e.before.get();
                if (!Inactive(target)) return MMS_E_STALE;
                bits = nullptr; hr = target->Lock(0, e.description.Size, &bits, 0);
                if (FAILED(hr)) return Result(hr);
                bool finite = true;
                if (bits) for (unsigned i = 0; i < e.count; ++i) {
                    float position[3]; const float amount = g.disabled[i] ? -0.004999999888241291f : g.distance;
                    for (unsigned c = 0; c < 3; ++c) {
                        position[c] = g.vertices[i].position[c] + g.vertices[i].normal[c] * amount;
                        finite &= std::isfinite(position[c]);
                    }
                    // Both retained resources preserve diffuse; only the
                    // inactive target's position lanes receive the live pose.
                    std::memcpy(static_cast<unsigned char*>(bits) + i * sizeof(mdl::EdgeVertex), position, 12);
                }
                const HRESULT done = target->Unlock();
                if (!bits || FAILED(done)) return bits ? Result(done) : MMS_E_FAILED;
                if (!finite) return MMS_E_ARGUMENT;
                target->AddRef(); g.handoff.reset(target); next->geometry.push_back(std::move(g));
            }
            next->bytes = resource_budget::Collector::Sum(RecordBytes(), resourceBytes);
            next->bytes = resource_budget::Collector::Sum(next->bytes, next->bookkeeping.Result().totalBytes);
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return Saturate(Saturate(RecordBytes(), resourceBytes), bookkeeping); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
MmsResult NativeModelOutlineColorPlan::Begin(MMDApp& app) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
        if (!Ready(app, false)) return MMS_E_BUSY;
        if (const auto status = history->CheckNativeEdit()) return status;
        std::uint64_t revision = 0; if (const auto status = history->Revision(revision)) return status;
        auto next = std::make_shared<State>(app, *history, revision);
        resource_budget::Collector bookkeeping;
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.ModelSlot(slot)) {
            if (std::any_of(next->entries.begin(), next->entries.end(), [&](const auto& e) { return e.object == object; })) return MMS_E_STALE;
            const auto& m = *mdl::Mdl(object); State::Entry e; e.slot = slot; e.object = object;
            e.initial = SceneObjectIdentity(object); e.owner = RetainSceneObject(app, MMS_OBJECT_MODEL, slot);
            e.count = m.vertexCount; e.physicsMode = m.physicsMode;
            e.edge = m.vertexBuffer2;
            if (m.physicsMode != 2 && m.vertexCount && m.vertexBuffer2) {
                if (!m.rawVertices) return MMS_E_NOT_READY;
                D3DVERTEXBUFFER_DESC body{};
                if (!Describe(static_cast<IDirect3DVertexBuffer9*>(m.vertexBuffer), m.vertexCount, sizeof(mdl::SkinnedVertexBase), bodyFvf, true, body) ||
                    !Describe(static_cast<IDirect3DVertexBuffer9*>(m.vertexBuffer2), m.vertexCount, sizeof(mdl::EdgeVertex), edgeFvf, false, e.description)) return MMS_E_NOT_READY;
                for (unsigned other = 0; other < kModelSlotCount; ++other) if (auto* o = app.ModelSlot(other)) {
                    const auto& candidate = *mdl::Mdl(o);
                    if ((other != slot && candidate.vertexBuffer2 == m.vertexBuffer2) || candidate.vertexBuffer == m.vertexBuffer2) return MMS_E_STALE;
                }
                e.main = m.vertexBuffer; e.raw = m.rawVertices;
                auto* buffer = static_cast<IDirect3DVertexBuffer9*>(m.vertexBuffer2); buffer->AddRef(); e.before.reset(buffer);
                // Two complete managed outline resources, each with the
                // conservative system+device-copy charge used by the ledger.
                next->resourceBytes = resource_budget::Collector::Sum(next->resourceBytes,
                    resource_budget::Collector::Product(e.description.Size, 4));
            }
            if (const auto status = e.owner->MeasureBookkeeping(bookkeeping)) return status;
            next->entries.push_back(std::move(e));
        }
        if (!bookkeeping.Result().Complete()) return MMS_E_NOT_READY;
        next->bookkeeping = bookkeeping.Result().totalBytes;
        if (!next->InitialCurrent()) return MMS_E_STALE;
        state_ = std::move(next); return MMS_OK;
    });
}
MmsResult NativeModelOutlineColorPlan::Set(int red, int green, int blue) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    if (red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255) return MMS_E_ARGUMENT;
    if (!state_->InitialCurrent()) return MMS_E_STALE;
    state_->after = {red, green, blue}; state_->staged = true; return MMS_OK;
}
MmsResult NativeModelOutlineColorPlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    if (!state_ || !state_->staged) return MMS_E_NOT_READY;
    if (!state_->InitialCurrent()) return MMS_E_STALE;
    if (state_->after != state_->before) out = std::move(state_); else state_.reset();
    return MMS_OK;
}
bool TryEditBoundModelOutlineColor(MMDApp* app, void* ownerWindow) noexcept {
    if (!app) return false; auto* history = BoundNativePoseHistory(app); if (!history) return false;
    static thread_local bool active = false; if (active) return true;
    const auto result = Boundary([&]() -> MmsResult {
        struct Active { bool& value; Active(bool& b) : value(b) { value = true; } ~Active() { value = false; } } activity(active);
        WindowGuard windows(*app);
        NativeModelOutlineColorPlan plan; if (const auto status = plan.Begin(*app)) return status;
        CHOOSECOLORA cc{}; cc.lStructSize = sizeof(cc); cc.hwndOwner = static_cast<HWND>(ownerWindow);
        const auto color = ReadColor(*app); cc.rgbResult = RGB(BYTE(color[0]), BYTE(color[1]), BYTE(color[2]));
        std::array<COLORREF, 16> custom{};
        static_assert(sizeof(app->state.customColorTable) <= sizeof(custom));
        std::memcpy(custom.data(), app->state.customColorTable, sizeof(app->state.customColorTable)); cc.lpCustColors = custom.data();
        static_assert(sizeof(app->state.customColorTable) + sizeof(app->state.pad209) == sizeof(custom));
        std::memcpy(reinterpret_cast<unsigned char*>(custom.data()) + sizeof(app->state.customColorTable),
            &app->state.pad209, sizeof(app->state.pad209));
        cc.Flags = CC_RGBINIT | CC_FULLOPEN; app->state.dialogFlags[6] = 1; app->state.enterKeyState = 1;
        BOOL accepted = FALSE;
        { bake::ScopedUiInputGate input; accepted = ChooseColorA(&cc); }
        if (!accepted || windows.requested) return MMS_OK;
        if (const auto status = plan.Set(GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult))) return status;
        std::shared_ptr<HistoryChange> change; if (const auto status = plan.Take(change)) return status;
        const bool changed = bool(change);
        if (changed) {
            struct Publishing { const MMDApp* before; Publishing(MMDApp* a) : before(publication) { publication = a; } ~Publishing() { publication = before; } } authorized(app);
            bake::ScopedUiInputGate input;
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            if (const auto status = history->PrepareNative("Model outline color", std::move(change), transaction, SharedHistory::NativeIntent::ExplicitCommit)) return status;
            if (const auto status = history->CommitNative(std::move(transaction))) return status;
        }
        std::memcpy(app->state.customColorTable, custom.data(), sizeof(app->state.customColorTable));
        std::memcpy(&app->state.pad209, reinterpret_cast<const unsigned char*>(custom.data()) + sizeof(app->state.customColorTable), sizeof(app->state.pad209));
        if (!changed) return MMS_OK;
        Boundary([&]() -> MmsResult { bake::ScopedUiInputGate input; PublishBoundPoseEdit(*app, *history, false); return MMS_OK; });
        return MMS_OK;
    });
    if (result != MMS_OK) runtime_log::Write("PLUGIN_ABI model outline color refused: %d", result);
    return true;
}
}
#endif
