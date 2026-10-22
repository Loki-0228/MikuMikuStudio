// Actual hidden host, original C ABI DLL and one production journal.
// The renderer-version fixture verifies synchronous history/UI effects;
// it does not establish visible SafePoint, GPU or texture-reload behavior.
// Only fixture-owned windows are addressed; no global input is generated.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "../src/plugins/history_runtime.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include "../src/plugins/native_object_commands.hpp"
#include "../src/plugins/native_history_commands.hpp"
#include "../src/plugins/native_global_commands.hpp"
#include "../src/plugins/native_morph_commands.hpp"
#include "../src/plugins/native_audio_history.hpp"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool OwnTopLevel(HWND window) {
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId() && GetParent(window) == nullptr;
}
decltype(&ShowWindow) realShow = nullptr;
decltype(&SetWindowPos) realPosition = nullptr;
BOOL WINAPI Hidden(HWND window, int command) {
    return realShow(window, OwnTopLevel(window) ? SW_HIDE : command);
}
BOOL WINAPI HiddenPosition(HWND window, HWND after, int x, int y, int width, int height, UINT flags) {
    if (OwnTopLevel(window)) flags = (flags & ~SWP_SHOWWINDOW) | SWP_NOACTIVATE;
    return realPosition(window, after, x, y, width, height, flags);
}
int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::fprintf(stderr, "FAIL unexpected fixture dialog: %s\n", text); std::exit(2);
}
int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fwprintf(stderr, L"FAIL unexpected fixture dialog: %ls\n", text); std::exit(2);
}
void Hook(void* target, void* replacement, void** original = nullptr) {
    Require(MH_CreateHook(target, replacement, original) == MH_OK, "create actual host observation hook");
    Require(MH_EnableHook(target) == MH_OK, "enable actual host observation hook");
}

fs::path ExecutableDirectory() {
    wchar_t path[32768]{}; const auto length = GetModuleFileNameW(nullptr, path, 32768);
    Require(length && length < 32768, "read dedicated fixture executable path");
    const fs::path executable(path);
    Require(executable.stem() == L"history_effects_host_tests", "only the dedicated effects host owns this package setup");
    const auto directory = executable.parent_path();
    // runner 必须先复制 EXE 及依赖到全新 scratch，不能直接在构建/稳定版目录运行。
    const auto name = directory.filename().wstring();
    Require(name.rfind(L"mms-history-effects-host-", 0) == 0,
        "copy EXE into a fresh mms-history-effects-host-* scratch directory before running");
    return directory;
}
void Package(const fs::path& source) {
    Require(source.is_absolute() && fs::is_regular_file(source), "manager_edit_probe DLL is an existing absolute argument");
    const auto directory = ExecutableDirectory();
    const auto plugins = directory / L"PLUGINS";
    Require(!fs::exists(plugins), "reject existing PLUGINS before creating even the scratch owner marker");
    const auto claim = directory / L"history-effects-owner.lock";
    HANDLE lock = CreateFileW(claim.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "refuse a reused fixture scratch directory"); CloseHandle(lock);
    Require(fs::create_directory(plugins), "PLUGINS must not exist; never reuse, remove or repair user package trees");
    const auto package = plugins / L"history-effects-probe";
    Require(fs::create_directory(package), "create exclusively owned probe package");
    Require(fs::copy_file(source, package / L"manager_edit_probe.dll", fs::copy_options::none),
        "copy original probe DLL without overwriting any file");
    std::ofstream manifest(package / L"plugin.json", std::ios::binary | std::ios::out);
    Require(bool(manifest), "create own original probe manifest");
    // 与 ManagerEditCase::Package 一样不要求 mms.edit；ordinary 应启动并证明 gate 关闭。
    manifest << R"({"schemaVersion":1,"id":"org.mms.test.historyeffects","name":"History effects host fixture","version":"1.0.0","kind":"native","abi":{"major":1,"minMinor":0},"entry":"manager_edit_probe.dll","architecture":"x64","minWindows":"6.1.7601","capabilities":["ui.command"],"requiresServices":[{"id":"mms.ui","major":1,"minMinor":0}],"dependencies":[],"nativeDependencies":[],"reloadPolicy":"restart"})";
    manifest.close(); Require(bool(manifest), "finish own probe manifest");
    Require(SetCurrentDirectoryW(directory.c_str()) != FALSE, "keep legacy relative-path I/O within the owned scratch directory");
}
void PumpOwn() {
    MSG message{}; unsigned dispatched = 0;
    while (dispatched++ < 512 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Require(message.message != WM_QUIT, "own hidden host stays alive");
        if (!message.hwnd) continue;
        DWORD process = 0; GetWindowThreadProcessId(message.hwnd, &process);
        Require(process == GetCurrentProcessId(), "dispatch only the fixture process's HWNDs");
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
void SettleInitial(MMDApp& app) {
    for (unsigned n = 0; n < 4; ++n) { PumpOwn(); plugins::Refresh(&app); }
    PumpOwn();
}
void ClearPublicationScalars(MMDApp& app) {
    // UI 分类控制。直接清 fixture 标量避免 ModifiedFlag 自身增加 SceneRevision。
    // 不调用保存器，不声称保存成功，也不更改 journal。
    app.state.sceneModified = 0; app.RecordedFrameCountByte1() = 0; app.PhysicsResetPending() = 0;
}

#if !MMD_ORIGINAL_PROFILE
using plugins::HistoryEffects;
struct Probe {
    int (*value)() = nullptr; uint64_t (*flags)() = nullptr;
    void Resolve() {
        const auto module = GetModuleHandleW(L"manager_edit_probe.dll");
        Require(module != nullptr, "actual plugin runtime loaded original manager_edit_probe DLL");
        value = reinterpret_cast<decltype(value)>(GetProcAddress(module, "GetProbeValue"));
        flags = reinterpret_cast<decltype(flags)>(GetProcAddress(module, "GetProbeFlags"));
        Require(value && flags, "resolve only original probe exports");
    }
};
unsigned FindMenu(HMENU menu, const wchar_t* label) {
    unsigned result = 0;
    for (int i = 0; menu && i < GetMenuItemCount(menu); ++i) {
        wchar_t text[512]{}; GetMenuStringW(menu, UINT(i), text, 512, MF_BYPOSITION);
        if (!std::wcscmp(text, label)) {
            const auto id = GetMenuItemID(menu, i);
            Require(id && id != UINT(-1) && !result, "exactly one ordinary probe menu item matches"); result = id;
        }
        if (const auto child = GetSubMenu(menu, i)) {
            const auto nested = FindMenu(child, label);
            if (nested) { Require(!result, "probe labels have no duplicate registrations"); result = nested; }
        }
    }
    return result;
}
struct Menus {
    HWND window = nullptr;
    unsigned once = 0, undo = 0, redo = 0;
    void Capture(MMDApp& app) {
        window = app.MainWindow(); DWORD process = 0;
        const auto thread = GetWindowThreadProcessId(window, &process);
        Require(IsWindow(window) && process == GetCurrentProcessId() && thread == GetCurrentThreadId(),
            "cache only the fixture process and UI thread's actual main window");
        const auto menu = GetMenu(window); Require(menu != nullptr, "actual main menu exists");
        once = FindMenu(menu, L"Edit probe once"); undo = FindMenu(menu, L"Edit probe undo"); redo = FindMenu(menu, L"Edit probe redo");
        Require(once && undo && redo && once != undo && once != redo && undo != redo, "cache original DLL once/undo/redo menu IDs");
        // NativeUi 从 0xa000 分配，0xc000 前停止；ObserveMessage 对这个实际
        // 范围的插件 WM_COMMAND 不设置传统 MessageSeen 编辑求值标记。
        for (const auto id : {once, undo, redo})
            Require(id >= 0xa000 && id <= 0xbfff, "original plugin menu IDs lie in the actual ObserveMessage exclusion range");
    }
    void Invoke(unsigned id) const {
        DWORD process = 0; const auto thread = GetWindowThreadProcessId(window, &process);
        Require(IsWindow(window) && process == GetCurrentProcessId() && thread == GetCurrentThreadId() &&
            (id == once || id == undo || id == redo) && id >= 0xa000 && id <= 0xbfff,
            "send only cached plugin commands to the fixture's own living UI-thread HWND");
        // 真实 MainWndProc/CommandDispatch 路径，无 WM_INITMENU、主动 Refresh
        // 或 Pump。SendMessage 的 LRESULT 不是插件 MmsResult，故不据此判断
        // 成功；后续仍严格检查原 DLL value/flags 与 journal/renderer 状态。
        (void)SendMessageW(window, WM_COMMAND, WPARAM(id), 0);
    }
};

struct Counts {
    unsigned post = 0, historyUi = 0, frame = 0, selection = 0, viewport = 0, geometry = 0;
    unsigned global = 0, morph = 0, audio = 0, curve = 0, forwardedHistoryUi = 0, failedHistoryUi = 0;
    unsigned actualHistoryUiFailures = 0, historyUiFailures = 0;
    unsigned rendererShutdown = 0, wholeInvalidation = 0, topEnable = 0;
    std::array<HistoryEffects, 32> masks{};
    bool maskOverflow = false;
};
struct Observers {
    inline static Observers* active = nullptr;
    MMDApp& app; Counts counts{};
    bool enqueueSceneOnce = false, enqueueNoneOnce = false, returnFalseOnce = false;
    decltype(&PostViewRefresh) post = nullptr;
    // 依赖 root 冻结的新三参接口，必须由 native_object_commands.hpp 正式声明。
    decltype(&plugins::RefreshBoundHistoryUi) historyUi = nullptr;
    decltype(&FrameDriver) frame = nullptr;
    decltype(&SelectionReeval) selection = nullptr;
    decltype(&updates::RequestViewport) viewport = nullptr;
    decltype(&plugins::RefreshBoundGlobalReadout) global = nullptr;
    decltype(&plugins::RefreshBoundMorphReadout) morph = nullptr;
    decltype(&plugins::RefreshBoundAudioUi) audio = nullptr;
    decltype(&curve_editor::NotifyOwnedChanged) curve = nullptr;
    decltype(&ShutdownD3D12Native) shutdown = nullptr;
    decltype(&InvalidateRect) invalidate = nullptr;
    decltype(&RedrawWindow) redraw = nullptr;
    decltype(&EnableWindow) enable = nullptr;
    explicit Observers(MMDApp& value) : app(value) {
        Require(!active, "one actual history effects observer"); active = this;
    }
    void Install() {
        Hook(reinterpret_cast<void*>(&PostViewRefresh), reinterpret_cast<void*>(&Post), reinterpret_cast<void**>(&post));
        Hook(reinterpret_cast<void*>(&plugins::RefreshBoundHistoryUi), reinterpret_cast<void*>(&HistoryUi), reinterpret_cast<void**>(&historyUi));
        Hook(reinterpret_cast<void*>(&FrameDriver), reinterpret_cast<void*>(&Frame), reinterpret_cast<void**>(&frame));
        Hook(reinterpret_cast<void*>(&SelectionReeval), reinterpret_cast<void*>(&Selection), reinterpret_cast<void**>(&selection));
        Hook(reinterpret_cast<void*>(&updates::RequestViewport), reinterpret_cast<void*>(&Viewport), reinterpret_cast<void**>(&viewport));
        Hook(reinterpret_cast<void*>(&plugins::RefreshBoundGlobalReadout), reinterpret_cast<void*>(&Global), reinterpret_cast<void**>(&global));
        Hook(reinterpret_cast<void*>(&plugins::RefreshBoundMorphReadout), reinterpret_cast<void*>(&Morph), reinterpret_cast<void**>(&morph));
        Hook(reinterpret_cast<void*>(&plugins::RefreshBoundAudioUi), reinterpret_cast<void*>(&Audio), reinterpret_cast<void**>(&audio));
        Hook(reinterpret_cast<void*>(&curve_editor::NotifyOwnedChanged), reinterpret_cast<void*>(&Curve), reinterpret_cast<void**>(&curve));
        Hook(reinterpret_cast<void*>(&ShutdownD3D12Native), reinterpret_cast<void*>(&Shutdown), reinterpret_cast<void**>(&shutdown));
        Hook(reinterpret_cast<void*>(&InvalidateRect), reinterpret_cast<void*>(&Invalidate), reinterpret_cast<void**>(&invalidate));
        Hook(reinterpret_cast<void*>(&RedrawWindow), reinterpret_cast<void*>(&Redraw), reinterpret_cast<void**>(&redraw));
        Hook(reinterpret_cast<void*>(&EnableWindow), reinterpret_cast<void*>(&Enable), reinterpret_cast<void**>(&enable));
    }
    void Reset() { counts = {}; Require(!enqueueSceneOnce && !enqueueNoneOnce && !returnFalseOnce, "prior one-shot observer controls were consumed"); }
    static void Post(MMDApp* app) { auto& s = *active; if (app == &s.app) ++s.counts.post; s.post(app); }
    static void Frame(MMDApp* app) { auto& s = *active; if (app == &s.app) ++s.counts.frame; s.frame(app); }
    static void Selection(MMDApp* app) { auto& s = *active; if (app == &s.app) ++s.counts.selection; s.selection(app); }
    static void Viewport(MMDApp* app, bool geometry) { auto& s = *active; if (app == &s.app) { ++s.counts.viewport; s.counts.geometry += geometry; } s.viewport(app, geometry); }
    static void Global(MMDApp& app) noexcept { auto& s = *active; if (&app == &s.app) ++s.counts.global; s.global(app); }
    static void Morph(MMDApp& app) noexcept { auto& s = *active; if (&app == &s.app) ++s.counts.morph; s.morph(app); }
    static bool Audio(MMDApp& app) noexcept { auto& s = *active; if (&app == &s.app) ++s.counts.audio; return s.audio(app); }
    static void Curve(void* owner) noexcept { auto& s = *active; if (owner == s.app.MainWindow()) ++s.counts.curve; s.curve(owner); }
    static void Shutdown() { auto& s = *active; ++s.counts.rendererShutdown; s.shutdown(); }
    static BOOL WINAPI Invalidate(HWND window, const RECT* rect, BOOL erase) {
        auto& s = *active; if (window == s.app.MainWindow() && !rect) ++s.counts.wholeInvalidation;
        return s.invalidate(window, rect, erase);
    }
    static BOOL WINAPI Redraw(HWND window, const RECT* rect, HRGN region, UINT flags) {
        auto& s = *active; if (window == s.app.MainWindow() && !rect && !region) ++s.counts.wholeInvalidation;
        return s.redraw(window, rect, region, flags);
    }
    static BOOL WINAPI Enable(HWND window, BOOL enabled) {
        auto& s = *active;
        if (window == s.app.MainWindow() || (s.app.FloatingWindow() && window == s.app.FloatingWindow())) ++s.counts.topEnable;
        return s.enable(window, enabled);
    }
    static bool HistoryUi(MMDApp& app, plugins::SharedHistory& history, HistoryEffects effects) noexcept {
        auto& s = *active;
        if (&app == &s.app) {
            if (s.counts.historyUi < s.counts.masks.size()) s.counts.masks[s.counts.historyUi] = effects;
            else s.counts.maskOverflow = true;
            ++s.counts.historyUi;
            if (s.enqueueSceneOnce) {
                s.enqueueSceneOnce = false;
                plugins::RequestBoundHistoryUiRefresh(&app, HistoryEffects::SceneReadout);
            }
            if (s.enqueueNoneOnce) {
                s.enqueueNoneOnce = false;
                plugins::RequestBoundHistoryUiRefresh(&app, HistoryEffects::None);
            }
            if (s.returnFalseOnce) {
                s.returnFalseOnce = false; ++s.counts.failedHistoryUi; ++s.counts.historyUiFailures; return false;
            }
            ++s.counts.forwardedHistoryUi;
        }
        const bool result = s.historyUi(app, history, effects);
        if (&app == &s.app && !result) { ++s.counts.historyUiFailures; ++s.counts.actualHistoryUiFailures; }
        return result;
    }
    void ViewportOnly(const char* phase) const {
        const auto& c = counts;
        std::printf("EFFECTS phase=%s viewport=%u geometry=%u readout=%u,%u,%u,%u,%u scene=%u,%u history_ui=%u\n",
            phase, c.viewport, c.geometry, c.post, c.global, c.morph, c.audio, c.curve, c.selection, c.frame, c.historyUi);
        std::printf("GUARD phase=%s shutdown=%u whole_invalidation=%u top_enable=%u message_seen=%u\n",
            phase, c.rendererShutdown, c.wholeInvalidation, c.topEnable, unsigned(app.MessageSeen()));
        Require(c.viewport > 0 && !c.geometry && !c.post && !c.global && !c.morph && !c.audio && !c.curve &&
            !c.selection && !c.frame && !c.maskOverflow && !c.rendererShutdown && !c.wholeInvalidation && !c.topEnable,
            "Viewport effects request viewport(false) without scene/readout, renderer shutdown or whole-window mutation");
    }
    void NoneOnly(const char* phase) const {
        const auto& c = counts;
        std::printf("NONE phase=%s viewport=%u readout=%u,%u,%u,%u,%u shutdown=%u whole_invalidation=%u top_enable=%u message_seen=%u\n",
            phase, c.viewport, c.post, c.global, c.morph, c.audio, c.curve, c.rendererShutdown, c.wholeInvalidation, c.topEnable, unsigned(app.MessageSeen()));
        Require(!c.viewport && !c.geometry && !c.post && !c.global && !c.morph && !c.audio && !c.curve &&
            !c.selection && !c.frame && !c.maskOverflow && !c.rendererShutdown && !c.wholeInvalidation && !c.topEnable && !c.historyUiFailures,
            "None effects update journal controls without scene, render or whole-window mutation");
    }
};

struct SceneData {
    MMDApp& app; mdl::CameraKey* camera; mdl::LightKey* light;
    uint64_t cameraId, lightId, sceneRevision; int frame, cameraMode, selectedModel, selectedAccessory;
    unsigned messageSeen;
    std::vector<mdl::CameraKey> cameraBytes; std::vector<mdl::LightKey> lightBytes;
    std::array<float, 7> values;
    explicit SceneData(MMDApp& a) : app(a), camera(a.CameraKeys()), light(a.LightKeys()),
        cameraId(mdl::KeyPoolIdentity(camera)), lightId(mdl::KeyPoolIdentity(light)), sceneRevision(a.SceneRevision()), frame(a.CurrentFrame()),
        cameraMode(a.CameraMode()), selectedModel(a.SelectedModelSlot()), selectedAccessory(a.SelectedAccessorySlot()),
        messageSeen(a.MessageSeen()),
        cameraBytes(camera, camera + mdl::KeyPoolCapacity(camera)), lightBytes(light, light + mdl::KeyPoolCapacity(light)),
        values{a.CameraFov(), a.LightColor()[0], a.LightColor()[1], a.LightColor()[2], a.LightDirection()[0], a.LightDirection()[1], a.LightDirection()[2]} {}
    void Check() const {
        const std::array<float, 7> after{app.CameraFov(), app.LightColor()[0], app.LightColor()[1], app.LightColor()[2],
            app.LightDirection()[0], app.LightDirection()[1], app.LightDirection()[2]};
        Require(app.SceneRevision() == sceneRevision && app.CurrentFrame() == frame && app.CameraMode() == cameraMode &&
            app.SelectedModelSlot() == selectedModel && app.SelectedAccessorySlot() == selectedAccessory && !std::memcmp(values.data(), after.data(), sizeof(values)) &&
            app.CameraKeys() == camera && app.LightKeys() == light && mdl::KeyPoolIdentity(camera) == cameraId && mdl::KeyPoolIdentity(light) == lightId &&
            mdl::KeyPoolCapacity(camera) == cameraBytes.size() && mdl::KeyPoolCapacity(light) == lightBytes.size() &&
            !std::memcmp(camera, cameraBytes.data(), cameraBytes.size() * sizeof(mdl::CameraKey)) &&
            !std::memcmp(light, lightBytes.data(), lightBytes.size() * sizeof(mdl::LightKey)),
            "Viewport-only publication leaves actual scene revision/playhead/key identities/bytes and global authored values unchanged");
        Require(!app.SceneModified() && !app.RecordedFrameCountByte1() && !app.PhysicsResetPending(),
            "Viewport-only history does not set project/recording/physics scalars");
        Require(unsigned(app.MessageSeen()) == messageSeen && app.WindowLayoutReady() && !app.SceneMutationInProgress(),
            "Viewport-only history preserves the pending-input marker and ready layout without a scene mutation");
    }
};
struct RendererVersion { int value = 0; };
class RendererChange final : public plugins::HistoryChange {
    std::shared_ptr<RendererVersion> renderer_; int before_, after_; HistoryEffects effects_;
    class Prepared final : public plugins::PreparedHistoryChange {
        std::shared_ptr<RendererVersion> renderer_; int expected_, next_;
    public:
        Prepared(std::shared_ptr<RendererVersion> renderer, int expected, int next) : renderer_(std::move(renderer)), expected_(expected), next_(next) {}
        bool IsCurrent() const noexcept override { return renderer_->value == expected_; }
        void Publish() noexcept override { renderer_->value = next_; }
    };
public:
    RendererChange(std::shared_ptr<RendererVersion> renderer, int after, HistoryEffects effects) :
        renderer_(std::move(renderer)), before_(renderer_->value), after_(after), effects_(effects) {}
    MmsResult Prepare(bool forward, std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
        const auto expected = forward ? before_ : after_, next = forward ? after_ : before_;
        if (renderer_->value != expected) return MMS_E_STALE;
        out = std::make_unique<Prepared>(renderer_, expected, next); return MMS_OK;
    }
    uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + sizeof(Prepared) + sizeof(RendererVersion); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
    HistoryEffects Effects() const noexcept override { return effects_; }
};
void CommitRenderer(MMDApp& app, plugins::SharedHistory& history, const std::shared_ptr<RendererVersion>& renderer,
    HistoryEffects effects = HistoryEffects::Viewport) {
    const auto before = renderer->value;
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Require(history.PrepareNative("Effects renderer version fixture", std::make_shared<RendererChange>(renderer, before + 1, effects), token,
        plugins::SharedHistory::NativeIntent::ExplicitCommit) == MMS_OK, "reserve actual native renderer-only node");
    Require(history.CommitNative(std::move(token)) == MMS_OK && renderer->value == before + 1 &&
        history.LastPublicationEffects() == effects && !history.LastPublicationRequiresPhysicsReset(), "commit actual node with explicit renderer fixture effects");
    plugins::PublishBoundHistoryEffects(app, history, history.LastPublicationEffects(), history.LastPublicationRequiresPhysicsReset());
}
void Buttons(MMDApp& app, plugins::SharedHistory& history) {
    const auto undo = GetDlgItem(app.MainWindow(), panel::kUndoButton), redo = GetDlgItem(app.MainWindow(), panel::kRedoButton);
    Require(undo && redo && bool(IsWindowEnabled(undo)) == bool(history.UndoCount()) &&
        bool(IsWindowEnabled(redo)) == bool(history.RedoCount()), "actual native undo/redo button enablement matches the common journal");
}
void DrainDirect(MMDApp& app) { plugins::Refresh(&app); }
void PureViewportPaths(MMDApp& app, plugins::SharedHistory& history, Probe& probe, const Menus& menus, Observers& observer,
    const std::shared_ptr<RendererVersion>& renderer) {
    for (auto mode : {plugins::SharedHistory::RecordingMode::ExplicitOnly, plugins::SharedHistory::RecordingMode::AllOperations}) {
        Require(history.SetMode(mode) == MMS_OK, "set common history fixture recording mode");
        DrainDirect(app); ClearPublicationScalars(app); app.MessageSeen() = 0; SceneData invariant(app); observer.Reset();
        CommitRenderer(app, history, renderer); Buttons(app, history); observer.ViewportOnly("native-commit"); invariant.Check();
        const auto committed = renderer->value;
        Require(history.NextPublicationEffects(false) == HistoryEffects::Viewport, "next actual local undo previews frozen Viewport mask");
        observer.Reset(); Require(plugins::RunBoundObjectHistory(app, history, false) == MMS_OK && renderer->value == committed - 1,
            "actual local RunBoundObjectHistory undoes renderer node");
        Buttons(app, history); observer.ViewportOnly("local-undo"); invariant.Check();
        Require(history.NextPublicationEffects(true) == HistoryEffects::Viewport, "next actual local redo previews frozen Viewport mask");
        observer.Reset(); Require(plugins::RunBoundObjectHistory(app, history, true) == MMS_OK && renderer->value == committed,
            "actual local RunBoundObjectHistory redoes renderer node");
        Buttons(app, history); observer.ViewportOnly("local-redo"); invariant.Check();

        const auto pluginValue = probe.value(); const auto undo = history.UndoCount(); observer.Reset();
        menus.Invoke(menus.undo);
        Require(renderer->value == committed - 1 && probe.value() == pluginValue && history.UndoCount() + 1 == undo &&
            history.LastPublicationEffects() == HistoryEffects::Viewport && (probe.flags() & 3) == 1,
            "real DLL mms.edit undo is authorized and forwards into this same native journal");
        DrainDirect(app); Buttons(app, history); observer.ViewportOnly("plugin-undo"); invariant.Check();
        Require(observer.counts.historyUi == 1 && observer.counts.masks[0] == HistoryEffects::Viewport, "deferred real plugin undo consumes Viewport mask once");
        observer.Reset(); menus.Invoke(menus.redo);
        Require(renderer->value == committed && probe.value() == pluginValue && history.LastPublicationEffects() == HistoryEffects::Viewport,
            "real DLL mms.edit redo reaches native renderer node");
        DrainDirect(app); Buttons(app, history); observer.ViewportOnly("plugin-redo"); invariant.Check();
        Require(observer.counts.historyUi == 1 && observer.counts.masks[0] == HistoryEffects::Viewport, "deferred real plugin redo consumes Viewport mask once");
        app.MessageSeen() = 1; SceneData pendingInput(app); observer.Reset();
        Require(plugins::RunBoundObjectHistory(app, history, false) == MMS_OK, "renderer undo also preserves an existing pending-input marker");
        observer.ViewportOnly("local-undo-pending-input"); pendingInput.Check();
    }
}
void NoEffectsPaths(MMDApp& app, plugins::SharedHistory& history, Probe& probe, const Menus& menus, Observers& observer,
    const std::shared_ptr<RendererVersion>& renderer) {
    for (auto mode : {plugins::SharedHistory::RecordingMode::ExplicitOnly, plugins::SharedHistory::RecordingMode::AllOperations}) {
        Require(history.SetMode(mode) == MMS_OK, "set None-node recording mode");
        DrainDirect(app); ClearPublicationScalars(app); app.MessageSeen() = 0; SceneData invariant(app); observer.Reset();
        CommitRenderer(app, history, renderer, HistoryEffects::None); Buttons(app, history); observer.NoneOnly("native-commit"); invariant.Check();
        const auto committed = renderer->value;
        observer.Reset(); Require(plugins::RunBoundObjectHistory(app, history, false) == MMS_OK && renderer->value == committed - 1,
            "local undo traverses a real frozen None node");
        Buttons(app, history); observer.NoneOnly("local-undo"); invariant.Check();
        observer.Reset(); Require(plugins::RunBoundObjectHistory(app, history, true) == MMS_OK && renderer->value == committed,
            "local redo traverses a real frozen None node");
        Buttons(app, history); observer.NoneOnly("local-redo"); invariant.Check();
        const auto value = probe.value(); observer.Reset(); menus.Invoke(menus.undo);
        Require(renderer->value == committed - 1 && probe.value() == value && history.LastPublicationEffects() == HistoryEffects::None,
            "original DLL undo reaches the same real None node");
        DrainDirect(app); Buttons(app, history); observer.NoneOnly("plugin-undo"); invariant.Check();
        Require(observer.counts.historyUi == 1 && observer.counts.masks[0] == HistoryEffects::None, "deferred None publication still updates buttons once");
        observer.Reset(); menus.Invoke(menus.redo);
        Require(renderer->value == committed && probe.value() == value && history.LastPublicationEffects() == HistoryEffects::None,
            "original DLL redo reaches the same real None node");
        DrainDirect(app); Buttons(app, history); observer.NoneOnly("plugin-redo"); invariant.Check();
        Require(observer.counts.historyUi == 1 && observer.counts.masks[0] == HistoryEffects::None, "deferred None redo consumes its own pending flag");
    }
}
void RequireAllConsumed(const Observers& observer) {
    const auto& c = observer.counts;
    Require(c.historyUi == 1 && c.masks[0] == HistoryEffects::All && !c.maskOverflow && c.forwardedHistoryUi == 1 &&
        !c.actualHistoryUiFailures && !c.historyUiFailures && c.post && c.global && c.morph && c.audio && c.curve && c.viewport && !c.geometry,
        "deferred union consumes prior All through successful real full scene/readout path exactly once");
}
void DeferredUnionAndSavedDirty(MMDApp& app, plugins::SharedHistory& history, Probe& probe, const Menus& menus, Observers& observer,
    const std::shared_ptr<RendererVersion>& renderer) {
    DrainDirect(app); ClearPublicationScalars(app); observer.Reset();
    const auto value = probe.value(); menus.Invoke(menus.once);
    Require(probe.value() == value + 1 && history.LastPublicationEffects() == HistoryEffects::All && app.SceneModified() &&
        app.RecordedFrameCountByte1() && app.PhysicsResetPending() &&
        (probe.flags() & (32 | 64 | 16384 | 32768)) == (32 | 64 | 16384 | 32768),
        "real plugin once publishes conservative All and immediate native scalars through original transaction callbacks");
    CommitRenderer(app, history, renderer); menus.Invoke(menus.undo);
    Require(history.LastPublicationEffects() == HistoryEffects::Viewport && probe.value() == value + 1,
        "last publication is renderer-only while an older All UI obligation still exists");
    Require(!observer.counts.historyUi,
        "no explicit host safe point consumed the deferred union during the cached menu sequence");
    // 关键区间从 once 到此没有 Pump/Refresh。缓存 menu IDs 避免查菜单时补刷新。
    // 这里模拟成功保存后的 dirty 清零，只检验延迟 UI 不会再次标脏。
    app.state.sceneModified = 0; observer.Reset();
    DrainDirect(app); RequireAllConsumed(observer);
    Require(!app.SceneModified(), "consuming old All UI after simulated saved-dirty clear must not mark project dirty again");
    Buttons(app, history);

    // 反向：先排 Viewport，再真正发布 All；合并结果仍必须完整消费。
    observer.Reset(); ClearPublicationScalars(app); CommitRenderer(app, history, renderer); menus.Invoke(menus.undo);
    Require(history.LastPublicationEffects() == HistoryEffects::Viewport, "prepare deferred Viewport before opposite ordering");
    const auto before = probe.value(); menus.Invoke(menus.once);
    Require(probe.value() == before + 1 && history.LastPublicationEffects() == HistoryEffects::All && app.SceneModified(),
        "later real plugin once publishes All without consuming earlier deferred Viewport");
    Require(!observer.counts.historyUi,
        "reverse cached menu sequence also leaves history effects deferred until the explicit safe point");
    app.state.sceneModified = 0; observer.Reset(); DrainDirect(app); RequireAllConsumed(observer);
    Require(!app.SceneModified(), "reverse deferred union also keeps the simulated saved project clean"); Buttons(app, history);
}
void ReentrantAndRefusedUi(MMDApp& app, plugins::SharedHistory& history, Observers& observer) {
    DrainDirect(app); ClearPublicationScalars(app); observer.Reset();
    uint64_t revision = 0; Require(history.Revision(revision) == MMS_OK, "capture journal before UI-only request tests");
    const auto undo = history.UndoCount(), redo = history.RedoCount();
    app.MessageSeen() = 0; SceneData noneInvariant(app);
    observer.enqueueNoneOnce = true; plugins::RequestBoundHistoryUiRefresh(&app, HistoryEffects::None);
    DrainDirect(app);
    Require(!observer.enqueueNoneOnce && observer.counts.historyUi == 1 && observer.counts.masks[0] == HistoryEffects::None,
        "None requests still update history controls and retain a same-mask request enqueued during consumption");
    DrainDirect(app);
    Require(observer.counts.historyUi == 2 && observer.counts.masks[1] == HistoryEffects::None && !observer.counts.historyUiFailures,
        "same None request survives separately from the empty pending-effects mask");
    DrainDirect(app);
    Require(observer.counts.historyUi == 2 && !observer.counts.post && !observer.counts.global && !observer.counts.viewport &&
        !observer.counts.audio && !observer.counts.morph && !observer.counts.curve && !observer.counts.selection && !observer.counts.frame &&
        !observer.counts.rendererShutdown && !observer.counts.wholeInvalidation && !observer.counts.topEnable,
        "successful None consumption stops without scene/readout work");
    noneInvariant.Check(); Buttons(app, history); observer.Reset();
    observer.enqueueSceneOnce = true; plugins::RequestBoundHistoryUiRefresh(&app, HistoryEffects::SceneReadout);
    DrainDirect(app);
    Require(!observer.enqueueSceneOnce && observer.counts.forwardedHistoryUi == 1 && observer.counts.historyUi == 1 &&
        observer.counts.masks[0] == HistoryEffects::SceneReadout, "first safe point consumes only its taken SceneReadout mask");
    DrainDirect(app);
    Require(observer.counts.forwardedHistoryUi == 2 && observer.counts.historyUi == 2 &&
        observer.counts.masks[1] == HistoryEffects::SceneReadout && !observer.counts.actualHistoryUiFailures && !observer.counts.historyUiFailures && observer.counts.post >= 2,
        "same mask enqueued within actual effects consumer survives to second safe point");
    DrainDirect(app); Require(observer.counts.historyUi == 2, "third safe point has no repeated UI work");
    Require(!app.SceneModified() && !app.RecordedFrameCountByte1() && !app.PhysicsResetPending(), "reentrant UI requests never publish project scalars");

    observer.Reset(); observer.returnFalseOnce = true;
    plugins::RequestBoundHistoryUiRefresh(&app, HistoryEffects::SceneReadout); DrainDirect(app);
    Require(!observer.returnFalseOnce && observer.counts.historyUi == 1 && observer.counts.failedHistoryUi == 1 &&
        !observer.counts.forwardedHistoryUi, "one explicitly injected UI refusal is observed, not claimed as a genuine UI failure");
    DrainDirect(app);
    Require(observer.counts.failedHistoryUi == 1 && observer.counts.forwardedHistoryUi == 1 &&
        observer.counts.historyUi == 2 && observer.counts.masks[0] == HistoryEffects::SceneReadout &&
        observer.counts.masks[1] == HistoryEffects::SceneReadout && !observer.counts.actualHistoryUiFailures && observer.counts.historyUiFailures == 1 && observer.counts.post,
        "next safe point retains and successfully forwards the refused UI obligation");
    DrainDirect(app); Require(observer.counts.historyUi == 2, "successful retry stops without polling forever");
    uint64_t after = 0;
    Require(history.Revision(after) == MMS_OK && after == revision && history.UndoCount() == undo && history.RedoCount() == redo &&
        !app.SceneModified() && !app.RecordedFrameCountByte1() && !app.PhysicsResetPending(), "UI queue controls neither edit journal nor project/physics data");
}
#endif
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) { std::fprintf(stderr, "usage: history_effects_host_tests <absolute-manager_edit_probe.dll>\n"); return 2; }
#if MMD_ORIGINAL_PROFILE
    std::puts("SKIP actual history effects host: original plugin runtime gate remains closed"); return 0;
#else
    SetProcessDPIAware(); auto app = std::make_unique<MMDApp>();
    bool initialized = false, minhook = false;
    // 外层持有 observer，保证异常清理触发真实转发时，回调上下文仍然存活。
    std::unique_ptr<Observers> observer;
    try {
        Package(argv[1]); Require(MH_Initialize() == MH_OK, "initialize only this process's hidden-host hooks"); minhook = true;
        Hook(reinterpret_cast<void*>(&ShowWindow), reinterpret_cast<void*>(Hidden), reinterpret_cast<void**>(&realShow));
        Hook(reinterpret_cast<void*>(&SetWindowPos), reinterpret_cast<void*>(HiddenPosition), reinterpret_cast<void**>(&realPosition));
        Hook(reinterpret_cast<void*>(&MessageBoxA), reinterpret_cast<void*>(UnexpectedA)); Hook(reinterpret_cast<void*>(&MessageBoxW), reinterpret_cast<void*>(UnexpectedW));
        g_Block = app.get(); std::memset(&app->state, 0, sizeof(app->state)); app->InitDefaults();
        Require(InitMainWindowAndD3D(app.get(), GetModuleHandleW(nullptr), SW_HIDE), "initialize real hidden MMDApp and D3D9 device"); initialized = true;
        Require(app->MainWindow() && app->Renderer() && app->Renderer()->device && app->WindowLayoutReady() && !IsWindowVisible(app->MainWindow()),
            "actual fixture has a hidden ready native window and real device");
        Require(app->CameraKeys() && app->LightKeys(), "actual native initialization owns camera/light key pools for byte invariants");
        SettleInitial(*app); Probe ordinary; ordinary.Resolve(); Menus ordinaryMenus; ordinaryMenus.Capture(*app);
        Require((ordinary.flags() & 3) == 2 && !plugins::BoundNativePoseHistory(app.get()), "normal production runtime leaves public mms.edit unavailable");
        auto* ordinaryHistory = plugins::NativeHistory(app.get()); Require(ordinaryHistory != nullptr, "normal runtime owns its private journal");
        const auto ordinaryUndo = ordinaryHistory->UndoCount(); const auto ordinaryValue = ordinary.value();
        ordinaryMenus.Invoke(ordinaryMenus.once);
        Require(ordinary.value() == ordinaryValue && ordinaryHistory->UndoCount() == ordinaryUndo && (ordinary.flags() & 3) == 2,
            "ordinary menu invocation cannot write through closed production mms.edit");
        plugins::Shutdown();
        Require(plugins::InitializeHistoryTestRuntime(app.get()), "reinitialize explicit migration runtime with one bound common journal");
        SettleInitial(*app); Probe probe; probe.Resolve(); Menus menus; menus.Capture(*app);
        auto* history = plugins::NativeHistory(app.get());
        Require(history && plugins::BoundNativePoseHistory(app.get()) == history && (probe.flags() & 3) == 1,
            "explicit migration runtime binds original DLL and native writers to the same journal");
        Require(app->CameraMode() != 0, "camera-mode host makes PostViewRefresh an observable pose-readout entry");
        observer = std::make_unique<Observers>(*app); observer->Install();
        const auto renderer = std::make_shared<RendererVersion>();
        PureViewportPaths(*app, *history, probe, menus, *observer, renderer);
        NoEffectsPaths(*app, *history, probe, menus, *observer, renderer);
        DeferredUnionAndSavedDirty(*app, *history, probe, menus, *observer, renderer);
        ReentrantAndRefusedUi(*app, *history, *observer);
        ShutdownCleanup(app.get()); initialized = false; g_Block = nullptr;
        // hooks 必须先卸载，再结束 observer 生命周期；不保留悬空 active 指针。
        MH_Uninitialize(); minhook = false; Observers::active = nullptr; observer.reset();
        std::puts("PASS actual history effects host (renderer classification fixture; not texture 278)"); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL actual history effects host: %s\n", error.what());
        if (initialized) ShutdownCleanup(app.get());
        g_Block = nullptr; if (minhook) MH_Uninitialize(); Observers::active = nullptr; observer.reset(); return 1;
    }
#endif
}
