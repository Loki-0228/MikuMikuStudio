#include "../src/plugins/native_vmd_import.hpp"
#include "../src/plugins/native_model_vmd_source.hpp"
#include <filesystem>
namespace {
struct VmdMigrationMessage {
    HHOOK hook = nullptr;
    unsigned seen = 0;
    unsigned confirmations = 0;
    std::function<void()> duringConfirmation;
    static thread_local VmdMigrationMessage* current;
    static LRESULT CALLBACK Inspect(int code, WPARAM wp, LPARAM lp) {
        if (code >= 0 && current && reinterpret_cast<const CWPRETSTRUCT*>(lp)->message == WM_INITDIALOG) {
            const auto dialog = reinterpret_cast<const CWPRETSTRUCT*>(lp)->hwnd;
            wchar_t name[32]{}; GetClassNameW(dialog, name, 32);
            if (!std::wcscmp(name, L"#32770")) {
                ++current->seen; ShowWindow(dialog, SW_HIDE);
                wchar_t body[1024]{}; GetDlgItemTextW(dialog, 0xffff, body, 1024);
                if(std::wcsstr(body,L"Only matching names")){
                    ++current->confirmations;if(current->duringConfirmation)current->duringConfirmation();
                    const auto button=GetDlgItem(dialog,IDOK);Check(button!=nullptr,"model mismatch dialog has real OK button");
                    PostMessageW(dialog,WM_COMMAND,MAKEWPARAM(IDOK,BN_CLICKED),reinterpret_cast<LPARAM>(button));
                    return CallNextHookEx(nullptr,code,wp,lp);
                }
                Check(std::wcsstr(body, L"VMD") && std::wcsstr(body, L"No scene data was changed"),
                    "actual VMD loader reports a file error without changing scene data");
                // This host's MB_OK dialog exposes its only button as
                // IDCANCEL. Use the observed child rather than assuming IDOK.
                const auto button = GetDlgItem(dialog, IDOK) ? GetDlgItem(dialog, IDOK) : GetDlgItem(dialog, IDCANCEL);
                Check(button != nullptr, "VMD rejection dialog has an acknowledge button");
                PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), reinterpret_cast<LPARAM>(button));
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    VmdMigrationMessage() {
        Check(!current, "no nested VMD migration prompt"); current = this;
        hook = SetWindowsHookExW(WH_CALLWNDPROCRET, Inspect, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "install VMD loader prompt hook");
    }
    ~VmdMigrationMessage() { UnhookWindowsHookEx(hook); current = nullptr; }
};
thread_local VmdMigrationMessage* VmdMigrationMessage::current = nullptr;
void VmdModalReentryChecks(){
    const auto path=std::filesystem::temp_directory_path()/(L"mms-vmd-modal-"+std::to_wstring(GetCurrentProcessId())+L".vmd");
    std::array<unsigned char,58> bytes{};constexpr char signature[]="Vocaloid Motion Data 0002";
    std::memcpy(bytes.data(),signature,sizeof(signature)-1);std::memcpy(bytes.data()+30,"mismatched-model",16);
    const auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);DWORD written=0;
    Check(file!=INVALID_HANDLE_VALUE&&WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size(),"write minimal valid model VMD mismatch fixture");CloseHandle(file);
    plugins::ModelVmdSource decoded;Check(plugins::DecodeModelVmdSource(nullptr,path.c_str(),0,decoded)==MMS_OK&&decoded.modelName=="mismatched-model","modal fixture is a valid private model VMD source");
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly})for(unsigned mutation=0;mutation<6;++mutation){
        PoseNumericFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->EnglishUI()=1;f.app->CameraMode()=0;
        std::printf("  VMD modal mode=%u mutation=%u frame=%d camera=%u\n",unsigned(mode),mutation,f.app->CurrentFrame(),unsigned(f.app->CameraMode()));
        f.history.SetMode(mode);const auto first=f.first.Digest(),second=f.second.Digest();
        VmdMigrationMessage message;message.duringConfirmation=[&]{
            if(mutation==0)++f.app->CurrentFrame();
            else if(mutation==1)f.app->SelectedModelSlot()=1;
            else if(mutation==2)f.app->ModelSlot(0)=f.second.raw();
            else if(mutation==3)Check(f.history.Reset()==MMS_OK,"modal hook can change journal revision");
            // MessageBox may release capture while closing. Use a durable
            // authoring-dialog gate to test post-modal admission instead.
            else if(mutation==4)f.app->state.accessoryEditDialogOpen=1;
            else {plugins::NativePoseTestBinding temporary(*f.app,f.history);}
        };
        g_Block=f.app.get();LoadVmdFile(path.c_str());
        std::printf("  VMD modal dialogs=%u confirmations=%u\n",message.seen,message.confirmations);
        Check(message.confirmations==1&&message.seen==2,"modal reentry reports stale context after actual OK instead of importing");
        Check(f.first.Digest()==first&&f.second.Digest()==second&&!f.history.UndoCount(),"modal target time selection revision and dialog changes cannot publish VMD edits");
        if(GetCapture()==f.app->MainWindow())ReleaseCapture();f.app->ModelSlot(0)=f.first.raw();
    }
    DeleteFileW(path.c_str());
}
void NativeVmdImportChecks() {
    Check(plugins::CheckVmdImportHistory(nullptr) == MMS_E_ARGUMENT,
        "null VMD admission does not pretend to allow import");
    for (auto mode : {plugins::SharedHistory::RecordingMode::AllOperations,
                      plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        for (bool camera : {false}) {
            std::printf("  VMD gate mode=%u camera=%u\n", unsigned(mode), unsigned(camera)); std::fflush(stdout);
            PoseNumericFixture f;
            Check(plugins::CheckVmdImportHistory(f.app.get()) == MMS_OK &&
                !plugins::RefuseBoundVmdImport(f.app.get()),
                "ordinary unbound VMD import retains its legacy path");
            Check(f.history.SetMode(mode) == MMS_OK, "set VMD history recording mode");
            plugins::NativePoseTestBinding binding(*f.app, f.history);
            f.app->EnglishUI() = 1; f.app->CameraMode() = camera;
            f.Commit(panel::kReadoutPosYEdit, "3");
            // ExplicitOnly may intentionally omit this adjustment. The gate
            // must preserve either journal shape without depending on mode.
            const auto undo = f.history.UndoCount(), redo = f.history.RedoCount();
            const auto before = f.first.Digest(), ring = PoseRingDigest(f.first);
            const auto selection = f.first.selected;
            const auto appBefore = std::vector<unsigned char>(reinterpret_cast<unsigned char*>(f.app.get()),
                reinterpret_cast<unsigned char*>(f.app.get()) + sizeof(MMDApp));
            std::printf("  VMD gate invoking loader\n"); std::fflush(stdout);
            Check(plugins::CheckVmdImportHistory(f.app.get()) == MMS_E_NOT_READY,
                "bound legacy VMD admission helper remains closed");
            VmdMigrationMessage message;
            // Both exported controlled routes must report an absent source
            // without publishing a partial motion or using the legacy writer.
            const auto missing = (std::filesystem::temp_directory_path() /
                (L"mms-vmd-missing-" + std::to_wstring(GetCurrentProcessId()) + L".vmd")).native();
            Check(GetFileAttributesW(missing.c_str()) == INVALID_FILE_ATTRIBUTES,
                "VMD rejection fixture source is absent");
            Check(LoadVmdMotion(f.app.get(), "?:\\mms-vmd-import-not-present.vmd") == 0,
                "ANSI VMD route reports no import");
            g_Block = f.app.get(); LoadVmdFile(missing.c_str());
            Check(message.seen == 2, "ANSI and Unicode VMD routes each reject once");
            Check(f.first.Digest() == before && PoseRingDigest(f.first) == ring &&
                f.first.selected == selection && f.history.UndoCount() == undo && f.history.RedoCount() == redo &&
                !std::memcmp(f.app.get(), appBefore.data(), appBefore.size()),
                "rejected VMD leaves app state model keys selection legacy rings and common journal unchanged");
            Check(f.history.Enter() == MMS_OK, "VMD rejection leaves journal gate available"); f.history.Leave();
        }
    }
    VmdModalReentryChecks();
}
}
