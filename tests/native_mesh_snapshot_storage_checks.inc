// MAINTENANCE: regression-keep; production native-mesh snapshot storage.
// The MME notification boundary is controlled in this process. This does not
// certify the third-party plugin, its hooks, or its effect/resource lifetime.
namespace mme_storage {
namespace fs = std::filesystem;
constexpr UINT kAutoSave = 40022;
constexpr UINT_PTR kMenuSubclass = 0x4d4d5353;
constexpr char kInstallationMarker[] = "MMS MME snapshot test installation v1\n";
fs::path FreshOutput(const wchar_t* argument) {
    const auto marker = bake_storage::InstallDirectory() / L"mme-snapshot-test-installation.txt";
    const auto bytes = bake_storage::Bytes(marker);
    Check(std::string(bytes.begin(), bytes.end()) == kInstallationMarker,
        "mme-storage requires an explicitly marked copied installation");
    return bake_storage::FreshOutput(argument);
}
void NewFile(const fs::path& path, const std::string& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create only a previously absent snapshot test file");
    DWORD written = 0;
    const BOOL saved = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr);
    const BOOL closed = CloseHandle(file);
    Check(saved && written == bytes.size() && closed, "write complete owned snapshot test file");
}
std::wstring Ini(const fs::path& path, const wchar_t* section, const wchar_t* key) {
    wchar_t value[32768]{};
    const DWORD size = GetPrivateProfileStringW(section, key, L"__missing__", value, 32768, path.c_str());
    Check(size < 32766, "read complete controlled EMM property");
    return value;
}
void PutIni(const fs::path& path, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    Check(WritePrivateProfileStringW(section, key, value, path.c_str()) != FALSE,
        "write owned EMM fixture property");
}
struct Context {
    DWORD thread = GetCurrentThreadId();
    HMODULE sentinel = GetModuleHandleW(nullptr);
    fs::path root, model, lastSaved, lastOpened;
    unsigned remaining = 0, fakeQueries = 0, saves = 0, opens = 0, toggles = 0;
    bool armed = false, generateEmm = true;
    HWND window = nullptr;
};
Context* current = nullptr;
decltype(&GetModuleHandleW) realModule = nullptr;
decltype(&NotifyEffectFileSave) realSave = nullptr;
decltype(&NotifyEffectFileOpen) realOpen = nullptr;
HMODULE WINAPI ModuleBoundary(LPCWSTR name) {
    if (current && current->armed && GetCurrentThreadId() == current->thread &&
        name && wcscmp(name, L"MMEffect.dll") == 0 && current->remaining) {
        --current->remaining; ++current->fakeQueries;
        return current->sentinel;
    }
    return realModule(name);
}
void SaveBoundary(const wchar_t* name) {
    if (!current || !current->armed || GetCurrentThreadId() != current->thread) {
        realSave(name); return;
    }
    Check(current->remaining == 0 && current->fakeQueries == 2,
        "real DisableMeshEffects and PreserveEffectAssignments consumed exactly two module gates");
    Check(name && *name, "production snapshot save notification has a path");
    const fs::path pmm(name);
    Check(pmm.is_absolute() && pmm.filename() == L"assignments.pmm" &&
        pmm.parent_path().parent_path() == current->root,
        "actual production snapshot is inside this EXE/data/mme_mesh_snapshots");
    Check(fs::is_directory(pmm.parent_path()), "production created the unique snapshot directory");
    const auto placeholder = bake_storage::Bytes(pmm);
    Check(std::string(placeholder.begin(), placeholder.end()) == "Native mesh assignment transfer\n",
        "production wrote the real assignment transfer placeholder before notifying");
    Check((GetMenuState(GetMenu(current->window), kAutoSave, MF_BYCOMMAND) & MF_CHECKED) != 0,
        "production enables assignment autosave before its save notification");
    ++current->saves; current->lastSaved = pmm;
    if (!current->generateEmm) return;
    auto emm = pmm; emm.replace_extension(L".emm");
    // A BOM keeps non-ASCII installation/model paths representable by Win32 INI.
    NewFile(emm, std::string("\xff\xfe", 2));
    PutIni(emm, L"Info", L"Version", L"3");
    PutIni(emm, L"Object", L"Pmd1", current->model.c_str());
    PutIni(emm, L"Effect", L"Default", L"untouched-default.fx");
    PutIni(emm, L"Effect", L"Pmd1", L"selected-original.fx");
    PutIni(emm, L"Effect", L"Pmd1[0]", L"selected-material.fx");
    PutIni(emm, L"Effect", L"Pmd2", L"unselected-original.fx");
    PutIni(emm, L"Effect@offscreen", L"Pmd1", L"selected-offscreen.fx");
    PutIni(emm, L"Effect@offscreen", L"Pmd2", L"unselected-offscreen.fx");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, emm.c_str());
}
void OpenBoundary(const wchar_t* name) {
    if (!current || !current->armed || GetCurrentThreadId() != current->thread) {
        realOpen(name); return;
    }
    Check(current->remaining == 0 && current->saves == 1, "open follows the actual production snapshot save");
    const fs::path project(name ? name : L"");
    Check(project == current->lastSaved.parent_path() / L"baked.pmm",
        "production opens its edited snapshot in the same unique directory");
    const auto placeholder = bake_storage::Bytes(project);
    Check(std::string(placeholder.begin(), placeholder.end()) == "Baked effect assignments",
        "production wrote the real edited project placeholder");
    auto edited = project; edited.replace_extension(L".emm");
    Check(Ini(edited, L"Object", L"Pmd1") == current->model.wstring(), "edited snapshot keeps the actual model path");
    Check(Ini(edited, L"Effect", L"Pmd1") == L"none" &&
        Ini(edited, L"Effect", L"Pmd1[0]") == L"none" &&
        Ini(edited, L"Effect@offscreen", L"Pmd1") == L"none",
        "real production INI editing disables the selected object and its material/offscreen assignments");
    Check(Ini(edited, L"Effect", L"Default") == L"untouched-default.fx" &&
        Ini(edited, L"Effect", L"Pmd2") == L"unselected-original.fx" &&
        Ini(edited, L"Effect@offscreen", L"Pmd2") == L"unselected-offscreen.fx",
        "real production editing preserves unrelated assignments");
    ++current->opens; current->lastOpened = project;
}
LRESULT CALLBACK MenuBoundary(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (current && window == current->window && message == WM_COMMAND && LOWORD(wp) == kAutoSave) {
        const auto state = GetMenuState(GetMenu(window), kAutoSave, MF_BYCOMMAND);
        Check(state != UINT(-1), "controlled autosave command exists");
        Check(CheckMenuItem(GetMenu(window), kAutoSave, MF_BYCOMMAND |
            ((state & MF_CHECKED) ? MF_UNCHECKED : MF_CHECKED)) != DWORD(-1), "toggle owned autosave menu state");
        ++current->toggles; return 0;
    }
    return DefSubclassProc(window, message, wp, lp);
}
struct Hooks {
    explicit Hooks(Context& context) {
        Check(current == nullptr && !GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll"),
            "controlled storage test has no third-party effect runtime");
        current = &context;
        const auto initialized = MH_Initialize();
        Check(initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED, "initialize process-local snapshot boundary hooks");
        Check(MH_CreateHook(reinterpret_cast<void*>(&GetModuleHandleW), reinterpret_cast<void*>(&ModuleBoundary),
            reinterpret_cast<void**>(&realModule)) == MH_OK, "create exact module gate hook");
        Check(MH_CreateHook(reinterpret_cast<void*>(&NotifyEffectFileSave), reinterpret_cast<void*>(&SaveBoundary),
            reinterpret_cast<void**>(&realSave)) == MH_OK, "create snapshot save boundary hook");
        Check(MH_CreateHook(reinterpret_cast<void*>(&NotifyEffectFileOpen), reinterpret_cast<void*>(&OpenBoundary),
            reinterpret_cast<void**>(&realOpen)) == MH_OK, "create snapshot open boundary hook");
        for (auto address : {reinterpret_cast<void*>(&GetModuleHandleW), reinterpret_cast<void*>(&NotifyEffectFileSave),
            reinterpret_cast<void*>(&NotifyEffectFileOpen)})
            Check(MH_EnableHook(address) == MH_OK, "enable only owned snapshot hooks");
        Check(SetWindowSubclass(context.window, MenuBoundary, kMenuSubclass, 0) != FALSE, "subclass only the owned hidden test window");
    }
    ~Hooks() {
        current->armed = false;
        Check(RemoveWindowSubclass(current->window, MenuBoundary, kMenuSubclass) != FALSE, "remove owned menu subclass");
        for (auto address : {reinterpret_cast<void*>(&GetModuleHandleW), reinterpret_cast<void*>(&NotifyEffectFileSave),
            reinterpret_cast<void*>(&NotifyEffectFileOpen)}) {
            Check(MH_DisableHook(address) == MH_OK, "disable only owned snapshot hooks");
            Check(MH_RemoveHook(address) == MH_OK, "remove only owned snapshot hooks");
        }
        current = nullptr;
    }
};
using Files = std::vector<bake_storage::File>;
Files SnapshotFiles(const fs::path& root) {
    Files files;
    if (fs::exists(root)) for (const auto& entry : fs::recursive_directory_iterator(root)) {
        Check(!entry.is_symlink(), "owned snapshot tree contains no redirected entries");
        if (entry.is_regular_file()) files.push_back({entry.path(), bake_storage::Bytes(entry.path())});
    }
    return files;
}
void PreserveFiles(const Files& files) {
    for (const auto& file : files) Check(bake_storage::Bytes(file.path) == file.bytes,
        "prior snapshot path and bytes remain readable after later operations");
}
void NoFallback(const bake_storage::Environment& environment) {
    for (const auto& root : {environment.cwd, environment.temporary})
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            Check(!entry.is_symlink(), "owned fallback witnesses contain no redirected entries");
            Check(entry.path().filename().wstring().find(L"MMS-Mesh-") != 0 &&
                entry.path().filename() != L"mme_mesh_snapshots",
                "snapshot creation never falls back to cwd or TEMP");
        }
}
bool Attempt(Context& context, MMDApp& app, const void* model, std::string& error) {
    context.remaining = 2; context.fakeQueries = context.saves = context.opens = context.toggles = 0;
    context.lastSaved.clear(); context.lastOpened.clear(); context.armed = true;
    const bool result = native_mesh::DisableMeshEffects(&app, {model}, error);
    context.armed = false;
    Check(context.remaining == 0 && context.fakeQueries == 2,
        "each attempt entered both real production module gates");
    Check(!GetModuleHandleW(L"MMEffect.dll"), "test module boundary does not persist beyond the two production gates");
    return result;
}
void Run(MMDApp& app, const fs::path& out, const bake_storage::Environment& environment) {
    environment.Reenter();
    auto* model = app.ModelSlot(0); Check(model != nullptr, "snapshot test has a real loaded model");
    Context context; context.root = environment.install / L"data" / L"mme_mesh_snapshots";
    context.model = fs::path(mdl::Mdl(model)->path); context.window = app.MainWindow();
    Check(fs::equivalent(context.model, out / L"triangle.pmx"), "snapshot test targets only its generated model");
    Check(!fs::exists(context.root), "snapshot root is absent before controlled production calls");
    native_mesh::Initialize(&app);
    Hooks hooks(context);
    const auto menu = GetMenu(app.MainWindow());
    Check(menu && GetMenuState(menu, kAutoSave, MF_BYCOMMAND) == UINT(-1), "no preexisting autosave command will be replaced");
    std::string error;
    Check(!Attempt(context, app, model, error) && error.find("autosave command unavailable") != std::string::npos &&
        context.saves == 0 && context.opens == 0 && !fs::exists(context.root),
        "missing autosave command rejects before any snapshot publication");
    const int position = GetMenuItemCount(menu);
    const auto popup = CreatePopupMenu(); Check(popup != nullptr, "create owned hidden autosave menu");
    Check(AppendMenuW(popup, MF_STRING | MF_UNCHECKED, kAutoSave, L"Snapshot test autosave") &&
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), L"Snapshot storage test"), "attach only owned hidden menu");
    fs::create_directories(context.root.parent_path());
    const std::string obstruction = "MMS owned snapshot root obstruction\n";
    NewFile(context.root, obstruction); error.clear();
    Check(!Attempt(context, app, model, error) && !error.empty() && context.saves == 0 && context.opens == 0 && context.toggles == 0,
        "occupied snapshot root fails without changing assignments or autosave");
    const auto blocked = bake_storage::Bytes(context.root);
    Check(std::string(blocked.begin(), blocked.end()) == obstruction, "root obstruction bytes remain intact");
    Check(DeleteFileW(context.root.c_str()) != FALSE, "delete only the verified test-owned obstruction file");
    NoFallback(environment);
    Files preserved; fs::path first;
    for (bool checked : {false, true}) {
        Check(CheckMenuItem(menu, kAutoSave, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED)) != DWORD(-1),
            "set owned initial autosave state");
        error.clear();
        const bool success = Attempt(context, app, model, error);
        if (!success) std::printf("snapshot failure: %s\n", error.c_str());
        Check(success && context.saves == 1 && context.opens == 1 && context.toggles == (checked ? 0u : 2u),
            "real snapshot save, registration drain and edited snapshot open complete once");
        Check(((GetMenuState(menu, kAutoSave, MF_BYCOMMAND) & MF_CHECKED) != 0) == checked,
            "production restores the initial autosave setting");
        if (first.empty()) first = context.lastSaved.parent_path();
        else Check(first != context.lastSaved.parent_path(), "successive calls allocate separate snapshot directories");
        PreserveFiles(preserved);
        preserved = SnapshotFiles(context.root);
        Check(preserved.size() == (checked ? 8u : 4u), "each successful snapshot retains exactly four transfer files");
        auto original = context.lastSaved; original.replace_extension(L".emm");
        Check(Ini(original, L"Effect", L"Pmd1") == L"selected-original.fx" &&
            Ini(original, L"Effect", L"Pmd1[0]") == L"selected-material.fx", "production retains the original assignments unchanged");
        std::printf("snapshot retained=%ls\n", context.lastSaved.parent_path().c_str());
        NoFallback(environment);
    }
    Check(CheckMenuItem(menu, kAutoSave, MF_BYCOMMAND | MF_UNCHECKED) != DWORD(-1), "prepare initially disabled autosave failure case");
    context.generateEmm = false; error.clear();
    Check(!Attempt(context, app, model, error) && error.find("did not save its assignment snapshot") != std::string::npos &&
        context.saves == 1 && context.opens == 0 && context.toggles == 2,
        "missing EMM is a real failure after save notification and restores autosave");
    Check((GetMenuState(menu, kAutoSave, MF_BYCOMMAND) & MF_CHECKED) == 0, "failed snapshot restores the disabled autosave state");
    auto missing = context.lastSaved; missing.replace_extension(L".emm");
    Check(!fs::exists(missing) && !fs::exists(context.lastSaved.parent_path() / L"baked.pmm"), "failed snapshot never publishes an edited mapping");
    PreserveFiles(preserved); NoFallback(environment);
    native_mesh::ClearScene();
    PreserveFiles(preserved);
    Check(DeleteMenu(menu, UINT(position), MF_BYPOSITION) != FALSE, "remove only the appended test menu");
    Check(GetMenuState(menu, kAutoSave, MF_BYCOMMAND) == UINT(-1), "test menu is fully removed");
    Check(!IsWindowVisible(app.MainWindow()), "snapshot storage host remains hidden");
    std::puts("PASS: production MME snapshot data paths, unique files, autosave restoration, failures and retained references; notification boundary is controlled");
}
} // namespace mme_storage
