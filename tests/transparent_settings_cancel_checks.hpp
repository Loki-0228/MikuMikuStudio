// MAINTENANCE: regression-keep. Real SaveSceneAs cancellation; no failure-dialog interception.
#pragma once
// Include inside transparent_sort_settings_tests.cpp's anonymous namespace,
// after ProjectChecks. Add mikumikustudio/ui_translation.hpp at file scope.
// Uses the existing isolated project host and MinHook lifetime, never global input.
#if !MMD_ORIGINAL_PROFILE
namespace transparent_settings_cancel {

struct Entry {
    fs::path relative;
    bool directory=false;
    std::vector<char> bytes;
};
std::vector<Entry> Directory(const fs::path& root) {
    std::vector<Entry> result;
    for (const auto& item:fs::recursive_directory_iterator(root)) {
        Check(!item.is_symlink() && (item.is_directory() || item.is_regular_file()),
              "cancel fixture contains only owned regular files and directories");
        Entry entry{item.path().lexically_relative(root),item.is_directory(),{}};
        if (!entry.directory) {
            entry.bytes=Bytes(item.path());
            Check(entry.bytes.size()==item.file_size(),"read every byte of the owned cancel fixture");
        }
        result.push_back(entry);
    }
    return result;
}
void SameDirectory(const fs::path& root,const std::vector<Entry>& before) {
    const auto after=Directory(root);
    Check(after.size()==before.size(),"cancel creates no temporary, version, or partial project entry");
    for (const auto& expected:before) {
        unsigned matches=0;
        for (const auto& actual:after) if (actual.relative==expected.relative) {
            ++matches;
            Check(actual.directory==expected.directory && actual.bytes==expected.bytes,
                  "cancel preserves each owned directory entry and all file bytes");
        }
        Check(matches==1,"cancel preserves every original relative file name exactly once");
    }
}
void SameDocument(const ext::Document& before,const ext::Document& after) {
    Check(before.records.size()==after.records.size(),"cancel preserves retained extension record count");
    for (std::size_t n=0;n<before.records.size();++n)
        Check(before.records[n].key==after.records[n].key && before.records[n].value==after.records[n].value,
              "cancel preserves retained extension keys, values, and order");
}

// SaveSceneAs legitimately changes these UI-session fields before the picker.
// Restore them even if a C++ filesystem/allocation exception leaves this case.
struct Session {
    MMDApp& app;
    fs::path cwd=fs::current_path();
    decltype(MMDAppState::enterKeyState) enter;
    int dialog;
    explicit Session(MMDApp& value):app(value),enter(value.state.enterKeyState),dialog(value.state.dialogFlags[5]) {}
    ~Session() {
        app.state.enterKeyState=enter; app.state.dialogFlags[5]=dialog;
        Check(SetCurrentDirectoryW(cwd.c_str())!=FALSE,"restore process-only cwd after cancelled Save As");
    }
};

struct Picker {
    using Function=decltype(&GetUiSaveFileNameW);
    inline static Picker* active=nullptr;
    Function original=nullptr;
    HWND owner=nullptr;
    DWORD thread=0;
    unsigned calls=0;
    bool armed=false;
    explicit Picker(HWND window):owner(window),thread(GetCurrentThreadId()) {
        Check(!active && thread==hostThread && Owned(owner),"cancel picker targets only this hidden host HWND and thread");
        Check(MH_CreateHook(reinterpret_cast<void*>(&GetUiSaveFileNameW),reinterpret_cast<void*>(&Cancel),
            reinterpret_cast<void**>(&original))==MH_OK,"install exact application save-picker trampoline");
        active=this;
        if (MH_EnableHook(reinterpret_cast<void*>(&GetUiSaveFileNameW))!=MH_OK) {
            active=nullptr; MH_RemoveHook(reinterpret_cast<void*>(&GetUiSaveFileNameW));
            Check(false,"enable scoped save-picker cancellation");
        }
        armed=true;
    }
    ~Picker() {
        armed=false;
        Check(MH_DisableHook(reinterpret_cast<void*>(&GetUiSaveFileNameW))==MH_OK &&
              MH_RemoveHook(reinterpret_cast<void*>(&GetUiSaveFileNameW))==MH_OK,
              "remove only the scoped save-picker hook before host teardown");
        active=nullptr;
    }
    static BOOL Cancel(LPOPENFILENAMEW value) {
        auto& self=*active;
        if (!self.armed || GetCurrentThreadId()!=self.thread || !value ||
            value->hwndOwner!=self.owner || !Owned(value->hwndOwner)) return self.original(value);
        ++self.calls;
        Check(value->lStructSize==sizeof(OPENFILENAMEW) && value->lpstrFile && value->nMaxFile>=2 &&
              value->lpstrFile[0]==L'\0' && value->lpstrDefExt && !wcscmp(value->lpstrDefExt,L"pmm") &&
              value->lpstrFilter && !wcscmp(value->lpstrFilter,L"PolygonMovieMaker files(*.pmm)") &&
              value->nFilterIndex==1 &&
              (value->Flags&(OFN_OVERWRITEPROMPT|OFN_HIDEREADONLY))==(OFN_OVERWRITEPROMPT|OFN_HIDEREADONLY),
              "real SaveSceneAs reaches its PMM picker with the expected owned request");
        // No destination is supplied or accepted. Existing unexpected-dialog
        // guards remain installed, and every nonmatching call is forwarded.
        return FALSE;
    }
};
} // namespace transparent_settings_cancel
#endif

void SettingsCancelChecks(MMDApp& app,const fs::path& out) {
#if !MMD_ORIGINAL_PROFILE
    using namespace transparent_settings_cancel;
    Check(out.is_absolute() && fs::is_directory(out),"cancel case requires the existing owned absolute projects directory");
    const auto folder=out/L"save-as-cancel";
    Check(!fs::exists(folder) && fs::create_directory(folder),"create fresh cancel-only project directory");
    Session session(app);
    Check(!SceneSaveInProgress() && !SceneSaveBlocksEdits(),"cancel baseline begins outside every save transaction");
    Check(!autosave::Current().enabled && !autosave::Current().keepPrevious && !autosave::Current().numberedManualVersions,
          "cancel fixture uses disabled periodic saving and manual version retention");
    Command(app,306); if (Scope(app)) Command(app,309);
    ext::Set(app.PmmProjectSettings().document,unknownKey,unknownBytes);
    const auto path=folder/L"baseline.pmm";
    Save(app,path);
    const auto savedDocument=Read(path);
    Has(savedDocument,scopeKey,{0}); Has(savedDocument,tierKey,{2}); Has(savedDocument,unknownKey,unknownBytes);
    const fs::path sidecar(path.native()+L".mms-plugins.json");
    Check(fs::is_regular_file(sidecar) && fs::file_size(sidecar)>0,
          "real baseline save publishes the correctly suffixed nonempty plugin sidecar");
    const auto pmm=Bytes(path),side=Bytes(sidecar);
    const auto files=Directory(folder);
    Command(app,309); Command(app,307);
    Check(Scope(app) && app.TransparentMaterialSortPrecision()==TransparentSortPrecision::Maximum &&
          app.SceneModified() && app.PmmProjectSettings().sortingCrossObjectsEdited && app.PmmProjectSettings().sortingPrecisionEdited,
          "real commands leave both scope and precision edits unsaved before cancellation");
    const auto settingsBefore=app.PmmProjectSettings();
    Has(settingsBefore.document,unknownKey,unknownBytes);
    const auto frame=app.CurrentFrame();
    const auto revision=app.SceneRevision(),generation=app.SceneGeneration();
    const std::wstring filename=app.EnvFileName(),directory=app.DirUser(),workspace=app.PathWorkspace().projectDirectory;
    const std::wstring status=background_save::StatusText(&app);
    MSG completion{};
    Check(!PeekMessageW(&completion,app.MainWindow(),background_save::kComplete,background_save::kComplete,PM_NOREMOVE),
          "cancel begins without an outstanding save completion message");
    bool accepted=true;
    {
        Picker picker(app.MainWindow());
        accepted=SaveSceneAs(&app,app.MainWindow());
        Check(picker.calls==1,"actual production SaveSceneAs consumes exactly one controlled cancellation");
    }
    Check(!accepted,"real SaveSceneAs reports the picker cancellation");
    Check(!SceneSaveInProgress() && !SceneSaveBlocksEdits() &&
          !PeekMessageW(&completion,app.MainWindow(),background_save::kComplete,background_save::kComplete,PM_NOREMOVE),
          "cancel leaves no running save, edit gate, or queued save completion");
    Check(status==background_save::StatusText(&app),"cancel does not publish background-save status");
    Check(Scope(app) && app.TransparentMaterialSortPrecision()==TransparentSortPrecision::Maximum && app.SceneModified() &&
          app.PmmProjectSettings().sortingCrossObjectsEdited && app.PmmProjectSettings().sortingPrecisionEdited,
          "cancel preserves both live settings, both edited flags, and dirty state");
    Check(app.PmmProjectSettings().processorEdited==settingsBefore.processorEdited &&
          app.PmmProjectSettings().antialiasEdited==settingsBefore.antialiasEdited,
          "cancel preserves unrelated project-setting edit markers");
    SameDocument(settingsBefore.document,app.PmmProjectSettings().document);
    Check(app.CurrentFrame()==frame && app.SceneRevision()==revision && app.SceneGeneration()==generation,
          "cancel neither moves the playhead nor advances scene revision or generation");
    Check(filename==app.EnvFileName() && directory==app.DirUser() && workspace==app.PathWorkspace().projectDirectory,
          "cancel preserves active project and remembered project directory paths");
    Check(Bytes(path)==pmm && Bytes(sidecar)==side,"cancel preserves exact PMM and paired identity/payload bytes");
    SameDirectory(folder,files);
    HiddenWindows();
#else
    // The original profile has no enhanced sorting state or plugin sidecar.
    // Its existing real project roundtrips remain the profile-specific evidence.
    (void)app; (void)out;
#endif
}
