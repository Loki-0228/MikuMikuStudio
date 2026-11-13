#include "../src/plugins/native_vmd_import.hpp"
#include "../src/plugins/native_model_vmd_source.hpp"
#include <filesystem>
namespace {
struct VmdMigrationMessage {
    HHOOK hook = nullptr;
    unsigned seen = 0;
    unsigned confirmations = 0;
    std::function<void()> duringConfirmation;
    std::function<void()> beforeResponse;   // set synthetic thread key state just before closing
    int mismatchResponse = IDOK;
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
                    Check(GetDlgItem(dialog,IDOK)!=nullptr,"model mismatch dialog has real OK button");
                    const auto button=GetDlgItem(dialog,current->mismatchResponse);
                    Check(button!=nullptr,"model mismatch dialog exposes the requested real button");
                    if(current->beforeResponse)current->beforeResponse();
                    PostMessageW(dialog,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(button),BN_CLICKED),reinterpret_cast<LPARAM>(button));
                    return CallNextHookEx(nullptr,code,wp,lp);
                }
                Check(std::wcsstr(body, L"VMD") && std::wcsstr(body, L"No scene data was changed"),
                    "actual VMD loader reports a file error without changing scene data");
                // This host's MB_OK dialog exposes its only button as
                // IDCANCEL. Use the observed child rather than assuming IDOK.
                const auto button = GetDlgItem(dialog, IDOK) ? GetDlgItem(dialog, IDOK) : GetDlgItem(dialog, IDCANCEL);
                Check(button != nullptr, "VMD rejection dialog has an acknowledge button");
                if (current->beforeResponse) current->beforeResponse();
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
// The ABI VMD importer's own MessageBoxW calls (model-name mismatch and file
// errors) bypass the legacy loader's Enter consumption. Only this thread's key
// table is written, so GetKeyState in production sees the synthetic state
// without SendInput, MinHook, or a foreground switch.
struct SyntheticThreadEnter {
    BYTE saved[256]{};
    SyntheticThreadEnter() {
        Check(GetKeyboardState(saved) != FALSE, "save thread keyboard state for VMD modal Enter");
    }
    ~SyntheticThreadEnter() { SetKeyboardState(saved); }
    void Set(bool held) const {
        BYTE keys[256]{};
        keys[VK_RETURN] = held ? 0x80 : 0;
        Check(SetKeyboardState(keys) != FALSE, "set only the test thread Enter state");
        Check(((GetKeyState(VK_RETURN) & 0x8000) != 0) == held,
            "synthetic Enter state is visible through GetKeyState");
    }
};
// The shared numeric pose fixture owns a hidden main window and never owns the
// real foreground, so leaving the polled-key eligibility latch to
// GetForegroundWindow would discard the Enter edges under test. Point the
// existing test seam at this subcase's own main window; the real query returns
// once the scope clears. No SetForegroundWindow, capture change, or injected
// input is involved.
struct PollKeyForegroundScope {
    PoseNumericFixture* fixture = nullptr;
    explicit PollKeyForegroundScope(PoseNumericFixture& f) : fixture(&f) {
        SetPollKeyForegroundForTests(f.app.get(), f.app->MainWindow());
    }
    ~PollKeyForegroundScope() { Clear(); }
    void Clear() {
        if (fixture == nullptr) return;
        ClearPollKeyForegroundForTests(fixture->app.get());
        fixture = nullptr;
    }
};
// One real frame poll for an Enter subcase. Every poll must keep the scoped
// foreground eligible and report the expected polled Enter cell; only the
// first idle qualification may regain eligibility, because a freshly
// constructed app starts ineligible. A failed sample prints the phase, both
// cells, the eligibility latches, raw key state and window evidence, then does
// the cleanup std::exit would skip - clear the foreground override and restore
// the saved thread key table - before the terminating Check keeps the original
// failure text and status.
void VmdEnterPoll(PoseNumericFixture& f, SyntheticThreadEnter& keyboard,
    PollKeyForegroundScope& foreground, const char* phase, int expected,
    bool allowRegain, const char* description) {
    MouseInteractionBegin(f.app.get());
    const int actual = f.app->state.enterKeyState;
    const bool eligible = PollKeyEligible(f.app.get());
    const bool regained = PollKeyRegained(f.app.get());
    if (actual == expected && eligible && (allowRegain || !regained)) return;
    std::printf(
        "FAIL VMD Enter poll phase=%s expected=%d actual=%d eligible=%d "
        "regained=%d regainAllowed=%d rawGetKeyState=0x%04x "
        "rawGetAsyncKeyState=0x%04x foreground=%p main=%p overrideEnabled=%d "
        "overrideValue=%p\n",
        phase, expected, actual, int(eligible), int(regained),
        int(allowRegain),
        static_cast<unsigned>(static_cast<std::uint16_t>(GetKeyState(VK_RETURN))),
        static_cast<unsigned>(static_cast<std::uint16_t>(GetAsyncKeyState(VK_RETURN))),
        static_cast<void*>(GetForegroundWindow()), static_cast<void*>(f.app->MainWindow()),
        int(f.app->PolledKeys().HasForegroundOverride()),
        f.app->PolledKeys().ForegroundOverride());
    std::fflush(stdout);
    foreground.Clear();
    SetKeyboardState(keyboard.saved);
    Check(false, description);
}
// After the modal returns, the real frame poll must see a still-held Enter as
// already consumed (3), a release edge (2), idle (0), and only a later press
// as a new press (1). No frame render or registrar runs here.
void CheckVmdModalEnter(PoseNumericFixture& f, SyntheticThreadEnter& keyboard,
                        PollKeyForegroundScope& foreground, bool held) {
    if (!held) {
        VmdEnterPoll(f, keyboard, foreground, "post-modal nonheld idle", 0, false,
            "released VMD modal Enter leaves the frame poll idle");
        return;
    }
    VmdEnterPoll(f, keyboard, foreground, "post-modal held poll 1 of 2", 3, false,
        "held VMD modal Enter stays consumed while the key is still down");
    VmdEnterPoll(f, keyboard, foreground, "post-modal held poll 2 of 2", 3, false,
        "held VMD modal Enter stays consumed while the key is still down");
    keyboard.Set(false);
    VmdEnterPoll(f, keyboard, foreground, "post-modal release edge", 2, false,
        "releasing the VMD modal Enter records a release edge");
    VmdEnterPoll(f, keyboard, foreground, "post-modal idle after release", 0, false,
        "released VMD modal Enter returns the frame poll to idle");
    keyboard.Set(true);
    VmdEnterPoll(f, keyboard, foreground, "post-modal fresh press", 1, false,
        "the next deliberate Enter after a VMD modal is a fresh press");
}
void VmdEnterConsumptionChecks() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"mms-vmd-enter-" + std::to_wstring(GetCurrentProcessId()) + L".vmd");
    std::array<unsigned char, 58> bytes{};
    constexpr char signature[] = "Vocaloid Motion Data 0002";
    std::memcpy(bytes.data(), signature, sizeof(signature) - 1);
    std::memcpy(bytes.data() + 30, "mismatched-model", 16);
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    DWORD written = 0;
    Check(file != INVALID_HANDLE_VALUE &&
        WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) &&
        written == bytes.size(), "write minimal valid model VMD mismatch fixture for Enter latch");
    CloseHandle(file);
    plugins::ModelVmdSource decoded;
    Check(plugins::DecodeModelVmdSource(nullptr, path.c_str(), 0, decoded) == MMS_OK &&
        decoded.modelName == "mismatched-model", "Enter latch fixture is a valid private model VMD source");
    SyntheticThreadEnter keyboard;
    for (auto mode : {plugins::SharedHistory::RecordingMode::AllOperations,
                      plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        for (bool held : {true, false}) {
            for (int response : {IDOK, IDCANCEL}) {
                PoseNumericFixture f;
                plugins::NativePoseTestBinding binding(*f.app, f.history);
                f.app->EnglishUI() = 1; f.app->CameraMode() = 0;
                Check(f.history.SetMode(mode) == MMS_OK, "set VMD Enter latch history recording mode");
                const auto first = f.first.Digest(), second = f.second.Digest(), ring = PoseRingDigest(f.first);
                const auto undo = f.history.UndoCount(), redo = f.history.RedoCount();
                PollKeyForegroundScope foreground(f);
                keyboard.Set(false);
                VmdEnterPoll(f, keyboard, foreground, "pre-modal idle qualification 1 of 2", 0, true,
                    "fresh VMD Enter subcase starts the frame poll idle");
                VmdEnterPoll(f, keyboard, foreground, "pre-modal idle qualification 2 of 2", 0, false,
                    "sustained VMD Enter foreground keeps the frame poll idle");
                keyboard.Set(held);
                {
                    VmdMigrationMessage message;
                    message.mismatchResponse = response;
                    message.beforeResponse = [&] { keyboard.Set(held); };
                    g_Block = f.app.get(); LoadVmdFile(path.c_str());
                    Check(message.seen == 1 && message.confirmations == 1,
                        "one real VMD model-mismatch prompt was answered");
                    Check(((GetKeyState(VK_RETURN) & 0x8000) != 0) == held,
                        "synthetic Enter state survives the VMD model-mismatch dialog");
                }
                Check(f.app->state.enterKeyState == (held ? 3 : 0), held
                    ? "Enter held through the VMD model-mismatch prompt is consumed"
                    : "mouse-like VMD model-mismatch response leaves the next Enter free");
                Check(f.first.Digest() == first && f.second.Digest() == second &&
                    PoseRingDigest(f.first) == ring && f.history.UndoCount() == undo &&
                    f.history.RedoCount() == redo,
                    "VMD model-mismatch response preserves model data and journal");
                CheckVmdModalEnter(f, keyboard, foreground, held);
                foreground.Clear();
                keyboard.Set(false);
                if (GetCapture() == f.app->MainWindow()) ReleaseCapture();
            }
        }
    }
    const auto missing = (std::filesystem::temp_directory_path() /
        (L"mms-vmd-enter-missing-" + std::to_wstring(GetCurrentProcessId()) + L".vmd")).native();
    Check(GetFileAttributesW(missing.c_str()) == INVALID_FILE_ATTRIBUTES,
        "VMD Enter latch failure fixture source is absent");
    for (auto mode : {plugins::SharedHistory::RecordingMode::AllOperations,
                      plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        for (bool held : {true, false}) {
            for (bool wide : {false, true}) {
                PoseNumericFixture f;
                plugins::NativePoseTestBinding binding(*f.app, f.history);
                f.app->EnglishUI() = 1; f.app->CameraMode() = 0;
                Check(f.history.SetMode(mode) == MMS_OK, "set VMD failure Enter latch history recording mode");
                const auto first = f.first.Digest(), second = f.second.Digest(), ring = PoseRingDigest(f.first);
                const auto undo = f.history.UndoCount(), redo = f.history.RedoCount();
                PollKeyForegroundScope foreground(f);
                keyboard.Set(false);
                VmdEnterPoll(f, keyboard, foreground, "pre-modal failure idle qualification 1 of 2", 0, true,
                    "fresh VMD failure Enter subcase starts the frame poll idle");
                VmdEnterPoll(f, keyboard, foreground, "pre-modal failure idle qualification 2 of 2", 0, false,
                    "sustained VMD failure Enter foreground keeps the frame poll idle");
                keyboard.Set(held);
                {
                    VmdMigrationMessage message;
                    message.beforeResponse = [&] { keyboard.Set(held); };
                    g_Block = f.app.get();
                    if (wide) LoadVmdFile(missing.c_str());
                    else Check(LoadVmdMotion(f.app.get(), "?:\\mms-vmd-import-not-present.vmd") == 0,
                        "ANSI VMD failure route reports no import");
                    Check(message.seen == 1, "one real VMD failure acknowledgement was answered");
                    Check(((GetKeyState(VK_RETURN) & 0x8000) != 0) == held,
                        "synthetic Enter state survives the VMD failure dialog");
                }
                Check(f.app->state.enterKeyState == (held ? 3 : 0), held
                    ? "Enter held through the VMD failure acknowledgement is consumed"
                    : "mouse-like VMD failure acknowledgement leaves the next Enter free");
                Check(f.first.Digest() == first && f.second.Digest() == second &&
                    PoseRingDigest(f.first) == ring && f.history.UndoCount() == undo &&
                    f.history.RedoCount() == redo,
                    "VMD failure acknowledgement preserves model data and journal");
                CheckVmdModalEnter(f, keyboard, foreground, held);
                foreground.Clear();
                keyboard.Set(false);
                if (GetCapture() == f.app->MainWindow()) ReleaseCapture();
            }
        }
    }
    DeleteFileW(path.c_str());
}
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
    VmdEnterConsumptionChecks();
}
}
