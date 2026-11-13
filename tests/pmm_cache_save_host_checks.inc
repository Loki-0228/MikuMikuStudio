// Opt in with MMS_PMM_CACHE_SAVE_ONLY=1 through project_open_identity_runner.py.
// CPU save fixtures; production capture/write/completion stay real.
// MMS_PMM_CACHE_RESET_ONLY=1 selects a separate inherited-desktop, real-device
// Reset regression. The private-desktop CPU cases remain independent.
// Enter wmain on the launcher's desktop, then isolate before creating windows.
// This does not replace full UI acceptance.
// This .inc is included inside the host test's anonymous namespace. Keep
// production header declarations at global scope, then rejoin that namespace.
} // namespace
#include "mikumikustudio/bone_key_pool.hpp"
#include "../src/io/pmm_io_common.hpp"
#include "../src/io/pmm_input.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/clipboard_layout.hpp"
#include "btBulletDynamicsCommon.h"
namespace {
namespace host_cache_save {
bool expectFailure = false;
unsigned failureMessages = 0, cancelledPickers = 0;

class DesktopScope {
    HDESK original_ = nullptr; // Borrowed from GetThreadDesktop; never close it.
    HDESK private_ = nullptr;
    DWORD thread_ = GetCurrentThreadId();
    bool attached_ = false;
    static bool Failed(const char* operation, DWORD error) {
        std::fprintf(stderr, "FAIL cache save desktop: %s Win32=%lu\n", operation, error);
        return false;
    }
    static BOOL CALLBACK Destroy(HWND window, LPARAM parameter) {
        auto& ok = *reinterpret_cast<bool*>(parameter);
        // Destroying an owner can already have destroyed a later enumerated window.
        if (IsWindow(window) && !DestroyWindow(window)) {
            Failed("DestroyWindow", GetLastError()); ok = false;
        }
        return TRUE;
    }
public:
    DesktopScope() = default;
    DesktopScope(const DesktopScope&) = delete;
    DesktopScope& operator=(const DesktopScope&) = delete;
    bool Open() {
        original_ = GetThreadDesktop(thread_);
        if (!original_) return Failed("GetThreadDesktop(original)", GetLastError());
        wchar_t name[128]{};
        swprintf_s(name, L"MMS-PmmCacheSave-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
        private_ = CreateDesktopW(name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!private_) return Failed("CreateDesktopW", GetLastError());
        if (!SetThreadDesktop(private_)) return Failed("SetThreadDesktop(private)", GetLastError());
        attached_ = true;
        wchar_t actual[128]{};
        DWORD needed = 0;
        BOOL input = TRUE;
        const auto current = GetThreadDesktop(thread_);
        if (!current || !GetUserObjectInformationW(current, UOI_NAME, actual, sizeof(actual), &needed) ||
            !GetUserObjectInformationW(current, UOI_IO, &input, sizeof(input), &needed))
            return Failed("verify private desktop", GetLastError());
        if (std::wcscmp(name, actual) || input || current != private_)
            return Failed("private desktop identity/non-input check", ERROR_INVALID_DATA);
        std::printf("CACHE_SAVE private-desktop=%ls non-input=1 attached-before-host=1\n", actual);
        return true;
    }
    bool CloseWindows() {
        if (!attached_) return true;
        if (GetCurrentThreadId() != thread_ || GetThreadDesktop(thread_) != private_)
            return Failed("window cleanup requires the private fixture thread", ERROR_INVALID_STATE);
        bool ok = true;
        if (!EnumThreadWindows(thread_, Destroy, reinterpret_cast<LPARAM>(&ok)))
            return Failed("EnumThreadWindows", GetLastError());
        return ok;
    }
    bool Close() {
        if (!private_) return true;
        if (attached_) {
            if (!CloseWindows()) return false;
            // All fixture hooks must also be removed before this call. Never
            // resume fixture work on the original desktop if cleanup fails.
            if (!SetThreadDesktop(original_)) return Failed("SetThreadDesktop(original)", GetLastError());
            attached_ = false;
            if (GetThreadDesktop(thread_) != original_)
                return Failed("verify restored desktop", ERROR_INVALID_DATA);
        }
        if (!CloseDesktop(private_)) return Failed("CloseDesktop(private)", GetLastError());
        private_ = nullptr;
        std::puts("CACHE_SAVE desktop-restored=1 private-desktop-closed=1");
        return true;
    }
    ~DesktopScope() { if (!Close()) std::_Exit(1); }
};

#if !MMD_ORIGINAL_PROFILE
void EmptySceneBoundary() {
    Check(!app->Renderer() && !app->Physics(), "empty save fixture has neither renderer nor physics world");
    for (unsigned slot = 0; slot < 255; ++slot)
        Check(!app->ModelSlot(slot) && !app->AccessorySlot(slot), "empty save fixture contains no scene objects");
    // Establish an anonymous fixture generation through the real ownership
    // services. This is not a call to, or a substitute acceptance of, ResetAppState.
    Check(plugins::ResetPluginProject(app), "retire the fixture's previous plugin project identity");
    auto transition = project_relocation::LoadScope::ResetAccepted(*app);
    app->AdvanceSceneGeneration();
    app->EnvFileName()[0] = 0;
    app->PmmProjectSettings() = {};
    app->CameraKeys()[0] = CameraRoot();
    app->LightKeys()[0] = LightRoot();
    app->ShadowKeys()[0] = {};
    app->ShadowKeys()[0].distance = 0.01125f;
    app->GravityKeys()[0] = {};
    app->GravityKeys()[0].acceleration = 9.8f;
    app->GravityKeys()[0].direction[1] = -1.f;
    app->CameraFov() = 43.f;
    for (unsigned i = 0; i < 3; ++i) {
        app->LightColor()[i] = app->LightKeys()[0].color[i];
        app->LightDirection()[i] = app->LightKeys()[0].direction[i];
    }
    app->SceneModified() = 0;
    transition.Complete(cache_manager::SceneOutcome::ResetComplete);
    Check(bool(cache_manager::CaptureActiveScene()), "fixture boundary established an anonymous active cache scene");
}

LRESULT CALLBACK EmptyWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (app && window == app->MainWindow() && background_save::HandleMessage(app, message, wp)) return 0;
    return DefWindowProcW(window, message, wp, lp);
}

std::uint64_t Publish(wchar_t digit, unsigned size) {
    const auto path = cache_manager::Root() / L"Textures" / L"shared-texture" /
        (std::wstring(64, digit) + L".mtc");
    fs::create_directories(path.parent_path());
    Check(cache_manager::Managed(path), "save payload is in the isolated managed root");
    cache_manager::WriteReservation reservation(path, 1024 * 1024);
    Check(bool(reservation), "reserve real save cache payload");
    const fs::path temporary = path.wstring() + L".tmp-save-host";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE && reservation.TrackTemporary(temporary),
          "track the newly created empty save cache file");
    std::vector<std::uint32_t> words(size / sizeof(std::uint32_t));
    words[0] = 0x4354504d; words[1] = 2; words[2] = size - 76;
    DWORD written = 0;
    Check(WriteFile(file, words.data(), size, &written, nullptr) && written == size && CloseHandle(file),
          "write and close recognized texture cache bytes");
    Check(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) && reservation.Commit(),
          "publish actual registered cache bytes without overwriting an existing payload");
    file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FILE_STANDARD_INFO info{};
    Check(file != INVALID_HANDLE_VALUE &&
          GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info)) &&
          info.EndOfFile.QuadPart == size && CloseHandle(file), "measure actual cache allocation charge");
    return static_cast<std::uint64_t>((std::max)(info.EndOfFile.QuadPart, info.AllocationSize.QuadPart));
}

void Owner(const fs::path& expected, std::uint64_t bytes, const fs::path& forbidden = {}) {
    const auto view = cache_manager::Inspect();
    Check(view.error.empty(), "inspect healthy real save cache ownership");
    unsigned current = 0;
    for (const auto& row : view.projects) {
        if (!forbidden.empty())
            Check(_wcsicmp(row.path.c_str(), forbidden.c_str()) != 0, "rejected or copy target has no cache owner row");
        if (!row.current) continue;
        ++current;
        std::printf("CACHE_SAVE owner=%ls bytes=%llu expected=%ls bytes=%llu\n", row.path.c_str(),
                    static_cast<unsigned long long>(row.totalBytes), expected.c_str(),
                    static_cast<unsigned long long>(bytes));
        Check(!expected.empty() && !_wcsicmp(row.path.c_str(), expected.c_str()) && row.totalBytes == bytes,
              "real completion binds precisely the expected target and payload charge");
    }
    Check(current == (expected.empty() ? 0u : 1u), "exactly the expected current cache owner count");
}

void ProjectBytes(const fs::path& path, std::uint64_t expected) {
    const auto view = cache_manager::Inspect();
    Check(view.error.empty(), "inspect historical owner after replacement");
    unsigned found = 0;
    for (const auto& row : view.projects) if (!_wcsicmp(row.path.c_str(), path.c_str())) {
        ++found;
        Check(row.totalBytes == expected, "new scene bytes cannot accrue to the previous project");
    }
    Check(found == 1, "historical owner remains independently inspectable");
}

void SavedFile(const fs::path& path) {
    const auto bytes = Bytes(path);
    Check(bytes.size() > 30 && !std::memcmp(bytes.data(), "Polygon Movie maker 0002", 24),
          "production writer published a nonempty native V2 PMM with the expected header");
    Check(bytes.size() > 55 && bytes[54] == 0,
          "real model combo readback serialized zero models, not a missing-control sentinel");
}

// A test gate delays only worker scheduling. Capture, disk publication and
// completion always execute the production implementations with their results.
struct Worker {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD uiThread = GetCurrentThreadId();
    std::atomic<DWORD> writer{0};
    unsigned completions = 0;
    bool success = false;
    Worker(const fs::path& destination, bool backup = false) {
        Check(entered && release, "create independent save worker gates");
        background_save::Options options;
        options.backup = backup;
        options.beforeWrite = [this] {
            writer.store(GetCurrentThreadId());
            if (!SetEvent(entered)) return false;
            return WaitForSingleObject(release, 15000) == WAIT_OBJECT_0;
        };
        options.completed = [this](bool ok) {
            Check(GetCurrentThreadId() == uiThread, "real save completion runs on the host thread");
            ++completions; success = ok;
        };
        Check(background_save::Request(app, destination.c_str(), std::move(options)),
              "real background save accepts and captures the live scene");
        Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0 && writer.load() != uiThread,
              "real disk worker reaches the gate on a different thread");
        Check(SceneSaveInProgress() && !SceneSaveBlocksEdits() && !completions,
              "capture released the edit gate while completion is still pending");
    }
    void Finish(bool expectedSuccess) {
        Check(SetEvent(release) != FALSE, "release the real writer");
        background_save::Wait(app);
        Check(!SceneSaveInProgress() && !SceneSaveBlocksEdits() && completions == 1 && success == expectedSuccess,
              "production Wait delivers exactly one actual disk result and releases save gates");
        // Consume only this worker's posted wake; do not dispatch unrelated UI.
        MSG message{};
        while (PeekMessageW(&message, app->MainWindow(), background_save::kComplete,
                            background_save::kComplete, PM_REMOVE))
            DispatchMessageW(&message);
        Check(completions == 1, "queued worker wake cannot complete a save twice after Wait");
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    ~Worker() {
        if (!completions) { SetEvent(release); background_save::Wait(app); }
        CloseHandle(entered); CloseHandle(release);
    }
};

// Only the data required by the real PMM capture is owned here. This is a
// production ModelRecord with registered pools, not a loaded/renderable mesh.
// Do not send it through LoadSceneFile, ResetAppState or model disposal.
struct SaveModel {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{};
    mdl::MorphRecord morph{};
    unsigned char physics = 1, selection = 1;
    HWND combo = GetDlgItem(app->MainWindow(), panel::kMainComboModel);
    SaveModel() {
        Check(!app->ModelSlot(0) && !app->Renderer() && !app->Physics(),
              "model save fixture starts without a model, D3D device or physics world");
        auto& m = *record;
        strcpy_s(m.name, "CacheModel"); strcpy_s(m.nameEn, "CacheModelEN");
        wcscpy_s(m.path, L"cache-save-data-only.pmx");
        m.boneCount = m.morphCount = 1;
        m.boneTable = &bone; m.morphs = &morph;
        m.bonePhysicsState = &physics; m.boneSelection = &selection;
        m.boneKeys = mdl::BoneKeyPoolCreate(8);
        m.morphKeys = mdl::MorphKeyPoolCreate(8);
        m.displayKeys = mdl::KeyPoolCreate<mdl::DisplayKey>(4);
        Check(mdl::BoneKeyCapacity(m) == 8 && mdl::MorphKeyCapacity(m) == 8 &&
              mdl::KeyPoolCapacity(m.displayKeys) == 4 && mdl::BoneKeyPoolIdentity(m.boneKeys) &&
              mdl::MorphKeyPoolIdentity(m.morphKeys) && mdl::KeyPoolIdentity(m.displayKeys),
              "model uses owned production bone, morph and display key pools");
        strcpy_s(bone.name, "save-bone"); bone.parent = -1;
        bone.rotQuat[3] = bone.rotQuat2[3] = 1.f; bone.physicsDisabled = 1;
        strcpy_s(morph.name, "save-morph");
        m.comboSelIndex = m.comboSelIndex2 = 1; m.loadComplete = 1;
        m.selectedBone = 0;
        for (auto& selected : m.selectedMorphs) selected = 0;
        m.maxFrame = 37; m.edgeScale = 1.75f; m.toonFlag = m.postLoadFlag2 = 1;
        auto& root = m.boneKeys[0]; root.rotation[3] = 1.f;
        root.allocated = 1; root.next = 5; root.position[1] = -2.5f;
        for (unsigned i = 0; i < 16; ++i) root.interpolation[i] = static_cast<unsigned char>(i + 11);
        m.boneKeys[5] = root; m.boneKeys[5].frame = 37; m.boneKeys[5].next = 0;
        m.boneKeys[5].physicsDisabled = 1;
        m.morphKeys[0].allocated = 1; m.morphKeys[0].value = 0.125f; m.morphKeys[0].next = 6;
        m.morphKeys[6] = m.morphKeys[0]; m.morphKeys[6].frame = 37; m.morphKeys[6].next = 0;
        m.displayKeys[0].visible = m.displayKeys[0].allocated = 1;
        SetValues(false);
        Check(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"CacheModel")) == 1,
              "real model combo agrees with the occupied production slot");
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(record.get());
    }
    void SetValues(bool edited) {
        bone.trans[0] = edited ? -8.5f : 3.25f;
        morph.value = edited ? 0.875f : 0.375f;
        record->boneKeys[5].position[0] = edited ? 9.5f : 6.25f;
        record->morphKeys[6].value = edited ? 0.625f : 0.25f;
    }
    SaveModel(const SaveModel&) = delete;
    SaveModel& operator=(const SaveModel&) = delete;
    ~SaveModel() {
        Check(!SceneSaveInProgress() && app->ModelSlot(0) == reinterpret_cast<unsigned char*>(record.get()),
              "model data remains alive until real save completion");
        app->ModelSlot(0) = nullptr;
        Check(SendMessageW(combo, CB_DELETESTRING, 1, 0) == 1,
              "remove only the fixture model combo row");
        mdl::BoneKeyPoolDestroy(record->boneKeys);
        mdl::MorphKeyPoolDestroy(record->morphKeys);
        mdl::KeyPoolDestroy(record->displayKeys);
    }
};

// Read the published file, never the capture buffer. Existing production PMM
// segment readers decode keys with strict short-read checks. Scalar checks
// cover the complete model block and the following camera boundary; this is
// deliberately not a claim that the full project loader or Reset ran.
void SavedModelFile(const fs::path& path, bool edited) {
    int fd = -1;
    Check(_wsopen_s(&fd, path.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYWR, 0) == 0 && fd >= 0,
          "open the actual published model PMM for segment readback");
    pmm_io::Input input(fd);
    Check(input.EnableCheckedReads(), "require complete model PMM segment reads");
    const auto read = [&](auto& value) { pmm_io::Rd(fd, &value, sizeof(value)); };
    const auto byte = [&]() { unsigned char value{}; read(value); return value; };
    const auto integer = [&]() { std::int32_t value{}; read(value); return value; };
    const auto scalar = [&]() { float value{}; read(value); return value; };
    const auto name = [&]() {
        const auto length = byte(); std::string value(length, '\0');
        if (length) pmm_io::Rd(fd, value.data(), length);
        return value;
    };
    char magic[30]{}; read(magic);
    Check(!std::memcmp(magic, "Polygon Movie maker 0002", 24), "model save is a native V2 PMM");
    Check(integer() == 640 && integer() == 480 && integer() == 360 && scalar() == 43.f,
          "model save retains the real header dimensions and camera FOV");
    for (unsigned i = 0; i < 7; ++i) Check(byte() == (app->state.optflag[i] != 0), "saved option byte");
    Check(byte() == 0 && byte() == 1 && byte() == 0, "selected global track, one model and slot zero");
    Check(name() == "CacheModel" && name() == "CacheModelEN", "both model names survive capture");
    char modelPath[256]{}; read(modelPath);
    Check(!std::memcmp(modelPath, "cache-save-data-only.pmx", sizeof("cache-save-data-only.pmx")),
          "fixed-width model path survives capture");
    Check(byte() == 0 && integer() == 1 && name() == "save-bone" &&
          integer() == 1 && name() == "save-morph" && integer() == 0 && integer() == 0,
          "bone and morph identities and empty IK/parent tables survive capture");
    Check(byte() == 1 && byte() == 1 && integer() == 0, "model display order, loaded flag and bone selection");
    for (unsigned i = 0; i < 4; ++i) Check(integer() == 0, "saved selected morph");
    Check(byte() == 0 && integer() == 0 && integer() == 37, "group state, list position and model max frame");
    const auto boneKey = [&](bool sparse) {
        mdl::BoneKey key{}; pmm_io::ReadPmmBoneKey<pmm_io::PmmStream::V2>(fd, key);
        Check(key.frame == (sparse ? 37u : 0u) && key.previous == 0 && key.next == (sparse ? 0u : 5u) &&
              key.allocated == 1 && key.physicsDisabled == (sparse ? 1 : 0), "bone key links and flags read back");
        Check(key.position[0] == (sparse ? (edited ? 9.5f : 6.25f) : 0.f) &&
              key.position[1] == -2.5f && key.position[2] == 0.f &&
              key.rotation[0] == 0.f && key.rotation[1] == 0.f && key.rotation[2] == 0.f && key.rotation[3] == 1.f,
              "bone key nondefault translation and rotation read back");
        for (unsigned i = 0; i < 16; ++i)
            Check(key.interpolation[i] == i + 11, "all asymmetric bone interpolation bytes retain their lane order");
    };
    boneKey(false);
    Check(integer() == 1 && integer() == 5, "sparse bone count and original noncontiguous pool index");
    boneKey(true);
    const auto morphKey = [&](bool sparse) {
        mdl::MorphKey key{}; pmm_io::ReadPmmMorphKey(fd, key);
        Check(key.frame == (sparse ? 37u : 0u) && key.previous == 0 && key.next == (sparse ? 0u : 6u) &&
              key.allocated == 1 && key.value == (sparse ? (edited ? 0.625f : 0.25f) : 0.125f),
              "morph key links, allocation and nondefault weight read back");
    };
    morphKey(false);
    Check(integer() == 1 && integer() == 6, "sparse morph count and original noncontiguous pool index");
    morphKey(true);
    Check(integer() == 0 && integer() == 0 && integer() == 0 && byte() == 1 && byte() == 1 && integer() == 0,
          "display root visibility, allocation, links and empty sparse section");
    Check(scalar() == (edited ? -8.5f : 3.25f) && scalar() == 0.f && scalar() == 0.f &&
          scalar() == 0.f && scalar() == 0.f && scalar() == 0.f && scalar() == 1.f &&
          byte() == 1 && byte() == 1 && byte() == 1 && scalar() == (edited ? 0.875f : 0.375f),
          "current bone pose, physics/selection flags and morph pose are captured independently of keys");
    Check(byte() == 1 && scalar() == 1.75f && byte() == 1 && byte() == 1,
          "model edge, toon and display settings survive capture");
    mdl::CameraKey camera{}; pmm_io::ReadPmmCameraKey<pmm_io::PmmStream::V2>(fd, camera);
    const auto expected = CameraRoot();
    Check(camera.frame == expected.frame && camera.distance == expected.distance &&
          camera.parentModel == expected.parentModel && camera.fov == expected.fov && integer() == 0,
          "model section ends exactly at the real camera root and sparse count");
    std::printf("CACHE_SAVE model-readback=%ls edited=%u bone=1 morph=1 sparse-bone=5 sparse-morph=6\n",
                path.c_str(), edited ? 1u : 0u);
}

void ModelSaveCases() {
    std::puts("STAGE cache save: production model data/pools; segment readback; full Load/Reset not covered");
    EmptySceneBoundary();
    const auto first = directory / L"model-first.pmm", second = directory / L"model-save-as.pmm";
    const auto charge = Publish(L'6', 64 * 1024);
    {
        SaveModel model;
        wcscpy_s(app->EnvFileName(), 256, first.c_str()); app->SceneModified() = 1;
        Owner({}, 0, first);
        Check(SaveSceneFile(app) && !app->SceneModified(), "real synchronous model capture/write/completion succeeds");
        SavedModelFile(first, false); Owner(first, charge);
        const auto generation = app->SceneGeneration();
        app->SceneModified() = 1;
        const auto revision = app->SceneRevision();
        {
            Worker write(second);
            Check(!fs::exists(second), "model snapshot is captured before any worker publication");
            Owner(first, charge, second);
            model.SetValues(true); app->SceneModified() = 1;
            Check(app->SceneRevision() != revision, "new bone/morph edits advance the live revision while save is pending");
            write.Finish(true);
        }
        Check(app->SceneGeneration() == generation && app->SceneModified() &&
              !std::wcscmp(app->EnvFileName(), second.c_str()) && model.bone.trans[0] == -8.5f && model.morph.value == 0.875f,
              "model save-as completion preserves newer live model edits and dirty state");
        SavedModelFile(second, false); Owner(second, charge); ProjectBytes(first, charge);
        { Worker write(second); write.Finish(true); }
        Check(!app->SceneModified(), "subsequent model save without new edits clears dirty");
        SavedModelFile(second, true); SavedModelFile(first, false); Owner(second, charge);
        Check(!app->Renderer() && !app->Physics(), "actual model saves did not create rendering or physics devices");
    }
    EmptySceneBoundary(); Owner({}, 0);
}

// A separate process mode: never fall back to the CPU/private-desktop host.
HDESK inheritedDesktop = nullptr;
BOOL WINAPI DenyForeground(HWND) { return FALSE; }
HWND WINAPI DenyActivation(HWND) { return nullptr; }
decltype(&SetWindowPlacement) realPlacement = nullptr;
BOOL WINAPI HiddenPlacement(HWND window, const WINDOWPLACEMENT* source) {
    if (!source) return realPlacement(window, source);
    auto placement = *source; placement.showCmd = SW_HIDE;
    return realPlacement(window, &placement);
}
BOOL CALLBACK CheckHiddenWindow(HWND window, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId())
        Check(!IsWindowVisible(window), "real Reset fixture has no visible top-level window");
    return TRUE;
}
void HiddenResetBoundary() {
    Check(GetThreadDesktop(GetCurrentThreadId()) == inheritedDesktop,
          "real Reset mode remains on the inherited desktop without fallback");
    Check(EnumWindows(CheckHiddenWindow, 0) != FALSE, "inspect visibility of this process's windows");
    Check(!GetActiveWindow(), "real Reset fixture never acquires an active window");
}

// Compare the entire native PMM prefix using the existing trailer reader's
// independently validated boundary. Filename-dependent extension records are
// excluded, not model data. The old model is gone before the worker writes.
auto StandardSaveBytes(const fs::path& path) {
    int fd = -1;
    Check(_wsopen_s(&fd, path.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYWR, 0) == 0 && fd >= 0,
          "open actual Reset regression PMM");
    pmm_io::Input input(fd);
    pmm_extension::Document document; std::uint64_t standard = 0;
    Check(pmm_extension::Read(fd, document, standard) == pmm_extension::ReadResult::Loaded,
          "existing PMM reader validates published extension and standard boundary");
    auto bytes = Bytes(path);
    Check(standard > 55 && standard <= bytes.size() &&
          !std::memcmp(bytes.data(), "Polygon Movie maker 0002", 24), "valid native PMM prefix boundary");
    bytes.resize(static_cast<std::size_t>(standard));
    return bytes;
}

void RealResetSaveCases() {
    Check(!monitor && !host_cache::touchTail && !host_cache::finishes && !SceneSaveInProgress(),
          "real Reset regression has no parser faults or synthetic boundary observer");
    Check(realReset == nullptr, "ResetAppState has no test hook in the real Reset mode");
    Check(app->Renderer() && app->Renderer()->device && app->Physics() && app->Physics()->world &&
          app->Physics()->groundBody, "production initialization supplied actual D3D and Bullet resources");
    D3DDEVICE_CREATION_PARAMETERS creation{};
    Check(SUCCEEDED(app->Renderer()->device->GetCreationParameters(&creation)) && creation.DeviceType == D3DDEVTYPE_HAL,
          "real Reset requires an actual HAL device; software/reference fallback is not acceptance");
    HiddenResetBoundary();
    app->EnglishUI() = 1;
    auto settings = autosave::Current();
    settings.enabled = settings.keepPrevious = settings.numberedManualVersions = false;
    std::wstring error;
    Check(autosave::Configure(app, settings, error), "disable periodic/version saves in isolated Reset host");
    Check(ResetAppState(app), "establish initial anonymous scene through unhooked production Reset");
    Check(cache_manager::Root() == host_cache::base / L"Cache" && cache_manager::CaptureActiveScene(),
          "real Reset established an isolated active cache scene");
    const auto source = directory / L"reset-owned.pmd";
    Wire modelFile; modelFile.bytes = geometry_test_reference::Pmd(3, true).bytes; modelFile.Write(source);
    // Use the loader's normal noninteractive entry option for this fixture;
    // no model-information or missing-English-name dialog is acknowledged.
    LoadModelFile(app, source.c_str(), false);
    auto* raw = app->ModelSlot(0);
    Check(raw && mdl::Mdl(raw)->boneCount == 1, "real loader owns the generated model and its bone");
    auto& model = *mdl::Mdl(raw);
    model.boneTable[0].trans[0] = 7.25f;
    model.boneKeys[0].position[0] = -3.5f;
    auto* oldBones = model.boneKeys; auto* oldMorphs = model.morphKeys; auto* oldDisplay = model.displayKeys;
    Check(mdl::BoneKeyPoolIdentity(oldBones) && mdl::MorphKeyPoolIdentity(oldMorphs) &&
          mdl::KeyPoolIdentity(oldDisplay), "loaded old model owns all three registered key pools");
    const auto first = directory / L"reset-before.pmm", stale = directory / L"reset-stale.pmm";
    const auto replacement = directory / L"reset-new.pmm";
    const auto oldCharge = Publish(L'7', 64 * 1024);
    wcscpy_s(app->EnvFileName(), 256, first.c_str()); app->SceneModified() = 1;
    Check(SaveSceneFile(app), "real model baseline is saved before the Reset race");
    const auto frozen = StandardSaveBytes(first);
    Check(frozen[54] == 1, "baseline native prefix contains the loaded model");
    Owner(first, oldCharge);
    const auto oldGeneration = app->SceneGeneration();
    const auto oldToken = cache_manager::CaptureActiveScene();
    const auto oldCameraIdentity = mdl::KeyPoolIdentity(app->CameraKeys());
    Check(bool(oldToken) && oldCameraIdentity, "capture pre-Reset cache and global pool identities");
    std::uint64_t newCharge = 0;
    {
        Worker write(stale);
        Check(!fs::exists(stale), "old snapshot is frozen while its disk worker is gated");
        Owner(first, oldCharge, stale);
        // No replacement, generation edit, cache reset or manual disposal here.
        Check(ResetAppState(app), "unhooked production Reset completes while the old writer is gated");
        Check(SceneSaveInProgress() && !write.completions && !fs::exists(stale),
              "Reset did not drain the old worker or complete its save");
        Check(app->SceneGeneration() != oldGeneration && !*app->EnvFileName() && !app->SceneModified() &&
              !app->sceneRecoveryPending && !app->SceneMutationInProgress(), "real Reset restores anonymous healthy scene identity");
        for (unsigned slot = 0; slot < 255; ++slot)
            Check(!app->ModelSlot(slot) && !app->AccessorySlot(slot), "production Reset released old scene objects");
        // Registry lookups do not dereference the retired allocations.
        Check(!mdl::BoneKeyPoolIdentity(oldBones) && !mdl::MorphKeyPoolIdentity(oldMorphs) &&
              !mdl::KeyPoolIdentity(oldDisplay), "production model disposal retired all old key-pool allocations");
        std::printf("CACHE_RESET globals old=%llu new=%llu frame=%d fov=%.9g root_fov=%u distance=%.9g\n",
            static_cast<unsigned long long>(oldCameraIdentity),
            static_cast<unsigned long long>(mdl::KeyPoolIdentity(app->CameraKeys())),
            int(app->CurrentFrame()), double(app->CameraFov()), unsigned(app->CameraKeys()[0].fov),
            double(app->CameraDistance()));
        // Reset's final ReloadModels seeks the fresh camera root (FOV 30),
        // after the earlier transient UI echo assigned FOV 45.
        Check(mdl::KeyPoolIdentity(app->CameraKeys()) &&
              mdl::KeyPoolIdentity(app->CameraKeys()) != oldCameraIdentity &&
              app->CurrentFrame() == 0 && app->CameraFov() == 30.f && app->CameraKeys()[0].fov == 30 &&
              app->CameraDistance() == -45.f,
              "Reset rebuilt global tracks and restored the default camera");
        Check(app->Physics() && app->Physics()->world && app->Physics()->groundBody,
              "Reset retains a valid default physics world and ground");
        const auto gravity = app->Physics()->world->getGravity();
        Check(std::fabs(gravity.x()) < 0.001f && std::fabs(gravity.y() + 98.f) < 0.001f &&
              std::fabs(gravity.z()) < 0.001f, "real Bullet world has Reset's default gravity");
        Check(cache_manager::CaptureActiveScene() && !cache_manager::BindSavedScene(oldToken, stale.c_str()),
              "old cache token is rejected after production Reset issues a new scene");
        Owner({}, 0, stale); ProjectBytes(first, oldCharge);
        newCharge = Publish(L'8', 128 * 1024);
        mdl::CameraClipboardRecord edit{}; edit.frame = 23; edit.distance = -62.5f;
        edit.fov = 51; edit.perspective = 1; edit.parentModel = -1;
        std::memset(edit.interpolation, 20, 12); std::memset(edit.interpolation + 12, 107, 12);
        const auto revisionBeforeEdit = app->SceneRevision();
        Check(RegisterCameraKey(app, &edit) != 0, "new default scene accepts a real camera key registration while save is pending");
        app->SceneModified() = 1;
        const auto key = app->CameraKeys()[0].next;
        Check(key && key < mdl::KeyPoolCapacity(app->CameraKeys()) && app->CameraKeys()[key].frame == 23 &&
              app->CameraKeys()[key].distance == -62.5f && app->SceneRevision() != revisionBeforeEdit,
              "new edit is retained in the rebuilt production track and advances revision");
        const auto newRevision = app->SceneRevision(), newGeneration = app->SceneGeneration();
        HiddenResetBoundary();
        write.Finish(true);
        Check(StandardSaveBytes(stale) == frozen, "old worker writes the entire frozen native scene after its model was freed");
        Check(app->SceneGeneration() == newGeneration && app->SceneRevision() == newRevision &&
              app->SceneModified() && !*app->EnvFileName() && app->CameraKeys()[key].frame == 23 &&
              app->CameraKeys()[key].distance == -62.5f,
              "old completion cannot retarget, clean or overwrite the new scene and its edit");
        Owner({}, 0, stale); ProjectBytes(first, oldCharge);
    }
    { Worker write(replacement); Owner({}, 0, replacement); write.Finish(true); }
    Check(!app->SceneModified() && !std::wcscmp(app->EnvFileName(), replacement.c_str()),
          "new default scene's own real save commits its identity and clears dirty");
    SavedFile(replacement); Owner(replacement, newCharge, stale); ProjectBytes(first, oldCharge);
    {
        int fd = -1;
        Check(_wsopen_s(&fd, replacement.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYWR, 0) == 0 && fd >= 0,
              "open new scene's real saved camera track");
        pmm_io::Input input(fd);
        Check(input.EnableCheckedReads() && pmm_io::Seek(fd, 55, SEEK_SET) == 55,
              "read camera section immediately after the empty-model header");
        mdl::CameraKey root{}, edited{};
        pmm_io::ReadPmmCameraKey<pmm_io::PmmStream::V2>(fd, root);
        std::int32_t count = 0; std::uint32_t index = 0;
        pmm_io::Rd(fd, &count, sizeof(count)); pmm_io::Rd(fd, &index, sizeof(index));
        Check(count == 1 && index != 0 && root.next == index, "new saved camera track contains its registered sparse key");
        pmm_io::ReadPmmCameraKey<pmm_io::PmmStream::V2>(fd, edited);
        Check(edited.frame == 23 && edited.distance == -62.5f && edited.fov == 51 &&
              !edited.previous && !edited.next, "new default world edit survives actual save and segment readback");
    }
    Check(LoadSceneFile(stale.c_str()), "complete production loader reopens the frozen old scene after its original model was disposed");
    Check(app->ModelSlot(0) && mdl::Mdl(app->ModelSlot(0))->boneCount == 1 &&
          mdl::Mdl(app->ModelSlot(0))->boneKeys[0].position[0] == -3.5f &&
          !std::wcscmp(mdl::Mdl(app->ModelSlot(0))->path, source.c_str()) &&
          !std::wcscmp(app->EnvFileName(), stale.c_str()) && !app->sceneRecoveryPending,
          "full old PMM reload restores the actual generated model source and frozen native key");
    Owner(stale, 0); ProjectBytes(first, oldCharge); ProjectBytes(replacement, newCharge);
    Check(LoadSceneFile(replacement.c_str()), "complete production loader reopens the newly saved reset scene");
    const auto reopenedKey = app->CameraKeys()[0].next;
    Check(!app->ModelSlot(0) && reopenedKey && reopenedKey < mdl::KeyPoolCapacity(app->CameraKeys()) &&
          app->CameraKeys()[reopenedKey].frame == 23 && app->CameraKeys()[reopenedKey].distance == -62.5f &&
          !std::wcscmp(app->EnvFileName(), replacement.c_str()) && !app->sceneRecoveryPending,
          "full reset-scene reload restores its own camera edit and healthy project identity");
    Owner(replacement, newCharge); ProjectBytes(first, oldCharge);
    HiddenResetBoundary();
    std::puts("CACHE_RESET real-reset=1 old-model-pools-retired=1 frozen-native-bytes=1 new-edit=1 stale-owner-rejected=1 full-reload=2");
}

BOOL CancelPicker(LPOPENFILENAMEW picker) {
    Check(picker && picker->hwndOwner == app->MainWindow() && picker->lpstrFile &&
          picker->nMaxFile == 256 && !picker->lpstrFile[0], "real SaveSceneAs reaches its initially empty picker");
    ++cancelledPickers;
    return FALSE; // Only cancellation is injected; never fabricate a selection.
}
#endif
} // namespace host_cache_save

bool CacheSaveFailureMessage(HWND window, LPCWSTR text, LPCWSTR caption, UINT flags) {
    if (!host_cache_save::expectFailure) return false;
    Check(window == app->MainWindow() && app->EnglishUI() == 1 && text && caption &&
          !std::wcscmp(caption, L"PMM") && flags == (MB_OK | MB_ICONERROR) &&
          !std::wcscmp(text, L"The PMM file could not be saved completely. Check free space and write access."),
          "only the exact expected production save-failure notification is intercepted");
    ++host_cache_save::failureMessages;
    return true;
}

bool CacheSaveRequested() {
    const auto* value = _wgetenv(L"MMS_PMM_CACHE_SAVE_ONLY");
    if (!value) return false;
    Check(!std::wcscmp(value, L"1"), "cache save mode is explicitly enabled with value 1");
    Check(!MMD_ORIGINAL_PROFILE, "cache save ownership requires the full profile");
    return true;
}

bool CacheResetRequested() {
    const auto* value = _wgetenv(L"MMS_PMM_CACHE_RESET_ONLY");
    if (!value) return false;
    Check(!std::wcscmp(value, L"1") && !MMD_ORIGINAL_PROFILE,
          "real Reset mode requires explicit value 1 and full profile");
    Check(!_wgetenv(L"MMS_PMM_CACHE_SAVE_ONLY"), "real Reset and private-desktop save modes are mutually exclusive");
    return true;
}

void PrepareCacheResetHost(const fs::path& executable) {
#if !MMD_ORIGINAL_PROFILE
    const auto root = executable.parent_path();
    Check(fs::equivalent(fs::current_path(), root), "real Reset must run in the runner's staged EXE directory");
    // The unchanged runner stages only the EXE and opens result.log before
    // launching. Refuse installations, prior runs, plugins and user settings.
    for (const auto& entry : fs::directory_iterator(root))
        Check(entry.is_regular_file() && (entry.path() == executable || entry.path().filename() == L"result.log") &&
              !(GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT),
              "real Reset requires a fresh runner directory containing only its EXE and log");
    Check(SetEnvironmentVariableW(L"MMS_PLUGIN_TEST_DIR", (root / L"PLUGINS").c_str()) &&
          SetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE", L"1") &&
          SetEnvironmentVariableW(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG", (root / L"data" / L"reset-autosave.ini").c_str()),
          "exclude inherited external plugin roots and unrelated model cache producers");
    // Diagnostic builds also honor CRT environment variables; keep their
    // optional writes away from any inherited application/project directory.
    Check(_putenv_s("MIKUMIKUSTUDIO_PMM_TRACE_DIR", "") == 0 &&
          _putenv_s("MIKUMIKUSTUDIO_SPIN_CANARY", "") == 0 &&
          _putenv_s("MIKUMIKUSTUDIO_TRACE_FILE", "") == 0,
          "disable inherited external model trace destinations");
    host_cache_save::inheritedDesktop = GetThreadDesktop(GetCurrentThreadId());
    Check(host_cache_save::inheritedDesktop != nullptr, "capture inherited desktop without switching it");
    std::puts("STAGE CACHE_RESET inherited-desktop real-D3D hidden-host; initialization failure is fatal, no CPU fallback");
#endif
}

void PrepareCacheSaveHost(const fs::path& executableDirectory) {
    Check(!fs::exists(executableDirectory / L"data"), "cache save mode requires a fresh staged EXE with no data directory");
    Check(!fs::exists(executableDirectory / L"MMV4MMS.dll") && !fs::exists(executableDirectory / L"d3d9.dll"),
          "empty-scene fixture excludes local MMV and MME renderer plugins");
#if !MMD_ORIGINAL_PROFILE
    Check(system_settings::Path() == executableDirectory / L"data" / L"settings.json",
          "host settings resolve only inside the fresh executable directory");
#endif
    Check(SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", L"0") &&
          SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DISABLE", L"1") &&
          SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1"),
          "enable isolated disk ownership and disable unrelated cache producers before host init");
}

void InitCacheSaveEmptyHost() {
#if !MMD_ORIGINAL_PROFILE
    std::puts("STAGE cache save minimal host: scope=empty-scene no-render-device=1 full-reset=not-covered");
    Check(!app->Renderer() && !app->Physics(), "default initialization did not create a rendering device");
    const auto module = GetModuleHandleW(nullptr);
    constexpr wchar_t className[] = L"MMS-PmmCacheSaveEmptyHost";
    WNDCLASSW wc{}; wc.hInstance = module; wc.lpfnWndProc = host_cache_save::EmptyWindow; wc.lpszClassName = className;
    Check(RegisterClassW(&wc) != 0, "register private empty-save message owner");
    const auto window = CreateWindowExW(0, className, L"Empty save fixture", WS_OVERLAPPED,
                                       0, 0, 640, 480, nullptr, nullptr, module, nullptr);
    Check(window && !IsWindowVisible(window), "create hidden message owner on the already verified private desktop");
    app->Hwnd() = window;
    const auto child = [&](const wchar_t* type, int id, const wchar_t* value, DWORD style = 0) {
        const auto control = CreateWindowExW(0, type, value, WS_CHILD | style, 0, 0, 100, 24,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module, nullptr);
        Check(control != nullptr, "create real control required by the native PMM writer");
        return control;
    };
    const auto models = child(L"COMBOBOX", panel::kMainComboModel, L"", CBS_DROPDOWNLIST);
    Check(SendMessageW(models, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Camera / Light")) == 0 &&
          SendMessageW(models, CB_SETCURSEL, 0, 0) == 0, "model selector contains only the global-track entry");
    child(L"COMBOBOX", panel::kAccessoryCombo, L"", CBS_DROPDOWNLIST);
    child(L"EDIT", panel::kPlayStartFrameEdit, L"0");
    child(L"EDIT", panel::kPlayStopFrameEdit, L"300");
    child(L"EDIT", panel::kGotoFrameEdit, L"0");
    app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>();
    app->LightKeys() = mdl::KeyPoolCreate<mdl::LightKey>();
    app->ShadowKeys() = mdl::KeyPoolCreate<mdl::SelfShadowKey>();
    app->GravityKeys() = mdl::KeyPoolCreate<mdl::GravityKey>();
    app->state.renderW = 640; app->state.renderH = 480; app->state.sidebarWidth = 360;
    app->SetSelectedModelSlot(0); app->state.optflag[0] = 1;
    Check(wcscpy_s(app->ExeDir(), 256, directory.parent_path().parent_path().c_str()) == 0,
          "minimal host uses its isolated executable directory for picker cancellation");
#endif
}

void ShutdownCacheSaveEmptyHost() {
#if !MMD_ORIGINAL_PROFILE
    background_save::Shutdown(app);
    autosave::Shutdown();
    mmv4mms::Shutdown();
    mdl::KeyPoolDestroy(app->CameraKeys());
    mdl::KeyPoolDestroy(app->LightKeys());
    mdl::KeyPoolDestroy(app->ShadowKeys());
    mdl::KeyPoolDestroy(app->GravityKeys());
    plugins::background_identity::Forget(app);
    app->Hwnd() = nullptr;
    Check(UnregisterClassW(L"MMS-PmmCacheSaveEmptyHost", GetModuleHandleW(nullptr)) != FALSE,
          "release the private empty-save window class after destroying its windows");
#endif
}

void CacheSaveHostCases() {
#if !MMD_ORIGINAL_PROFILE
    using namespace host_cache_save;
    Check(!monitor && !host_cache::touchTail && !host_cache::finishes && !SceneSaveInProgress(),
          "save cases start without parser fault injection or a previous writer");
    Check(cache_manager::Root() == host_cache::base / L"Cache", "save cases use the fresh pre-init cache root");
    app->EnglishUI() = 1;
    auto settings = autosave::Current();
    settings.enabled = false; settings.keepPrevious = false; settings.numberedManualVersions = false;
    std::wstring error;
    Check(autosave::Configure(app, settings, error), "configure deterministic isolated save publication");
    const auto first = directory / L"save-first.pmm", second = directory / L"save-as.pmm";
    const auto copied = directory / L"save-copy.pmm", backup = directory / L"save-backup.pmm";
    const auto rejected = directory / L"absent-parent" / L"save-failed.pmm";
    const auto stale = directory / L"save-stale.pmm", replacement = directory / L"save-replacement.pmm";

    std::puts("STAGE cache save: synchronous anonymous first save");
    EmptySceneBoundary();
    const auto firstBytes = Publish(L'4', 64 * 1024);
    Owner({}, 0, first);
    wcscpy_s(app->EnvFileName(), 256, first.c_str());
    app->SceneModified() = 1;
    Check(SaveSceneFile(app) && !app->SceneModified(), "synchronous production capture/write/completion succeeds");
    SavedFile(first); Owner(first, firstBytes);
    const auto generation = app->SceneGeneration();

    std::puts("STAGE cache save: asynchronous save-as and newer edits");
    app->SceneModified() = 1;
    const auto revision = app->SceneRevision();
    {
        Worker write(second);
        Check(!fs::exists(second), "gated writer has not yet created its new destination");
        Owner(first, firstBytes, second);
        app->CameraFov() += 5; app->SceneModified() = 1;
        Check(app->SceneRevision() != revision, "new live edit advances the real scene revision");
        write.Finish(true);
    }
    Check(app->SceneGeneration() == generation && app->SceneModified() &&
          !std::wcscmp(app->EnvFileName(), second.c_str()), "accepted save-as keeps newer edits dirty in the same generation");
    SavedFile(second); Owner(second, firstBytes);
    {
        Worker write(second); write.Finish(true);
    }
    Check(!app->SceneModified(), "unchanged asynchronous ordinary save clears dirty");
    Owner(second, firstBytes);

    std::puts("STAGE cache save: synchronous and asynchronous copies do not rebind");
    app->SceneModified() = 1;
    const auto copyRevision = app->SceneRevision();
    Check(SaveSceneCopy(app, copied.c_str()), "real synchronous backup copy succeeds");
    SavedFile(copied); Owner(second, firstBytes, copied);
    { Worker write(backup, true); write.Finish(true); }
    SavedFile(backup); Owner(second, firstBytes, backup);
    Check(app->SceneModified() && app->SceneRevision() == copyRevision &&
          !std::wcscmp(app->EnvFileName(), second.c_str()), "both copy paths preserve live identity and dirty state");

    std::puts("STAGE cache save: actual SaveSceneAs picker cancellation");
    const auto cwd = fs::current_path();
    Hook(&GetUiSaveFileNameW, &CancelPicker);
    Check(!SaveSceneAs(app, app->MainWindow()) && cancelledPickers == 1 && !SceneSaveInProgress(),
          "real save-as returns cancellation without scheduling a writer");
    Check(MH_DisableHook(reinterpret_cast<void*>(&GetUiSaveFileNameW)) == MH_OK,
          "remove the scoped cancellation injection");
    fs::current_path(cwd);
    Check(app->SceneGeneration() == generation && app->SceneModified() && app->SceneRevision() == copyRevision &&
          !std::wcscmp(app->EnvFileName(), second.c_str()), "picker cancellation preserves live scene identity and edits");
    Owner(second, firstBytes);

    std::puts("STAGE cache save: genuine asynchronous and synchronous disk failures");
    Check(!fs::exists(rejected.parent_path()), "failure target has an actually absent parent");
    expectFailure = true;
    { Worker write(rejected); write.Finish(false); }
    expectFailure = false;
    Check(failureMessages == 1 && !fs::exists(rejected) && app->SceneGeneration() == generation && app->SceneModified() &&
          !std::wcscmp(app->EnvFileName(), second.c_str()), "failed save-as leaves the accepted path and edits intact");
    Owner(second, firstBytes, rejected);
    const auto previous = Bytes(second);
    HANDLE lock = CreateFileW(second.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "deny replacement of the actual synchronous save target");
    expectFailure = true;
    const bool saved = SaveSceneFile(app);
    expectFailure = false;
    Check(CloseHandle(lock) != FALSE, "release only the fixture's publication lock");
    Check(!saved && failureMessages == 2 && Bytes(second) == previous && app->SceneModified(),
          "failed synchronous atomic replacement preserves file bytes and dirty state");
    Owner(second, firstBytes);

    std::puts("STAGE cache save: old completion after fixture generation replacement (not full Reset)");
    std::uint64_t replacementBytes = 0;
    {
        Worker write(stale);
        EmptySceneBoundary();
        Check(app->SceneGeneration() != generation &&
              cache_manager::CaptureActiveScene() && !*app->EnvFileName(),
              "fixture generation replaces the captured empty scene while its disk writer is gated");
        replacementBytes = Publish(L'5', 128 * 1024);
        app->SceneModified() = 1;
        Owner({}, 0, stale);
        write.Finish(true);
    }
    SavedFile(stale);
    Check(!*app->EnvFileName() && app->SceneModified(), "successful old disk publication cannot rename or clean the new scene");
    Owner({}, 0, stale); ProjectBytes(second, firstBytes);
    { Worker write(replacement); Owner({}, 0, replacement); write.Finish(true); }
    SavedFile(replacement);
    Check(!std::wcscmp(app->EnvFileName(), replacement.c_str()) && !app->SceneModified(),
          "replacement scene's real asynchronous first save commits its own identity");
    Owner(replacement, replacementBytes, stale); ProjectBytes(second, firstBytes);
    EmptySceneBoundary();
    Owner({}, 0);
    ModelSaveCases();
#endif
}
