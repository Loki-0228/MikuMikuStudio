#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/texture_lookup.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "../src/plugins/native_model_toon.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include "../src/render/model_toon.hpp"

using namespace mikumikustudio;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); ExitProcess(7); }
}
struct ReleaseCom { void operator()(IUnknown* value) const noexcept { if (value) value->Release(); } };
using ComIdentity = std::unique_ptr<IUnknown, ReleaseCom>;
ComIdentity Identity(IDirect3DTexture9* texture) {
    Check(texture != nullptr, "a native Toon texture exists before checking its COM identity");
    IUnknown* raw = nullptr;
    const auto result = texture->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&raw));
    ComIdentity identity(raw); Check(SUCCEEDED(result) && identity, "query the real D3D9 texture canonical COM identity");
    return identity;
}
bool SameIdentity(IDirect3DTexture9* texture, IUnknown* expected) {
    return texture && Identity(texture).get() == expected;
}
void NativeTexture(IDirect3DTexture9* texture, IDirect3DDevice9* expected) {
    Check(texture != nullptr, "production resolver supplies a real Toon texture");
    D3DSURFACE_DESC description{};
    Check(SUCCEEDED(texture->GetLevelDesc(0, &description)) && description.Width && description.Height &&
        description.Type == D3DRTYPE_SURFACE && description.Pool == D3DPOOL_MANAGED,
        "real D3DX-loaded Toon has a queryable managed D3D9 surface");
    IDirect3DDevice9* raw = nullptr; const auto result = texture->GetDevice(&raw);
    std::unique_ptr<IDirect3DDevice9, ReleaseCom> device(raw);
    Check(SUCCEEDED(result) && device.get() == expected, "real Toon belongs to the hidden production renderer device");
}
bool FileExists(const std::wstring& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring FixtureDirectory(const wchar_t* model, const wchar_t* toon) {
    wchar_t full[32768]{};
    const auto length = GetFullPathNameW(model, static_cast<DWORD>(std::size(full)), full, nullptr);
    Check(length && length < std::size(full), "resolve the runner's private fixture model path");
    const std::wstring path(full, length); const auto slash = path.find_last_of(L"\\/");
    Check(slash != std::wstring::npos && !_wcsicmp(path.c_str() + slash + 1, L"fixture.pmd"),
        "real history test is restricted to the runner's fixture.pmd");
    Check(toon && *toon && !std::wcspbrk(toon, L"\\/:") && std::wcscmp(toon, L".") && std::wcscmp(toon, L"..") &&
        _wcsicmp(toon, L"after-history.bmp"), "original Toon argument is a distinct plain filename inside the private fixture");
    const auto directory = path.substr(0, slash + 1);
    Check(FileExists(directory + L".mms-toon-test-fixture") && FileExists(directory + toon) &&
        FileExists(directory + L"after-history.bmp"), "require the runner marker and both existing fixture images");
    return directory;
}
struct ExclusiveFixtureRead {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit ExclusiveFixtureRead(const std::wstring& path) {
        handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(handle != INVALID_HANDLE_VALUE, "exclusively hold the exact fixture texture without changing or renaming it");
        const auto probe = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto error = GetLastError(); if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
        Check(probe == INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION,
            "fixture source cannot be reopened while history replays retained texture resources");
    }
    ~ExclusiveFixtureRead() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    ExclusiveFixtureRead(const ExclusiveFixtureRead&) = delete;
    ExclusiveFixtureRead& operator=(const ExclusiveFixtureRead&) = delete;
};
void RealHistory(MMDApp& app, const std::wstring& directory, const wchar_t* originalName) {
    using namespace plugins;
    auto* const object = app.ModelSlot(0); auto& model = *mdl::Mdl(object);
    auto& material = mdl::Materials(object)[0];
    Check(!_wcsicmp(model.modelDirectory, directory.c_str()), "native model and runner agree on the exact private resource directory");
    const auto resolve = [&]() { return ResolveModelToonTexture(&app, app.Renderer(), model, material); };
    auto* const original = resolve(); NativeTexture(original, app.Renderer()->device);
    auto originalIdentity = Identity(original); ComIdentity afterIdentity;
    const auto objectId = SceneObjectIdentity(object).id;
    const int frame = app.CurrentFrame();
    const auto source = directory + originalName, destination = directory + L"after-history.bmp";
    for (auto mode : {SharedHistory::RecordingMode::AllOperations, SharedHistory::RecordingMode::ExplicitOnly}) {
        StorageStore storage;
        SharedHistory history(storage, [&]() {
            return app.PlaybackActive() || app.FrameStepPlayback() || app.state.recordPlaybackStartPending ||
                app.SceneMutationInProgress() ? MMS_E_BUSY : MMS_OK;
        });
        NativePoseTestBinding binding(app, history);
        Check(history.SetMode(mode) == MMS_OK, "real Toon history uses the requested standard recording mode");
        std::array<char, sizeof(model.pmdToonFileNames)> beforeBytes{};
        std::memcpy(beforeBytes.data(), model.pmdToonFileNames, beforeBytes.size());
        const auto* previousCache = FindTextureResource(app.Renderer(), destination.c_str());
        IUnknown* const previousAfterTexture = previousCache ? previousCache->comObject : nullptr;
        const auto cacheGeneration = texture_lookup_detail::generation.load();
        NativeModelToonPlan plan; ModelToonDraft before;
        Check(plan.Begin(app, 0) == MMS_OK && plan.Read(before) == MMS_OK && before[0] == originalName,
            "capture the real loaded Toon table and its actual before resource using the default production loader");
        auto after = before; after[0] = L"after-history.bmp";
        Check(plan.Set(after) == MMS_OK && !std::memcmp(model.pmdToonFileNames, beforeBytes.data(), beforeBytes.size()) &&
            SameIdentity(resolve(), originalIdentity.get()) && !history.UndoCount(),
            "staging the filename leaves the visible real Toon texture and shared history untouched");
        std::shared_ptr<HistoryChange> change;
        Check(plan.Take(change) == MMS_OK && change && SameIdentity(resolve(), originalIdentity.get()),
            "prepare the real after image through D3DX privately with full resource validation");
        const auto* privateCache = FindTextureResource(app.Renderer(), destination.c_str());
        Check((privateCache ? privateCache->comObject : nullptr) == previousAfterTexture &&
            texture_lookup_detail::generation.load() == cacheGeneration,
            "real D3DX preparation does not publish a cache entry or invalidate the live texture lookup");
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        Check(history.PrepareNative("real D3D9 Toon filename", change, transaction, SharedHistory::NativeIntent::ExplicitCommit) == MMS_OK && transaction,
            "admit the production Toon change through the normal common-history resource budget");
        Check(history.CommitNative(std::move(transaction)) == MMS_OK && history.UndoCount() == 1 && !history.RedoCount() &&
            history.RetainedBytes() && history.RetainedBytes() <= history.MaxBytes(),
            "one explicitly committed real Toon change records one normally budgeted node in either history mode");
        change.reset(); // The common journal must retain the change on its own.
        auto* const edited = resolve(); NativeTexture(edited, app.Renderer()->device);
        auto editedIdentity = Identity(edited);
        Check(editedIdentity.get() != originalIdentity.get(), "real edited Toon selects a distinct actual COM resource");
        if (!afterIdentity) afterIdentity = std::move(editedIdentity);
        else Check(editedIdentity.get() == afterIdentity.get(), "both history modes reuse the same actual cached after resource");
        const auto* cached = FindTextureResource(app.Renderer(), destination.c_str());
        Check(cached && cached->comObject && Identity(static_cast<IDirect3DTexture9*>(cached->comObject)).get() == afterIdentity.get() &&
            std::strcmp(model.pmdToonFileNames[0], "after-history.bmp") == 0 && app.EnhancedModelDirty(),
            "publication exposes the prepared real cache identity to the production material resolver");
        std::array<char, sizeof(model.pmdToonFileNames)> afterBytes{};
        std::memcpy(afterBytes.data(), model.pmdToonFileNames, afterBytes.size());
        {
            ExclusiveFixtureRead beforeLock(source), afterLock(destination);
            Check(history.History(false) == MMS_OK && !history.UndoCount() && history.RedoCount() == 1 &&
                !std::memcmp(model.pmdToonFileNames, beforeBytes.data(), beforeBytes.size()) && SameIdentity(resolve(), originalIdentity.get()),
                "real undo restores every original filename byte and the original COM identity while both source files deny reads");
            Check(history.History(true) == MMS_OK && history.UndoCount() == 1 && !history.RedoCount() &&
                !std::memcmp(model.pmdToonFileNames, afterBytes.data(), afterBytes.size()) && SameIdentity(resolve(), afterIdentity.get()),
                "real redo restores every edited filename byte and the same after COM identity without reopening either image");
            Check(app.ModelSlot(0) == object && SceneObjectPresent(object, objectId) && app.CurrentFrame() == frame &&
                !history.LastPublicationRequiresPhysicsReset(), "real Toon replay preserves object identity, playback position and physics state");
            Check(history.History(false) == MMS_OK && SameIdentity(resolve(), originalIdentity.get()),
                "restore the exact original real Toon before the next history-mode case");
        }
        change.reset();
        Check(history.Reset() == MMS_OK, "release all real Toon history holders before the production renderer is shut down");
        history.Collect();
        std::printf("PASS real D3D9 Toon edit/undo/redo: mode=%s\n",
            mode == SharedHistory::RecordingMode::AllOperations ? "all" : "explicit");
    }
    Check(FileExists(source) && FileExists(destination), "both fixture image filenames remain present after the read-only lock scopes");
}
LRESULT CALLBACK Guard(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        wchar_t name[32]{}; GetClassNameW(reinterpret_cast<HWND>(wp), name, 32);
        Check(std::wcscmp(name, L"#32770") != 0, "unexpected dialog during isolated native Toon import");
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}
int wmain(int argc, wchar_t** argv) {
    const bool importOnly = argc == 4 && !std::wcscmp(argv[3], L"--import-only");
    if (argc != 3 && !importOnly) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto directory = FixtureDirectory(argv[1], argv[2]);
    const auto hook = SetWindowsHookExW(WH_CBT, Guard, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "install dialog guard on the private test desktop");
    auto app = std::make_unique<MMDApp>(); g_Block = app.get(); app->InitDefaults(); app->EnglishUI() = 1;
    Check(InitMainWindowAndD3D(app.get(), GetModuleHandleW(nullptr), SW_HIDE), "initialize hidden production D3D9 host");
    ShowWindow(app->MainWindow(), SW_HIDE);
    LoadModelFile(app.get(), argv[1], false);
    const auto* object = app->ModelSlot(0); Check(object != nullptr, "real PMD with a custom CP932 Toon loads");
    const auto& model = *mdl::Mdl(object);
    std::string encoded; Check(text_encoding::EncodeExact(argv[2], 932, encoded), "fixture Toon filename is exact CP932");
    Check(std::string(model.pmdToonFileNames[0], strnlen_s(model.pmdToonFileNames[0], 100)) == encoded,
        "initial PMD import preserves the actual CP932 filename instead of substituting a default Toon");
    const auto path = std::wstring(model.modelDirectory) + argv[2];
    const auto* cached = FindTextureResource(app->Renderer(), path.c_str());
    Check(cached && cached->comObject, "initial real D3DX import publishes the correct Unicode cache path");
    Check(model.materialCount && mdl::Materials(const_cast<unsigned char*>(object))[0].toonReference == 0,
        "loaded material selects the custom filename table entry");
    const auto* selected = ResolveModelToonTexture(app.get(), app->Renderer(), model,
        mdl::Materials(const_cast<unsigned char*>(object))[0]);
    Check(selected == cached->comObject, "production renderer resolves exactly the texture loaded from the custom Toon file");
    if (!importOnly) RealHistory(*app, directory, argv[2]);
    std::printf("PASS native PMD CP932 Toon %s: %u checks, ACP=%u\n",
        importOnly ? "full-field import" : "import and real shared history", checks, GetACP());
    ShutdownCleanup(app.get()); g_Block = nullptr; UnhookWindowsHookEx(hook);
    return 0;
}
