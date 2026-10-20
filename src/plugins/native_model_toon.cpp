#include "native_model_toon.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/shared_toon.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/texture_lookup.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <commctrl.h>
#include <climits>
#include <cstring>
#include <cwchar>
#include <exception>
#include <stdexcept>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
using ToonNames = std::array<std::array<char, 100>, 10>;
struct ReleaseTexture { void operator()(IDirect3DTexture9* p) const noexcept { if (p) p->Release(); } };
using ToonTextureRef = std::unique_ptr<IDirect3DTexture9, ReleaseTexture>;
thread_local const MMDApp* publication = nullptr;
thread_local bool* closing = nullptr;
bool Ready(MMDApp& a, bool replay) noexcept {
    const bool authorized = replay && (publication == &a || NativeObjectPublicationAuthorized(&a));
    return (!closing || !*closing) && (!bake::UiLocked() || authorized) &&
        (!a.SceneMutationInProgress() || authorized) && !SceneSaveBlocksEdits() && !NativeObjectUiPending(&a) &&
        !GetCapture() && !a.PlaybackActive() && !a.FrameStepPlayback() && !a.state.recordPlaybackStartPending &&
        !a.RecordingWindow() && !a.FrameRangeDialog() && !a.state.frameCopyDialog && !a.GravitySettingDialog() &&
        !a.GroundShadowColorDialog() && !a.AccessoryOrderArray() && !a.state.accessoryEditDialogOpen &&
        !NativeModelOutlineDialogBlocks(a) && a.MainWindow() &&
        GetWindowThreadProcessId(a.MainWindow(), nullptr) == GetCurrentThreadId();
}
struct WindowGuard {
    std::array<HWND, 2> windows{};
    std::array<bool, 2> close{};
    bool requested = false;
    bool* previous = closing;
    static constexpr UINT_PTR id = 0x4d4d5354;
    static LRESULT CALLBACK Filter(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& s = *reinterpret_cast<WindowGuard*>(data);
        if (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE)) {
            s.requested = true;
            for (unsigned i = 0; i < s.windows.size(); ++i) if (s.windows[i] == h) s.close[i] = true;
            return 0;
        }
        if (msg == WM_COMMAND || msg == WM_NOTIFY || msg == WM_HSCROLL || msg == WM_VSCROLL ||
            (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)) return 0;
        return DefSubclassProc(h, msg, wp, lp);
    }
    explicit WindowGuard(MMDApp& a) {
        windows = {a.MainWindow(), a.FloatingWindow() == a.MainWindow() ? nullptr : a.FloatingWindow()};
        closing = &requested;
        for (auto h : windows) if (h && !SetWindowSubclass(h, Filter, id, reinterpret_cast<DWORD_PTR>(this))) {
            Release(); throw std::runtime_error("Toon owner window guard");
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
bool TextValid(const std::wstring& text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = std::uint16_t(text[i]);
        if (!c) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || std::uint16_t(text[i]) < 0xdc00 || std::uint16_t(text[i]) > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}
bool DecodeName(const std::array<char, 100>& raw, std::wstring& out) {
    const auto length = strnlen_s(raw.data(), raw.size());
    // The native consumer expects a terminated CP932 filename. Do not repair
    // an unknown byte encoding or an unterminated legacy field on undo.
    return length < raw.size() && !text_encoding::HasUtf8Bom(raw.data()) &&
        text_encoding::Decode(raw.data(), 932, out) && TextValid(out);
}
}
struct NativeModelToonPlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    struct MaterialSource {
        std::uint8_t reference = 0;
        std::array<wchar_t, 512> path{};
    };
    MMDApp& app;
    SharedHistory* const history;
    const unsigned slot;
    unsigned char* const object;
    const MmsObjectRef initial;
    const std::uint64_t binding, scene, revision;
    std::shared_ptr<RetainedSceneObject> owner;
    D3DRenderer* renderer = nullptr;
    IDirect3DDevice9* device = nullptr;
    std::array<wchar_t, 256> directory{};
    std::array<ToonTextureRef, 11> shared;
    std::array<float, 30> edge{};
    mdl::ModelMaterialRecord* materials = nullptr;
    std::vector<MaterialSource> materialSources;
    unsigned char physicsMode = 0;
    ToonNames before{}, after{};
    ModelToonDraft beforeDraft, afterDraft;
    std::unique_ptr<import_texture_cache::Preparation> preparation;
    std::shared_ptr<import_texture_cache::Retained> textures;
    std::unique_ptr<import_texture_cache::Publication> cachePublication;
    std::uint64_t serial = 1, retainedBytes = 0;
    bool staged = false, finished = false, applied = false, published = false;
    State(MMDApp& a, SharedHistory& h, unsigned s, std::uint64_t rev)
        : app(a), history(&h), slot(s), object(a.ModelSlot(s)), initial(SceneObjectIdentity(object)),
          binding(BoundNativePoseGeneration(&a)), scene(a.SceneGeneration()), revision(rev) {}
    bool Source(bool forward, bool first) const noexcept {
        if (BoundNativePoseHistory(&app) != history || app.SceneGeneration() != scene || app.ModelSlot(slot) != object ||
            !owner || !owner->Present() || !SceneObjectPresent(object, initial.id) ||
            (first && (BoundNativePoseGeneration(&app) != binding || SceneObjectIdentity(object).generation != initial.generation)) ||
            app.Renderer() != renderer || !renderer || renderer->device != device ||
            std::memcmp(app.state.toonEdgeTable, edge.data(), sizeof(edge))) return false;
        for (unsigned i = 0; i < shared.size(); ++i) if (app.ToonTexture(i) != shared[i].get()) return false;
        const auto& m = *mdl::Mdl(object);
        const auto& expected = forward ? before : after;
        if (m.physicsMode != physicsMode || mdl::Materials(object) != materials || m.materialCount != materialSources.size() ||
            std::memcmp(m.modelDirectory, directory.data(), sizeof(directory)) ||
            std::memcmp(m.pmdToonFileNames, expected.data(), sizeof(expected))) return false;
        for (std::size_t i = 0; i < materialSources.size(); ++i)
            if (materials[i].toonReference != materialSources[i].reference ||
                std::memcmp(materials[i].toonPath, materialSources[i].path.data(), sizeof(materialSources[i].path))) return false;
        return true;
    }
    bool InitialCurrent() const noexcept {
        if (!Source(true, true)) return false;
        std::uint64_t current = 0; return !history->Revision(current) && current == revision;
    }
    bool SharedCurrent(const ToonNames& names) const noexcept {
        for (const auto& name : names) if (const auto index = SharedToonSlot(name.data()))
            if (!shared[index]) return false;
        for (const auto& material : materialSources) {
            const auto index = static_cast<std::int8_t>(material.reference);
            if ((index == -1 || index >= 10 || (physicsMode != 2 && index < 0)) && !shared[0]) return false;
        }
        return true;
    }
    MmsResult Required(const wchar_t* path, bool existingOnly) {
        // Capturing an old texture must not turn an unresolved old filename
        // into a newly rendered image merely because its file exists now.
        // The cache and Preparation both select the first exact path match.
        if (existingOnly) {
            const auto* entry = FindTextureResource(renderer, path);
            if (!entry || !entry->comObject) return MMS_E_NOT_READY;
        }
        return preparation->LoadRequired(path);
    }
    MmsResult Load(const ToonNames& names, const ModelToonDraft& draft, bool existingOnly) {
        if (!SharedCurrent(names)) return MMS_E_NOT_READY;
        for (unsigned i = 0; i < names.size(); ++i) if (names[i][0] && !SharedToonSlot(names[i].data())) {
            const std::wstring path = std::wstring(directory.data()) + draft[i];
            if (const auto r = Required(path.c_str(), existingOnly)) return r;
        }
        if (physicsMode == 2) for (const auto& material : materialSources) {
            const auto index = static_cast<std::int8_t>(material.reference);
            if (index < -1 && material.path[0])
                if (const auto r = Required(material.path.data(), existingOnly)) return r;
        }
        return preparation->Status();
    }
    void Measure(resource_budget::Collector& c) const {
        c.Add(this, sizeof(State) + 4 * sizeof(void*), 1); c.Vector(materialSources);
        for (const auto* draft : {&beforeDraft, &afterDraft}) for (const auto& name : *draft) c.String(name);
        if (textures) textures->MeasureResources(c);
        if (cachePublication) c.Array(cachePublication.get(), 1);
        for (const auto& texture : shared) if (texture) import_texture_cache::MeasureTextureResources(texture.get(), c);
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> state;
        const bool forward, first;
        const std::uint64_t serial, binding, generation;
        mutable resource_budget::Collector resources;
        std::uint64_t bytes = 0;
        bool consumed = false;
        Candidate(std::shared_ptr<State> s, bool f)
            : state(std::move(s)), forward(f), first(!state->published), serial(state->serial),
              binding(BoundNativePoseGeneration(&state->app)), generation(SceneObjectIdentity(state->object).generation) {}
        bool Metadata() const noexcept {
            return !consumed && state->serial == serial && forward != state->applied && state->Source(forward, first) &&
                BoundNativePoseGeneration(&state->app) == binding && SceneObjectIdentity(state->object).generation == generation &&
                Ready(state->app, true) && state->textures && state->textures->IsCurrent() &&
                (!first || (state->cachePublication && state->cachePublication->IsCurrent())) &&
                state->owner->CoversBookkeeping(resources);
        }
        bool IsCurrent() const noexcept override {
            return Metadata() && resources.Covers([&](auto& c) { state->Measure(c); }) && Metadata();
        }
        void Publish() noexcept override {
            // Driver/resource queries were checked before the journal's first
            // void publication. No allocation, file access, evaluation or UI.
            if (!Metadata()) std::terminate();
            if (first) state->cachePublication->Publish();
            const auto& names = forward ? state->after : state->before;
            std::memcpy(mdl::Mdl(state->object)->pmdToonFileNames, names.data(), sizeof(names));
            state->app.EnhancedModelDirty() = 1;
            state->applied = forward; state->published = true; ++state->serial; consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!staged || !finished || forward == applied || !Source(forward, !published) || (!published && !InitialCurrent())) return MMS_E_STALE;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            if (!Ready(app, true)) return MMS_E_BUSY;
            // A replaced cache is a stale source, not an incomplete texture
            // description. Check leases before measurement can mark missing;
            // genuine descriptor/budget failures keep their existing result.
            if (!textures || !textures->IsCurrent() ||
                (!published && (!cachePublication || !cachePublication->IsCurrent()))) return MMS_E_STALE;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward);
            Measure(next->resources);
            if (const auto r = owner->MeasureBookkeeping(next->resources)) return r;
            if (!next->resources.Result().Complete()) return MMS_E_NOT_READY;
            next->bytes = next->resources.Result().totalBytes;
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return retainedBytes; }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
MmsResult NativeModelToonPlan::Begin(MMDApp& app, unsigned slot, import_texture_cache::TextureLoader loader) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
        if (slot >= kModelSlotCount || !app.ModelSlot(slot)) return MMS_E_ARGUMENT;
        if (!Ready(app, false)) return MMS_E_BUSY;
        if (const auto r = history->CheckNativeEdit()) return r;
        std::uint64_t revision = 0; if (const auto r = history->Revision(revision)) return r;
        auto s = std::make_shared<State>(app, *history, slot, revision);
        s->owner = RetainSceneObject(app, MMS_OBJECT_MODEL, slot);
        s->renderer = app.Renderer(); if (!s->renderer || !s->renderer->device) return MMS_E_NOT_READY;
        s->device = s->renderer->device;
        const auto& m = *mdl::Mdl(s->object); s->physicsMode = m.physicsMode; s->materials = mdl::Materials(s->object);
        if (m.materialCount && !s->materials) return MMS_E_STALE;
        std::memcpy(s->directory.data(), m.modelDirectory, sizeof(s->directory));
        if (wcsnlen_s(s->directory.data(), s->directory.size()) == s->directory.size() || !TextValid(s->directory.data())) return MMS_E_NOT_READY;
        std::memcpy(s->before.data(), m.pmdToonFileNames, sizeof(s->before)); s->after = s->before;
        for (unsigned i = 0; i < s->before.size(); ++i) if (!DecodeName(s->before[i], s->beforeDraft[i])) return MMS_E_NOT_READY;
        s->afterDraft = s->beforeDraft;
        for (std::uint32_t i = 0; i < m.materialCount; ++i) {
            State::MaterialSource source; source.reference = s->materials[i].toonReference;
            std::memcpy(source.path.data(), s->materials[i].toonPath, sizeof(source.path));
            if (wcsnlen_s(source.path.data(), source.path.size()) == source.path.size() || !TextValid(source.path.data())) return MMS_E_NOT_READY;
            s->materialSources.push_back(std::move(source));
        }
        for (unsigned i = 0; i < s->shared.size(); ++i) if (auto* texture = app.ToonTexture(i)) { texture->AddRef(); s->shared[i].reset(texture); }
        std::memcpy(s->edge.data(), app.state.toonEdgeTable, sizeof(s->edge));
        s->preparation = std::make_unique<import_texture_cache::Preparation>(*s->renderer, loader);
        if (const auto r = s->Load(s->before, s->beforeDraft, true)) return r;
        if (!s->InitialCurrent()) return MMS_E_STALE;
        state_ = std::move(s); return MMS_OK;
    });
}
MmsResult NativeModelToonPlan::Read(ModelToonDraft& out) const noexcept {
    if (!state_) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        if (!state_->InitialCurrent()) return MMS_E_STALE;
        auto draft = state_->afterDraft; out = std::move(draft); return MMS_OK;
    });
}
MmsResult NativeModelToonPlan::Set(const ModelToonDraft& draft) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto& s = *state_; if (s.finished) return MMS_E_NOT_READY;
        if (!s.InitialCurrent()) return MMS_E_STALE;
        if (!Ready(s.app, false)) return MMS_E_BUSY;
        auto names = s.before; auto value = draft;
        for (unsigned i = 0; i < value.size(); ++i) {
            if (!TextValid(value[i])) return MMS_E_ARGUMENT;
            if (value[i] == s.beforeDraft[i]) continue;
            std::string bytes;
            if (!text_encoding::EncodeExact(value[i].c_str(), 932, bytes) || bytes.size() >= names[i].size()) return MMS_E_ARGUMENT;
            names[i].fill(0); std::memcpy(names[i].data(), bytes.data(), bytes.size());
        }
        if (!s.SharedCurrent(names)) return MMS_E_NOT_READY;
        if (!s.InitialCurrent()) return MMS_E_STALE;
        s.after = names; s.afterDraft = std::move(value); s.staged = true; return MMS_OK;
    });
}
MmsResult NativeModelToonPlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    if (!state_ || !state_->staged || state_->finished) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto& s = *state_;
        if (!s.InitialCurrent()) return MMS_E_STALE;
        if (!Ready(s.app, false)) return MMS_E_BUSY;
        if (s.before == s.after) { state_.reset(); return MMS_OK; }
        if (const auto r = s.Load(s.after, s.afterDraft, false)) return r;
        if (!s.InitialCurrent()) return MMS_E_STALE;
        if (const auto r = s.preparation->Finish(s.textures, s.cachePublication)) return r;
        s.finished = true; s.preparation.reset();
        resource_budget::Collector resources; s.Measure(resources);
        if (const auto r = s.owner->MeasureBookkeeping(resources)) return r;
        if (!resources.Result().Complete()) return MMS_E_NOT_READY;
        s.retainedBytes = resources.Result().totalBytes;
        if (!s.InitialCurrent() || !s.textures->IsCurrent() || !s.cachePublication->IsCurrent()) return MMS_E_STALE;
        out = std::move(state_); return MMS_OK;
    });
}
namespace {
struct Dialog {
    ModelToonDraft draft;
    MmsResult error = MMS_OK;
    HWND composing = nullptr;
    static std::wstring Text(HWND edit) {
        if (!edit || !IsWindow(edit) || !IsWindowUnicode(edit)) throw std::runtime_error("Toon EDIT missing");
        const auto length = SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0);
        if (length < 0 || length >= INT_MAX) throw std::runtime_error("Toon EDIT length");
        std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
        const auto copied = SendMessageW(edit, WM_GETTEXT, value.size(), reinterpret_cast<LPARAM>(value.data()));
        if (copied != length || value[static_cast<std::size_t>(length)] || !IsWindow(edit) ||
            SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0) != length) throw std::runtime_error("Toon EDIT read");
        value.resize(static_cast<std::size_t>(length)); return value;
    }
    static bool Write(HWND edit, const std::wstring& value) {
        return edit && IsWindowUnicode(edit) && SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value.c_str())) && Text(edit) == value;
    }
    void Save(HWND h) {
        ModelToonDraft value;
        for (unsigned i = 0; i < value.size(); ++i) value[i] = Text(GetDlgItem(h, 709 + i));
        draft = std::move(value);
    }
    static LRESULT CALLBACK Edit(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
        auto& s = *reinterpret_cast<Dialog*>(data);
        if (msg == WM_IME_STARTCOMPOSITION) s.composing = h;
        if (msg == WM_IME_ENDCOMPOSITION && s.composing == h) s.composing = nullptr;
        if (msg == WM_GETDLGCODE && (wp == VK_RETURN || s.composing == h)) return DefSubclassProc(h, msg, wp, lp) | DLGC_WANTALLKEYS;
        if (msg == WM_KEYDOWN && wp == VK_RETURN && s.composing != h) {
            const auto r = Boundary([&]() -> MmsResult { s.Save(GetParent(h)); return MMS_OK; });
            if (r) { s.error = r; EndDialog(GetParent(h), IDCANCEL); } return 0;
        }
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, Edit, id);
        return DefSubclassProc(h, msg, wp, lp);
    }
    static INT_PTR CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* s = reinterpret_cast<Dialog*>(GetWindowLongPtrW(h, DWLP_USER));
        if (msg == WM_INITDIALOG) { s = reinterpret_cast<Dialog*>(lp); SetWindowLongPtrW(h, DWLP_USER, lp); }
        if (!s) return FALSE;
        bool handled = false;
        const auto result = Boundary([&]() -> MmsResult {
            if (msg == WM_INITDIALOG) {
                handled = true; if (!IsWindowUnicode(h) || !GetDlgItem(h, IDOK) || !GetDlgItem(h, IDCANCEL)) return MMS_E_FAILED;
                for (unsigned i = 0; i < s->draft.size(); ++i) {
                    const auto edit = GetDlgItem(h, 709 + i);
                    if (!edit || !IsWindowUnicode(edit) || !SetWindowSubclass(edit, Edit, WindowGuard::id, reinterpret_cast<DWORD_PTR>(s))) return MMS_E_FAILED;
                    SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);
                    if (!Write(edit, s->draft[i])) return MMS_E_FAILED;
                }
                return MMS_OK;
            }
            if (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE) || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
                handled = true; EndDialog(h, IDCANCEL); return MMS_OK;
            }
            if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
                handled = true; if (s->composing) return MMS_OK;
                s->Save(h); EndDialog(h, IDOK);
            }
            return MMS_OK;
        });
        if (result) { s->error = result; EndDialog(h, IDCANCEL); return TRUE; }
        return handled ? TRUE : FALSE;
    }
};
}
bool TryEditBoundModelToon(MMDApp* app, void* ownerWindow) noexcept {
    if (!app) return false; auto* history = BoundNativePoseHistory(app); if (!history) return false;
    static thread_local bool active = false; if (active) return true;
    const auto result = Boundary([&]() -> MmsResult {
        if (app->CameraMode() || g_Block != app) return MMS_E_NOT_READY;
        struct Active { bool& b; explicit Active(bool& v) : b(v) { b = true; } ~Active() { b = false; } } guard(active);
        NativeModelToonPlan plan; if (const auto r = plan.Begin(*app, app->SelectedModelSlot())) return r;
        Dialog dialog; if (const auto r = plan.Read(dialog.draft)) return r;
        WindowGuard windows(*app);
        INT_PTR answer = -1;
        { bake::ScopedUiInputGate gate; answer = UiDialogBoxParamW(static_cast<HINSTANCE>(app->HInstance()),
            MAKEINTRESOURCEW(app->EnglishUI() ? 682 : 681), static_cast<HWND>(ownerWindow), Dialog::Proc, reinterpret_cast<LPARAM>(&dialog)); }
        if (dialog.error) return dialog.error;
        if (answer == -1) return MMS_E_FAILED;
        if (answer != IDOK || windows.requested) return MMS_OK;
        if (const auto r = plan.Set(dialog.draft)) return r;
        std::shared_ptr<HistoryChange> change; if (const auto r = plan.Take(change)) return r;
        if (!change) return MMS_OK;
        {
            struct PublishScope { const MMDApp* before; explicit PublishScope(MMDApp* a) : before(publication) { publication = a; } ~PublishScope() { publication = before; } } authorized(app);
            bake::ScopedUiInputGate gate;
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            if (const auto r = history->PrepareNative("Model Toon filenames", std::move(change), transaction, SharedHistory::NativeIntent::ExplicitCommit)) return r;
            if (const auto r = history->CommitNative(std::move(transaction))) return r;
        }
        PublishBoundPoseEdit(*app, *history, false);
        RefreshBoundObjectUi(*app, *history); return MMS_OK;
    });
    if (result) runtime_log::Write("PLUGIN_ABI model Toon filenames refused: %d", result);
    return true;
}
}
#endif
