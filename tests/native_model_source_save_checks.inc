// CPU integration contracts for the actual command route; separate from the real InitMainWindow host.
// 在 full native_edit_history_tests.cpp 的 native_model_source_path_checks.inc
// 之后包含：该位置已有 ObjectCommandFixture、model_source_checks、MainWndProc、
// MinHook 和原 serializer/纹理专项声明。
// 原 STATIC 自有 HWND 直接调用 MainWndProc 的 263 分派，不是 InitMainWindow
// 完整宿主；不声称 Manager fallback、PMM 重开、GPU 或真机兼容已验收。
// 不替换 CmdViewMenu、writer、journal 或其结果。目录与文件全部由此夹具独占，
// 不删除失败产物，保留现场。只拦原保存对话框，IAT 观察 _close/MoveFileExW 并转发。
#include "../src/window/enhanced_model_save.hpp"
#include "mikumikustudio/subrecord_layout.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <MinHook.h>
#include <commdlg.h>
#include <io.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if !MMD_ORIGINAL_PROFILE
namespace model_source_save_checks {
using namespace plugins;
namespace fs = std::filesystem;
using model_source_checks::Path;
using model_source_checks::Read;
using model_source_checks::Replaced;
using model_source_checks::Same;

struct Files {
    fs::path directory, target;
    Files() {
        wchar_t temp[32768]{}; const auto count = GetTempPathW(32768, temp);
        Check(count && count < 32768, "263 has a native temporary root");
        static unsigned serial = 0; bool created = false;
        for (unsigned n = 0; n < 128 && !created; ++n) {
            directory = fs::path(temp) / (L"mms-source-263-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial));
            created = CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
            if (!created) Check(GetLastError() == ERROR_ALREADY_EXISTS, "263 only retries a colliding owned-directory name");
        }
        Check(created, "263 exclusively creates its own asset directory");
        target = directory / L"enhanced.pmd";
        Check(target.wstring().size() < 256, "263 destination fits the original fixed path field");
    }
    void Sentinel() const {
        std::ofstream out(target, std::ios::binary | std::ios::trunc); out << "OLD OWNED MODEL FILE\n";
        Check(bool(out), "263 writes its own old destination sentinel");
    }
    std::vector<unsigned char> Bytes() const {
        std::ifstream in(target, std::ios::binary); Check(bool(in), "263 reads only its own existing output");
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    std::vector<fs::path> Entries() const {
        std::vector<fs::path> result; for (const auto& entry : fs::directory_iterator(directory)) result.push_back(entry.path().filename());
        std::sort(result.begin(), result.end()); return result;
    }
};

struct Fixture : model_source_checks::Fixture {
    Fixture() {
        auto& m = *mdl::Mdl(app->ModelSlot(0));
        Check(!m.boneTable && !m.rawVertices && !m.indices && !m.materials && !m.boneCount && !m.morphCount,
            "263 extends only the original fixture's empty standard PMD CPU allocations");
        m.boneCount = 1; m.boneTable = static_cast<mdl::BoneRecord*>(std::calloc(1, sizeof(mdl::BoneRecord)));
        m.vertexCount = 3; m.rawVertices = static_cast<mdl::PmdVertex*>(std::calloc(3, sizeof(mdl::PmdVertex)));
        m.indexCount = 3; m.indices = std::calloc(3, sizeof(uint32_t));
        m.materialCount = 1; m.materials = std::calloc(1, sizeof(mdl::ModelMaterialRecord));
        Check(m.boneTable && m.rawVertices && m.indices && m.materials, "263 owns complete calloc PMD payloads released by original DeleteModel");
        std::strcpy(m.boneTable[0].name, "root"); std::strcpy(m.boneTable[0].nameEn, "root");
        m.boneTable[0].parent = m.boneTable[0].tailBone = -1; m.boneTable[0].tailIdx = -1; m.boneTable[0].rotQuat[3] = 1;
        for (unsigned i = 0; i < 3; ++i) {
            m.rawVertices[i].position[0] = float(i); m.rawVertices[i].position[1] = float(i == 2);
            m.rawVertices[i].normal[1] = 1; m.rawVertices[i].weightPercent = 100;
            static_cast<uint32_t*>(m.indices)[i] = i;
        }
        auto& material = *static_cast<mdl::ModelMaterialRecord*>(m.materials);
        std::fill_n(material.diffuse, 4, 1.f); material.faceVertexCount = 3; material.toonReference = 0xff;
        m.groupCount = 2; // 两个内部固定组，文件中实际自定义组数为零。
        Check(!app->Renderer() && !material.texturePath[0] && !material.spherePath[0] && !material.toonPath[0],
            "263 uses real CPU serializer data with no renderer or texture path");
    }
};

// 标准 PMD 完整结构校验，使用有界读取，不只检查三字节魔数。
void CompletePmd(const Files& files) {
    const auto bytes = files.Bytes(); size_t at = 0;
    const auto skip = [&](size_t count) { Check(at <= bytes.size() && count <= bytes.size() - at, "263 PMD section is complete"); at += count; };
    const auto integer = [&](unsigned width) {
        Check(width <= 4 && at <= bytes.size() && width <= bytes.size() - at, "263 PMD count is complete");
        uint32_t value = 0; std::memcpy(&value, bytes.data() + at, width); at += width; return value;
    };
    Check(bytes.size() >= 7 && !std::memcmp(bytes.data(), "Pmd", 3), "263 publishes an actual PMD header");
    at = 3; float version = 0; std::memcpy(&version, bytes.data() + at, 4); at += 4;
    Check(version == 1.f, "263 retains original PMD version 1"); skip(20 + 256);
    Check(integer(4) == 3, "263 writes all three fixture vertices"); skip(3 * 38);
    Check(integer(4) == 3, "263 writes all native DWORD-slot indices as packed WORDs");
    for (unsigned i = 0; i < 3; ++i) Check(integer(2) == i, "263 preserves each original CPU index");
    Check(integer(4) == 1, "263 writes one original material"); skip(70);
    Check(integer(2) == 1, "263 writes one original bone"); skip(39);
    Check(!integer(2) && !integer(2) && !integer(1) && !integer(1) && !integer(4), "263 writes complete empty IK/morph/display sections");
    Check(integer(1) == 1, "263 writes the English section flag"); skip(20 + 256 + 20);
    skip(10 * 100); Check(!integer(4) && !integer(4) && at == bytes.size(), "263 completes Toon/rigid/joint footer with no truncation or trailing private fields");
}

struct Snapshot {
    Fixture& f; model_source_checks::Snapshot source;
    model_source_checks::Bytes vertices, indices, materials, bones;
    uint64_t historyRevision; size_t undo, redo; uint64_t retained;
    explicit Snapshot(Fixture& value, SharedHistory& history) : f(value), source(value),
        vertices(mdl::Mdl(value.app->ModelSlot(0))->rawVertices, 3 * sizeof(mdl::PmdVertex)),
        indices(mdl::Mdl(value.app->ModelSlot(0))->indices, 3 * sizeof(uint32_t)),
        materials(mdl::Mdl(value.app->ModelSlot(0))->materials, sizeof(mdl::ModelMaterialRecord)),
        bones(mdl::Mdl(value.app->ModelSlot(0))->boneTable, sizeof(mdl::BoneRecord)),
        historyRevision(0), undo(history.UndoCount()), redo(history.RedoCount()), retained(history.RetainedBytes()) {
        Check(history.Revision(historyRevision) == MMS_OK, "263 captures actual journal revision");
    }
    void Data(const Path& a, bool published) const {
        auto& app = *f.app; const auto& m = *mdl::Mdl(app.ModelSlot(0));
        source.models[0].ExceptPath(app.ModelSlot(0), a); source.models[1].ExceptPath(app.ModelSlot(1), source.paths[1]);
        vertices.Unchanged(m.rawVertices, 3 * sizeof(mdl::PmdVertex)); indices.Unchanged(m.indices, 3 * sizeof(uint32_t));
        materials.Unchanged(m.materials, sizeof(mdl::ModelMaterialRecord)); bones.Unchanged(m.boneTable, sizeof(mdl::BoneRecord));
        for (unsigned i = 0; i < 2; ++i) {
            source.accessories[i].Unchanged(app.AccessorySlot(i), sizeof(mdl::AccessoryRecord));
            source.accessoryKeys[i].Unchanged(app.AccessoryKeys(i), mdl::KeyPoolCapacity(app.AccessoryKeys(i)) * sizeof(mdl::AccessoryKey));
        }
        const auto values = model_source_checks::Snapshot::Values(app);
        Check(source.scene == app.SceneGeneration() && source.message == app.MessageSeen() && source.frame == app.CurrentFrame() &&
            source.mode == app.CameraMode() && source.selectedModel == app.SelectedModelSlot() && source.selectedAccessory == app.SelectedAccessorySlot() &&
            source.timeLow == app.TimeNowLow() && source.timeHigh == app.TimeNowHigh() && !std::memcmp(values.data(), source.values.data(), sizeof(values)) &&
            source.physics == app.PhysicsResetPending() && source.enhanced == app.EnhancedModelDirty() && source.input == app.ViewportInputActive() &&
            app.WindowLayoutReady() && !app.SceneMutationInProgress(), "263 preserves unrelated source payloads, directory buffers, physics, Enhanced and editor transport/time");
        if (app.SceneRevision() != source.revision + unsigned(published))
            std::printf("MODEL_SOURCE_263 revision before=%llu after=%llu published=%u\n",
                static_cast<unsigned long long>(source.revision), static_cast<unsigned long long>(app.SceneRevision()), unsigned(published));
        // SceneModified is a proxy whose assignment advances SceneRevision.
        // This counter is an edit-version check, not a pose evaluation count.
        Check(app.SceneRevision() == source.revision + unsigned(published) &&
            app.SceneModified() == (published ? 1 : source.dirty) && app.RecordedFrameCountByte1() == (published ? 1 : source.recorded),
            "263 changes project dirty scalars and their revision once per successful source publication");
        f.CwdUnchanged();
    }
    void HistoryUnchanged(SharedHistory& history) const {
        uint64_t revision = 0;
        Check(history.Revision(revision) == MMS_OK && revision == historyRevision && history.UndoCount() == undo &&
            history.RedoCount() == redo && history.RetainedBytes() == retained, "263 refused/no-op/cancel path preserves real journal and redo");
    }
    void Unchanged(SharedHistory& history) const { Data(source.paths[0], false); HistoryUnchanged(history); }
};

enum class DialogAction { Accept, Cancel, OtherModel, RebindSame, TailStale, CloseCancel };
struct Context {
    Fixture& f; SharedHistory& history; Files& files;
    DialogAction action = DialogAction::Accept;
    unsigned dialogs = 0, errors = 0, temporaryCloses = 0, moves = 0, successes = 0;
    bool savedError = false, mutateAfterClose = false, rejectAfterMove = false, gateBusy = false;
    unsigned rejectedGateCalls = 0;
};

struct Errors {
    static inline Context* active = nullptr;
    HHOOK hook = nullptr;
    explicit Errors(Context& context) {
        Check(!active, "263 has one own-thread PMD error observer"); active = &context;
        hook = SetWindowsHookExW(WH_CALLWNDPROCRET, Inspect, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "263 observes completion of its own modal initialization without relying on activation");
    }
    ~Errors() { if (hook) UnhookWindowsHookEx(hook); active = nullptr; }
    static LRESULT CALLBACK Inspect(int code, WPARAM wp, LPARAM lp) {
        const auto* returned = code >= 0 ? reinterpret_cast<const CWPRETSTRUCT*>(lp) : nullptr;
        if (returned && returned->message == WM_INITDIALOG && active) {
            auto& c = *active; const auto window = returned->hwnd;
            wchar_t name[32]{}, caption[64]{}; GetClassNameW(window, name, 32); GetWindowTextW(window, caption, 64);
            if (!std::wcscmp(name, L"#32770")) {
                Check(!std::wcscmp(caption, L"PMD") && GetWindow(window, GW_OWNER) == c.f.app->MainWindow(), "263 accepts only a real PMD error owned by its own fixture HWND");
                std::printf("MODEL_SOURCE_263 own error initialized saved=%u\n", unsigned(c.savedError));
                ++c.errors;
                std::puts("MODEL_SOURCE_263 inspect released operation");
                Check(!NativeObjectOperationActive(c.f.app.get()) && !NativeObjectPublicationAuthorized(c.f.app.get()) &&
                    !bake::UiLocked() && c.history.CheckNativeNavigation() == MMS_OK,
                    "263 releases Operation, candidate and pending journal token before showing the real error");
                std::puts("MODEL_SOURCE_263 inspect localized error text");
                wchar_t body[2048]{}; GetDlgItemTextW(window, 0xffff, body, 2048);
                Check(std::wcsstr(body, c.savedError ? L"模型文件已保存" : L"模型保存未能完成") != nullptr &&
                    (!c.savedError || std::wcsstr(body, L"已保留保存的文件")), "263 real PMD error distinguishes written output from refused source switching");
                Check(EndDialog(window, IDOK) != FALSE, "263 ends only its own fully initialized real error dialog");
                std::puts("MODEL_SOURCE_263 ended own initialized dialog");
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
};

struct Dialog {
    static inline Context* active = nullptr;
    bool initialized = false;
    explicit Dialog(Context& context) {
        Check(!active && CommDlgExtendedError() == 0, "263 cancel fixture starts with the genuine common-dialog error state clear"); active = &context;
        const auto status = MH_Initialize(); initialized = status == MH_OK;
        Check(initialized || status == MH_ERROR_ALREADY_INITIALIZED, "263 uses the current process's MinHook runtime");
        Check(MH_CreateHook(reinterpret_cast<void*>(&GetUiSaveFileNameW), reinterpret_cast<void*>(&Choose), nullptr) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(&GetUiSaveFileNameW)) == MH_OK, "263 controls only the original save-file dialog result");
    }
    ~Dialog() {
        MH_DisableHook(reinterpret_cast<void*>(&GetUiSaveFileNameW)); MH_RemoveHook(reinterpret_cast<void*>(&GetUiSaveFileNameW));
        active = nullptr; if (initialized) MH_Uninitialize();
    }
    static BOOL Choose(LPOPENFILENAMEW value) {
        Check(active && value, "263 actual route reaches its owned save dialog"); auto& c = *active; ++c.dialogs;
        Check(value->hwndOwner == c.f.app->MainWindow() && value->nMaxFile == 256 && (value->Flags & OFN_NOCHANGEDIR) &&
            NativeObjectOperationActive(c.f.app.get()) && !NativeObjectPublicationAuthorized(c.f.app.get()) && bake::UiLocked() &&
            !c.f.app->SceneMutationInProgress() && IsWindowEnabled(c.f.app->MainWindow()),
            "263 real lightweight operation keeps the input gate without a scene publication or disabling its own window");
        if (c.action == DialogAction::OtherModel) c.f.app->SetSelectedModelSlot(1);
        if (c.action == DialogAction::RebindSame) {
            const auto generation = BoundNativePoseGeneration(c.f.app.get());
            { NativePoseTestBinding nested(*c.f.app, c.history); }
            Check(BoundNativePoseHistory(c.f.app.get()) == &c.history && BoundNativePoseGeneration(c.f.app.get()) != generation,
                "263 stale fixture restores the same binding pointer but advances its actual generation");
        }
        if (c.action == DialogAction::TailStale) mdl::Mdl(c.f.app->ModelSlot(0))->path[251] ^= 1;
        if (c.action == DialogAction::CloseCancel) {
            SendMessageW(c.f.app->MainWindow(), WM_CLOSE, 0, 0);
            Check(IsWindow(c.f.app->MainWindow()), "263 own close is deferred while the modal operation is active");
        }
        if (c.action == DialogAction::Cancel || c.action == DialogAction::CloseCancel) return FALSE;
        const auto path = c.files.target.wstring(); Check(path.size() < value->nMaxFile, "263 dialog destination is within the original capacity");
        std::wmemcpy(value->lpstrFile, path.c_str(), path.size() + 1); return TRUE;
    }
    void Run(Context& c, unsigned notification = 0) {
        const auto window = c.f.app->MainWindow(); DWORD process = 0;
        const auto thread = GetWindowThreadProcessId(window, &process);
        Check(notification <= 1 && IsWindow(window) && process == GetCurrentProcessId() && thread == GetCurrentThreadId() &&
            g_Block == c.f.app.get() && c.f.app->Hwnd() == window, "263 calls production MainWndProc only on its own STATIC fixture window and app");
        const auto before = c.dialogs;
        const auto result = MainWndProc(window, WM_COMMAND, MAKEWPARAM(263, notification), 0);
        Check(result == 0 && c.dialogs == before + 1, "263 actual WndProc/CommandDispatch reaches the real save dialog exactly once");
    }
};

// IAT patching只作用当前 EXE 的命名导入槽。/MT 或缺少命名导入时明确失败，
// 不改用替代 writer、伪造 _close 结果或钩其他模块来“补足”证据。
void** Import(const char* requested) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)); auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    Check(dos->e_magic == IMAGE_DOS_SIGNATURE, "263 own process has a PE DOS header");
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew); Check(nt->Signature == IMAGE_NT_SIGNATURE, "263 own image has a PE NT header");
    const auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    Check(rva && rva < nt->OptionalHeader.SizeOfImage, "263 own process has a bounded import directory");
    auto* library = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + rva);
    for (; library->Name; ++library) if (library->OriginalFirstThunk) {
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + library->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + library->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
            auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (!std::strcmp(reinterpret_cast<const char*>(name->Name), requested)) return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}
struct Iat {
    using Close = int (__cdecl*)(int); using Move = BOOL (WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
    static inline Iat* active = nullptr;
    Context& c; void** closeSlot = nullptr; void** moveSlot = nullptr; Close close = nullptr; Move move = nullptr;
    DWORD closeProtection = 0, moveProtection = 0;
    static void Put(void** slot, void* value) {
        DWORD prior = 0, discarded = 0;
        Check(VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &prior) != FALSE, "263 exposes only its own IAT slot for writing");
        *slot = value; Check(VirtualProtect(slot, sizeof(*slot), prior, &discarded) != FALSE, "263 restores original IAT page protection");
    }
    explicit Iat(Context& value) : c(value) {
        Check(!active, "263 has a single own-IAT observer");
        closeSlot = Import("_close"); moveSlot = Import("MoveFileExW");
        Check(closeSlot && moveSlot, "263 requires real named _close and MoveFileExW imports for this timing proof");
        MEMORY_BASIC_INFORMATION a{}, b{}; Check(VirtualQuery(closeSlot, &a, sizeof(a)) && VirtualQuery(moveSlot, &b, sizeof(b)), "263 captures IAT protections");
        closeProtection = a.Protect; moveProtection = b.Protect;
        close = reinterpret_cast<Close>(*closeSlot); move = reinterpret_cast<Move>(*moveSlot); active = this;
        Put(closeSlot, reinterpret_cast<void*>(&Closed)); Put(moveSlot, reinterpret_cast<void*>(&Moved));
    }
    ~Iat() {
        Put(closeSlot, reinterpret_cast<void*>(close)); Put(moveSlot, reinterpret_cast<void*>(move));
        MEMORY_BASIC_INFORMATION a{}, b{};
        Check(*closeSlot == reinterpret_cast<void*>(close) && *moveSlot == reinterpret_cast<void*>(move) &&
            VirtualQuery(closeSlot, &a, sizeof(a)) && VirtualQuery(moveSlot, &b, sizeof(b)) &&
            a.Protect == closeProtection && b.Protect == moveProtection, "263 restores actual IAT pointers and protections"); active = nullptr;
    }
    static int __cdecl Closed(int file) {
        auto& s = *active; wchar_t path[32768]{};
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file));
        const auto length = handle != INVALID_HANDLE_VALUE ? GetFinalPathNameByHandleW(handle, path, 32768, FILE_NAME_NORMALIZED) : 0;
        const wchar_t* normalized = !std::wcsncmp(path, L"\\\\?\\", 4) ? path + 4 : path;
        const auto directoryLength = std::wcslen(s.c.files.directory.c_str());
        const bool owned = length && length < 32768 && std::wcslen(normalized) > directoryLength &&
            !_wcsnicmp(normalized, s.c.files.directory.c_str(), directoryLength) &&
            (normalized[directoryLength] == L'\\' || normalized[directoryLength] == L'/');
        const int result = s.close(file); // 必须先真的成功关闭，再改变自有旧来源 tail。
        if (owned && result == 0) {
            ++s.c.temporaryCloses;
            if (s.c.mutateAfterClose) { s.c.mutateAfterClose = false; mdl::Mdl(s.c.f.app->ModelSlot(0))->path[251] ^= 1; }
        }
        return result;
    }
    static BOOL WINAPI Moved(LPCWSTR before, LPCWSTR after, DWORD flags) {
        auto& s = *active; const bool owned = after && !_wcsicmp(after, s.c.files.target.c_str());
        const BOOL result = s.move(before, after, flags); // 从不替换真实 rename 的结果。
        if (owned) {
            ++s.c.moves; s.c.successes += result != FALSE;
            if (result && s.c.rejectAfterMove) {
                s.c.gateBusy = true;
            }
        }
        return result;
    }
};

void RoundTripAndNoop() {
    for (auto mode : {SharedHistory::RecordingMode::AllOperations, SharedHistory::RecordingMode::ExplicitOnly}) {
        Files files; Fixture f; NativePoseTestBinding binding(*f.app, f.history); Context c{f, f.history, files};
        Check(f.history.SetMode(mode) == MMS_OK, "263 explicit save participates in both actual history modes");
        const Snapshot before(f, f.history); const auto after = Replaced(before.source.paths[0], files.target.c_str());
        { Errors errors(c); Dialog dialog(c); Iat io(c); dialog.Run(c, mode == SharedHistory::RecordingMode::ExplicitOnly ? 1 : 0); }
        Check(!c.errors && c.temporaryCloses == 1 && c.moves == 1 && c.successes == 1 && f.history.UndoCount() == before.undo + 1 &&
            !f.history.RedoCount() && f.history.LastPublicationEffects() == HistoryEffects::ProjectData && !f.history.LastPublicationRequiresPhysicsReset(),
            "263 actual menu save makes one file and one explicit ProjectData/no-physics node");
        before.Data(after, true); CompletePmd(files); const auto fileBytes = files.Bytes();
        const Snapshot saved(f, f.history);
        Check(RunBoundObjectHistory(*f.app, f.history, false) == MMS_OK, "263 real bound undo restores source only");
        saved.Data(before.source.paths[0], true); Check(files.Bytes() == fileBytes, "263 undo neither deletes nor rewrites the external PMD");
        const Snapshot undone(f, f.history); const auto redoCount = f.history.RedoCount();
        // 改写自己的外部文件后，redo 仍只恢复来源；不会重新导出覆盖现场。
        files.Sentinel(); const auto sentinel = files.Bytes();
        Check(RunBoundObjectHistory(*f.app, f.history, true) == MMS_OK && !f.history.RedoCount() && redoCount == 1, "263 actual redo replays source bytes without reading output");
        undone.Data(after, true); Check(files.Bytes() == sentinel, "263 redo leaves independently changed output bytes untouched");
        const Snapshot again(f, f.history); Context noop{f, f.history, files};
        { Errors errors(noop); Dialog dialog(noop); Iat io(noop); dialog.Run(noop); }
        again.Unchanged(f.history); Check(!noop.errors && noop.moves == 1 && noop.temporaryCloses == 1, "263 no-op source still closes and publishes a real file"); CompletePmd(files);
    }
}

void RedoCancelAndSamePath() {
    Files files; Fixture f; NativePoseTestBinding binding(*f.app, f.history);
    const auto original = Read(f.app->ModelSlot(0));
    const auto same = Replaced(original, files.target.c_str()); std::memcpy(mdl::Mdl(f.app->ModelSlot(0))->path, same.data(), sizeof(same));
    auto change = f.Change((files.directory / L"later.pmd").c_str()); model_source_checks::Commit(f, change);
    Check(RunBoundObjectHistory(*f.app, f.history, false) == MMS_OK && f.history.RedoCount() == 1, "263 fixture owns a real redo branch");
    const Snapshot before(f, f.history); Context c{f, f.history, files}; c.action = DialogAction::Cancel;
    { Errors errors(c); Dialog dialog(c); dialog.Run(c); }
    before.Unchanged(f.history); Check(!c.errors && !fs::exists(files.target), "263 cancel preserves real redo and creates no output");
    c.action = DialogAction::Accept; files.Sentinel();
    { Errors errors(c); Dialog dialog(c); Iat io(c); dialog.Run(c); }
    before.Unchanged(f.history); Check(!c.errors && c.moves == 1 && f.history.RedoCount() == 1, "263 samepath still writes while preserving redo and all old source tail bytes"); CompletePmd(files);
}

void DialogStalenessAndBudget() {
    for (auto action : {DialogAction::OtherModel, DialogAction::RebindSame, DialogAction::TailStale}) {
        Files files; Fixture f; NativePoseTestBinding binding(*f.app, f.history); Context c{f, f.history, files}; c.action = action;
        const Snapshot before(f, f.history); const auto entries = files.Entries();
        { Errors errors(c); Dialog dialog(c); Iat io(c); dialog.Run(c); }
        Check(c.errors == 1 && !c.temporaryCloses && !c.moves && !fs::exists(files.target) && files.Entries() == entries,
            "263 rejects changed selection/binding generation/source tail before opening output and never saves a different model");
        if (action == DialogAction::OtherModel) { Check(f.app->SelectedModelSlot() == 1, "263 preserves the controlled selected-slot change"); f.app->SetSelectedModelSlot(before.source.selectedModel); }
        if (action == DialogAction::TailStale) {
            auto expected = before.source.paths[0]; expected[251] ^= 1; model_source_checks::Match(f.app->ModelSlot(0), expected);
            std::memcpy(mdl::Mdl(f.app->ModelSlot(0))->path, before.source.paths[0].data(), sizeof(Path));
        }
        before.Unchanged(f.history);
    }
    Files files; Fixture f; NativePoseTestBinding binding(*f.app, f.history);
    NativeModelSourcePlan measure; std::shared_ptr<HistoryChange> change;
    Check(measure.Begin(*f.app, 0) == MMS_OK && measure.Set(files.target.c_str()) == MMS_OK && measure.Take(change) == MMS_OK && change && change->RetainedBytes() > 1,
        "263 budget is measured from the real complete retained source participant");
    const auto cost = change->RetainedBytes(); change.reset();
    Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly, 128, cost - 1) == MMS_OK, "263 sets an actual budget below measured native cost");
    const Snapshot before(f, f.history); Context c{f, f.history, files};
    { Errors errors(c); Dialog dialog(c); Iat io(c); dialog.Run(c); }
    before.Unchanged(f.history); Check(c.errors == 1 && !c.temporaryCloses && !c.moves && !fs::exists(files.target) && files.Entries().empty(),
        "263 actual PrepareNative budget failure precedes any temporary file opening");
}

void ReplacementAndLateValidation() {
    {
        Files files; files.Sentinel(); Fixture f; NativePoseTestBinding binding(*f.app, f.history); Context c{f, f.history, files};
        const Snapshot before(f, f.history); const auto old = files.Bytes(); const auto entries = files.Entries();
        HANDLE held = CreateFileW(files.target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "263 locks only replacement of its own old destination");
        { Errors errors(c); Dialog dialog(c); Iat io(c); dialog.Run(c); }
        CloseHandle(held); before.Unchanged(f.history);
        Check(c.errors == 1 && c.temporaryCloses == 1 && c.moves == 1 && !c.successes && files.Bytes() == old && files.Entries() == entries,
            "263 real MoveFileEx refusal preserves old output, source, directory and journal");
    }
    for (bool noop : {false, true}) {
        Files files; files.Sentinel(); Fixture f; NativePoseTestBinding binding(*f.app, f.history);
        if (noop) { const auto same = Replaced(Read(f.app->ModelSlot(0)), files.target.c_str()); std::memcpy(mdl::Mdl(f.app->ModelSlot(0))->path, same.data(), sizeof(same)); }
        Context c{f, f.history, files}; c.mutateAfterClose = true; const Snapshot before(f, f.history);
        const auto old = files.Bytes(); const auto entries = files.Entries();
        { Errors errors(c); Dialog dialog(c); Iat io(c); dialog.Run(c); }
        auto stale = before.source.paths[0]; stale[251] ^= 1; model_source_checks::Match(f.app->ModelSlot(0), stale);
        Check(c.errors == 1 && c.temporaryCloses == 1 && !c.mutateAfterClose && !c.moves && files.Bytes() == old && files.Entries() == entries,
            "263 validator runs after actual successful _close and rejects stale changed/no-op source before MoveFileEx");
        std::memcpy(mdl::Mdl(f.app->ModelSlot(0))->path, before.source.paths[0].data(), sizeof(Path)); before.Unchanged(f.history);
    }
}

void WrittenFileButFinalCommitRefused() {
    Files files; Fixture f; StorageStore storage; Context* gateContext = nullptr;
    SharedHistory journal(storage, [&] {
        if (gateContext && gateContext->gateBusy) { ++gateContext->rejectedGateCalls; return MMS_E_BUSY; }
        if ((NativeObjectOperationActive(f.app.get()) || f.app->SceneMutationInProgress()) && !NativeObjectPublicationAuthorized(f.app.get())) return MMS_E_BUSY;
        return MMS_OK;
    });
    NativePoseTestBinding binding(*f.app, journal); Context c{f, journal, files}; gateContext = &c;
    c.savedError = true; c.rejectAfterMove = true;
    // 独立完整 gate，不替换原 journal；真实 bridge 从当前 binding 捕获此 journal。
    // Move IAT 修改同一个 fixture gate 的 bool，不能假造 CommitNative 的返回值。
    NativeModelSourcePlan probe; Check(probe.Begin(*f.app, 0) == MMS_OK && probe.Journal() == &journal, "263 final-commit probe captures the actual controlled journal");
    const Snapshot before(f, journal);
    { Errors errors(c); Dialog dialog(c); Iat io(c); dialog.Run(c); }
    Check(c.successes == 1 && c.moves == 1 && c.temporaryCloses == 1 && c.gateBusy && c.rejectedGateCalls == 1 && c.errors == 1 && probe.IsCurrent(),
        "263 real file publication precedes an actual gate-refused final commit with current source");
    CompletePmd(files); before.Unchanged(journal);
    Check(!f.history.UndoCount() && !journal.UndoCount() && !journal.RedoCount(), "263 saved-but-unpublished source creates no history in either journal");
    gateContext = nullptr;
    Check(journal.Reset() == MMS_OK, "263 releases controlled source retention before its fixture model owner"); journal.Collect();
}

void DeferredClose() {
    HWND closed = nullptr;
    {
        Files files; Fixture f; NativePoseTestBinding binding(*f.app, f.history); Context c{f, f.history, files}; c.action = DialogAction::CloseCancel;
        const Snapshot before(f, f.history); const auto window = f.app->MainWindow();
        { Errors errors(c); Dialog dialog(c); dialog.Run(c); }
        before.Unchanged(f.history);
        Check(!c.errors && !fs::exists(files.target) && IsWindow(window) && !NativeObjectOperationActive(f.app.get()) && f.history.CheckNativeNavigation() == MMS_OK,
            "263 close cancellation leaves no file/token and returns before destroying the owned fixture window");
        MSG posted{}; Check(PeekMessageW(&posted, window, WM_CLOSE, WM_CLOSE, PM_REMOVE) && posted.hwnd == window,
            "263 operation reposts the exact own close after releasing guards");
        // 只取出自己的延后 close，不调用 MainWndProc 完整应用 teardown，
        // 不泵其他消息。ObjectCommandFixture 正常析构先释放模型，再毁自有窗口。
        closed = window;
    }
    Check(closed && !IsWindow(closed), "263 dedicated close scope safely releases scene ownership before destroying only its own STATIC window");
}
} // namespace model_source_save_checks
static void NativeModelSourceSaveChecks() {
    model_source_save_checks::RoundTripAndNoop();
    model_source_save_checks::RedoCancelAndSamePath();
    model_source_save_checks::DialogStalenessAndBudget();
    model_source_save_checks::ReplacementAndLateValidation();
    model_source_save_checks::WrittenFileButFinalCommitRefused();
    model_source_save_checks::DeferredClose();
}
#else
static void NativeModelSourceSaveChecks() { std::puts("SKIP 263 full source-history bridge in original profile"); }
#endif
