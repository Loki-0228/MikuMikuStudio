#include "../src/plugins/native_vmd_import.hpp"
#include <filesystem>
namespace {
struct VmdMigrationMessage {
    HHOOK hook = nullptr;
    unsigned seen = 0;
    static thread_local VmdMigrationMessage* current;
    static LRESULT CALLBACK Inspect(int code, WPARAM wp, LPARAM lp) {
        if (code >= 0 && current && reinterpret_cast<const CWPRETSTRUCT*>(lp)->message == WM_INITDIALOG) {
            const auto dialog = reinterpret_cast<const CWPRETSTRUCT*>(lp)->hwnd;
            wchar_t name[32]{}; GetClassNameW(dialog, name, 32);
            if (!std::wcscmp(name, L"#32770")) {
                ++current->seen; ShowWindow(dialog, SW_HIDE);
                wchar_t body[1024]{}; GetDlgItemTextW(dialog, 0xffff, body, 1024);
                Check(std::wcsstr(body, L"VMD") && std::wcsstr(body, L"test mode"),
                    "actual VMD loader reports shared-history limitation before file access");
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
                "bound VMD model import remains protected by the migration gate");
            VmdMigrationMessage message;
            // An absent source cannot provide a parsed motion. Seeing the
            // migration message rather than an open error verifies that both
            // exported paths reject before trying to read it.
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
}
}
