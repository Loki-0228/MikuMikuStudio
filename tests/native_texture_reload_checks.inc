// Included after native_model_toon_checks.inc in the full native history
// test executable; the dedicated CLI runs these resource-version checks.
// No production edit service is enabled. These tests own a real D3D9 device;
// they do not use ImportTestTexture::GetDevice(E_NOTIMPL) as a valid texture.
#include "../src/plugins/native_texture_reload.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "../src/plugins/native_history_commands.hpp"
#include "../src/plugins/native_global_commands.hpp"
#include "../src/plugins/native_morph_commands.hpp"
#include "../src/plugins/native_audio_history.hpp"
#include <type_traits>

namespace mikumikustudio {
void CmdViewMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
}

#if !MMD_ORIGINAL_PROFILE && defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS) && defined(_WIN64)
extern "C" {
extern mikumikustudio::d3dx::FnCreateTexInMemEx __imp_D3DXCreateTextureFromFileInMemoryEx;
extern mikumikustudio::d3dx::FnCreateTexFromFileExA __imp_D3DXCreateTextureFromFileExA;
extern mikumikustudio::d3dx::FnCreateTexFromFileExW __imp_D3DXCreateTextureFromFileExW;
}
#endif

#if !MMD_ORIGINAL_PROFILE
namespace texture_reload_checks {
using namespace plugins;
namespace tr = texture_reload;
namespace itc = import_texture_cache;
static_assert(!std::is_copy_constructible_v<NativeTextureReloadPlan>);
static_assert(!std::is_copy_assignable_v<NativeTextureReloadPlan>);

struct Fixture;
thread_local Fixture* active = nullptr;
IDirect3DTexture9* MakeTexture(IDirect3DDevice9* device, DWORD pixel, UINT size = 4) {
    IDirect3DTexture9* raw = nullptr;
    Check(device && size && SUCCEEDED(device->CreateTexture(size, size, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &raw, nullptr)) && raw, "278 owns a real managed D3D9 texture");
    tr::Texture texture(raw); D3DLOCKED_RECT locked{};
    Check(SUCCEEDED(raw->LockRect(0, &locked, nullptr, 0)), "278 fixture locks its real texture");
    for (unsigned y = 0; y < size; ++y) {
        auto* row = reinterpret_cast<DWORD*>(static_cast<unsigned char*>(locked.pBits) + y * locked.Pitch);
        for (unsigned x = 0; x < size; ++x) row[x] = pixel;
    }
    Check(SUCCEEDED(raw->UnlockRect(0)), "278 fixture unlocks its real texture");
    Microsoft::WRL::ComPtr<IDirect3DDevice9> owner;
    Check(SUCCEEDED(raw->GetDevice(&owner)) && owner.Get() == device,
        "278 fixture texture reports the actual owning device");
    return texture.release();
}
constexpr DWORD cachePixel = 0x7f446688;
constexpr std::int32_t cacheTag = 0x03886644;
HRESULT CacheLoader(IDirect3DDevice9*, const wchar_t*, IDirect3DTexture9**);
MmsResult SharedLoader(MMDApp&, tr::PreparedSharedToon&);

struct TextureWitness {
    Microsoft::WRL::ComPtr<IDirect3DTexture9> first, second;
    ULONG atTransfer = 0;
    static ULONG References(IDirect3DTexture9* texture) {
        const auto count = texture->AddRef(); texture->Release(); return count;
    }
    void Capture(IDirect3DTexture9* texture, IDirect3DDevice9* expected) {
        Check(texture && !first && !second, "278 cleanup witness captures one live real COM object once");
        Microsoft::WRL::ComPtr<IDirect3DDevice9> device;
        Check(SUCCEEDED(texture->GetDevice(&device)) && device.Get() == expected,
            "278 cleanup witness refers to the fixture's actual D3D9 device");
        first = texture; second = texture; atTransfer = References(texture);
    }
    void TransferredReferenceReleased() const {
        Check(first && second && atTransfer && References(first.Get()) == atTransfer - 1,
            "278 failed/retired raw output releases exactly its transferred COM reference");
    }
    void StillOwnedByOutput() const {
        Check(first && second && References(first.Get()) == atTransfer,
            "278 successful raw output is still owned in addition to independent COM witnesses");
    }
};
struct InjectedTextureFailure {};

struct Fixture : model_toon_checks::Fixture {
    Fixture* previous = active;
    unsigned cacheCalls = 0, sharedCalls = 0;
    unsigned failCacheAt = 0, failSharedAt = 0, throwCacheAt = 0;
    bool offline = false, wrongCacheDevice = false, wrongSharedDevice = false;
    Microsoft::WRL::ComPtr<IDirect3DDevice9> foreignDevice;
    TextureWitness thrownTexture;
    std::wstring cwd;
    Fixture() : model_toon_checks::Fixture(true) {
        active = this;
        // ObjectCommandFixture value-initializes MMDAppState, including its
        // executable directory. Restore the real module's absolute directory
        // without changing the process-wide working directory.
        std::array<wchar_t, 32768> directory{}, module{};
        const auto cwdLength = GetCurrentDirectoryW(DWORD(directory.size()), directory.data());
        Check(cwdLength && cwdLength < directory.size(), "278 records the original process directory");
        cwd.assign(directory.data(), cwdLength);
        const auto moduleLength = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size()));
        Check(moduleLength && moduleLength < module.size(), "278 obtains the real executable filename without truncation");
        const std::wstring filename(module.data(), moduleLength);
        const auto separator = filename.find_last_of(L"\\/");
        const bool absolute = filename.size() > 2 &&
            ((filename[1] == L':' && (filename[2] == L'\\' || filename[2] == L'/')) ||
                (filename[0] == L'\\' && filename[1] == L'\\'));
        Check(absolute && separator != std::wstring::npos && separator + 1 < 256,
            "278 actual executable directory fits the existing 256-wchar app field");
        std::wmemcpy(app->ExeDir(), filename.data(), separator + 1); app->ExeDir()[separator + 1] = L'\0';
        const auto afterLength = GetCurrentDirectoryW(DWORD(directory.size()), directory.data());
        Check(afterLength == cwd.size() && cwd == directory.data(), "278 restores ExeDir without changing CWD");
        // The inherited fixture's fake shared textures are appropriate for
        // its old tests only. Replace every one before testing the new plan.
        for (unsigned i = 0; i < 11; ++i) {
            app->ToonTexture(i)->Release();
            app->ToonTexture(i) = MakeTexture(uiDevice.Get(), 0xff102030 + i);
        }
        Check(textures.state.live == 0, "278 success contains no inherited non-device fake textures");
        AppendReal(L"D:\\texture-reload\\cache-0.bmp", 0xff123456, std::int32_t(0xa5123456u));
        AppendReal(L"D:\\texture-reload\\cache-1.bmp", 0xff654321, std::int32_t(0x9b654321u));
        const auto finalLength = GetCurrentDirectoryW(DWORD(directory.size()), directory.data());
        Check(finalLength == cwd.size() && cwd == directory.data(), "278 fixture resource setup preserves CWD");
    }
    ~Fixture() { active = previous; }
    size_t Used() const {
        size_t count = 0; while (count < 10000 && textures.renderer->resourcePool[count].heapBuffer) ++count; return count;
    }
    size_t AppendReal(const wchar_t* path, DWORD pixel, std::int32_t tag) {
        const size_t index = Used(); Check(index < 10000, "278 append fixture fits original cache");
        auto* texture = MakeTexture(uiDevice.Get(), pixel); Append(path, texture);
        textures.renderer->resourcePool[index].tag = tag; return index;
    }
    TextureReloadLoaders Loaders() const { return {CacheLoader, SharedLoader}; }
    std::shared_ptr<HistoryChange> Change() {
        NativeTextureReloadPlan plan; std::shared_ptr<HistoryChange> change;
        Check(plan.Begin(*app, Loaders()) == MMS_OK && plan.Take(change) == MMS_OK && change,
            "278 prepares all original resources privately through frozen loaders");
        return change;
    }
    void ForeignDevice() {
        D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
        Check(SUCCEEDED(uiD3d->CreateDevice(0, D3DDEVTYPE_HAL, app->MainWindow(),
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &foreignDevice)),
            "278 invalid-origin test owns a second real device");
    }
    void UnchangedPosition() {
        wchar_t directory[32768]{};
        Check(GetCurrentDirectoryW(32768, directory) && cwd == directory && Unmoved(),
            "278 never seeks, evaluates pose or changes the working directory");
    }
};
HRESULT CacheLoader(IDirect3DDevice9* device, const wchar_t* path, IDirect3DTexture9** out) {
    Check(active && device == active->uiDevice.Get() && path && *path && out,
        "278 cache loader receives the real captured source device and path");
    auto& f = *active; ++f.cacheCalls; *out = nullptr;
    if (f.offline) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (f.failCacheAt && f.cacheCalls == f.failCacheAt) return E_OUTOFMEMORY;
    *out = MakeTexture(f.wrongCacheDevice ? f.foreignDevice.Get() : device, cachePixel);
    if (f.throwCacheAt && f.cacheCalls == f.throwCacheAt) {
        f.thrownTexture.Capture(*out, device);
        // TextureLoader is deliberately not noexcept. This controlled throw
        // stays inside PrepareCacheTexture's catch; it crosses no Win32 or
        // D3DX ABI callback and leaves the owned raw out for that helper.
        throw InjectedTextureFailure{};
    }
    return S_OK;
}
MmsResult SharedLoader(MMDApp& app, tr::PreparedSharedToon& out) {
    Check(active && active->app.get() == &app, "278 shared loader receives the captured app");
    auto& f = *active; ++f.sharedCalls;
    if (f.offline) return MMS_E_NOT_READY;
    tr::PreparedSharedToon candidate;
    for (unsigned i = 0; i < candidate.textures.size(); ++i) {
        if (f.failSharedAt && i + 1 == f.failSharedAt) return MMS_E_MEMORY;
        candidate.textures[i].reset(MakeTexture(f.wrongSharedDevice ? f.foreignDevice.Get() : f.uiDevice.Get(), 0xff445560 + i));
    }
    for (unsigned i = 0; i < candidate.edge.size(); ++i) candidate.edge[i] = .55f + .005f * i;
    out = std::move(candidate); return MMS_OK;
}
struct Slot {
    void* name = nullptr; IUnknown* texture = nullptr; std::int32_t tag = 0;
};
Slot At(Fixture& f, size_t index) {
    const auto& value = f.textures.renderer->resourcePool[index]; return {value.heapBuffer, value.comObject, value.tag};
}
bool Same(const Slot& a, const Slot& b) { return a.name == b.name && a.texture == b.texture && a.tag == b.tag; }
struct Snapshot {
    std::array<IDirect3DTexture9*, 11> shared{};
    std::array<float, 30> edge{};
    std::vector<Slot> prefix;
    explicit Snapshot(Fixture& f) {
        for (unsigned i = 0; i < shared.size(); ++i) shared[i] = f.app->ToonTexture(i);
        std::memcpy(edge.data(), f.app->state.toonEdgeTable, sizeof(edge));
        for (size_t i = 0; i < f.Used(); ++i) prefix.push_back(At(f, i));
    }
    void Matches(Fixture& f) const {
        for (unsigned i = 0; i < shared.size(); ++i)
            Check(f.app->ToonTexture(i) == shared[i], "278 preserves exact shared COM identity in this direction");
        Check(!std::memcmp(edge.data(), f.app->state.toonEdgeTable, sizeof(edge)), "278 preserves all thirty edge floats bit-for-bit");
        for (size_t i = 0; i < prefix.size(); ++i)
            Check(Same(At(f, i), prefix[i]), "278 restores original cache name, COM identity and all four tag bytes");
    }
};
void Commit(Fixture& f, const std::shared_ptr<HistoryChange>& change) {
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("Reload textures test", change, token, SharedHistory::NativeIntent::ExplicitCommit) == MMS_OK,
        "278 reserves one complete native participant before publication");
    failAfter = 0; const auto result = f.history.CommitNative(std::move(token)); failAfter = -1;
    Check(result == MMS_OK, "278 final native resource exchange allocates nothing");
}

// Only the two preparation hooks provide controlled resources. The third
// hook observes and forwards the real builtin dispatch, without substituting
// the menu handler or its result.
struct MenuLoaders {
    using PrepareCache = MmsResult (*)(IDirect3DDevice9*, const wchar_t*, itc::TextureLoader, tr::PreparedCacheTexture&) noexcept;
    static inline PrepareCache originalCache = nullptr;
    static inline decltype(&CmdViewMenu) originalView = nullptr;
    static inline MenuLoaders* current = nullptr;
    Fixture* running = nullptr;
    std::uint16_t expectedNotification = 0;
    unsigned builtinCalls = 0;
    bool initialized = false;
    static MmsResult Shared(MMDApp& app, tr::PreparedSharedToon& out) noexcept {
        return Boundary([&] { return SharedLoader(app, out); });
    }
    static MmsResult Cache(IDirect3DDevice9* device, const wchar_t* path, itc::TextureLoader, tr::PreparedCacheTexture& out) noexcept {
        return originalCache(device, path, CacheLoader, out);
    }
    static void View(MMDApp* app, HWND window, std::uint16_t id, std::uint16_t notification) {
        Check(current && current->running && app == current->running->app.get() &&
            window == app->MainWindow() && id == 278 && notification == current->expectedNotification && notification <= 1,
            "278 actual MainWndProc/CommandDispatch reaches its original builtin family with the fixture HWND");
        ++current->builtinCalls; originalView(app, window, id, notification);
    }
    MenuLoaders() {
        Check(!current, "278 has only one live private menu hook owner"); current = this;
        const auto status = MH_Initialize(); initialized = status == MH_OK;
        Check(initialized || status == MH_ERROR_ALREADY_INITIALIZED, "278 initializes process-local private loader hooks");
        Check(MH_CreateHook(reinterpret_cast<void*>(&tr::PrepareSharedToon), reinterpret_cast<void*>(&Shared), nullptr) == MH_OK &&
            MH_CreateHook(reinterpret_cast<void*>(&tr::PrepareCacheTexture), reinterpret_cast<void*>(&Cache),
                reinterpret_cast<void**>(&originalCache)) == MH_OK &&
            MH_CreateHook(reinterpret_cast<void*>(&CmdViewMenu), reinterpret_cast<void*>(&View),
                reinterpret_cast<void**>(&originalView)) == MH_OK,
            "278 hooks private preparation and a forwarding builtin-family observer");
        Check(MH_EnableHook(reinterpret_cast<void*>(&tr::PrepareSharedToon)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(&tr::PrepareCacheTexture)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(&CmdViewMenu)) == MH_OK, "278 enables controlled loaders and the actual menu observer");
    }
    ~MenuLoaders() {
        MH_DisableHook(reinterpret_cast<void*>(&tr::PrepareSharedToon)); MH_DisableHook(reinterpret_cast<void*>(&tr::PrepareCacheTexture));
        MH_DisableHook(reinterpret_cast<void*>(&CmdViewMenu)); MH_RemoveHook(reinterpret_cast<void*>(&CmdViewMenu));
        MH_RemoveHook(reinterpret_cast<void*>(&tr::PrepareSharedToon)); MH_RemoveHook(reinterpret_cast<void*>(&tr::PrepareCacheTexture));
        originalCache = nullptr; originalView = nullptr; current = nullptr; if (initialized) MH_Uninitialize();
    }
    void Run(Fixture& f, std::uint16_t notification = 0) {
        DWORD process = 0; const auto window = f.app->MainWindow();
        const auto thread = GetWindowThreadProcessId(window, &process);
        Check(!running && notification <= 1 && g_Block == f.app.get() && f.app->Hwnd() == f.app->MainWindow(),
            "278 actual dispatcher owns the fixture app and main window");
        Check(IsWindow(window) && process == GetCurrentProcessId() && thread == GetCurrentThreadId(),
            "278 invokes the production WndProc only for the fixture's living UI-thread HWND");
        running = &f; expectedNotification = notification; const auto before = builtinCalls;
        // The fixture retains its STATIC procedure for construction, teardown
        // and unrelated messages. This direct production WndProc invocation
        // covers the complete command entry, including its real ObserveMessage
        // classification, without routing lifecycle messages into app startup.
        // This is a production entry call on an owned fixture HWND, not an
        // InitMainWindow-created complete host.
        const auto result = MainWndProc(window, WM_COMMAND, MAKEWPARAM(278, notification), 0);
        running = nullptr;
        Check(!result && builtinCalls == before + 1, "278 real WndProc command entry executes the builtin menu handler exactly once");
    }
};

// Hidden-fixture synchronous call observation only. Every spy forwards to
// production and none suppresses a repaint, edit, renderer call or UI update.
// No messages are pumped here: later ordinary SafePoint/GPU drawing is outside
// this observation window and is not asserted to avoid FrameDriver forever.
struct EffectsObserver {
    static inline EffectsObserver* active = nullptr;
    struct Counts {
        unsigned publish = 0, viewport = 0, geometry = 0, post = 0, frame = 0, selection = 0;
        unsigned global = 0, morph = 0, audio = 0, curve = 0, shutdown = 0, whole = 0, topEnable = 0;
        bool wrongMask = false, physics = false;
    } counts;
    MMDApp& app; SharedHistory& history;
    decltype(&PublishBoundHistoryEffects) publish = nullptr;
    decltype(&updates::RequestViewport) viewport = nullptr;
    decltype(&PostViewRefresh) post = nullptr;
    decltype(&FrameDriver) frame = nullptr;
    decltype(&SelectionReeval) selection = nullptr;
    decltype(&RefreshBoundGlobalReadout) global = nullptr;
    decltype(&RefreshBoundMorphReadout) morph = nullptr;
    decltype(&RefreshBoundAudioUi) audio = nullptr;
    decltype(&curve_editor::NotifyOwnedChanged) curve = nullptr;
    decltype(&ShutdownD3D12Native) shutdown = nullptr;
    decltype(&InvalidateRect) invalidate = nullptr;
    decltype(&RedrawWindow) redraw = nullptr;
    decltype(&EnableWindow) enable = nullptr;
    std::array<void*, 13> targets{}; size_t installed = 0;
    template<class Function, class Hook> void Install(Function target, Hook hook, Function& original) {
        auto* address = reinterpret_cast<void*>(target);
        Check(installed < targets.size() && MH_CreateHook(address, reinterpret_cast<void*>(hook),
            reinterpret_cast<void**>(&original)) == MH_OK && MH_EnableHook(address) == MH_OK,
            "278 installs a forwarding synchronous effects observer");
        targets[installed++] = address;
    }
    EffectsObserver(MMDApp& value, SharedHistory& journal) : app(value), history(journal) {
        Check(!active && MenuLoaders::current, "278 effects observers share only the live menu fixture's hook initialization"); active = this;
        Install(&PublishBoundHistoryEffects, &Publish, publish);
        Install(&updates::RequestViewport, &Viewport, viewport);
        Install(&PostViewRefresh, &Post, post); Install(&FrameDriver, &Frame, frame);
        Install(&SelectionReeval, &Selection, selection);
        Install(&RefreshBoundGlobalReadout, &Global, global); Install(&RefreshBoundMorphReadout, &Morph, morph);
        Install(&RefreshBoundAudioUi, &Audio, audio); Install(&curve_editor::NotifyOwnedChanged, &Curve, curve);
        Install(&ShutdownD3D12Native, &Shutdown, shutdown);
        Install(&InvalidateRect, &Invalidate, invalidate); Install(&RedrawWindow, &Redraw, redraw);
        Install(&EnableWindow, &Enable, enable);
    }
    ~EffectsObserver() {
        while (installed) { auto* target = targets[--installed]; MH_DisableHook(target); MH_RemoveHook(target); }
        active = nullptr;
    }
    void Reset() { counts = {}; }
    static void Publish(MMDApp& app, SharedHistory& history, HistoryEffects effects, bool physics) noexcept {
        auto& s = *active;
        if (&app == &s.app) {
            ++s.counts.publish; s.counts.wrongMask |= &history != &s.history || effects != HistoryEffects::Viewport;
            s.counts.physics |= physics;
        }
        s.publish(app, history, effects, physics);
    }
    static void Viewport(MMDApp* app, bool geometry) {
        auto& s = *active; if (app == &s.app) { ++s.counts.viewport; s.counts.geometry += geometry; } s.viewport(app, geometry);
    }
    static void Post(MMDApp* app) { auto& s = *active; if (app == &s.app) ++s.counts.post; s.post(app); }
    static void Frame(MMDApp* app) { auto& s = *active; if (app == &s.app) ++s.counts.frame; s.frame(app); }
    static void Selection(MMDApp* app) { auto& s = *active; if (app == &s.app) ++s.counts.selection; s.selection(app); }
    static void Global(MMDApp& app) noexcept { auto& s = *active; if (&app == &s.app) ++s.counts.global; s.global(app); }
    static void Morph(MMDApp& app) noexcept { auto& s = *active; if (&app == &s.app) ++s.counts.morph; s.morph(app); }
    static bool Audio(MMDApp& app) noexcept { auto& s = *active; if (&app == &s.app) ++s.counts.audio; return s.audio(app); }
    static void Curve(void* owner) noexcept { auto& s = *active; if (owner == s.app.MainWindow()) ++s.counts.curve; s.curve(owner); }
    static void Shutdown() { auto& s = *active; ++s.counts.shutdown; s.shutdown(); }
    static BOOL WINAPI Invalidate(HWND window, const RECT* rect, BOOL erase) {
        auto& s = *active; if (window == s.app.MainWindow() && !rect) ++s.counts.whole;
        return s.invalidate(window, rect, erase);
    }
    static BOOL WINAPI Redraw(HWND window, const RECT* rect, HRGN region, UINT flags) {
        auto& s = *active; if (window == s.app.MainWindow() && !rect && !region) ++s.counts.whole;
        return s.redraw(window, rect, region, flags);
    }
    static BOOL WINAPI Enable(HWND window, BOOL enabled) {
        auto& s = *active;
        if (window == s.app.MainWindow() || (s.app.FloatingWindow() && window == s.app.FloatingWindow())) ++s.counts.topEnable;
        return s.enable(window, enabled);
    }
    void ViewportOnly(const char* phase, size_t undo, size_t redo) const {
        const auto& c = counts;
        std::printf("RELOAD_EFFECTS phase=%s publish=%u viewport=%u geometry=%u post=%u global=%u morph=%u audio=%u curve=%u frame=%u selection=%u shutdown=%u whole=%u top_enable=%u seen=%u\n",
            phase, c.publish, c.viewport, c.geometry, c.post, c.global, c.morph, c.audio, c.curve, c.frame,
            c.selection, c.shutdown, c.whole, c.topEnable, unsigned(app.MessageSeen()));
        Check(c.publish == 1 && !c.wrongMask && !c.physics && c.viewport > 0 && !c.geometry && !c.post &&
            !c.global && !c.morph && !c.audio && !c.curve && !c.frame && !c.selection && !c.shutdown && !c.whole && !c.topEnable,
            "278 forwards one real Viewport-only publication without broad UI, authored readout or synchronous renderer/scene evaluation");
        Check(history.LastPublicationEffects() == HistoryEffects::Viewport &&
            !HasHistoryEffect(history.LastPublicationEffects(), HistoryEffects::ProjectData) &&
            !history.LastPublicationRequiresPhysicsReset() && history.UndoCount() == undo && history.RedoCount() == redo,
            "278 frozen history effects and cursor remain renderer-only in this direction");
        const auto undoButton = GetDlgItem(app.MainWindow(), panel::kUndoButton);
        const auto redoButton = GetDlgItem(app.MainWindow(), panel::kRedoButton);
        Check(undoButton && redoButton && bool(IsWindowEnabled(undoButton)) == bool(undo) &&
            bool(IsWindowEnabled(redoButton)) == bool(redo), "278 real history button enabled states match the actual shared journal");
    }
};

struct RawBytes {
    const void* address = nullptr;
    std::vector<unsigned char> bytes;
    RawBytes() = default;
    RawBytes(const void* source, size_t size) : address(source) {
        Check(source || !size, "278 authored snapshot has an owned range or zero bytes");
        if (size) { bytes.resize(size); std::memcpy(bytes.data(), source, size); }
    }
    void Unchanged(const void* source, size_t size) const {
        Check(source == address && size == bytes.size() && (!size || !std::memcmp(source, bytes.data(), size)),
            "278 preserves authored allocation identity, capacity and every captured byte");
    }
};
struct AuthoredSnapshot {
    struct Model {
        mdl::ModelRecord* pointer;
        uint64_t boneId, morphId, displayId;
        RawBytes record, bone, morph, display, materials;
        explicit Model(mdl::ModelRecord* m) : pointer(m), boneId(mdl::BoneKeyPoolIdentity(m->boneKeys)),
            morphId(mdl::MorphKeyPoolIdentity(m->morphKeys)), displayId(mdl::KeyPoolIdentity(m->displayKeys)),
            record(m, sizeof(*m)), bone(m->boneKeys, mdl::BoneKeyCapacity(m->boneKeys) * sizeof(mdl::BoneKey)),
            morph(m->morphKeys, mdl::MorphKeyCapacity(m->morphKeys) * sizeof(mdl::MorphKey)),
            display(m->displayKeys, mdl::KeyPoolCapacity(m->displayKeys) * sizeof(mdl::DisplayKey)),
            materials(m->materials, size_t(m->materialCount) * sizeof(mdl::ModelMaterialRecord)) {}
        void Unchanged(mdl::ModelRecord* m) const {
            Check(m == pointer && m && boneId == mdl::BoneKeyPoolIdentity(m->boneKeys) &&
                morphId == mdl::MorphKeyPoolIdentity(m->morphKeys) && displayId == mdl::KeyPoolIdentity(m->displayKeys),
                "278 preserves original model and each dedicated bone/morph/display pool identity");
            record.Unchanged(m, sizeof(*m)); bone.Unchanged(m->boneKeys, mdl::BoneKeyCapacity(m->boneKeys) * sizeof(mdl::BoneKey));
            morph.Unchanged(m->morphKeys, mdl::MorphKeyCapacity(m->morphKeys) * sizeof(mdl::MorphKey));
            display.Unchanged(m->displayKeys, mdl::KeyPoolCapacity(m->displayKeys) * sizeof(mdl::DisplayKey));
            materials.Unchanged(m->materials, size_t(m->materialCount) * sizeof(mdl::ModelMaterialRecord));
        }
    };
    MMDApp& app;
    uint64_t revision, generation, cameraId, lightId;
    int frame, cameraMode, model, accessory;
    uint32_t message;
    uint8_t input, dirty, recorded, enhanced, physics;
    std::array<float, 9> values;
    RawBytes camera, light;
    std::array<Model, 2> models;
    std::array<RawBytes, 2> accessories, accessoryKeys;
    static std::array<float, 9> Values(MMDApp& a) {
        return {a.CameraFov(), a.CameraPositionX(), a.CameraDistance(), a.LightColor()[0], a.LightColor()[1], a.LightColor()[2],
            a.LightDirection()[0], a.LightDirection()[1], a.LightDirection()[2]};
    }
    explicit AuthoredSnapshot(MMDApp& a) : app(a), revision(a.SceneRevision()), generation(a.SceneGeneration()),
        cameraId(mdl::KeyPoolIdentity(a.CameraKeys())), lightId(mdl::KeyPoolIdentity(a.LightKeys())),
        frame(a.CurrentFrame()), cameraMode(a.CameraMode()), model(a.SelectedModelSlot()), accessory(a.SelectedAccessorySlot()),
        message(a.MessageSeen()), input(a.ViewportInputActive()), dirty(a.SceneModified()), recorded(a.RecordedFrameCountByte1()),
        enhanced(a.EnhancedModelDirty()), physics(a.PhysicsResetPending()), values(Values(a)),
        camera(a.CameraKeys(), mdl::KeyPoolCapacity(a.CameraKeys()) * sizeof(mdl::CameraKey)),
        light(a.LightKeys(), mdl::KeyPoolCapacity(a.LightKeys()) * sizeof(mdl::LightKey)),
        models{Model(mdl::Mdl(a.ModelSlot(0))), Model(mdl::Mdl(a.ModelSlot(1)))},
        accessories{RawBytes(a.AccessorySlot(0), sizeof(mdl::AccessoryRecord)), RawBytes(a.AccessorySlot(1), sizeof(mdl::AccessoryRecord))},
        accessoryKeys{RawBytes(a.AccessoryKeys(0), mdl::KeyPoolCapacity(a.AccessoryKeys(0)) * sizeof(mdl::AccessoryKey)),
            RawBytes(a.AccessoryKeys(1), mdl::KeyPoolCapacity(a.AccessoryKeys(1)) * sizeof(mdl::AccessoryKey))} {}
    void Unchanged() const {
        const auto after = Values(app);
        Check(revision == app.SceneRevision() && generation == app.SceneGeneration() && frame == app.CurrentFrame() &&
            cameraMode == app.CameraMode() && model == app.SelectedModelSlot() && accessory == app.SelectedAccessorySlot() &&
            !std::memcmp(values.data(), after.data(), sizeof(values)), "278 does not seek, replace the scene or overwrite camera/light authored values");
        Check(message == app.MessageSeen() && input == app.ViewportInputActive() && dirty == app.SceneModified() &&
            recorded == app.RecordedFrameCountByte1() && enhanced == app.EnhancedModelDirty() && physics == app.PhysicsResetPending() &&
            app.WindowLayoutReady() && !app.SceneMutationInProgress() && !NativeObjectUiPending(&app) && IsWindowEnabled(app.MainWindow()),
            "278 preserves all pending-input, project/recording/model/physics flags and ready host state");
        Check(cameraId == mdl::KeyPoolIdentity(app.CameraKeys()) && lightId == mdl::KeyPoolIdentity(app.LightKeys()),
            "278 preserves original camera/light pool identities");
        camera.Unchanged(app.CameraKeys(), mdl::KeyPoolCapacity(app.CameraKeys()) * sizeof(mdl::CameraKey));
        light.Unchanged(app.LightKeys(), mdl::KeyPoolCapacity(app.LightKeys()) * sizeof(mdl::LightKey));
        for (unsigned i = 0; i < models.size(); ++i) {
            models[i].Unchanged(mdl::Mdl(app.ModelSlot(i)));
            accessories[i].Unchanged(app.AccessorySlot(i), sizeof(mdl::AccessoryRecord));
            accessoryKeys[i].Unchanged(app.AccessoryKeys(i), mdl::KeyPoolCapacity(app.AccessoryKeys(i)) * sizeof(mdl::AccessoryKey));
        }
    }
};
void PermissionGateBeforeResourceCapture() {
    Fixture f; StorageStore storage; unsigned gateCalls = 0; MmsResult permission = MMS_E_BUSY;
    // A real SharedHistory gate is deliberately controlled by this host-only
    // fixture. It tests CheckNativeEdit routing/order, not Manager callbacks.
    SharedHistory permissionHistory(storage, [&] { ++gateCalls; return permission; });
    NativePoseTestBinding binding(*f.app, permissionHistory); const Snapshot before(f);
    const std::uint8_t sceneDirty = f.app->SceneModified();
    const auto modelDirty = f.app->EnhancedModelDirty();
    NativeTextureReloadPlan plan;
    failAfter = 0; const auto denied = plan.Begin(*f.app, f.Loaders()); failAfter = -1;
    Check(denied == MMS_E_BUSY && gateCalls == 1 && !f.sharedCalls && !f.cacheCalls,
        "278 permission gate refuses before State allocation, resource retention or any loader");
    before.Matches(f); f.UnchangedPosition();
    Check(!permissionHistory.UndoCount() && !permissionHistory.RedoCount() &&
        f.app->SceneModified() == sceneDirty && f.app->EnhancedModelDirty() == modelDirty,
        "278 refused Begin preserves resources, journal, dirty flags and current frame");
    MenuLoaders menu; menu.Run(f);
    Check(gateCalls == 2 && !f.sharedCalls && !f.cacheCalls && !permissionHistory.UndoCount(),
        "real menu 278 consumes the shared permission refusal without a legacy reload fallback");
    before.Matches(f); f.UnchangedPosition();
    permission = MMS_OK; menu.Run(f);
    Check(permissionHistory.UndoCount() == 1 && !permissionHistory.RedoCount() && !f.history.UndoCount() &&
        f.sharedCalls == 1 && f.cacheCalls == before.prefix.size(),
        "278 uses the same real gate's successful authorization for one bound explicit commit");
    f.UnchangedPosition();
}

#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS) && defined(_WIN64)
namespace default_toon_iat {
template<class Function> struct Slot {
    Function* address; Function original; bool installed = true;
    Slot(Function* value, Function hook) : address(value), original(*value) { Set(hook); }
    void Set(Function value) {
        DWORD prior = 0, discarded = 0;
        Check(VirtualProtect(address, sizeof(*address), PAGE_READWRITE, &prior) != FALSE, "278 makes only its own IAT slot writable");
        *address = value;
        Check(VirtualProtect(address, sizeof(*address), prior, &discarded) != FALSE, "278 restores IAT page protection after each write");
    }
    void Restore() { if (installed) { Set(original); installed = false; } }
    ~Slot() { Restore(); }
    Slot(const Slot&) = delete; Slot& operator=(const Slot&) = delete;
};
DWORD Pixel(unsigned slot) { return 0xff000000u | ((20 + 13 * slot) << 16) | ((30 + 11 * slot) << 8) | (40 + 7 * slot); }
struct Files {
    std::wstring base, directory, dataDirectory;
    std::array<std::wstring, 10> paths;
    bool ansi = false;
    Files(bool unicode, unsigned missing) {
        std::array<wchar_t, MAX_PATH> temp{}, unique{};
        const auto count = GetTempPathW(DWORD(temp.size()), temp.data());
        Check(count && count < temp.size() && GetTempFileNameW(temp.data(), L"mtr", 0, unique.data()), "278 reserves an owned temporary asset root");
        base = unique.data(); Check(DeleteFileW(base.c_str()) && CreateDirectoryW(base.c_str(), nullptr), "278 converts only its reserved temp file into a directory");
        std::wstring alias = base; std::string bytes;
        if (!unicode && !text_encoding::EncodeExact(alias.c_str(), CP_ACP, bytes)) {
            std::array<wchar_t, 32768> shortName{};
            const auto length = GetShortPathNameW(base.c_str(), shortName.data(), DWORD(shortName.size()));
            if (length && length < shortName.size()) alias.assign(shortName.data(), length);
        }
        directory = alias + (unicode ? L"\\wide-\xd83d\xde42" : L"\\ascii");
        Check(directory.size() < 256 && CreateDirectoryW(directory.c_str(), nullptr), "278 asset directory fits the real ExeDir field");
        dataDirectory = directory + L"\\data"; Check(CreateDirectoryW(dataDirectory.c_str(), nullptr), "278 creates its own data subdirectory");
        ansi = text_encoding::EncodeExact(directory.c_str(), CP_ACP, bytes);
        for (unsigned i = 1; i <= 10; ++i) {
            wchar_t name[32]{}; swprintf_s(name, L"\\toon%02u.bmp", i); paths[i - 1] = dataDirectory + name;
            if (i == missing) continue;
            static_assert(sizeof(BITMAPFILEHEADER) == 14 && sizeof(BITMAPINFOHEADER) == 40);
            std::array<unsigned char, 14 + 40 + 4 * 4 * 3> image{};
            BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfSize = DWORD(image.size()); file.bfOffBits = 54;
            BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = info.biHeight = 4;
            info.biPlanes = 1; info.biBitCount = 24; info.biCompression = BI_RGB; info.biSizeImage = 48;
            std::memcpy(image.data(), &file, sizeof(file)); std::memcpy(image.data() + sizeof(file), &info, sizeof(info));
            const auto pixel = Pixel(i);
            for (unsigned p = 54; p < image.size(); p += 3) {
                image[p] = BYTE(pixel); image[p + 1] = BYTE(pixel >> 8); image[p + 2] = BYTE(pixel >> 16);
            }
            HANDLE output = CreateFileW(paths[i - 1].c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(output != INVALID_HANDLE_VALUE, "278 creates each real BMP only inside its own asset directory");
            DWORD written = 0; const auto saved = WriteFile(output, image.data(), DWORD(image.size()), &written, nullptr);
            const auto closed = CloseHandle(output); Check(saved && closed && written == image.size(), "278 writes complete real BMP bytes");
        }
    }
    ~Files() {
        for (const auto& path : paths) if (!path.empty()) DeleteFileW(path.c_str());
        RemoveDirectoryW(dataDirectory.c_str()); RemoveDirectoryW(directory.c_str()); RemoveDirectoryW(base.c_str());
    }
    Files(const Files&) = delete; Files& operator=(const Files&) = delete;
};
struct Directory {
    MMDApp& app; std::array<wchar_t, 256> previous{}, frozen{};
    Directory(MMDApp& value, const std::wstring& path) : app(value) {
        Check(!path.empty() && path.size() < frozen.size(), "278 IAT asset path fits existing ExeDir storage");
        std::memcpy(previous.data(), app.ExeDir(), sizeof(previous));
        std::wmemcpy(frozen.data(), path.c_str(), path.size() + 1); RestoreFrozen();
    }
    void RestoreFrozen() { std::memcpy(app.ExeDir(), frozen.data(), sizeof(frozen)); }
    ~Directory() { std::memcpy(app.ExeDir(), previous.data(), sizeof(previous)); }
};
struct Probe;
thread_local Probe* current = nullptr;
#define TOON_FILE_PARAMS(Path) IDirect3DDevice9* device, Path path, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, DWORD filter, DWORD mipFilter, D3DCOLOR key, void* info, void* palette, IDirect3DTexture9** out
#define TOON_FILE_ARGS device, path, width, height, levels, usage, format, pool, filter, mipFilter, key, info, palette, out
struct Probe {
    struct Resource {
        Microsoft::WRL::ComPtr<IDirect3DTexture9> first, second;
        ULONG retainedProbe = 0; bool optionalFailure = false;
    };
    Fixture& fixture; Files& files; Directory& directory; Probe* previous = current;
    const d3dx::FnCreateTexInMemEx originalMemory = __imp_D3DXCreateTextureFromFileInMemoryEx;
    const d3dx::FnCreateTexFromFileExA originalAnsi = __imp_D3DXCreateTextureFromFileExA;
    const d3dx::FnCreateTexFromFileExW originalWide = __imp_D3DXCreateTextureFromFileExW;
    const HMODULE originalModule = d3dx::Get().module;
    const unsigned missing, fatalFile;
    const bool fatalFirstMemory, fatalFallback;
    unsigned memoryCalls = 0, fileCalls = 0, ansiCalls = 0, wideCalls = 0, firstFileCalls = 0, laterFileCalls = 0;
    std::array<Resource, 16> resources{}; size_t resourceCount = 0;
    Slot<d3dx::FnCreateTexInMemEx> memory{&__imp_D3DXCreateTextureFromFileInMemoryEx, &Memory<0>};
    Slot<d3dx::FnCreateTexFromFileExA> ansi{&__imp_D3DXCreateTextureFromFileExA, &Ansi<0>};
    Slot<d3dx::FnCreateTexFromFileExW> wide{&__imp_D3DXCreateTextureFromFileExW, &Wide<0>};
    Probe(Fixture& f, Files& paths, Directory& location, unsigned absent, unsigned failedFile, bool failedMemory, bool failedFallback)
        : fixture(f), files(paths), directory(location), missing(absent), fatalFile(failedFile),
          fatalFirstMemory(failedMemory), fatalFallback(failedFallback) { current = this; }
    ~Probe() {
        memory.Restore(); ansi.Restore(); wide.Restore();
        Check(d3dx::Get().Load() && d3dx::Get().fromMemEx == originalMemory && d3dx::Get().fromFileExA == originalAnsi &&
            d3dx::Get().fromFileExW == originalWide && d3dx::Get().module == originalModule, "278 restores every original IAT slot and shared loader view");
        current = previous;
    }
    static ULONG References(IDirect3DTexture9* texture) { const auto value = texture->AddRef(); texture->Release(); return value; }
    void Retain(IDirect3DTexture9* texture, bool optionalFailure = false) {
        Check(texture && resourceCount < resources.size(), "278 IAT evidence owns only real bounded texture witnesses");
        Microsoft::WRL::ComPtr<IDirect3DDevice9> device;
        Check(SUCCEEDED(texture->GetDevice(&device)) && device.Get() == fixture.uiDevice.Get(), "278 default D3DX created the texture on the actual device");
        auto& resource = resources[resourceCount++]; resource.first = texture; resource.second = texture;
        resource.retainedProbe = References(texture); resource.optionalFailure = optionalFailure;
    }
    void CheckOwnership(bool batchFailed) {
        for (size_t i = 0; i < resourceCount; ++i) {
            const auto& resource = resources[i];
            const auto released = batchFailed || resource.optionalFailure ? 1u : 0u;
            Check(References(resource.first.Get()) == resource.retainedProbe - released,
                "278 releases exactly the transferred failed/private reference while independent real COM witnesses stay alive");
        }
    }
    void BeforeFile(unsigned stage, bool isAnsi, const std::wstring& path, IDirect3DDevice9* device,
        UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
        DWORD filter, DWORD mipFilter, D3DCOLOR key, void* info, void* palette, IDirect3DTexture9** out) {
        ++fileCalls;
        Check(fileCalls <= files.paths.size() && path == files.paths[fileCalls - 1], "278 uses the pre-first-callback absolute path for every disk texture");
        Check(isAnsi == files.ansi, "278 selects the original A import exactly when the absolute path round-trips through ACP");
        Check(stage == (fileCalls == 1 ? 1u : 2u), "278 rereads IAT after the first memory call and after the first file call");
        if (fileCalls == 1) ++firstFileCalls; else ++laterFileCalls;
        Check(device == fixture.uiDevice.Get() && width == UINT(-1) && height == UINT(-1) && levels == 1 && !usage &&
            format == D3DFMT_A8R8G8B8 && pool == D3DPOOL_MANAGED && filter == DWORD(-1) && mipFilter == DWORD(-1) &&
            !key && info && !palette && out, "278 preserves the actual disk D3DX argument contract");
        directory.RestoreFrozen(); // The first callback's temporary ExeDir mutation ends here.
        if (fileCalls == 1) { ansi.Set(&Ansi<2>); wide.Set(&Wide<2>); }
    }
    HRESULT AfterFile(HRESULT result, IDirect3DTexture9** out) {
        if (fileCalls == missing) {
            Check(FAILED(result), "278 actually reaches the one absent disk file before embedded fallback");
            // A third-party hook may return a real owned texture even on
            // failure. The helper, rather than this wrapper, must release it.
            if (!*out) *out = MakeTexture(fixture.uiDevice.Get(), 0xffabcdef);
            Retain(*out, true); memory.Set(&Memory<2>); return result;
        }
        Check(SUCCEEDED(result) && *out, "278 default IAT path loads each existing real BMP"); Retain(*out);
        return fileCalls == fatalFile ? E_OUTOFMEMORY : result;
    }
    template<unsigned Stage> static HRESULT WINAPI Ansi(TOON_FILE_PARAMS(LPCSTR)) {
        Check(current && path, "278 ANSI callback has its live fixture"); auto& self = *current; ++self.ansiCalls;
        std::wstring decoded; Check(text_encoding::Decode(path, CP_ACP, decoded), "278 ANSI callback receives valid ACP bytes");
        self.BeforeFile(Stage, true, decoded, device, width, height, levels, usage, format, pool, filter, mipFilter, key, info, palette, out);
        return self.AfterFile(self.originalAnsi(TOON_FILE_ARGS), out);
    }
    template<unsigned Stage> static HRESULT WINAPI Wide(TOON_FILE_PARAMS(LPCWSTR)) {
        Check(current && path, "278 Unicode callback has its live fixture"); auto& self = *current; ++self.wideCalls;
        self.BeforeFile(Stage, false, path, device, width, height, levels, usage, format, pool, filter, mipFilter, key, info, palette, out);
        return self.AfterFile(self.originalWide(TOON_FILE_ARGS), out);
    }
    template<unsigned Stage> static HRESULT WINAPI Memory(IDirect3DDevice9* device, LPCVOID data, UINT bytes,
        UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
        DWORD filter, DWORD mipFilter, D3DCOLOR key, void* info, void* palette, IDirect3DTexture9** out) {
        Check(current, "278 embedded callback has its live fixture"); auto& self = *current; ++self.memoryCalls;
        const auto resourceId = self.memoryCalls == 1 ? 103u : self.missing + 103;
        const auto module = static_cast<HMODULE>(self.fixture.app->HInstance());
        const auto resource = FindResourceA(module, MAKEINTRESOURCEA(resourceId), "PNG");
        Check(resource && data == LockResource(LoadResource(module, resource)) && bytes == SizeofResource(module, resource),
            "278 memory callback consumes the actual embedded PNG bytes for that slot");
        Check((self.memoryCalls == 1 && Stage == 0) || (self.memoryCalls == 2 && self.missing && Stage == 2),
            "278 fallback rereads the memory IAT changed by the failing file callback");
        Check(device == self.fixture.uiDevice.Get() && width == UINT(-1) && height == UINT(-1) && levels == 1 && !usage &&
            format == D3DFMT_UNKNOWN && pool == D3DPOOL_MANAGED && filter == DWORD(-1) && mipFilter == DWORD(-1) &&
            !key && !info && !palette && out, "278 preserves the actual embedded D3DX argument contract");
        if (self.memoryCalls == 1) {
            self.memory.Set(&Memory<1>); self.ansi.Set(&Ansi<1>); self.wide.Set(&Wide<1>);
            if (!self.fatalFirstMemory) {
                const auto changed = self.files.directory + L"\\not-used";
                Check(changed.size() < 256, "278 temporary callback path fits real app storage");
                std::wmemcpy(self.fixture.app->ExeDir(), changed.c_str(), changed.size() + 1);
            }
        }
        const auto result = self.originalMemory(device, data, bytes, width, height, levels, usage, format, pool, filter, mipFilter, key, info, palette, out);
        Check(SUCCEEDED(result) && *out, "278 original memory D3DX loads a real embedded PNG texture"); self.Retain(*out);
        return (self.memoryCalls == 1 ? self.fatalFirstMemory : self.fatalFallback) ? E_OUTOFMEMORY : result;
    }
};
#undef TOON_FILE_ARGS
#undef TOON_FILE_PARAMS
struct Output {
    std::array<IDirect3DTexture9*, 11> textures{}; std::array<float, 30> edge{};
    explicit Output(const tr::PreparedSharedToon& value) : edge(value.edge) {
        for (unsigned i = 0; i < textures.size(); ++i) textures[i] = value.textures[i].get();
    }
    void Unchanged(const tr::PreparedSharedToon& value) const {
        for (unsigned i = 0; i < textures.size(); ++i) Check(textures[i] == value.textures[i].get(), "278 default loader failure preserves each caller-owned texture");
        Check(!std::memcmp(edge.data(), value.edge.data(), sizeof(edge)), "278 default loader failure preserves all caller-owned edge colours");
    }
};
void Run(bool unicode, unsigned missing = 0, unsigned fatalFile = 0, bool fatalMemory = false, bool fatalFallback = false) {
    Fixture f; Files files(unicode, missing);
    if (unicode && files.ansi) { std::puts("SKIP default Toon W-only branch: this ACP losslessly represents the valid Unicode asset path"); return; }
    Check(unicode || files.ansi, "278 ANSI IAT cohort has an ACP-exact asset directory (including any available short path)");
    Directory directory(*f.app, files.directory); const Snapshot source(f);
    const std::uint8_t sceneDirty = f.app->SceneModified();
    const auto modelDirty = f.app->EnhancedModelDirty();
    tr::PreparedSharedToon output;
    for (unsigned i = 0; i < output.textures.size(); ++i) output.textures[i].reset(MakeTexture(f.uiDevice.Get(), 0xff809010 + i));
    for (unsigned i = 0; i < output.edge.size(); ++i) output.edge[i] = .125f + float(i) / 128;
    const Output before(output);
    Check(d3dx::Get().Load(), "278 default loader starts with real imported D3DX functions");
    {
        Probe probe(f, files, directory, missing, fatalFile, fatalMemory, fatalFallback);
        // The helper itself is not detoured. Only the test process's genuine
        // IAT slots wrap and forward the actual D3DX file/memory operations.
        const auto result = tr::PrepareSharedToon(*f.app, output);
        const bool failed = fatalFile || fatalMemory || fatalFallback;
        Check(result == (failed ? MMS_E_MEMORY : MMS_OK), "278 default preparation reports the complete IAT cohort outcome");
        directory.RestoreFrozen(); source.Matches(f); f.UnchangedPosition();
        Check(f.app->SceneModified() == sceneDirty && f.app->EnhancedModelDirty() == modelDirty &&
            !f.history.UndoCount() && !f.history.RedoCount(), "278 default preparation publishes no scene, resource or history edit");
        const unsigned diskCount = fatalMemory ? 0 : fatalFile ? fatalFile : fatalFallback ? missing : 10;
        Check(probe.fileCalls == diskCount && probe.memoryCalls == (fatalMemory || !missing ? 1u : 2u), "278 calls slot zero, each reached disk slot and only the required embedded fallback");
        Check(probe.ansiCalls == (files.ansi ? diskCount : 0) && probe.wideCalls == (files.ansi ? 0 : diskCount),
            "278 actual A/W IAT call counts match lossless path selection");
        Check(probe.firstFileCalls == (diskCount ? 1u : 0u) && probe.laterFileCalls == (diskCount ? diskCount - 1 : 0),
            "278 callback-installed next IAT is used instead of cached function pointers");
        probe.CheckOwnership(failed);
        if (failed) before.Unchanged(output);
        else {
            const std::array<float, 30> defaults{
                .80078101f,.80078101f,.80078101f,.95703101f,.87890601f,.87890601f,.60156298f,.60156298f,.60156298f,
                .96875f,.93359399f,.91796899f,1.f,.90234399f,.86328101f,.76171899f,.671875f,.011719f,
                1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f};
            for (unsigned i = 0; i < output.textures.size(); ++i) Check(bool(output.textures[i]), "278 default helper owns all eleven real textures");
            auto expected = defaults;
            for (unsigned i = 1; i <= 10; ++i) if (i != missing) {
                const auto pixel = Pixel(i); expected[3 * (i - 1)] = BYTE(pixel >> 16) / 256.f;
                expected[3 * (i - 1) + 1] = BYTE(pixel >> 8) / 256.f; expected[3 * (i - 1) + 2] = BYTE(pixel) / 256.f;
            }
            Check(!std::memcmp(expected.data(), output.edge.data(), sizeof(expected)), "278 verifies every one of thirty sampled/default colour floats");
        }
    }
    f.UnchangedPosition();
}
}
#endif
void DefaultSharedLoaderIatChecks() {
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS) && defined(_WIN64)
    default_toon_iat::Run(false);
    default_toon_iat::Run(true);
    default_toon_iat::Run(false, 4);
    default_toon_iat::Run(false, 0, 6);
    default_toon_iat::Run(false, 0, 0, true);
    default_toon_iat::Run(false, 4, 0, false, true);
#else
    std::puts("SKIP default Toon IAT cohorts: static x64 D3DX imports are disabled");
#endif
}

#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS) && defined(_WIN64)
namespace default_cache_iat {
enum class Route { OnePixelRetry, NonfatalRetry, OnePixelThenNonfatalRetry, FatalNoRetry };
struct Probe;
thread_local Probe* current = nullptr;
struct Probe {
    Fixture& fixture; const Route route; Probe* previous = current;
    const wchar_t* const path = L"D:\\texture-reload\\default-wide-iat.bmp";
    const d3dx::FnCreateTexInMemEx originalMemory = __imp_D3DXCreateTextureFromFileInMemoryEx;
    const d3dx::FnCreateTexFromFileExA originalAnsi = __imp_D3DXCreateTextureFromFileExA;
    const d3dx::FnCreateTexFromFileExW originalWide = __imp_D3DXCreateTextureFromFileExW;
    const HMODULE originalModule = d3dx::Get().module;
    unsigned calls = 0;
    std::array<DWORD, 3> usages{};
    std::array<TextureWitness, 3> resources;
    default_toon_iat::Slot<d3dx::FnCreateTexFromFileExW> wide{&__imp_D3DXCreateTextureFromFileExW, &Wide<0>};
    Probe(Fixture& f, Route value) : fixture(f), route(value) { current = this; }
    ~Probe() {
        wide.Restore();
        Check(d3dx::Get().Load() && d3dx::Get().fromMemEx == originalMemory && d3dx::Get().fromFileExA == originalAnsi &&
            d3dx::Get().fromFileExW == originalWide && d3dx::Get().module == originalModule,
            "278 cache retry restores the actual W IAT slot, protection and shared API view");
        current = previous;
    }
    unsigned ExpectedCalls() const {
        return route == Route::FatalNoRetry ? 1u : route == Route::OnePixelThenNonfatalRetry ? 3u : 2u;
    }
    template<unsigned Stage> static HRESULT WINAPI Wide(IDirect3DDevice9* device, LPCWSTR path,
        UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
        DWORD filter, DWORD mipFilter, D3DCOLOR key, void* info, void* palette, IDirect3DTexture9** out) {
        Check(current, "278 cache W IAT callback retains its live isolated fixture"); auto& s = *current;
        const unsigned call = s.calls++;
        // Only the true __imp slot changes. No test writes api.fromFileExW;
        // LoadDefault itself must reread that slot before each real retry.
        Check(call == Stage && call < s.ExpectedCalls(), "278 each cache retry reaches the newly installed W IAT function");
        Check(device == s.fixture.uiDevice.Get() && path && !std::wcscmp(path, s.path) && width == UINT(-1) && height == UINT(-1) &&
            levels == 1 && usage == (call ? 0u : 1024u) && format == D3DFMT_A8R8G8B8 && pool == D3DPOOL_MANAGED &&
            filter == DWORD(-1) && mipFilter == DWORD(-1) && !key && info && !palette && out && !*out,
            "278 real default cache loader preserves exact 1024-to-zero usage, path, format and output arguments");
        s.usages[call] = usage;
        if (call) s.resources[call - 1].TransferredReferenceReleased();
        const bool onePixel = call == 0 && (s.route == Route::OnePixelRetry || s.route == Route::OnePixelThenNonfatalRetry);
        *out = MakeTexture(device, cachePixel, onePixel ? 1 : 4);
        D3DSURFACE_DESC description{};
        Check(SUCCEEDED((*out)->GetLevelDesc(0, &description)) && description.Width == (onePixel ? 1u : 4u) &&
            description.Height == description.Width, "278 IAT retry owns an actual one-pixel or full-size D3D9 texture");
        auto* image = static_cast<UINT*>(info); image[0] = description.Width; image[1] = description.Height;
        s.resources[call].Capture(*out, device);
        if constexpr (Stage < 2) s.wide.Set(&Wide<Stage + 1>);
        if (s.route == Route::FatalNoRetry) return E_OUTOFMEMORY;
        if ((s.route == Route::NonfatalRetry && call == 0) ||
            (s.route == Route::OnePixelThenNonfatalRetry && call == 1))
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); // intentionally owns nonnull *out
        return S_OK;
    }
    void Ownership() const {
        Check(calls == ExpectedCalls() && usages[0] == 1024 &&
            (calls < 2 || usages[1] == 0) && (calls < 3 || usages[2] == 0),
            "278 default loader performs exactly the existing retry sequence without an extra fatal retry");
        for (unsigned i = 0; i < calls; ++i) {
            if (route == Route::FatalNoRetry || i + 1 < calls) resources[i].TransferredReferenceReleased();
            else resources[i].StillOwnedByOutput();
        }
    }
};
void Run(Route route) {
    Fixture f; const Snapshot source(f); const AuthoredSnapshot authored(*f.app);
    tr::PreparedCacheTexture output; output.texture.reset(MakeTexture(f.uiDevice.Get(), 0xff203040)); output.tag = std::int32_t(0xe1d2c3b4u);
    TextureWitness originalOutput; originalOutput.Capture(output.texture.get(), f.uiDevice.Get());
    const auto* previous = output.texture.get(); const auto tag = output.tag;
    Check(d3dx::Get().Load(), "278 cache retry starts from the actual imported D3DX table");
    {
        Probe probe(f, route);
        // PrepareCacheTexture and LoadDefault are not detoured. The null
        // injected loader selects the real default W/IAT path; only that
        // import's controlled callbacks supply HRESULTs and real COM objects.
        const auto result = tr::PrepareCacheTexture(f.uiDevice.Get(), probe.path, nullptr, output);
        const bool fatal = route == Route::FatalNoRetry;
        Check(result == (fatal ? MMS_E_MEMORY : MMS_OK), "278 real default cache helper reports the complete retry outcome");
        probe.Ownership();
        if (fatal) {
            Check(output.texture.get() == previous && output.tag == tag, "278 fatal default retry preserves caller-owned output and all four tag bytes");
            originalOutput.StillOwnedByOutput();
        } else {
            Check(output.texture.get() == probe.resources[probe.calls - 1].first.Get() && output.texture.get() != previous &&
                output.tag == cacheTag, "278 successful default retry returns the last actual texture and fully sampled RGB/alpha tag");
            originalOutput.TransferredReferenceReleased();
        }
        source.Matches(f); authored.Unchanged(); f.UnchangedPosition();
        Check(!f.sharedCalls && !f.cacheCalls && !f.history.UndoCount() && !f.history.RedoCount(),
            "278 IAT retry cohort uses no injected helper loader and publishes no scene/history resources");
    }
}
}
#endif
void DefaultCacheLoaderIatChecks() {
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS) && defined(_WIN64)
    for (auto route : {default_cache_iat::Route::OnePixelRetry, default_cache_iat::Route::NonfatalRetry,
        default_cache_iat::Route::OnePixelThenNonfatalRetry, default_cache_iat::Route::FatalNoRetry})
        default_cache_iat::Run(route);
#else
    std::puts("SKIP default cache W IAT retry cohorts: static x64 D3DX imports are disabled");
#endif
}
void ActualMenuAndOfflineRoundTrip() {
    for (auto mode : {SharedHistory::RecordingMode::AllOperations, SharedHistory::RecordingMode::ExplicitOnly})
    for (unsigned pendingInput : {0u, 1u}) {
        Fixture f; NativePoseTestBinding binding(*f.app, f.history); MenuLoaders menu;
        EffectsObserver effects(*f.app, f.history);
        Check(f.history.SetMode(mode) == MMS_OK, "278 accepts both native recording modes");
        f.app->state.sceneModified = 0; f.app->RecordedFrameCountByte1() = 0;
        f.app->EnhancedModelDirty() = 0; f.app->PhysicsResetPending() = 0; f.app->MessageSeen() = pendingInput;
        const Snapshot before(f); const AuthoredSnapshot authored(*f.app);
        const std::uint16_t notification = mode == SharedHistory::RecordingMode::ExplicitOnly ? 1 : 0;
        std::printf("RELOAD_ENTRY mode=%u notification=%u original_message_seen=%u\n", unsigned(mode), unsigned(notification), pendingInput);
        effects.Reset(); menu.Run(f, notification); const Snapshot after(f);
        Check(f.history.UndoCount() == 1 && !f.history.RedoCount() && f.sharedCalls == 1 && f.cacheCalls == before.prefix.size(),
            "real MainWndProc/CommandDispatch 278 records exactly one explicit commit in either mode");
        for (unsigned i = 0; i < 11; ++i) Check(after.shared[i] != before.shared[i], "278 replaces every shared Toon slot together");
        for (unsigned i = 0; i < after.edge.size(); ++i)
            Check(after.edge[i] == .55f + .005f * i, "278 actual menu publishes all thirty prepared shared Toon colour floats");
        for (size_t i = 0; i < after.prefix.size(); ++i)
            Check(after.prefix[i].texture != before.prefix[i].texture && after.prefix[i].name == before.prefix[i].name && after.prefix[i].tag == cacheTag,
                "278 replaces each used cache texture and complete sampled RGBA/alpha tag without replacing names");
        effects.ViewportOnly("actual-278-commit", 1, 0); authored.Unchanged();
        const auto tail = f.AppendReal(L"D:\\texture-reload\\later-import.bmp", 0xffabcdef, 0x13572468);
        const auto later = At(f, tail); const auto cacheCalls = f.cacheCalls, sharedCalls = f.sharedCalls; f.offline = true;
        effects.Reset(); Check(RunBoundObjectHistory(*f.app, f.history, false) == MMS_OK, "278 undo uses the original native history route");
        before.Matches(f); Check(Same(At(f, tail), later), "278 undo preserves a permanently appended cache tail");
        effects.ViewportOnly("actual-278-undo", 0, 1); authored.Unchanged();
        effects.Reset(); Check(RunBoundObjectHistory(*f.app, f.history, true) == MMS_OK, "278 redo uses the original native history route");
        after.Matches(f); Check(Same(At(f, tail), later), "278 redo preserves the later cache tail too");
        effects.ViewportOnly("actual-278-redo", 1, 0); authored.Unchanged();
        Check(f.cacheCalls == cacheCalls && f.sharedCalls == sharedCalls && f.history.UndoCount() == 1,
            "278 undo and redo retain COM resources without loading either original source again"); f.UnchangedPosition();
    }
}
void RawOutputThrowCleanup() {
    static_assert(std::is_same_v<itc::TextureLoader, decltype(&CacheLoader)>,
        "the controlled throwing loader has the supported non-noexcept TextureLoader signature");
    for (bool wholePlan : {false, true}) {
        Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot source(f);
        const AuthoredSnapshot authored(*f.app);
        tr::PreparedCacheTexture output; output.texture.reset(MakeTexture(f.uiDevice.Get(), 0xff204060)); output.tag = std::int32_t(0xa1b2c3d4u);
        const auto* previous = output.texture.get(); const auto tag = output.tag;
        f.throwCacheAt = wholePlan ? 2 : 1;
        if (!wholePlan) {
            Check(tr::PrepareCacheTexture(f.uiDevice.Get(), L"D:\\texture-reload\\throw.bmp", CacheLoader, output) == MMS_E_FAILED,
                "278 catches a supported raw-output TextureLoader exception after ownership transfer");
        } else {
            NativeTextureReloadPlan plan; std::shared_ptr<HistoryChange> change;
            Check(plan.Begin(*f.app, f.Loaders()) == MMS_OK && plan.Take(change) == MMS_E_FAILED && !change,
                "278 later-cache exception rejects the complete private eleven-Toon/cache batch");
        }
        Check(output.texture.get() == previous && output.tag == tag && f.cacheCalls == f.throwCacheAt &&
            f.sharedCalls == unsigned(wholePlan) && !f.history.UndoCount() && !f.history.RedoCount(),
            "278 raw-output throw preserves caller output, stops further loaders and publishes no history");
        f.thrownTexture.TransferredReferenceReleased(); source.Matches(f); authored.Unchanged(); f.UnchangedPosition();
    }
}
void PluginAggregateUsesConservativeEffects() {
    Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot before(f);
    auto change = f.Change(); Check(change->Effects() == HistoryEffects::Viewport,
        "278 standalone native participant declares only renderer effects before aggregation");
    camera_multiply_checks::PluginState plugin; StorageClient storage; storage.id = "test.texture-reload-aggregate";
    f.storage.Api(storage); auto serializer = plugin.Serializer(); MmsHandle provider = 0;
    Check(f.storage.Register(storage, Text(storage.id), &serializer, sizeof(serializer), &provider) == MMS_OK,
        "278 aggregate fixture registers the real plugin serializer in the common storage store");
    EditStore edits(f.history, f.storage); EditClient client; client.id = storage.id; auto api = edits.Api(client);
    uint64_t revision = 0; MmsHandle transaction = 0; auto participant = plugin.Participant(Text(client.id));
    Check(f.history.Revision(revision) == MMS_OK && api.begin(api.context, Text("texture reload plus plugin"), revision, &transaction) == MMS_OK &&
        api.join(api.context, transaction, &participant) == MMS_OK && edits.JoinNative(client, transaction, change) == MMS_OK &&
        api.prepare(api.context, transaction) == MMS_OK && api.commit(api.context, transaction) == MMS_OK,
        "278 resources and a real plugin participant commit in one actual EditStore.JoinNative transaction");
    const Snapshot after(f);
    Check(plugin.value == 42 && f.history.UndoCount() == 1 && !f.history.RedoCount() &&
        f.history.LastPublicationEffects() == HistoryEffects::All && f.history.LastPublicationRequiresPhysicsReset(),
        "278 mixed plugin node freezes conservative All/physics despite its native participant's Viewport mask");
    for (unsigned i = 0; i < before.shared.size(); ++i) Check(before.shared[i] != after.shared[i], "278 aggregate actually exchanges every shared resource");
    for (size_t i = 0; i < before.prefix.size(); ++i)
        Check(after.prefix[i].texture != before.prefix[i].texture && after.prefix[i].name == before.prefix[i].name && after.prefix[i].tag == cacheTag,
            "278 aggregate really commits the cache identities and complete sampled tags together with plugin data");
    const auto cacheCalls = f.cacheCalls, sharedCalls = f.sharedCalls; f.offline = true;
    Check(RunBoundObjectHistory(*f.app, f.history, false) == MMS_OK && !plugin.value &&
        !f.history.UndoCount() && f.history.RedoCount() == 1 && f.history.LastPublicationEffects() == HistoryEffects::All &&
        f.history.LastPublicationRequiresPhysicsReset(), "278 mixed-node undo succeeds through actual full Operation/publication authorization");
    before.Matches(f);
    Check(RunBoundObjectHistory(*f.app, f.history, true) == MMS_OK && plugin.value == 42 &&
        f.history.UndoCount() == 1 && !f.history.RedoCount() && f.history.LastPublicationEffects() == HistoryEffects::All &&
        f.history.LastPublicationRequiresPhysicsReset(), "278 mixed-node redo restores plugin and resources through full native history replay");
    after.Matches(f);
    Check(f.cacheCalls == cacheCalls && f.sharedCalls == sharedCalls && !NativeObjectOperationActive(f.app.get()) &&
        !f.app->SceneMutationInProgress() && IsWindowEnabled(f.app->MainWindow()),
        "278 aggregate replay rereads no files and releases the full operation guard");
    // All legitimately performs scene/readout/UI work. This cohort deliberately
    // makes none of the renderer-only spy or unchanged-author-data assertions.
    change.reset(); Check(f.history.Reset() == MMS_OK, "278 retires mixed plugin history before its serializer leaves scope"); f.history.Collect();
    Check(f.storage.Unregister(storage, provider) == MMS_OK, "278 releases the serializer only after all mixed history references retire");
}
void PrivateCancellationFailureAndBudget() {
    Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot before(f);
    const std::uint8_t sceneDirty = f.app->SceneModified();
    const auto modelDirty = f.app->EnhancedModelDirty();
    const auto unchanged = [&] {
        before.Matches(f); Check(!f.history.UndoCount() && !f.history.RedoCount() && f.app->SceneModified() == sceneDirty &&
            f.app->EnhancedModelDirty() == modelDirty, "278 rejection/cancel has no partial resource, history or dirty publication"); f.UnchangedPosition();
    };
    { NativeTextureReloadPlan plan; Check(plan.Begin(*f.app, f.Loaders()) == MMS_OK, "278 cancellation captures before"); }
    unchanged();
    { auto change = f.Change(); std::unique_ptr<SharedHistory::NativeTransaction> token;
      Check(f.history.PrepareNative("cancel 278", change, token) == MMS_OK, "278 cancellation owns a fully prepared history reservation"); token.reset(); }
    unchanged();
    for (unsigned kind = 0; kind < 2; ++kind) {
        f.failSharedAt = kind == 0 ? 6 : 0; f.failCacheAt = kind == 1 ? f.cacheCalls + 2 : 0;
        NativeTextureReloadPlan plan; std::shared_ptr<HistoryChange> out;
        Check(plan.Begin(*f.app, f.Loaders()) == MMS_OK && plan.Take(out) == MMS_E_MEMORY && !out,
            "278 rejects partial eleven-Toon or later cache preparation atomically"); unchanged();
    }
    f.failSharedAt = f.failCacheAt = 0;
    auto change = f.Change(); std::unique_ptr<PreparedHistoryChange> candidate;
    Check(change->Prepare(true, candidate) == MMS_OK && candidate, "278 measures the complete private resource candidate");
    std::uint64_t bytes = 0;
    Check(candidate->TargetRetainedBytes(bytes) && bytes >= 26 * 4 * 4 * sizeof(DWORD),
        "278 budgets both directions of all real shared and prefix cache textures"); candidate.reset();
    Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly, 128, 1) == MMS_OK, "278 sets an insufficient journal budget");
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("over-budget 278", change, token) == MMS_E_MEMORY && !token,
        "278 refuses the complete resource budget before any original pointer is released"); unchanged(); change.reset();
    Check(f.history.Configure(SharedHistory::RecordingMode::AllOperations, 128, 256ull * 1024 * 1024) == MMS_OK, "278 restores resource budget");
    unsigned failures = 0; bool ready = false;
    for (long limit = 0; limit < 512 && !ready; ++limit) {
        NativeTextureReloadPlan plan; std::shared_ptr<HistoryChange> next;
        failAfter = limit;
        const auto result = Boundary([&]() -> MmsResult {
            auto status = plan.Begin(*f.app, f.Loaders()); if (!status) status = plan.Take(next);
            return status ? status : f.history.PrepareNative("278 allocation sweep", next, token);
        }); failAfter = -1; token.reset(); unchanged();
        if (!result) ready = true; else { ++failures; Check(result == MMS_E_MEMORY, "278 allocation fault reports memory without publishing"); }
    }
    Check(ready && failures > 4, "278 allocation sweep reaches capture, load, retained resources and history reservation");
}
void UnboundAndWrongDevice() {
    {
        Fixture f; MenuLoaders menu; const Snapshot before(f); menu.Run(f); const Snapshot after(f);
        Check(!f.history.UndoCount() && f.sharedCalls == 1 && f.cacheCalls == 2 && after.shared[0] != before.shared[0],
            "unbound full configuration still uses private atomic 278 preparation/publication");
        f.offline = true; menu.Run(f); after.Matches(f);
        Check(!f.history.UndoCount(), "unbound failure is handled without destructive legacy fallthrough"); f.UnchangedPosition();
    }
    for (bool shared : {false, true}) {
        Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot before(f); f.ForeignDevice();
        f.wrongSharedDevice = shared; f.wrongCacheDevice = !shared;
        NativeTextureReloadPlan plan; std::shared_ptr<HistoryChange> out;
        Check(plan.Begin(*f.app, f.Loaders()) == MMS_OK && plan.Take(out) != MMS_OK && !out,
            "278 rejects actual textures originating from another real D3D9 device"); before.Matches(f);
        Check(!f.history.UndoCount(), "wrong device never creates a history node");
    }
}
void UsedPrefixAndRetainedStaleness() {
    {
    Fixture f; NativePoseTestBinding binding(*f.app, f.history);
    // A named entry beyond the first hole is outside legacy's used prefix.
    // It is owned and freed by the fixture, but reload must never load/write it.
    auto& hidden = f.textures.renderer->resourcePool[3]; const wchar_t name[] = L"D:\\texture-reload\\hidden.bmp";
    hidden.heapBuffer = std::malloc(sizeof(name)); Check(hidden.heapBuffer, "278 hidden tail name allocation");
    std::memcpy(hidden.heapBuffer, name, sizeof(name)); hidden.comObject = MakeTexture(f.uiDevice.Get(), 0xff010203); hidden.tag = 0x27581946;
    const auto hiddenBefore = At(f, 3); auto change = f.Change(); Commit(f, change);
    Check(f.cacheCalls == 2 && Same(At(f, 3), hiddenBefore), "278 reads only used prefix and leaves a hidden tail untouched");
    Check(f.history.History(false) == MMS_OK && Same(At(f, 3), hiddenBefore), "278 undo preserves hidden tail ownership");
    Check(f.history.History(true) == MMS_OK && Same(At(f, 3), hiddenBefore), "278 redo preserves hidden tail ownership");
    Check(f.history.Reset() == MMS_OK, "278 releases old records before isolated retained cache check"); f.history.Collect(); change.reset();
    }
    // Import preparation correctly rejects holes with hidden trailing entries.
    // Its independent retained-identity check needs a fresh contiguous cache;
    // the deliberately malformed cache above belongs only to the reload test.
    Fixture f; NativePoseTestBinding binding(*f.app, f.history);
    itc::Preparation textures(*f.textures.renderer, CacheLoader);
    Check(textures.LoadRequired(L"D:\\texture-reload\\cache-0.bmp") == MMS_OK, "261/import retained check captures a real existing resource");
    std::shared_ptr<itc::Retained> retained; std::unique_ptr<itc::Publication> publication;
    Check(textures.Finish(retained, publication) == MMS_OK && publication->IsCurrent(), "freeze real retained dependency"); publication->Publish();
    Check(retained->IsCurrent(), "published retained resource identity starts current");
    auto& prefix = f.textures.renderer->resourcePool[0]; auto* old = prefix.comObject;
    prefix.comObject = MakeTexture(f.uiDevice.Get(), 0xff998877); old->Release(); InvalidateTextureLookup();
    Check(!retained->IsCurrent(), "external replacement still invalidates old import/261 retained identity; 278 must not weaken this guard");
}
void ToonReloadImportDeleteChain() {
    Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot before(f); const auto names = f.Bytes();
    { model_toon_checks::ModalProbe toon(f); toon.Run(); Check(toon.calls == 1 && f.history.UndoCount() == 1,
        "chain begins with actual native 261 explicit confirmation"); }
    Check(f.Resolved() == before.shared[7], "261 resolves original shared Toon before 278"); const auto edited = f.Bytes();
    { MenuLoaders menu; menu.Run(f); } const Snapshot reloaded(f);
    Check(f.history.UndoCount() == 2 && f.Resolved() == reloaded.shared[7], "actual 278 follows 261 in the same journal");
    // This existing fixture supplies files and the private mesh parser only.
    // The parser and every texture use f's real device/renderer, never its
    // separate opaque-device app. Geometry-free object ownership is the same
    // contract already used by PrivateAccessoryJournalChecks.
    AccessoryImportFixture files; files.context.device = f.uiDevice.Get(); files.context.texture = "later-import.bmp"; files.Vac();
    itc::Preparation textures(*f.textures.renderer, CacheLoader); ai::Prepared prepared;
    Check(ai::Prepare(*f.app, files.vac.c_str(), textures, prepared) == ai::Result::Ready,
        "chain privately parses a later real accessory import with a prepared texture tail");
    static_cast<IUnknown*>(prepared.Object()->mesh)->Release(); prepared.Object()->mesh = nullptr;
    // Match the native import command's post-parse setup: two existing
    // accessories already occupy orders 0 and 1. The parser's default order
    // is not an admissible third object in the native deletion command.
    prepared.Object()->order = 2;
    auto* object = prepared.Object(); auto* keys = prepared.Keys();
    std::shared_ptr<itc::Retained> retained; std::unique_ptr<itc::Publication> publication;
    Check(textures.Finish(retained, publication) == MMS_OK, "chain freezes the original import texture publication");
    auto import = RetainedSceneObject::ImportAccessory(*f.app, 2, prepared, retained, std::move(publication));
    Commit(f, import); const auto identity = SceneObjectIdentity(object); const auto tail = At(f, 2);
    Check(f.app->AccessorySlot(2) == object && f.app->AccessoryKeys(2) == keys && f.history.UndoCount() == 3 && retained->IsCurrent(),
        "later import is one original scene-object participant, with a permanently appended cache entry");
    { ObjectModalProbe confirm(f, true); f.Command(true); Check(confirm.seen == 1 && !f.app->ModelSlot(0) && f.history.UndoCount() == 4,
        "chain ends with actual model deletion from the shared history"); }
    const auto cacheCalls = f.cacheCalls, sharedCalls = f.sharedCalls, meshCalls = files.context.calls;
    DeleteFileW(files.vac.c_str()); DeleteFileW(files.mesh.c_str()); f.offline = true;
    Check(f.history.History(false) == MMS_OK && f.Resolved() == reloaded.shared[7], "undo deletion restores model against reloaded textures");
    Check(f.history.History(false) == MMS_OK && !f.app->AccessorySlot(2), "undo import removes the retained object without deleting its permanent cache tail");
    Check(f.history.History(false) == MMS_OK, "undo 278 after undoing later import"); before.Matches(f);
    Check(Same(At(f, 2), tail), "undo 278 never removes a later import's permanently appended texture");
    Check(f.history.History(false) == MMS_OK && f.Bytes() == names && f.Resolved() == before.shared[1], "undo 261 restores original names and exact pre-reload resource");
    Check(f.history.History(true) == MMS_OK && f.Bytes() == edited && f.Resolved() == before.shared[7], "redo 261 uses retained original shared Toon identity");
    Check(f.history.History(true) == MMS_OK, "redo 278 restores all prepared resources"); reloaded.Matches(f);
    Check(f.history.History(true) == MMS_OK && f.app->AccessorySlot(2) == object && f.app->AccessoryKeys(2) == keys &&
        SceneObjectIdentity(object).id.lo == identity.id.lo && SceneObjectIdentity(object).id.hi == identity.id.hi && retained->IsCurrent(),
        "redo later import restores original object/key/texture identities without reading its source");
    Check(f.history.History(true) == MMS_OK && !f.app->ModelSlot(0), "redo deletion remains ordered after both texture operations and import");
    Check(f.cacheCalls == cacheCalls && f.sharedCalls == sharedCalls && files.context.calls == meshCalls,
        "the complete 261/278/import/delete roundtrip performs no file or texture reload");
    Check(f.history.History(false) == MMS_OK && f.history.History(false) == MMS_OK, "restore base model and park imported accessory before fixture teardown");
    import.reset(); retained.reset(); Check(f.history.Reset() == MMS_OK, "release complete chain before parser fixture lifetime ends"); f.history.Collect();
    mdl::KeyPoolDestroy(f.app->AccessoryKeys(2)); f.app->AccessoryKeys(2) = nullptr; files.Empty(); f.UnchangedPosition();
}
} // namespace texture_reload_checks
void NativeTextureReloadChecks() {
    std::puts("RUN default shared Toon real file/memory IAT"); texture_reload_checks::DefaultSharedLoaderIatChecks();
    std::puts("RUN default cache real W IAT retry/cleanup"); texture_reload_checks::DefaultCacheLoaderIatChecks();
    std::puts("RUN native texture reload shared permission gate"); texture_reload_checks::PermissionGateBeforeResourceCapture();
    std::puts("RUN native texture reload actual 278/offline/tail"); texture_reload_checks::ActualMenuAndOfflineRoundTrip();
    std::puts("RUN native texture reload raw out throw cleanup"); texture_reload_checks::RawOutputThrowCleanup();
    std::puts("RUN native texture reload mixed plugin conservative effects"); texture_reload_checks::PluginAggregateUsesConservativeEffects();
    std::puts("RUN native texture reload cancellation/budget/allocation"); texture_reload_checks::PrivateCancellationFailureAndBudget();
    std::puts("RUN native texture reload unbound/device validation"); texture_reload_checks::UnboundAndWrongDevice();
    std::puts("RUN native texture reload prefix and retained identity"); texture_reload_checks::UsedPrefixAndRetainedStaleness();
    std::puts("RUN native 261/278/import/delete chain"); texture_reload_checks::ToonReloadImportDeleteChain();
}
#endif
