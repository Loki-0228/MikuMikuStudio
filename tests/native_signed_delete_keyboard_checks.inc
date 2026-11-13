// Include after native_signed_delete_command_checks.inc (which already needs
// native_history_mode_checks.inc and negative_global_delete_target_checks.inc).
// Entry: bool NativeSignedDeleteKeyboardChecks(). Call serially: the fixtures
// temporarily own g_Block and the real native history binding. A false return
// prints LIMITATION and must not be reported by the caller as a passing test.
// Only the raw polled source and foreground eligibility use existing seams.
// Focus is real GetFocus on hidden controls on an inactive private desktop.
// No physical keyboard, actual foreground switching, full FrameDriver loop,
// model/selector, plugin-property input or visual acceptance is claimed here.
#include "mikumikustudio/keyboard_input.hpp"

namespace mikumikustudio {
void PollKeyboardStates(MMDApp*);
void ConsumeEditKeys(MMDApp*);
}

namespace native_signed_delete_keyboard_checks {
using namespace mikumikustudio;
namespace commands = native_signed_delete_command_checks;
namespace native = global_delete_target_checks;
using Fixture = commands::Fixture;
using Mode = commands::Mode;
using Sampler = void (*)(MMDApp*);

bool Limitation(const char* operation, DWORD error) {
    std::fprintf(stderr, "LIMITATION signed delete keyboard: %s (Win32=%lu)\n", operation, error);
    std::fflush(stderr);
    return false;
}

// Keep input cells out of the no-op comparison: polling is supposed to change
// them. Still compare both signed stores, authored curves, selection, live
// poses, dirty flags and the common journal, including its revision/redo tail.
struct Snapshot {
    commands::Keys keys;
    native::Authors authors;
    native::Poses poses;
    std::array<std::uint32_t, 6> cursors;
    std::array<unsigned char, 6> active;
    std::vector<std::uint8_t> negative, selection;
    std::size_t count, undo, redo;
    std::uint64_t revision = 0;
    int frame;
    unsigned char dirty, recorded;
    explicit Snapshot(Fixture& f)
        : keys(*f.app), authors(native::AuthorsOf(f)), poses(native::PosesOf(f)),
          cursors(native::Cursors(f)), active(native::Active(f)),
          negative(NegativeTestRecord(*f.app, negative_frames::kPmmKey)),
          selection(NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey)),
          count(negative_frames::Count(f.app.get())), undo(f.history.UndoCount()), redo(f.history.RedoCount()),
          frame(f.app->CurrentFrame()), dirty(f.app->SceneModified()), recorded(f.app->RecordedFrameCountByte1()) {
        Check(f.history.Revision(revision) == MMS_OK, "read keyboard deletion history revision");
    }
    bool SameSources(Fixture& f) const {
        return keys.Same(*f.app) && native::AuthorsSame(f, authors) &&
            negative == NegativeTestRecord(*f.app, negative_frames::kPmmKey) &&
            selection == NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey) &&
            count == negative_frames::Count(f.app.get());
    }
    bool Same(Fixture& f) const {
        std::uint64_t now = 0;
        return SameSources(f) && poses == native::PosesOf(f) && cursors == native::Cursors(f) &&
            active == native::Active(f) && frame == f.app->CurrentFrame() &&
            dirty == f.app->SceneModified() && recorded == f.app->RecordedFrameCountByte1() &&
            undo == f.history.UndoCount() && redo == f.history.RedoCount() &&
            f.history.Revision(now) == MMS_OK && revision == now;
    }
};

struct Windows {
    static constexpr int textId = 0x7001;
    static constexpr int buttonId = 0x7002;
    // Generic text, panel text, panel frame, and all five fresh frame probes.
    static constexpr int editIds[] = {textId, 0x220, 0x22A, 0x1AA, 0x1A9, 0x1A1, 0x19A, 0x199};
    HWND main = nullptr, foreign = nullptr, foreignButton = nullptr, button = nullptr;
    ~Windows() {
        SetFocus(nullptr); // This thread belongs only to the inactive desktop.
        if (foreign) DestroyWindow(foreign);
        // The existing command fixture owns main and its child controls.
    }
    bool Focus(HWND window) const {
        if (!window || !IsWindow(window) || !IsWindowEnabled(window) ||
            GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
            return Limitation("hidden focus target is unavailable on the worker thread", 0);
        SetLastError(ERROR_SUCCESS);
        SetFocus(window);
        const DWORD error = GetLastError();
        if (GetFocus() != window) {
            std::fprintf(stderr, "  focus target=%p actual=%p control=%d\n",
                static_cast<void*>(window), static_cast<void*>(GetFocus()), GetDlgCtrlID(window));
            return Limitation("SetFocus could not establish real hidden-control focus", error);
        }
        return true;
    }
    bool Create(Fixture& f) {
        main = f.app->MainWindow();
        Check(main && IsWindow(main) && !IsWindowVisible(main) &&
            GetWindowThreadProcessId(main, nullptr) == GetCurrentThreadId(),
            "reuse the existing live hidden signed command fixture window");
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const auto child = [&](HWND owner, const wchar_t* type, int id) {
            return CreateWindowExW(0, type, L"12", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 80, 20, owner, reinterpret_cast<HMENU>(INT_PTR(id)), instance, nullptr);
        };
        button = child(main, L"BUTTON", buttonId);
        if (!button) return Limitation("create hidden ordinary button", GetLastError());
        for (int id : editIds) {
            const HWND edit = child(main, L"EDIT", id);
            if (!edit) return Limitation("create hidden text/frame edit", GetLastError());
            Check(GetDlgItem(main, id) == edit && GetParent(edit) == main,
                "frame and text gates resolve actual child EDIT handles, never null placeholders");
        }
        foreign = CreateWindowExW(0, L"STATIC", L"foreign keyboard owner", WS_OVERLAPPED,
            0, 0, 160, 100, nullptr, nullptr, instance, nullptr);
        if (!foreign) return Limitation("create hidden foreign owner", GetLastError());
        foreignButton = child(foreign, L"BUTTON", buttonId);
        if (!foreignButton) return Limitation("create hidden foreign focus target", GetLastError());
        // Probe availability before making any focus-dependent coverage claim.
        if (!Focus(foreignButton) || !Focus(main) || !Focus(button)) return false;
        for (int id : editIds)
            if (!Focus(GetDlgItem(main, id))) return false;
        Check(!IsWindowVisible(main) && !IsWindowVisible(foreign) && !IsWindowVisible(button),
            "focus probes leave both top-level windows and their children hidden");
        return Focus(main);
    }
};

struct Input {
    Fixture& f;
    Sampler sampler;
    Input(Fixture& fixture, Sampler poll) : f(fixture), sampler(poll) {
        Check(!HasPolledKeyStateForTests(), "keyboard case starts without an inherited raw source");
    }
    ~Input() {
        ClearPolledKeyStateForTests();
        ClearPollKeyForegroundForTests(f.app.get());
    }
    void Step(const char* phase, bool down, HWND foreground, int expected,
        bool eligible = true, bool regained = false) {
        std::printf("  keyboard phase=%s focus=%p\n", phase, static_cast<void*>(GetFocus()));
        std::fflush(stdout);
        unsigned char source[256]{};
        source[VK_DELETE] = down ? 0x80 : 0;
        SetPolledKeyStateForTests(source);
        SetPollKeyForegroundForTests(f.app.get(), foreground);
        const HWND focus = GetFocus();
        Check(focus && IsWindow(focus), "consumer receives a real non-null thread focus");
        // Exactly one sampler per frame: sampling twice would turn 1 into 3
        // before the consumer and hide the very press this regression needs.
        sampler(f.app.get());
        Check(f.app->state.deleteKeyState == expected &&
            PollKeyEligible(f.app.get()) == eligible && PollKeyRegained(f.app.get()) == regained,
            "production sampler produces the expected Delete edge and eligibility latch");
        Check(GetFocus() == focus, "polling retains the actual focus target");
        ConsumeEditKeys(f.app.get());
    }
    void NoDelete(const char* phase, bool down, HWND foreground, int expected,
        bool eligible = true, bool regained = false) {
        const Snapshot before(f);
        const HWND focus = GetFocus();
        Step(phase, down, foreground, expected, eligible, regained);
        Check(GetFocus() == focus, "blocked or non-press consumer retains the actual focus target");
        Check(before.Same(f), "blocked or non-press keyboard frame preserves both signed stores and history");
    }
};

bool Case(Sampler sampler, const char* name, Mode mode) {
    std::printf("  signed keyboard sampler=%s mode=%d\n", name, int(mode));
    std::fflush(stdout);
    Fixture reference;
    commands::Seed(reference, false, commands::Selection::Positive, -5);
    Fixture f;
    commands::Seed(f, false, commands::Selection::Positive, -5);
    commands::ActiveApp active(f);
    Windows windows;
    if (!windows.Create(f)) return false;
    Check(f.history.SetMode(mode) == MMS_OK, "keyboard deletion uses the requested real recording mode");
    const Snapshot original(f);
    Check(f.app->CameraKeys()[1].frame == 12 && f.app->CameraKeys()[1].selected &&
        negative_frames::CellSelected(f.app.get(), -10, 1),
        "keyboard case starts with real selected positive globals and negative light");
    commands::Oracle oracle(reference, true, true, true);
    oracle.Publish(true, true);
    // Bind only after both fixture seeds and the independent oracle are ready.
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    Input input(f, sampler);
    const auto deleted = [&] {
        Check(GetFocus() == windows.main,
            "successful bound deletion returns real focus to main through the production command");
        Check(f.history.UndoCount() == 1 && f.history.RedoCount() == 0 &&
            f.app->SceneModified() && f.app->RecordedFrameCountByte1(),
            "a sampled press commits exactly one dirty common-history deletion");
        Check(!f.app->CameraKeys()[1].frame && !f.app->LightKeys()[1].frame &&
            negative_frames::Count(f.app.get()) + 1 == original.count,
            "the consumer deletes positive keys and the selected negative key together");
        commands::CheckOracle(f, reference);
        commands::CheckNoPrivateHistory(f);
    };
    const auto restore = [&] {
        Check(f.history.History(false) == MMS_OK && f.history.UndoCount() == 0 &&
            f.history.RedoCount() == 1 && original.SameSources(f),
            "one common undo restores exact mixed sources and marks while Delete is still held");
    };

    input.NoDelete("eligible idle baseline", false, windows.main, 0, true, true);
    input.Step("main focus press", true, windows.main, 1);
    deleted();
    for (unsigned i = 0; i < 3; ++i) input.NoDelete("hold after delete", true, windows.main, 3);
    // Restoring the actual deletion targets while the key stays down makes a
    // duplicate dispatch observable; an already empty selection is not proof.
    restore();
    for (unsigned i = 0; i < 3; ++i) input.NoDelete("hold with restored targets", true, windows.main, 3);
    input.NoDelete("release with restored targets", false, windows.main, 2);
    input.NoDelete("idle after release", false, windows.main, 0);

    if (!windows.Focus(windows.foreignButton)) return false;
    input.NoDelete("press in foreign owner", true, windows.foreign, 0, false);
    if (!windows.Focus(windows.button)) return false;
    input.NoDelete("regain while held", true, windows.main, 3, true, true);
    input.NoDelete("held after regain", true, windows.main, 3);
    // Lose eligibility with a latched held key, release/repress outside, and
    // regain again. No pending press may leak back into the command history.
    if (!windows.Focus(windows.foreignButton)) return false;
    input.NoDelete("lose owner while held", true, windows.foreign, 0, false);
    input.NoDelete("release outside owner", false, windows.foreign, 0, false);
    input.NoDelete("repress outside owner", true, windows.foreign, 0, false);
    if (!windows.Focus(windows.button)) return false;
    input.NoDelete("regain after outside repress", true, windows.main, 3, true, true);
    input.NoDelete("release after regain", false, windows.main, 2);
    input.Step("ordinary button fresh press", true, windows.main, 1);
    deleted();
    restore();
    input.NoDelete("release after button delete", false, windows.main, 2);

    for (int id : Windows::editIds) {
        std::printf("  signed keyboard edit-control=%#x\n", id);
        const HWND edit = GetDlgItem(windows.main, id);
        if (!windows.Focus(edit)) return false;
        // Eligibility remains true and the edge really is 1. Suppression must
        // therefore come from the real consumer's text/frame focus gates.
        input.NoDelete("text/frame focus press", true, windows.main, 1);
        input.NoDelete("text/frame focus hold", true, windows.main, 3);
        if (!windows.Focus(windows.button)) return false;
        input.NoDelete("leave edit while held", true, windows.main, 3);
        input.NoDelete("release after leaving edit", false, windows.main, 2);
        input.Step("button rearmed after edit", true, windows.main, 1);
        deleted();
        restore();
        input.NoDelete("release before next edit", false, windows.main, 2);
    }
    Check(!IsWindowVisible(windows.main) && !IsWindowVisible(windows.foreign),
        "all keyboard coverage completes with hidden windows");
    return true;
}

constexpr char desktopPrefix[] = "MMS-signed-delete-keys-";

// Only the supervisor owns these handles. The child receives the desktop name,
// never its handle, and lets process exit retire its entire GUI environment.
struct Supervisor {
    HDESK desktop = nullptr;
    HANDLE inherited[3]{};
    std::vector<unsigned char> attributeStorage;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
    PROCESS_INFORMATION child{};
    bool childExited = false;

    static bool CloseOwned(HANDLE& handle, const char* operation) {
        if (!handle) return true;
        const HANDLE owned = handle;
        handle = nullptr;
        return CloseHandle(owned) != FALSE || Limitation(operation, GetLastError());
    }
    bool ReleaseStartup() {
        if (attributes) {
            DeleteProcThreadAttributeList(attributes);
            attributes = nullptr;
        }
        bool ok = true;
        for (auto& handle : inherited)
            if (!CloseOwned(handle, "close owned child standard handle")) ok = false;
        return ok;
    }
    bool Release() {
        bool ok = ReleaseStartup();
        if (child.hProcess && !childExited) {
            std::fprintf(stderr, "  keyboard child pid=%lu has no confirmed terminal state; "
                "retaining its process/thread handles and private desktop\n", child.dwProcessId);
            return Limitation("child may still be running; private desktop was not closed", 0);
        }
        if (!CloseOwned(child.hThread, "close exited keyboard child thread")) ok = false;
        if (!CloseOwned(child.hProcess, "close exited keyboard child process")) ok = false;
        if (desktop) {
            const HDESK owned = desktop;
            desktop = nullptr;
            // Exactly one attempt, including setup failures. Error 170 is a
            // failure, never a skip, retry, or accepted cleanup outcome.
            if (!CloseDesktop(owned)) {
                Limitation("release private keyboard desktop after child exit or before child creation", GetLastError());
                ok = false;
            } else {
                std::fprintf(stderr, "  keyboard private desktop closed handle=%p child_exited=%d\n",
                    static_cast<void*>(owned), int(childExited));
                std::fflush(stderr);
            }
        }
        return ok;
    }
};
} // namespace native_signed_delete_keyboard_checks

// main must route --signed-delete-keyboard-child <desktop> here before any
// fixtures or supervisor dispatch, returning nonzero for false or exceptions.
bool NativeSignedDeleteKeyboardChild(const char* expectedDesktop) {
    using namespace native_signed_delete_keyboard_checks;
    const unsigned before = checks;
    if (!expectedDesktop ||
        std::strncmp(expectedDesktop, desktopPrefix, sizeof desktopPrefix - 1) != 0 ||
        !expectedDesktop[sizeof desktopPrefix - 1])
        return Limitation("child requires the parent's private desktop name", ERROR_INVALID_PARAMETER);
    // This is a borrowed initial desktop handle: do not close or reassign it.
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    if (!desktop) return Limitation("resolve keyboard child's startup desktop", GetLastError());
    char actual[128]{};
    DWORD needed = 0;
    if (!GetUserObjectInformationA(desktop, UOI_NAME, actual, sizeof actual, &needed))
        return Limitation("read keyboard child's startup desktop name", GetLastError());
    if (std::strcmp(actual, expectedDesktop) != 0 ||
        std::strncmp(actual, desktopPrefix, sizeof desktopPrefix - 1) != 0)
        return Limitation("keyboard child desktop does not match the parent's private name", ERROR_INVALID_DATA);
    BOOL input = TRUE;
    if (!GetUserObjectInformationW(desktop, UOI_IO, &input, sizeof input, &needed))
        return Limitation("verify keyboard child's desktop is not receiving user input", GetLastError());
    if (input) return Limitation("keyboard child refuses to run on the input desktop", ERROR_ACCESS_DENIED);
    std::printf("  keyboard child pid=%lu desktop=%s handle=%p\n",
        GetCurrentProcessId(), actual, static_cast<void*>(desktop));
    std::fflush(stdout);
    try {
        Check(failAfter == -1, "keyboard child runs outside allocation-failure injection");
        Check(!HasPolledKeyStateForTests(), "isolated worker starts without a raw key override");
        for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
            if (!Case(PollKeyboardStates, "PollKeyboardStates", mode) ||
                !Case(MouseInteractionBegin, "MouseInteractionBegin", mode)) return false;
            Check(!HasPolledKeyStateForTests(), "completed keyboard cases restore the raw key source");
        }
    } catch (...) {
        return Limitation("keyboard child threw; its process must exit without supervisor recursion", 0);
    }
    std::printf("PASS native signed delete keyboard child: %u checks\n", checks - before);
    std::fflush(stdout);
    return true;
}

bool NativeSignedDeleteKeyboardChecks() {
    using namespace native_signed_delete_keyboard_checks;
    Check(failAfter == -1, "keyboard integration runs outside allocation-failure injection");
    // Borrow the original desktop without ever closing it. GUI fixtures now
    // run exclusively in another process; no supervisor thread joins theirs.
    const HDESK original = GetThreadDesktop(GetCurrentThreadId());
    if (!original) return Limitation("resolve the supervisor's inherited desktop", GetLastError());
    char originalName[128]{};
    DWORD needed = 0;
    if (!GetUserObjectInformationA(original, UOI_NAME, originalName, sizeof originalName, &needed))
        return Limitation("read the supervisor's inherited desktop", GetLastError());
    if (std::strncmp(originalName, desktopPrefix, sizeof desktopPrefix - 1) == 0)
        return Limitation("private keyboard child must not re-enter the supervisor", ERROR_INVALID_PARAMETER);

    Supervisor owner;
    bool passed = false;
    try {
        passed = [&] {
            static LONG serial = 0;
            wchar_t name[128]{};
            swprintf_s(name, L"MMS-signed-delete-keys-%lu-%lu-%llu-%lu", GetCurrentProcessId(),
                GetCurrentThreadId(), static_cast<unsigned long long>(GetTickCount64()),
                static_cast<unsigned long>(InterlockedIncrement(&serial)));
            // A null SECURITY_ATTRIBUTES makes this desktop handle non-inheritable.
            owner.desktop = CreateDesktopW(name, nullptr, nullptr, 0,
                DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
            if (!owner.desktop) return Limitation("create inactive keyboard desktop", GetLastError());
            const HDESK afterCreate = GetThreadDesktop(GetCurrentThreadId());
            if (!afterCreate) return Limitation("resolve supervisor desktop after private desktop creation", GetLastError());
            if (afterCreate != original)
                return Limitation("private desktop creation changed the supervisor's desktop", 0);
            USEROBJECTFLAGS flags{};
            if (!GetUserObjectInformationW(owner.desktop, UOI_FLAGS, &flags, sizeof flags, &needed))
                return Limitation("verify private desktop handle inheritance", GetLastError());
            if (flags.fInherit) return Limitation("private desktop handle must not be inheritable", 0);

            wchar_t executable[32768]{};
            const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
            if (!length) return Limitation("locate the actual keyboard test executable", GetLastError());
            if (length >= 32768) return Limitation("keyboard executable path was truncated", ERROR_INSUFFICIENT_BUFFER);
            std::wstring command = L"\"" + std::wstring(executable) + L"\" --signed-delete-keyboard-child " + name;
            if (command.size() + 1 > 32767)
                return Limitation("keyboard child command line exceeds the Windows limit", ERROR_INSUFFICIENT_BUFFER);
            std::vector<wchar_t> mutableCommand(command.begin(), command.end());
            mutableCommand.push_back(L'\0');
            std::fprintf(stderr, "  keyboard supervisor desktop=%s child executable=%ls private desktop=%ls\n",
                originalName, executable, name);

            SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
            owner.inherited[0] = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                &sa, OPEN_EXISTING, 0, nullptr);
            if (owner.inherited[0] == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                owner.inherited[0] = nullptr;
                return Limitation("open owned NUL stdin for keyboard child", error);
            }
            const DWORD streams[] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
            for (int i = 0; i < 2; ++i) {
                const HANDLE source = GetStdHandle(streams[i]);
                if (!source || source == INVALID_HANDLE_VALUE)
                    return Limitation(i == 0 ? "keyboard child stdout is unavailable" :
                        "keyboard child stderr is unavailable", ERROR_INVALID_HANDLE);
                if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                    &owner.inherited[i + 1], 0, TRUE, DUPLICATE_SAME_ACCESS))
                    return Limitation("duplicate owned keyboard child diagnostic handle", GetLastError());
            }
            SIZE_T bytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            const DWORD sizeError = GetLastError();
            if (!bytes || sizeError != ERROR_INSUFFICIENT_BUFFER)
                return Limitation("size keyboard child attribute list", sizeError);
            owner.attributeStorage.resize(bytes);
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(owner.attributeStorage.data());
            if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
                return Limitation("initialize keyboard child attribute list", GetLastError());
            owner.attributes = attributes; // Only an initialized list may be deleted.
            if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                owner.inherited, sizeof owner.inherited, nullptr, nullptr))
                return Limitation("whitelist only owned keyboard child standard handles", GetLastError());
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof startup;
            startup.StartupInfo.lpDesktop = name;
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = owner.inherited[0];
            startup.StartupInfo.hStdOutput = owner.inherited[1];
            startup.StartupInfo.hStdError = owner.inherited[2];
            startup.lpAttributeList = attributes;
            std::fflush(stdout);
            std::fflush(stderr);
            if (!CreateProcessW(executable, mutableCommand.data(), nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &owner.child))
                return Limitation("start same-executable keyboard child on private desktop", GetLastError());
            std::fprintf(stderr, "  keyboard child started pid=%lu\n", owner.child.dwProcessId);
            std::fflush(stderr);
            bool ok = owner.ReleaseStartup();
            const DWORD wait = WaitForSingleObject(owner.child.hProcess, 30000);
            if (wait == WAIT_OBJECT_0) {
                owner.childExited = true;
            } else {
                const DWORD error = wait == WAIT_FAILED ? GetLastError() : 0;
                std::fprintf(stderr, "  keyboard child wait status=%lu pid=%lu\n", wait, owner.child.dwProcessId);
                Limitation(wait == WAIT_TIMEOUT ? "keyboard child exceeded thirty seconds" :
                    "wait for keyboard child failed", error);
                ok = false;
                // Terminate only the child we created, once. As in the preview
                // fixture, allow ten seconds to confirm asynchronous termination.
                if (!TerminateProcess(owner.child.hProcess, 124))
                    Limitation("terminate own keyboard child after unsuccessful wait", GetLastError());
                const DWORD terminal = WaitForSingleObject(owner.child.hProcess, 10000);
                if (terminal != WAIT_OBJECT_0) {
                    const DWORD terminalError = terminal == WAIT_FAILED ? GetLastError() : 0;
                    std::fprintf(stderr, "  keyboard child termination wait status=%lu\n", terminal);
                    return Limitation("own keyboard child's termination is unconfirmed", terminalError);
                }
                owner.childExited = true;
            }
            DWORD code = 1;
            if (!GetExitCodeProcess(owner.child.hProcess, &code))
                return Limitation("read terminated keyboard child exit code", GetLastError());
            std::fprintf(stderr, "  keyboard child exited pid=%lu code=%lu\n", owner.child.dwProcessId, code);
            if (code != 0) return Limitation("keyboard child returned a nonzero exit code", 0);
            return ok;
        }();
    } catch (const std::bad_alloc&) {
        Limitation("allocate keyboard child launch resources", ERROR_NOT_ENOUGH_MEMORY);
    } catch (...) {
        Limitation("keyboard supervisor setup threw an exception", 0);
    }
    const HDESK afterChild = GetThreadDesktop(GetCurrentThreadId());
    if (!afterChild) {
        Limitation("resolve supervisor desktop after keyboard child", GetLastError());
        passed = false;
    } else if (afterChild != original) {
        Limitation("keyboard supervision changed the borrowed original desktop", 0);
        passed = false;
    }
    const bool released = owner.Release();
    if (!passed || !released) return false;
    // This count belongs only to the supervisor. Child Check calls increment
    // its own address space and are reported by the child, never added here.
    Check(owner.childExited, "keyboard supervisor confirms child exit and successful private desktop release");
    return true;
}
