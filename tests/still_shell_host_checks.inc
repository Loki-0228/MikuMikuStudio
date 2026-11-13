// Use the product's real modal picker and drive only controls on the private
// non-input desktop. The application consumes the actual Shell result.
namespace still_shell {
struct Flow {
    HWND owner;
    const wchar_t* stem;
    UINT format;
    int answer;
    const wchar_t* alternate = nullptr;
    IFileSaveDialog* picker = nullptr;
    HWND dialog = nullptr, yes = nullptr, no = nullptr;
    UINT_PTR timer = 0;
    ULONGLONG deadline = 0;
    int step = 0;
    bool prompted = false, timedOut = false;
    HRESULT result = E_UNEXPECTED;
};

Flow* active = nullptr;
decltype(originalStillShow) realShow = nullptr;

bool ClassIs(HWND window, const wchar_t* expected) {
    wchar_t name[32]{};
    return GetClassNameW(window, name, static_cast<int>(std::size(name))) &&
        _wcsicmp(name, expected) == 0;
}

BOOL CALLBACK FindPicker(HWND window, LPARAM data) {
    auto& flow = *reinterpret_cast<Flow*>(data);
    wchar_t title[64]{};
    if (GetWindow(window, GW_OWNER) != flow.owner || !IsWindowVisible(window) || !ClassIs(window, L"#32770") ||
        !GetWindowTextW(window, title, static_cast<int>(std::size(title))) ||
        std::wcscmp(title, L"render to picture file") != 0)
        return TRUE;
    flow.dialog = window;
    return FALSE;
}

BOOL CALLBACK FindAnswers(HWND window, LPARAM data) {
    auto& flow = *reinterpret_cast<Flow*>(data);
    if (!ClassIs(window, L"Button")) return TRUE;
    wchar_t label[128]{};
    GetWindowTextW(window, label, static_cast<int>(std::size(label)));
    const auto id = GetDlgCtrlID(window);
    // The modern Shell's nested DirectUI buttons can both have ID zero.
    if (id == IDYES || std::wcsstr(label, L"&Y")) flow.yes = window;
    if (id == IDNO || std::wcsstr(label, L"&N")) flow.no = window;
    return TRUE;
}

BOOL CALLBACK FindConfirmation(HWND window, LPARAM data) {
    auto& flow = *reinterpret_cast<Flow*>(data);
    if (GetWindow(window, GW_OWNER) != flow.dialog || !IsWindowVisible(window) || !ClassIs(window, L"#32770"))
        return TRUE;
    flow.yes = flow.no = nullptr;
    EnumChildWindows(window, FindAnswers, data);
    return !flow.yes || !flow.no;
}

BOOL CALLBACK SelectJpeg(HWND window, LPARAM data) {
    auto& selected = *reinterpret_cast<bool*>(data);
    if (!ClassIs(window, L"ComboBox") || SendMessageW(window, CB_GETCOUNT, 0, 0) != 7) return TRUE;
    const auto length = SendMessageW(window, CB_GETLBTEXTLEN, 1, 0);
    wchar_t label[128]{};
    if (length <= 0 || length >= static_cast<LRESULT>(std::size(label)) ||
        SendMessageW(window, CB_GETLBTEXT, 1, reinterpret_cast<LPARAM>(label)) == CB_ERR ||
        std::wcscmp(label, L"JPEG (*.jpg;*.jpeg)") != 0) return TRUE;
    SetFocus(window);
    SendMessageW(window, CB_SHOWDROPDOWN, TRUE, 0);
    COMBOBOXINFO info{sizeof(info)};
    RECT item{};
    if (!GetComboBoxInfo(window, &info) || !IsWindowVisible(info.hwndList) ||
        SendMessageW(info.hwndList, LB_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&item)) == LB_ERR) return FALSE;
    const LPARAM center = MAKELPARAM((item.left + item.right) / 2, (item.top + item.bottom) / 2);
    selected = PostMessageW(info.hwndList, WM_LBUTTONDOWN, MK_LBUTTON, center) &&
               PostMessageW(info.hwndList, WM_LBUTTONUP, 0, center);
    return FALSE;
}

void CALLBACK Tick(HWND, UINT, UINT_PTR timer, DWORD) {
    if (!active || active->timer != timer) return;
    auto& flow = *active;
    const auto data = reinterpret_cast<LPARAM>(&flow);
    flow.dialog = flow.yes = flow.no = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindPicker, data);
    if (flow.dialog) EnumThreadWindows(GetCurrentThreadId(), FindConfirmation, data);
    if (GetTickCount64() >= flow.deadline) {
        flow.timedOut = true;
        KillTimer(nullptr, timer);
        std::printf("STILL-SHELL timeout step=%d format=%u answer=%d\n", flow.step, flow.format, flow.answer);
        if (flow.no) PostMessageW(flow.no, BM_CLICK, 0, 0);
        if (flow.dialog) PostMessageW(flow.dialog, WM_CLOSE, 0, 0);
        return;
    }
    if (!flow.dialog) return;
    if (flow.step == 0) {
        if (flow.format == 2) {
            bool selected = false;
            EnumChildWindows(flow.dialog, SelectJpeg, reinterpret_cast<LPARAM>(&selected));
            if (!selected) return;
        }
        flow.step = 1;
    } else if (flow.step == 1) {
        UINT type = 0;
        if (FAILED(flow.picker->GetFileTypeIndex(&type)) || type != flow.format) return;
        Check(SUCCEEDED(flow.picker->SetFileName(flow.stem)), "enter the stem without an extension");
        flow.step = 2;
        PostMessageW(flow.dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                     reinterpret_cast<LPARAM>(GetDlgItem(flow.dialog, IDOK)));
    } else if (flow.step == 2 && flow.yes && flow.no) {
        Check(flow.answer == IDYES || flow.answer == IDNO, "an overwrite confirmation is expected for this target");
        flow.prompted = true;
        flow.step = 3;
        PostMessageW(flow.answer == IDYES ? flow.yes : flow.no, BM_CLICK, 0, 0);
    } else if (flow.step == 3 && flow.answer == IDNO && !flow.yes && !flow.no && IsWindowEnabled(flow.dialog)) {
        flow.step = 4;
        if (flow.alternate) {
            Check(SUCCEEDED(flow.picker->SetFileName(flow.alternate)), "enter a different stem after refusing overwrite");
            PostMessageW(flow.dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                         reinterpret_cast<LPARAM>(GetDlgItem(flow.dialog, IDOK)));
        } else {
            PostMessageW(flow.dialog, WM_COMMAND, IDCANCEL, 0);
        }
    }
}

HRESULT STDMETHODCALLTYPE Show(IFileSaveDialog* picker, HWND owner) {
    if (!active || active->owner != owner) return realShow(picker, owner);
    auto& flow = *active;
    Check(!flow.picker, "the command opens one real picker");
    flow.picker = picker;
    flow.deadline = GetTickCount64() + 12000;
    flow.timer = SetTimer(nullptr, 0, 40, Tick);
    Check(flow.timer != 0, "start the private Shell message driver");
    flow.result = realShow(picker, owner);
    KillTimer(nullptr, flow.timer);
    return flow.result;
}

fs::path Choose(MMDApp* app, const wchar_t* stem, UINT format, int answer, const wchar_t* alternate = nullptr) {
    Flow flow{app->MainWindow(), stem, format, answer, alternate};
    wchar_t path[kStillCapturePathField]{};
    std::optional<seq::StillFileIdentity> overwrite;
    active = &flow;
    const auto result = ChooseStillImagePath(app, flow.owner, path, std::size(path), overwrite);
    active = nullptr;
    std::printf("STILL-SHELL result=%08lx format=%u answer=%d prompt=%d\n",
                static_cast<unsigned long>(result), format, answer, flow.prompted);
    Check(flow.picker && !flow.timedOut && flow.prompted == (answer != 0),
        "the real picker reaches the expected native confirmation state");
    Check(overwrite.has_value() == (answer == IDYES),
        "only the accepted native overwrite returns the identity of an existing file");
    if (answer == IDNO && !alternate) {
        Check(flow.step == 4 && result == HRESULT_FROM_WIN32(ERROR_CANCELLED) && !path[0],
            "rejecting overwrite returns to the picker and cancelling returns no path");
    } else {
        if (alternate) Check(flow.step == 4, "the original picker remains active after refusing overwrite");
        Check(SUCCEEDED(result) && path[0], "the production picker returns its actual Shell path");
    }
    return path;
}

constexpr char previousImage[] = "previous isolated image";
const wchar_t* selectedPathFile = L"selected-jpeg.path";

std::string Bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

void RunPicker(MMDApp* app) {
    Check(fs::is_empty(output), "own a fresh still export output folder");
    Microsoft::WRL::ComPtr<IFileSaveDialog> probe;
    Check(SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&probe))),
        "identify the real Shell Show entry");
    void* show = (*reinterpret_cast<void***>(probe.Get()))[3];
    Check(MH_CreateHook(show, reinterpret_cast<void*>(&Show), reinterpret_cast<void**>(&realShow)) == MH_OK &&
          MH_EnableHook(show) == MH_OK, "observe the real modal entry without replacing its result");
    probe.Reset();
    app->EnglishUI() = 1;
    EnableStillRememberedDirectory(app, output);
    const auto png = output / L"default.png", jpeg = output / L"photo.jpg";
    Check(Choose(app, L"default", 1, 0) == png, "the Shell appends the initial PNG extension");
    Check(Choose(app, L"photo", 2, 0) == jpeg, "native JPEG selection changes the extension of the same kind of stem");
    { std::ofstream file(jpeg, std::ios::binary); file << previousImage; Check(bool(file), "seed the existing output"); }
    Check(Choose(app, L"photo", 2, IDNO).empty() && Bytes(jpeg) == previousImage,
        "rejecting overwrite leaves the existing target unchanged");
    Check(Choose(app, L"photo", 2, IDNO, L"alternate") == output / L"alternate.jpg" &&
          Bytes(jpeg) == previousImage && !fs::exists(output / L"alternate.jpg"),
        "after refusing overwrite the same picker accepts a new JPEG name without touching either target");
    std::puts("PASS native still picker accepts a different name after overwrite rejection");
    const auto approved = Choose(app, L"photo", 2, IDYES);
    Check(approved == jpeg && Bytes(jpeg) == previousImage,
        "confirming overwrite returns the exact target without modifying its bytes during selection");
    std::ofstream receipt(output / selectedPathFile, std::ios::binary);
    receipt.write(reinterpret_cast<const char*>(approved.c_str()),
        static_cast<std::streamsize>(approved.native().size() * sizeof(wchar_t)));
    receipt.close();
    Check(bool(receipt), "record the accepted Shell result for the hidden renderer");
    Check(!IsWindowVisible(app->MainWindow()), "the production host stays hidden outside the private Shell modal");
    Check(MH_DisableHook(show) == MH_OK && MH_RemoveHook(show) == MH_OK, "retire the scoped Shell observer");
}

void RunOnPrivateDesktop() {
    Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)),
        "initialize the isolated Shell STA");
    const auto instance = GetModuleHandleW(nullptr);
    const wchar_t* ownerClass = L"MmsStillShellTestOwner";
    WNDCLASSW registration{};
    registration.hInstance = instance;
    registration.lpfnWndProc = DefWindowProcW;
    registration.lpszClassName = ownerClass;
    Check(RegisterClassW(&registration) != 0, "register the private Shell owner");
    const auto menu = CreateMenu();
    Check(menu && AppendMenuW(menu, MF_STRING | MF_CHECKED, 0x12D, L"Remember location"),
        "create the remember-directory menu");
    const auto owner = CreateWindowExW(WS_EX_NOACTIVATE, ownerClass, L"Still export fixture", WS_POPUP,
        0, 0, 300, 200, nullptr, menu, instance, nullptr);
    Check(owner != nullptr, "create the private Shell owner");
    auto app = std::make_unique<MMDApp>();
    app->InitDefaults();
    app->MainWindow() = owner;
    g_Block = app.get();
    RunPicker(app.get());
    g_Block = nullptr;
    std::puts("PASS native still export Shell selection and overwrite decisions");
    std::puts("STILL-SHELL releasing COM apartment");
    CoUninitialize();
    std::puts("STILL-SHELL COM apartment released; destroying owner");
    Check(DestroyWindow(owner), "destroy the private Shell owner and attached menu");
    Check(UnregisterClassW(ownerClass, instance), "release the private Shell owner class");
    std::puts("STILL-SHELL owner destroyed");
}

fs::path replayPath;
IFileSaveDialog* replayPicker = nullptr;
decltype(originalStillResult) realResult = nullptr;
HRESULT (STDMETHODCALLTYPE* realAdvise)(IFileSaveDialog*, IFileDialogEvents*, DWORD*) = nullptr;
IFileDialogEvents* replayEvents = nullptr;
bool replayCancellation = false;
bool replayNewDestination = false;

HRESULT STDMETHODCALLTYPE ReplayAdvise(IFileSaveDialog* picker, IFileDialogEvents* events, DWORD* cookie) {
    const HRESULT result = realAdvise(picker, events, cookie);
    if (SUCCEEDED(result)) replayEvents = events;
    return result;
}

HRESULT STDMETHODCALLTYPE ReplayShow(IFileSaveDialog* picker, HWND owner) {
    if (owner != g_Block->MainWindow() || replayPath.empty()) return realShow(picker, owner);
    Check(!replayPicker, "the replay intercepts only the single image command picker");
    replayPicker = picker;
    auto* events = std::exchange(replayEvents, nullptr);
    Check(events, "the still picker advises its overwrite observer before Show");
    if (replayCancellation) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (!replayNewDestination) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        FDE_OVERWRITE_RESPONSE response = FDEOR_REFUSE;
        Check(SUCCEEDED(SHCreateItemFromParsingName(replayPath.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
              SUCCEEDED(events->OnOverwrite(picker, item.Get(), &response)) && response == FDEOR_DEFAULT,
              "replay the native callback for the approved existing image");
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ReplayResult(IFileSaveDialog* picker, IShellItem** item) {
    if (picker != replayPicker) return realResult(picker, item);
    if (replayNewDestination) {
        std::ofstream placeholder(replayPath, std::ios::binary);
        Check(bool(placeholder), "create a shell item for the initially free path");
    }
    const auto result = SHCreateItemFromParsingName(replayPath.c_str(), nullptr, IID_PPV_ARGS(item));
    if (replayNewDestination) Check(fs::remove(replayPath), "remove the unoccupied path's placeholder");
    return result;
}

void RunCapture(MMDApp* app) {
    const auto receipt = Bytes(output / selectedPathFile);
    Check(!receipt.empty() && receipt.size() % sizeof(wchar_t) == 0 &&
          receipt.size() / sizeof(wchar_t) < kStillCapturePathField,
        "read the preceding private Shell run's recorded result");
    std::wstring path(receipt.size() / sizeof(wchar_t), L'\0');
    std::memcpy(path.data(), receipt.data(), receipt.size());
    replayPath = path;
    Check(replayPath == output / L"photo.jpg" && Bytes(replayPath) == previousImage,
        "the previous image still occupies exactly the target confirmed by the Shell");
    Microsoft::WRL::ComPtr<IFileSaveDialog> probe;
    Check(SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&probe))),
        "identify the picker entries for replaying the accepted result");
    auto** table = *reinterpret_cast<void***>(probe.Get());
    void* show = table[3];
    void* result = table[20];
    void* advise = table[7];
    Check(MH_CreateHook(show, reinterpret_cast<void*>(&ReplayShow), reinterpret_cast<void**>(&realShow)) == MH_OK &&
          MH_CreateHook(result, reinterpret_cast<void*>(&ReplayResult), reinterpret_cast<void**>(&realResult)) == MH_OK &&
          MH_CreateHook(advise, &ReplayAdvise, reinterpret_cast<void**>(&realAdvise)) == MH_OK &&
          MH_EnableHook(show) == MH_OK && MH_EnableHook(result) == MH_OK && MH_EnableHook(advise) == MH_OK,
        "replay only the chosen path as a genuine filesystem shell item");
    probe.Reset();
    app->RenderWidth() = 64;
    app->RenderHeight() = 48;
    const auto savedPath = StillWideField(app->CaptureSavePath(), kStillCapturePathField);
    const auto savedDirectory = StillWideField(app->DirUser(), kStillDirUserField);
    replayCancellation = true;
    SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
    Check(replayPicker && !seq::StillActive(app) && !app->RecordingWindow() &&
          StillFieldUnchanged(app->CaptureSavePath(), savedPath) && StillFieldUnchanged(app->DirUser(), savedDirectory) &&
          Bytes(replayPath) == previousImage,
        "cancelling at the real command leaves the existing image, output path and directory intact");
    replayPicker = nullptr;
    replayCancellation = false;
    SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
    Check(replayPicker && seq::StillActive(app) && app->RecordingWindow() &&
          replayPath == app->CaptureSavePath() && Bytes(replayPath) == previousImage,
        "the actual still command starts at the approved destination with the old file intact");
    for (unsigned step = 0; app->RecordingWindow() && step < 12; ++step) {
        app->state.messageSeen = 1;
        coordinator_fixture::Drive(app);
    }
    Check(!seq::StillActive(app) && !app->RecordingWindow(), "the actual capture reaches completion");
    unsigned width = 0, height = 0;
    ImagePixels(replayPath, &width, &height);
    std::ifstream file(replayPath, std::ios::binary);
    Check(width == 64 && height == 48 && file.get() == 0xff && file.get() == 0xd8 && file.get() == 0xff,
        "the approved existing target now contains a decodable 64 by 48 JPEG");
    for (void* hook : {show, result, advise})
        Check(MH_DisableHook(hook) == MH_OK && MH_RemoveHook(hook) == MH_OK, "retire the result replay hook");
}

d3dx::FnSaveSurfaceToFileW realImageEncode = nullptr;
unsigned partialEncodes = 0, successfulEncodes = 0;
bool failImageEncode = false;
enum class TargetChange { None, Create, Replace };
TargetChange changeTarget = TargetChange::None;
constexpr char competingImage[] = "image written by another operation";
enum class FinalMove { Normal, CompetingWriter, Fail };
FinalMove finalMove = FinalMove::Normal;
decltype(&MoveFileExW) realStillMove = nullptr;
unsigned interceptedFinalMoves = 0;

BOOL WINAPI AtFinalStillMove(LPCWSTR source, LPCWSTR destination, DWORD flags) {
    if (finalMove != FinalMove::Normal && seq::StillActive(g_Block) && destination &&
        replayPath.native() == destination) {
        ++interceptedFinalMoves;
        if (finalMove == FinalMove::Fail) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (fs::exists(replayPath)) {
            const auto moved = replayPath.parent_path() / L"pre-fix-displaced.jpg";
            Check(!fs::exists(moved) && realStillMove(destination, moved.c_str(), 0),
                  "move only the existing fixture image before injecting its replacement");
        }
        std::ofstream competing(replayPath, std::ios::binary);
        competing << competingImage;
        competing.close();
        Check(bool(competing), "create the competing file immediately before the real final move");
    }
    return realStillMove(source, destination, flags);
}

HRESULT WINAPI EncodeImage(const wchar_t* path, int format, IDirect3DSurface9* surface,
                            const void* palette, const RECT* rect) {
    if (!seq::StillActive(g_Block))
        return realImageEncode(path, format, surface, palette, rect);
    if (!failImageEncode) {
        const auto result = realImageEncode(path, format, surface, palette, rect);
        if (SUCCEEDED(result)) {
            ++successfulEncodes;
            if (changeTarget != TargetChange::None) {
                if (changeTarget == TargetChange::Replace) {
                    const auto previous = replayPath.parent_path() / L"displaced.jpg";
                    Check(MoveFileExW(replayPath.c_str(), previous.c_str(), 0), "move the confirmed image aside");
                }
                Check(!fs::exists(replayPath), "competing writer starts with a free path");
                std::ofstream competing(replayPath, std::ios::binary);
                competing << competingImage;
                competing.close();
                Check(bool(competing), "competing writer publishes real bytes before still image publication");
            }
        }
        return result;
    }
    ++partialEncodes;
    std::ofstream file(fs::path(path), std::ios::binary | std::ios::trunc);
    file << "incomplete encoded image";
    file.close();
    Check(bool(file), "the encoder fault writes real partial bytes before failing");
    return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
}

void RunOverwriteFailure(MMDApp* app) {
    Check(fs::is_empty(output), "own a fresh overwrite-failure output folder");
    replayPath = output / L"overwrite.jpg";
    { std::ofstream file(replayPath, std::ios::binary); file << previousImage; Check(bool(file), "seed an existing output"); }
    const auto entries = StillOutputEntries();
    Microsoft::WRL::ComPtr<IFileSaveDialog> picker;
    auto& api = d3dx::Get();
    Check(api.Load() && api.saveSurfaceToFileW && SUCCEEDED(CoCreateInstance(
        CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker))),
        "resolve the real Shell and D3DX encoder methods");
    auto** table = *reinterpret_cast<void***>(picker.Get());
    void* hooks[] = {table[3], table[20], reinterpret_cast<void*>(api.saveSurfaceToFileW), table[7],
                     reinterpret_cast<void*>(&MoveFileExW)};
    Check(MH_CreateHook(hooks[0], &ReplayShow, reinterpret_cast<void**>(&realShow)) == MH_OK &&
          MH_CreateHook(hooks[1], &ReplayResult, reinterpret_cast<void**>(&realResult)) == MH_OK &&
          MH_CreateHook(hooks[2], &EncodeImage, reinterpret_cast<void**>(&realImageEncode)) == MH_OK &&
          MH_CreateHook(hooks[3], &ReplayAdvise, reinterpret_cast<void**>(&realAdvise)) == MH_OK &&
          MH_CreateHook(hooks[4], &AtFinalStillMove, reinterpret_cast<void**>(&realStillMove)) == MH_OK,
        "install scoped still overwrite and encoder observations");
    for (void* hook : hooks) Check(MH_EnableHook(hook) == MH_OK, "activate the overwrite fixture hook");
    picker.Reset();
    app->RenderWidth() = 64;
    app->RenderHeight() = 48;
    const auto capture = [&] {
        replayPicker = nullptr;
        SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
        Check(replayPicker && seq::StillActive(app) && app->RecordingWindow(), "start the actual still command");
        for (unsigned step = 0; app->RecordingWindow() && step < 12; ++step) {
            app->state.messageSeen = 1;
            coordinator_fixture::Drive(app);
        }
        Check(!seq::StillActive(app) && !app->RecordingWindow(), "finish and release the actual still capture");
    };
    const auto before = warnings;
    expectWarning = true;
    failImageEncode = true;
    capture();
    failImageEncode = false;
    expectWarning = false;
    Check(partialEncodes == 1 && warnings == before + 1, "partial encoder failure reports exactly once");
    Check(Bytes(replayPath) == previousImage && StillOutputEntries() == entries,
        "encoder failure preserves old image bytes and leaves no temporary image");

    const HANDLE lock = CreateFileW(replayPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "block publication to the existing fixture image");
    expectWarning = true;
    capture();
    expectWarning = false;
    CloseHandle(lock);
    Check(successfulEncodes == 1 && warnings == before + 2 &&
          Bytes(replayPath) == previousImage && StillOutputEntries() == entries,
        "publication refusal reports once, preserves the old image, and removes the temporary file");

    capture();
    unsigned width = 0, height = 0;
    ImagePixels(replayPath, &width, &height);
    Check(successfulEncodes == 2 && width == 64 && height == 48 &&
          warnings == before + 2 && StillOutputEntries() == entries,
        "retry at the same destination writes one decodable image without a warning or temporary file");

    Check(output.native().size() < 247, "the fixture can construct a boundary-length child directory");
    const auto deep = output / std::wstring(247 - output.native().size(), L'd');
    const auto extended = L"\\\\?\\" + deep.native();
    const BOOL created = CreateDirectoryW(extended.c_str(), nullptr);
    if (!created) std::printf("boundary directory Win32=%lu length=%zu\n", GetLastError(), deep.native().size());
    Check(created && deep.native().size() == 248, "create the fixture's near-limit directory");
    replayPath = deep / L"ab.jpg";
    Check(replayPath.native().size() == 255, "the destination fits the still picker field exactly");
    { std::ofstream file(replayPath, std::ios::binary); file << previousImage; Check(bool(file), "seed the long destination"); }
    capture();
    ImagePixels(replayPath, &width, &height);
    Check(successfulEncodes == 3 && width == 64 && height == 48 && warnings == before + 2 &&
          std::distance(fs::directory_iterator(deep), fs::directory_iterator{}) == 1,
        "the exact-length path can be overwritten and decoded without a staging leak");

    replayPath = output / L"late.jpg";
    const auto beforeLate = StillOutputEntries();
    replayNewDestination = true;
    changeTarget = TargetChange::Create;
    expectWarning = true;
    expectStillDestinationChanged = true;
    capture();
    expectWarning = false;
    replayNewDestination = false;
    changeTarget = TargetChange::None;
    auto afterLate = beforeLate;
    afterLate.push_back(L"late.jpg");
    std::sort(afterLate.begin(), afterLate.end());
    Check(Bytes(replayPath) == competingImage && warnings == before + 3 && StillOutputEntries() == afterLate,
          "a newly occupied destination is preserved without leaking the encoded temporary image");

    changeTarget = TargetChange::Replace;
    expectWarning = true;
    capture();
    expectStillDestinationChanged = false;
    expectWarning = false;
    changeTarget = TargetChange::None;
    afterLate.push_back(L"displaced.jpg");
    std::sort(afterLate.begin(), afterLate.end());
    Check(Bytes(replayPath) == competingImage && Bytes(output / L"displaced.jpg") == competingImage &&
          warnings == before + 4 && StillOutputEntries() == afterLate,
          "a different file at the confirmed path is preserved without leaking staging files");
    capture();
    ImagePixels(replayPath, &width, &height);
    Check(width == 64 && height == 48 && warnings == before + 4 && StillOutputEntries() == afterLate,
          "choosing the destination again after the conflict saves a decodable JPEG");

    const auto confirmed = Bytes(replayPath);
    replayPath = output / L"final.jpg";
    Check(fs::copy_file(output / L"late.jpg", replayPath), "seed an already decodable image at the final test path");
    const auto beforeFinal = StillOutputEntries();
    finalMove = FinalMove::Fail;
    expectWarning = true;
    capture();
    finalMove = FinalMove::Normal;
    expectWarning = false;
    Check(interceptedFinalMoves == 1 && Bytes(replayPath) == confirmed && warnings == before + 5 &&
          StillOutputEntries() == beforeFinal, "a refused final move restores the original JPEG without a leftover file");

    finalMove = FinalMove::CompetingWriter;
    expectWarning = true;
    lastStillWarning.clear();
    capture();
    finalMove = FinalMove::Normal;
    expectWarning = false;
    Check(interceptedFinalMoves == 2 && Bytes(replayPath) == competingImage && warnings == before + 6,
          "a file arriving at the final still publication keeps its bytes and triggers a warning");
    const auto afterFinal = StillOutputEntries();
    std::vector<std::wstring> retained;
    std::set_difference(afterFinal.begin(), afterFinal.end(), beforeFinal.begin(), beforeFinal.end(),
                        std::back_inserter(retained));
    Check(retained.size() == 1 && afterFinal.size() == beforeFinal.size() + 1,
          "only the original image remains alongside the competing file");
    const auto recovery = output / retained.front();
    Check(Bytes(recovery) == confirmed && lastStillWarning.find(recovery.native()) != std::wstring::npos,
          "the warning gives the exact location of the original decodable JPEG");
    capture();
    ImagePixels(replayPath, &width, &height);
    Check(width == 64 && height == 48 && Bytes(recovery) == confirmed && warnings == before + 6 &&
          StillOutputEntries() == afterFinal, "same-path confirmation and retry decode without removing the retained JPEG");
    std::puts("PASS final still publication preserves both files, reports recovery and decodes a retry");
    for (void* hook : hooks)
        Check(MH_DisableHook(hook) == MH_OK && MH_RemoveHook(hook) == MH_OK, "remove overwrite observation");
}
}  // namespace still_shell
