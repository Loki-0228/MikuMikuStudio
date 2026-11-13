void CaptureBackupDialog(HWND dialog, int language) {
    RECT rect{}; GetWindowRect(dialog, &rect);
    const int width = rect.right-rect.left, height=rect.bottom-rect.top;
    HDC dc=GetWindowDC(dialog), memory=CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    void* pixels=nullptr; auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    const auto old=SelectObject(memory,bitmap);
    SendMessageW(dialog,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+width*height*4;
    std::ofstream stream(root/(L"backup-dialog-"+std::to_wstring(language)+L".bmp"),std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file),sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));
    stream.write(static_cast<const char*>(pixels),width*height*4);
    Check(bool(stream),"capture compact backup dialog without displaying it");
    SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(dialog,dc);
}
void BackupDialogChecks(HWND dialog,const as::DialogContext& context) {
    Check(Title(GetDlgItem(dialog,as::KeepPrevious))==as::Text(context.app,L"同时保存 .pmm1",L"Also keep .pmm1",L".pmm1 も保存する"),
        "previous-version checkbox has a short extension-based caption");
    Check(Title(GetDlgItem(dialog,as::Folder))==as::DisplayDirectory(context.value.directory),
        "backup folder displays an EXE-relative path");
    Check(context.tooltip && SendMessageW(context.tooltip,TTM_GETTOOLCOUNT,0,0)==2,
        "version checkbox and folder both have registered hover tools");
    wchar_t hint[1024]{};
    TOOLINFOW tool{sizeof(tool)};tool.hwnd=dialog;
    tool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(dialog,as::KeepPrevious));tool.lpszText=hint;
    SendMessageW(context.tooltip,TTM_GETTEXTW,1024,reinterpret_cast<LPARAM>(&tool));
    Check(wcsstr(hint,L".pmm1") && wcsstr(hint,L"MME") && wcsstr(hint,L".emm"),
        "hover text explains old versions and MME's conditional effect saving in all languages");
    Check(Title(GetDlgItem(dialog,as::Explanation)).find(L".emm")!=std::wstring::npos &&
        Title(GetDlgItem(dialog,as::Explanation)).find(L'\n')==std::wstring::npos,
        "footer contains only the one-line actual EMM save status");
    Check((IsDlgButtonChecked(dialog,as::NumberedManual)==BST_CHECKED)==context.value.numberedManualVersions,
        "independent manual history checkbox reflects the saved setting");
    for(int id:{as::Explanation,as::NumberedManual}) {
        HWND control=GetDlgItem(dialog,id);RECT available{};GetClientRect(control,&available);
        HDC dc=GetDC(control);auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0)));
        const auto text=Title(control);SIZE measured{};GetTextExtentPoint32W(dc,text.c_str(),int(text.size()),&measured);
        SelectObject(dc,old);ReleaseDC(control,dc);
        Check(measured.cx+(id==as::NumberedManual?24:0)<=available.right,"new footer labels fit in every language");
    }
    RECT bounds{};GetClientRect(dialog,&bounds);
    RECT units{0,0,300,183};MapDialogRect(dialog,&units);
    Check(bounds.right<=units.right && bounds.bottom<=units.bottom,"backup settings use the compact dialog size");
}
void BackupDirectoryChecks(MMDApp* app,const fs::path& config) {
    const auto before=as::Current();std::wstring error;
    const auto executable=local_data::ExecutableDirectory();
    Check(as::DisplayDirectory((executable/L"data"/L"backups").wstring())==L"data\\backups",
        "default backup folder is displayed relatively");
    auto custom=before;custom.directory=(executable/L"custom"/L"中文备份").wstring();
    Check(as::Configure(app,custom,error),"configure an EXE-contained custom backup folder");
    wchar_t stored[512]{};GetPrivateProfileStringW(L"autosave",L"directory",L"",stored,512,config.c_str());
    Check(std::wstring(stored)==L"custom\\中文备份","EXE-contained folder is stored relatively");
    const auto elsewhere=root/L"other-cwd";fs::create_directories(elsewhere);
    Check(SetCurrentDirectoryW(elsewhere.c_str())!=FALSE,"change working directory like a file picker");
    Check(as::LoadSettings(config).directory==custom.directory,"relative settings resolve against the EXE, not working directory");
    Check(SetCurrentDirectoryW(executable.c_str())!=FALSE,"restore test working directory");
    const auto sibling=executable.parent_path()/(executable.filename().wstring()+L"-other")/L"backups";
    Check(as::DisplayDirectory(sibling.wstring())==sibling.wstring(),"a sibling with the same prefix retains its absolute path");
    Check(as::Configure(app,before,error),"restore backup settings after relative-path checks");
}

INT_PTR CALLBACK NamingProbe(HWND dialog,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg!=WM_INITDIALOG)return as::NamingProc(dialog,msg,wp,lp);
    as::NamingProc(dialog,msg,wp,lp);
    auto* c=reinterpret_cast<as::NamingContext*>(lp);
    Check(Title(GetDlgItem(dialog,as::Preview)).find(L".pmm")!=std::wstring::npos,"naming preview includes automatic extension");
    const auto preview=GetDlgItem(dialog,as::Preview);
    Check((GetWindowLongW(preview,GWL_STYLE)&ES_READONLY)!=0,"naming preview is a read-only field");
    HDC previewDc=GetDC(dialog);
    Check(reinterpret_cast<HBRUSH>(SendMessageW(dialog,WM_CTLCOLORSTATIC,reinterpret_cast<WPARAM>(previewDc),
        reinterpret_cast<LPARAM>(preview)))==GetSysColorBrush(COLOR_3DFACE),"naming preview paints a grey read-only background");
    ReleaseDC(dialog,previewDc);
    SetDlgItemTextW(dialog,as::Pattern,L"start_end");
    SendDlgItemMessageW(dialog,as::Pattern,EM_SETSEL,5,5);
    as::NamingProc(dialog,WM_COMMAND,as::TokenFirst+3,0);
    Check(Title(GetDlgItem(dialog,as::Pattern))==L"start[/YYYY]_end","four-digit year button inserts at caret");
    SYSTEMTIME now{};GetLocalTime(&now);
    Check(Title(GetDlgItem(dialog,as::Preview))==L"start"+save_naming::Number(now.wYear,4)+L"_end.pmm","preview expands four-digit year");
    SetDlgItemTextW(dialog,as::Pattern,L"[/unknown]");
    Check(!IsWindowEnabled(GetDlgItem(dialog,IDOK)),"unknown token disables confirmation");
    SetDlgItemTextW(dialog,as::Pattern,L"[/NAME]_[/YY][/MM][/DD]_[/N]");
    CaptureBackupDialog(dialog,10+c->app->EnglishUI());
    as::NamingProc(dialog,WM_COMMAND,IDOK,0);
    Check(c->pattern==L"[/NAME]_[/YY][/MM][/DD]_[/N]","naming editor accepts ASCII tokens verbatim");
    return TRUE;
}
void NamingTests(MMDApp* app) {
    SYSTEMTIME time{};time.wYear=2026;time.wMonth=10;time.wDay=23;time.wHour=4;time.wMinute=5;time.wSecond=6;time.wMilliseconds=7;
    std::wstring result,error;
    Check(save_naming::Expand(L"[/NAME]_[/YYYY]_[/YY][/MM][/DD]_[/hh][/mm][/ss]_[/fff]_[/N]",L"工程",time,L"012",result,error) &&
        result==L"工程_2026_261023_040506_007_012","all ASCII tokens expand with correct padding and month/minute case");
    for(const auto* invalid:{L"",L"../bad",L"[/yy]",L"[/YYYY",L"CON",L"LPT1",L"bad.",L"bad ",L"bad\nline"})
        Check(!save_naming::Validate(invalid,error),"invalid filenames and tokens are rejected");
    for(int language:{0,1,2}) {
        app->EnglishUI()=language;as::NamingContext context{app,save_naming::IncrementDefault,L"工程",false};
        Check(UiDialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(as::kNamingDialog),app->MainWindow(),NamingProbe,
            reinterpret_cast<LPARAM>(&context))==IDOK,"naming dialog works in every UI language");
    }
    app->EnglishUI()=2;
}

// Opt-in real modal UI coverage. The old hidden probes above retain their semantics.
// CLI: exe <new-output> --dialog-layout-only <private-desktop> [--require-175]
//      [--geometry-counterexample outside|overlap]
// Runner: copy the EXE/dependencies to a disposable installation, set cwd there,
// and create dialog-layout-fixture.txt containing MMS-dialog-layout-fixture-v1.
// Do not copy configuration/project files; this mode writes a small mmconfig.
// Output must be a nonexistent direct child of that installation. After reading
// output/desktop-ready.txt, OpenDesktop on that exact name in the same window
// station, retain the handle until the child has exited, and atomically publish
// the same bytes as output/desktop-held.txt. Never activate the desktop. Bound
// the child lifetime externally (including shutdown); only kill that child on
// timeout. All handles attached to the UI desktop are left open until exit.
namespace backup_layout {
using SetContext = HANDLE(WINAPI*)(HANDLE);
using WindowDpi = UINT(WINAPI*)(HWND);
using WindowContext = HANDLE(WINAPI*)(HWND);
using Awareness = int(WINAPI*)(HANDLE);
using MonitorScale = HRESULT(WINAPI*)(HMONITOR, int*);
SetContext setContext = nullptr;
WindowDpi windowDpi = nullptr;
WindowContext windowContext = nullptr;
Awareness awareness = nullptr;
MonitorScale monitorScale = nullptr;
// Same-window DPI/monitor mapping evidence. These exports come from the already
// loaded system user32.dll. A missing export is recorded in the capture evidence
// and fails that capture only after the raw observation is flushed.
using ThreadDpiContext = HANDLE(WINAPI*)();
using LogicalToPhysicalPoint = BOOL(WINAPI*)(HWND, LPPOINT);
ThreadDpiContext threadDpiContext = nullptr;
LogicalToPhysicalPoint logicalToPhysical = nullptr;
std::wstring desktopName;
HDESK retainedDesktop = nullptr;
std::ofstream events;
unsigned captures = 0, geometryFindings = 0;
bool observed175 = false, legacyShell175Pair = false;
// Raw diagnostics only: monitor_175_observed records the Shell GetScaleFactorForMonitor
// percentage, and legacy_shell_175_pair_observed keeps the old joint condition.
// Neither is the effective-175% acceptance gate; the per-monitor coverage ledger below is.
// Independent observation: a real native (PMv2) window at 168 DPI, recorded regardless of monitor scale.
bool native168Observed = false;
// Opt-in real-layout counterexample: only the first real settings capture moves
// the genuine Folder control, then restores it before the rest of the sequence.
// Detection and the failure gate remain the production geometry observations.
enum class Counterexample { None, Outside, Overlap };
Counterexample counterexample = Counterexample::None;
unsigned counterexampleInjections = 0, counterexampleRestores = 0;
std::wstring counterexampleCaptureName;
RECT counterexampleOriginalScreen{}, counterexampleOriginalDialog{};
RECT counterexampleInjectedScreen{}, counterexampleInjectedDialog{};
RECT counterexampleRestoredScreen{}, counterexampleRestoredDialog{};
constexpr UINT_PTR driveTimer = 0x4d4d5301;

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        int(value.size()), nullptr, 0, nullptr, nullptr);
    Check(count > 0, "encode layout evidence as UTF-8");
    std::string result(count, '\0');
    Check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()),
        result.data(), count, nullptr, nullptr) == count, "finish UTF-8 evidence encoding");
    return result;
}
std::string Csv(const std::wstring& value) {
    std::string result = "\"";
    for (char ch : Utf8(value)) { if (ch == '"') result += '"'; result += ch; }
    return result + '"';
}
std::wstring ObjectName(HANDLE object) {
    wchar_t name[256]{}; DWORD needed = 0;
    Check(GetUserObjectInformationW(object, UOI_NAME, name, sizeof name, &needed) != FALSE,
        "read desktop/window station identity");
    return name;
}
void RequireDesktop() {
    const auto current = GetThreadDesktop(GetCurrentThreadId());
    BOOL input = TRUE; DWORD needed = 0;
    Check(ObjectName(current) == desktopName && _wcsicmp(desktopName.c_str(), L"Default") != 0,
        "UI thread remains on the exact private non-default desktop");
    Check(GetUserObjectInformationW(current, UOI_IO, &input, sizeof input, &needed) && !input,
        "UI thread desktop is not the input desktop; no foreground fallback");
}
void BootstrapDesktop() {
    constexpr DWORD desktopAccess = DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
        DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL;
    Check(!desktopName.empty() && desktopName.size() < 128 &&
        std::all_of(desktopName.begin(), desktopName.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
        }) && _wcsicmp(desktopName.c_str(), L"Default") != 0, "explicit ASCII private desktop name required");
    unsigned windows = 0;
    Check(EnumThreadWindows(GetCurrentThreadId(), [](HWND, LPARAM p) -> BOOL {
        ++*reinterpret_cast<unsigned*>(p); return TRUE;
    }, reinterpret_cast<LPARAM>(&windows)) && windows == 0, "switch desktop before creating any windows");
    if (ObjectName(GetThreadDesktop(GetCurrentThreadId())) == desktopName) {
        retainedDesktop = OpenDesktopW(desktopName.c_str(), 0, FALSE, desktopAccess);
    } else {
        SetLastError(ERROR_SUCCESS);
        const auto existing = OpenDesktopW(desktopName.c_str(), 0, FALSE, DESKTOP_READOBJECTS);
        const DWORD error = GetLastError();
        if (existing) CloseDesktop(existing);
        Check(!existing && error == ERROR_FILE_NOT_FOUND, "self-created desktop name must be fresh and accessible");
        retainedDesktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, desktopAccess, nullptr);
    }
    if (!retainedDesktop) std::printf("desktop creation/open error=%lu\n", GetLastError());
    Check(retainedDesktop && SetThreadDesktop(retainedDesktop), "attach private desktop without token or ACL changes");
    RequireDesktop();
    const auto ready = root / L"desktop-ready.txt", held = root / L"desktop-held.txt";
    const auto expected = Utf8(desktopName);
    { std::ofstream out(ready, std::ios::binary); out << expected; out.close(); Check(bool(out), "publish desktop name"); }
    std::printf("DIALOG-LAYOUT desktop=%s station=%s pid=%lu waiting-for-holder\n", expected.c_str(),
        Utf8(ObjectName(GetProcessWindowStation())).c_str(), GetCurrentProcessId());
    const auto deadline = GetTickCount64() + 30000;
    while (!fs::exists(held) && GetTickCount64() < deadline) Sleep(10);
    Check(fs::is_regular_file(held) && Bytes(held) == expected, "supervisor holds exact desktop before GUI initialization");
    RequireDesktop();
}
void SetEnv(const wchar_t* name, const fs::path& path) {
    Check(_wputenv_s(name, path.c_str()) == 0, "set isolated Win32 and CRT environment");
}
void PrepareEnvironment(const wchar_t* requested) {
    const auto installation = fs::canonical(local_data::ExecutableDirectory());
    Check(fs::equivalent(fs::current_path(), installation), "cwd must be the disposable EXE installation");
    Check(Bytes(installation / L"dialog-layout-fixture.txt") == "MMS-dialog-layout-fixture-v1",
        "runner explicitly marks the disposable installation (no trailing newline)");
    root = fs::absolute(requested).lexically_normal();
    Check(root.filename() != L"." && root.filename() != L".." && !root.filename().empty() &&
        fs::equivalent(root.parent_path(), installation) && !fs::exists(root),
        "output must be a strictly new direct child of the disposable installation");
    for (auto path = installation; !path.empty(); path = path.parent_path()) {
        const auto attributes = GetFileAttributesW(path.c_str());
        Check(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "fixture ancestors cannot redirect writes through reparse points");
        if (path == path.root_path()) break;
    }
    for (const auto* name : {L"data/settings.json", L"data/autosave.ini", L"data/backups", L"Cache",
        L"Plugins", L"MMEffect.dll", L"MMHack.dll", L"d3d9.dll"})
        Check(!fs::exists(installation / name), "fixture contains no existing user settings, backups, plugins or proxies");
    for (const auto& entry : fs::recursive_directory_iterator(installation)) {
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        Check(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "fixture files and data directories cannot redirect IO through reparse points");
        if (!entry.is_regular_file()) continue;
        const auto extension = entry.path().extension().wstring();
        Check(_wcsicmp(extension.c_str(), L".ini") && _wcsicmp(extension.c_str(), L".json") &&
            _wcsicmp(extension.c_str(), L".pmm"), "disposable fixture must not inherit settings or projects");
    }
    Check(fs::create_directory(root), "create new evidence root without reusing artifacts");
    fs::create_directories(installation / L"data");
    { std::ofstream config(installation / L"data" / L"mmconfig.ini");
      config << "0\n0\n960\n720\n0\n640\n480\n250\n2\n0\n"; config.close();
      Check(bool(config), "seed only private scalar window settings without user file paths"); }
    // Remove inherited test overrides, including trace destinations, before any
    // production singleton reads them. This changes only this disposable process.
    auto* block = GetEnvironmentStringsW(); Check(block != nullptr, "read inherited environment");
    std::vector<std::wstring> remove;
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
        const std::wstring entry(p); const auto split = entry.find(L'=');
        if (_wcsnicmp(p, L"MMS_", 4) == 0 || _wcsnicmp(p, L"MIKUMIKUSTUDIO_", 15) == 0 ||
            _wcsnicmp(p, L"PROBE_", 6) == 0) remove.push_back(entry.substr(0, split));
    }
    FreeEnvironmentStringsW(block);
    for (const auto& name : remove) Check(_wputenv_s(name.c_str(), L"") == 0, "clear inherited production/test override");
    for (const auto* dir : {L"local-profile", L"roaming-profile", L"temp", L"plugins", L"backups"})
        fs::create_directory(root / dir);
    SetEnv(L"LOCALAPPDATA", root / L"local-profile"); SetEnv(L"APPDATA", root / L"roaming-profile");
    SetEnv(L"TEMP", root / L"temp"); SetEnv(L"TMP", root / L"temp");
    SetEnv(L"MMS_PLUGIN_TEST_DIR", root / L"plugins");
    SetEnv(L"MMS_CACHE_MANAGER_DIR", root / L"cache");
    SetEnv(L"MMS_TEXTURE_CACHE_DIR", root / L"textures");
    SetEnv(L"MMS_ANTIALIAS_CONFIG", root / L"antialias.ini");
    SetEnv(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG", root / L"autosave.ini");
    crashDirectory = installation / L"data" / L"backups";
}
struct Monitor { HMONITOR handle; RECT work; std::wstring device; int scale; };
std::vector<Monitor> monitors;
// Effective-175% coverage ledger. One sample is one real CaptureDpiEvidence call
// for this capture's existing stem and this run's explicit language. Entries bind
// to the exact real HMONITOR and device; native and product-unaware samples stay in
// separate cells so one display's native sample never completes another display's
// product coverage.
struct DpiSample {
    bool apiComplete = false;
    HMONITOR actualMonitor = nullptr;
    bool deviceRead = false;
    std::wstring actualDevice;
    int threadAwareness = -1, windowAwareness = -1;
    UINT dpi = 0;
    bool rectRead = false;
    RECT logical{};
    POINT physicalTopLeft{}, physicalBottomRight{};
    bool topLeftMapped = false, bottomRightMapped = false;
    bool nativeIdentity = false, productRatio = false;
    long long logicalWidth = 0, logicalHeight = 0, physicalWidth = 0, physicalHeight = 0;
};
struct CoverageCell {
    std::vector<std::wstring> stems;
    unsigned records = 0;
    bool duplicate = false, rejected = false, allMatch = true;
};
struct MonitorCoverage {
    HMONITOR handle = nullptr;
    std::wstring device;
    int shellScale = 0;
    CoverageCell cells[2][3]{};
};
std::vector<MonitorCoverage> coverage;
// Two outer results times five real modal dialogs (settings plus four nested
// naming dialogs) times initial/edited stages: exactly 20 unique captures.
constexpr unsigned effective175CaptureCount = 20;
struct DialogState { HWND window = nullptr; bool naming = false; unsigned commands = 0; int lastCommand = 0; };
bool checkInterfaceScale = false;
struct ScaleBaseline { int width = 0, height = 0; LONG fontHeight = 0; };
ScaleBaseline scaleBaselines[2];
struct Scenario {
    as::DialogContext context;
    const Monitor* monitor = nullptr;
    std::wstring prefix;
    bool save = false, namingSave = false, backup = false, native = false;
    int language = -1;
    DialogState settings, naming;
};
Scenario* scenario = nullptr;
std::wstring Pattern(bool backup) { return backup ? L"备份_[/NAME]_[/YYYY]_[/N]" : L"增量_[/NAME]_[/N]"; }
bool Same(const as::Settings& a, const as::Settings& b) {
    return a.enabled == b.enabled && a.intervalMinutes == b.intervalMinutes && a.maxBackups == b.maxBackups &&
        a.overwriteOldest == b.overwriteOldest && a.keepPrevious == b.keepPrevious &&
        a.numberedManualVersions == b.numberedManualVersions && a.backupPattern == b.backupPattern &&
        a.incrementPattern == b.incrementPattern && a.directory == b.directory;
}
std::wstring FullText(HWND window) {
    std::wstring value(GetWindowTextLengthW(window) + 1, L'\0');
    value.resize(GetWindowTextW(window, value.data(), int(value.size()))); return value;
}
void RectFields(std::ostream& out, const RECT& rect) {
    out << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom;
}
std::wstring RectText(const RECT& rect) {
    return std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top) + L"," +
        std::to_wstring(rect.right) + L"," + std::to_wstring(rect.bottom);
}
// MapWindowPoints returns zero for a real failure, but a successful mapping with
// zero offset also reports zero on some Windows versions. Treat no Win32 error as
// success instead of rejecting an exact same-space conversion.
bool MapRect(HWND from, HWND to, RECT& rect) {
    SetLastError(ERROR_SUCCESS);
    const int mapped = MapWindowPoints(from, to, reinterpret_cast<POINT*>(&rect), 2);
    return mapped != 0 || GetLastError() == ERROR_SUCCESS;
}
const wchar_t* CounterexampleName() {
    if (counterexample == Counterexample::Outside) return L"outside";
    if (counterexample == Counterexample::Overlap) return L"overlap";
    return L"none";
}
void InjectCounterexample(HWND dialog, const std::wstring& stem) {
    Check(scenario && dialog && scenario->settings.window == dialog,
        "geometry counterexample targets the observed real settings dialog");
    HWND folder = GetDlgItem(dialog, as::Folder);
    Check(folder && IsWindow(folder) && IsWindowVisible(folder) && GetParent(folder) == dialog,
        "geometry counterexample moves the real Folder control as a direct dialog child");
    DWORD dialogProcess = 0, folderProcess = 0;
    Check(GetWindowThreadProcessId(dialog, &dialogProcess) == GetCurrentThreadId() &&
        GetWindowThreadProcessId(folder, &folderProcess) == GetCurrentThreadId() &&
        dialogProcess == GetCurrentProcessId() && folderProcess == GetCurrentProcessId(),
        "geometry counterexample only moves a window owned by this UI thread and process");
    RECT originalScreen{};
    Check(GetWindowRect(folder, &originalScreen), "read the real pre-injection Folder rectangle");
    RECT originalDialog = originalScreen;
    Check(MapRect(nullptr, dialog, originalDialog), "map the real Folder rectangle into dialog client coordinates");
    const int originalWidth = originalDialog.right - originalDialog.left;
    const int originalHeight = originalDialog.bottom - originalDialog.top;
    Check(originalWidth > 0 && originalHeight > 0 && originalWidth < 16384 && originalHeight < 16384,
        "geometry counterexample keeps the real Folder control at a bounded size");
    RECT target = originalDialog;
    int width = originalWidth, height = originalHeight;
    if (counterexample == Counterexample::Outside) {
        target.left = -originalWidth - 8; target.right = target.left + originalWidth;
    } else {
        HWND browse = GetDlgItem(dialog, as::Browse);
        RECT browseScreen{}, browseDialog{};
        Check(browse && IsWindow(browse) && GetParent(browse) == dialog && GetWindowRect(browse, &browseScreen),
            "geometry counterexample overlaps the real sibling Browse control");
        browseDialog = browseScreen;
        Check(MapRect(nullptr, dialog, browseDialog), "map the real Browse rectangle into the same dialog client coordinates");
        target = browseDialog;
        width = target.right - target.left;
        height = target.bottom - target.top;
        Check(width > 0 && height > 0 && width < 16384 && height < 16384,
            "real Browse control has a bounded dialog rectangle");
    }
    Check(SetWindowPos(folder, nullptr, target.left, target.top, width, height,
        SWP_NOZORDER | SWP_NOACTIVATE), "move the real Folder control for the requested geometry counterexample");
    RECT injectedScreen{};
    Check(GetWindowRect(folder, &injectedScreen), "read the perturbed real Folder rectangle");
    RECT injectedDialog = injectedScreen;
    Check(MapRect(nullptr, dialog, injectedDialog), "map the perturbed Folder rectangle into dialog client coordinates");
    counterexampleCaptureName = stem;
    counterexampleOriginalScreen = originalScreen; counterexampleOriginalDialog = originalDialog;
    counterexampleInjectedScreen = injectedScreen; counterexampleInjectedDialog = injectedDialog;
    ++counterexampleInjections;
    events << "counterexample mode=" << Utf8(CounterexampleName()) << " injected=true capture=" << Utf8(stem)
        << " control=Folder id=" << as::Folder << " before_screen=" << Utf8(RectText(originalScreen))
        << " before_dialog=" << Utf8(RectText(originalDialog)) << " target_dialog=" << Utf8(RectText(target))
        << " injected_screen=" << Utf8(RectText(injectedScreen))
        << " injected_dialog=" << Utf8(RectText(injectedDialog)) << '\n';
    events.flush();
}
void RestoreCounterexample(HWND dialog) {
    HWND folder = GetDlgItem(dialog, as::Folder);
    Check(folder && IsWindow(folder) && GetParent(folder) == dialog,
        "restore the real Folder control on the same settings dialog");
    const int width = counterexampleOriginalDialog.right - counterexampleOriginalDialog.left;
    const int height = counterexampleOriginalDialog.bottom - counterexampleOriginalDialog.top;
    Check(SetWindowPos(folder, nullptr, counterexampleOriginalDialog.left, counterexampleOriginalDialog.top,
        width, height, SWP_NOZORDER | SWP_NOACTIVATE), "restore the real Folder control after the counterexample capture");
    RECT restoredScreen{};
    Check(GetWindowRect(folder, &restoredScreen), "read the restored real Folder rectangle");
    RECT restoredDialog = restoredScreen;
    Check(MapRect(nullptr, dialog, restoredDialog), "map the restored Folder rectangle into dialog client coordinates");
    Check(EqualRect(&restoredDialog, &counterexampleOriginalDialog),
        "restored Folder rectangle strictly equals its real pre-injection dialog rectangle");
    counterexampleRestoredScreen = restoredScreen; counterexampleRestoredDialog = restoredDialog;
    ++counterexampleRestores;
    events << "counterexample_restore capture=" << Utf8(counterexampleCaptureName) << " restored=true control=Folder id="
        << as::Folder << " restored_screen=" << Utf8(RectText(restoredScreen))
        << " restored_dialog=" << Utf8(RectText(restoredDialog)) << '\n';
    events.flush();
}
bool DimensionMatches(long long logical, long long physical) {
    if (logical <= 0 || physical <= 0) return false;
    const long long difference = static_cast<long long>(physical) * 4 - static_cast<long long>(logical) * 7;
    return difference >= -4 && difference <= 4;
}
// Raw same-capture DPI, monitor and logical-to-physical mapping evidence.
// Observations and raw failures are written and flushed before the checks below,
// so a failing API keeps the exact values that produced it. This helper never
// changes a DPI awareness context, creates windows, synthesizes WM_DPICHANGED,
// scales a screenshot, reads a foreground/other window, or derives physical
// coordinates from dpi/96 arithmetic.
DpiSample CaptureDpiEvidence(HWND dialog, const std::wstring& stem) {
    const HMONITOR targetMonitor = scenario->monitor->handle;
    const HMONITOR actualMonitor = MonitorFromWindow(dialog, MONITOR_DEFAULTTONULL);
    const UINT64 tick = GetTickCount64();
    const DWORD processId = GetCurrentProcessId();
    const DWORD threadId = GetCurrentThreadId();
    const bool threadApiAvailable = threadDpiContext != nullptr;
    const bool mappingApiAvailable = logicalToPhysical != nullptr;
    const HANDLE threadCtx = threadApiAvailable ? threadDpiContext() : nullptr;
    const int threadAware = threadCtx ? awareness(threadCtx) : -1;
    const HANDLE windowCtx = windowContext(dialog);
    const int windowAware = windowCtx ? awareness(windowCtx) : -1;
    const UINT rawDpi = windowDpi(dialog);
    const int enumeratedScale = scenario->monitor->scale;
    MONITORINFOEXW actualInfo{}; actualInfo.cbSize = sizeof actualInfo;
    const BOOL actualInfoRead = actualMonitor ? GetMonitorInfoW(actualMonitor, &actualInfo) : FALSE;
    HRESULT liveScaleResult = E_FAIL; int liveScale = -1;
    if (actualMonitor) liveScaleResult = monitorScale(actualMonitor, &liveScale);
    SetLastError(ERROR_SUCCESS);
    RECT logicalScreen{};
    const BOOL rectRead = GetWindowRect(dialog, &logicalScreen);
    const DWORD rectError = GetLastError();
    POINT physicalTopLeft{logicalScreen.left, logicalScreen.top};
    POINT physicalBottomRight{logicalScreen.right, logicalScreen.bottom};
    BOOL topLeftMapped = FALSE, bottomRightMapped = FALSE;
    DWORD topLeftError = ERROR_SUCCESS, bottomRightError = ERROR_SUCCESS;
    const bool mappingCalled = mappingApiAvailable && rectRead;
    if (mappingCalled) {
        // Clear last-error before each call so the recorded code belongs to that
        // exact call; success is judged by the BOOL, never by stale last-error.
        SetLastError(ERROR_SUCCESS);
        topLeftMapped = logicalToPhysical(dialog, &physicalTopLeft);
        topLeftError = GetLastError();
        SetLastError(ERROR_SUCCESS);
        bottomRightMapped = logicalToPhysical(dialog, &physicalBottomRight);
        bottomRightError = GetLastError();
    }
    DpiSample sample;
    sample.apiComplete = threadApiAvailable && mappingApiAvailable && rectRead && topLeftMapped &&
        bottomRightMapped && actualMonitor != nullptr && actualInfoRead && liveScaleResult == S_OK;
    sample.actualMonitor = actualMonitor;
    sample.deviceRead = !!actualInfoRead;
    sample.actualDevice = actualInfo.szDevice;
    sample.threadAwareness = threadAware;
    sample.windowAwareness = windowAware;
    sample.dpi = rawDpi;
    sample.rectRead = !!rectRead;
    sample.logical = logicalScreen;
    sample.physicalTopLeft = physicalTopLeft;
    sample.physicalBottomRight = physicalBottomRight;
    sample.topLeftMapped = !!topLeftMapped;
    sample.bottomRightMapped = !!bottomRightMapped;
    // Widen one endpoint before subtracting: a valid 32-bit RECT/POINT span can
    // exceed INT_MAX, and the recorded values stay the raw observed spans.
    sample.logicalWidth = static_cast<long long>(logicalScreen.right) - logicalScreen.left;
    sample.logicalHeight = static_cast<long long>(logicalScreen.bottom) - logicalScreen.top;
    sample.physicalWidth = static_cast<long long>(physicalBottomRight.x) - physicalTopLeft.x;
    sample.physicalHeight = static_cast<long long>(physicalBottomRight.y) - physicalTopLeft.y;
    // Native PMv2 mapping is the identity for this HWND: the real physical corner
    // points equal the same HWND's GetWindowRect corners; nothing is synthesized.
    sample.nativeIdentity = topLeftMapped && bottomRightMapped && sample.logicalWidth > 0 &&
        sample.logicalHeight > 0 && physicalTopLeft.x == logicalScreen.left &&
        physicalTopLeft.y == logicalScreen.top && physicalBottomRight.x == logicalScreen.right &&
        physicalBottomRight.y == logicalScreen.bottom;
    // Product-unaware 175% windows read 96 DPI. The actually read physical rectangle
    // is 7/4 of the logical one; integer rounding of the two mapped corners can shift
    // the observed width or height by at most one physical pixel in total. The gate
    // below admits exactly that: a difference within [-4, 4], never widened.
    sample.productRatio = sample.logicalWidth > 0 && sample.logicalHeight > 0 &&
        DimensionMatches(sample.logicalWidth, sample.physicalWidth) &&
        DimensionMatches(sample.logicalHeight, sample.physicalHeight);
    std::ofstream evidence(root / (stem + L"-dpi.txt"), std::ios::binary);
    evidence << "capture=" << Utf8(stem) << '\n'
        << "tick_ms=" << tick << " pid=" << processId << " tid=" << threadId << '\n'
        << "hwnd=" << reinterpret_cast<UINT_PTR>(dialog) << '\n'
        << "actual_hmonitor=" << reinterpret_cast<UINT_PTR>(actualMonitor)
        << " target_hmonitor=" << reinterpret_cast<UINT_PTR>(targetMonitor) << '\n'
        << "actual_monitor_info_ok=" << !!actualInfoRead
        << " actual_device=" << Utf8(actualInfo.szDevice)
        << " target_device=" << Utf8(scenario->monitor->device) << '\n'
        << "thread_dpi_context=" << reinterpret_cast<UINT_PTR>(threadCtx)
        << " thread_awareness=" << threadAware
        << " thread_dpi_context_api_available=" << !!threadApiAvailable << '\n'
        << "window_dpi_context=" << reinterpret_cast<UINT_PTR>(windowCtx)
        << " window_awareness=" << windowAware << '\n'
        << "get_dpi_for_window_raw=" << rawDpi << '\n'
        << "enumerated_monitor_scale=" << enumeratedScale << '\n'
        << "live_monitor_scale_hr=0x" << std::hex << static_cast<unsigned long>(liveScaleResult)
        << std::dec << " live_monitor_scale_raw=" << liveScale << '\n'
        << "get_window_rect_ok=" << !!rectRead << " get_window_rect_last_error=" << rectError << '\n'
        << "logical_screen_rect=" << Utf8(RectText(logicalScreen)) << '\n'
        << "logical_width=" << logicalScreen.right - logicalScreen.left
        << " logical_height=" << logicalScreen.bottom - logicalScreen.top << '\n'
        << "logical_to_physical_api_available=" << !!mappingApiAvailable << '\n'
        << "mapping_call_state=" << (!mappingApiAvailable ? "skipped-api-unavailable" :
            !rectRead ? "skipped-no-window-rect" : "called") << '\n';
    if (mappingCalled) {
        evidence << "top_left_ok=" << !!topLeftMapped << " top_left_last_error=" << topLeftError << '\n'
            << "bottom_right_ok=" << !!bottomRightMapped << " bottom_right_last_error=" << bottomRightError << '\n'
            << "physical_top_left_point=" << physicalTopLeft.x << ',' << physicalTopLeft.y << '\n'
            << "physical_bottom_right_point=" << physicalBottomRight.x << ',' << physicalBottomRight.y << '\n'
            << "physical_screen_rect=" << physicalTopLeft.x << ',' << physicalTopLeft.y << ','
            << physicalBottomRight.x << ',' << physicalBottomRight.y << '\n'
            << "physical_width=" << physicalBottomRight.x - physicalTopLeft.x
            << " physical_height=" << physicalBottomRight.y - physicalTopLeft.y << '\n';
    } else {
        evidence << "top_left_ok=0 top_left_last_error=0 bottom_right_ok=0 bottom_right_last_error=0\n"
            << "physical_top_left_point=n/a physical_bottom_right_point=n/a physical_screen_rect=n/a\n"
            << "physical_width=n/a physical_height=n/a\n";
    }
    evidence << "coordinates=GetWindowRect corners through LogicalToPhysicalPointForPerMonitorDPI\n"
        << "dpi_ratio_simulation=false screenshot_scaling=false wm_dpichanged_synthesized=false\n"
        << "window_inspection=this-capture-dialog-only foreground_query=false\n";
    evidence.flush();
    evidence.close();
    Check(bool(evidence), "write same-window DPI mapping evidence before verdicts");
    Check(threadApiAvailable && mappingApiAvailable,
        "GetThreadDpiAwarenessContext and LogicalToPhysicalPointForPerMonitorDPI are required; missing APIs are not a pass");
    Check(rectRead && topLeftMapped && bottomRightMapped,
        "same-window logical-to-physical mapping succeeds for both captured dialog corners");
    Check(actualMonitor == targetMonitor && actualInfoRead && liveScaleResult == S_OK,
        "same capture observes the target monitor identity and its live scale");
    return sample;
}
bool SampleQualifies(const DpiSample& sample, bool native) {
    if (!sample.apiComplete || sample.actualMonitor == nullptr || !sample.deviceRead) return false;
    const int expectedAwareness = native ? 2 : 0;
    if (sample.windowAwareness != expectedAwareness || sample.threadAwareness != expectedAwareness) return false;
    if (native) return sample.dpi == 168 && sample.nativeIdentity;
    return sample.dpi == 96 && sample.productRatio;
}
void RecordCoverage(const DpiSample& sample, const std::wstring& stem) {
    const bool native = scenario->native;
    const int language = scenario->language;
    Check(language >= 0 && language < 3, "effective-175 coverage records the explicit run language");
    size_t index = coverage.size();
    for (size_t i = 0; i < coverage.size(); ++i)
        if (coverage[i].handle == sample.actualMonitor) { index = i; break; }
    // Bind by the exact real HMONITOR and device; never join native and product
    // samples from different displays. A changed identity is an explicit failure.
    Check(index < coverage.size() && sample.deviceRead && coverage[index].device == sample.actualDevice &&
        scenario->monitor && sample.actualMonitor == scenario->monitor->handle &&
        scenario->monitor->device == sample.actualDevice,
        "effective-175 sample binds to the exact target HMONITOR and device");
    auto& cell = coverage[index].cells[native ? 1 : 0][language];
    ++cell.records;
    bool duplicate = false;
    for (const auto& existing : cell.stems) if (existing == stem) duplicate = true;
    if (duplicate) cell.duplicate = true; else cell.stems.push_back(stem);
    const bool qualified = SampleQualifies(sample, native);
    // A single unqualified sample poisons the whole cell; later samples never reset it.
    if (!qualified) cell.rejected = true;
    cell.allMatch = cell.allMatch && qualified;
    events << "dpi_coverage monitor=" << index << " device=" << Utf8(sample.actualDevice)
        << " branch=" << (native ? "native" : "product-unaware") << " language=" << language
        << " capture=" << Utf8(stem) << " unique_captures=" << cell.stems.size()
        << " records=" << cell.records << " qualified=" << qualified << " duplicate=" << duplicate
        << " dpi=" << sample.dpi << " window_awareness=" << sample.windowAwareness
        << " thread_awareness=" << sample.threadAwareness
        << " logical_screen_rect=" << Utf8(RectText(sample.logical))
        << " physical_top_left=" << sample.physicalTopLeft.x << ',' << sample.physicalTopLeft.y
        << " physical_bottom_right=" << sample.physicalBottomRight.x << ',' << sample.physicalBottomRight.y
        << " logical_width=" << sample.logicalWidth << " logical_height=" << sample.logicalHeight
        << " physical_width=" << sample.physicalWidth << " physical_height=" << sample.physicalHeight
        << " native_identity=" << sample.nativeIdentity << " product_7_4_ratio=" << sample.productRatio << '\n';
}
bool CellQualified(const CoverageCell& cell) {
    return cell.records == effective175CaptureCount && cell.stems.size() == effective175CaptureCount &&
        !cell.duplicate && !cell.rejected && cell.allMatch;
}
bool MonitorQualified(const MonitorCoverage& monitor) {
    for (int branch = 0; branch < 2; ++branch) for (int language = 0; language < 3; ++language)
        if (!CellQualified(monitor.cells[branch][language])) return false;
    return true;
}
unsigned Effective175MonitorCount() {
    unsigned count = 0;
    for (const auto& monitor : coverage) if (MonitorQualified(monitor)) ++count;
    return count;
}
std::string EffectiveCoverageReport(unsigned qualifiedMonitors) {
    std::string report;
    for (size_t index = 0; index < coverage.size(); ++index) {
        const auto& monitor = coverage[index];
        report += "effective_175_monitor=" + std::to_string(index) + " device=" + Utf8(monitor.device) +
            " shell_monitor_scale_observation=" + std::to_string(monitor.shellScale) + '\n';
        for (int branch = 0; branch < 2; ++branch) for (int language = 0; language < 3; ++language) {
            const auto& cell = monitor.cells[branch][language];
            report += std::string("effective_175_branch=") + (branch ? "native" : "product-unaware") +
                " language=" + std::to_string(language) +
                " unique_captures=" + std::to_string(cell.stems.size()) +
                " records=" + std::to_string(cell.records) +
                " all_recorded_match=" + std::to_string(cell.allMatch && !cell.rejected) +
                " duplicate_stem=" + std::to_string(cell.duplicate) +
                " complete=" + std::to_string(cell.records == effective175CaptureCount &&
                    cell.stems.size() == effective175CaptureCount && !cell.duplicate) +
                " qualified=" + std::to_string(CellQualified(cell)) + '\n';
        }
        report += "effective_175_monitor_qualified=" + std::to_string(MonitorQualified(monitor)) + '\n';
    }
    report += "effective_175_monitor_count=" + std::to_string(qualifiedMonitors) + '\n';
    return report;
}

// Narrowly scoped GDI observation for the real production WM_PRINTCLIENT buffer
// contract. MinHook detours stay inert unless a stack-owned probe is active for
// this exact thread and memory DC; every other GDI call is forwarded unchanged.
struct PrintClientProbe {
    DWORD thread = 0;
    HDC target = nullptr;
    bool active = false;
    bool failAllocation = false;
    bool failCopy = false;
    unsigned allocationAttempts = 0, allocationSuccesses = 0, allocationFailures = 0;
    unsigned copyAttempts = 0, copySuccesses = 0, copyFailures = 0;
    HBITMAP allocated = nullptr;
    HBITMAP source = nullptr;
    BOOL copied = FALSE;
};
thread_local PrintClientProbe* activePrintClientProbe = nullptr;
using CreateDibSectionFn = HBITMAP(WINAPI*)(HDC, const BITMAPINFO*, UINT, void**, HANDLE, DWORD);
using BitBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
CreateDibSectionFn originalCreateDibSection = nullptr;
BitBltFn originalBitBlt = nullptr;
// Live WM_PAINT damage/clip/replay observation for the production BackdropProc.
// One stack-owned probe serves the exact fixture HWND on the current UI thread.
// Hooks below only record bounded facts while that probe is active; every verdict,
// log line and Check happens after UpdateWindow returns. This observes the real
// BeginPaint DC, real rcPaint, real GDI copy/fill/replay calls and the real update
// region instead of a memory DC, a synthesized WM_PAINT or a private helper.
constexpr unsigned livePaintStageCount = 7;
constexpr unsigned livePaintMaxPasses = 4;
constexpr unsigned livePaintMaxChildren = 32;
constexpr unsigned livePaintMaxBackground = 2;
struct LivePaintChild { HWND window = nullptr; RECT client{}; };
struct LivePaintClip {
    bool recorded = false;
    unsigned childRects = 0, clippedChildren = 0;
    int childVisible[livePaintMaxChildren]{};
    unsigned backgroundPoints = 0, backgroundVisible = 0;
    int backgroundPtVisible[livePaintMaxBackground]{-1, -1};
    POINT gap{};
    int gapVisible = -1;
    POINT outside{};
    int outsideVisible = -1;
    bool drawDcLive = false;
};
struct LivePaintProbe {
    DWORD thread = 0;
    HWND window = nullptr;
    bool active = false;
    HDC paintDc = nullptr;
    bool paintDcLive = false;
    bool beginPaintFailed = false;
    unsigned beginPaintCalls = 0;
    // Production paint lease: armed only after the first real matching BeginPaint
    // returns, revoked before its matching EndPaint is forwarded. EndPaint can
    // synchronously release the DC, after which child/system painting may reuse the
    // same numeric HDC; injections and copies must never match that reuse.
    bool firstPaintSeen = false;
    bool leaseActive = false;
    bool firstEndPaintSeen = false;
    bool leaseClosed = false;
    bool endPaintHdcMatched = false;
    bool laterBeginPaintSeen = false;
    unsigned endPaintCalls = 0;
    // Observation-only auxiliary same-dialog scope, active through the existing
    // Observe subclass after the first paint lease closed. It narrates the offscreen
    // or supplied-DC WM_ERASEBKGND/WM_PRINTCLIENT callback that can recreate the
    // production PanelBuffer between stages. It is never a main paint draw match, it
    // never touches first-pass counters or fault budgets, and its DC is valid only
    // while that exact callback is on the stack.
    HDC auxDc = nullptr;
    unsigned auxDepth = 0;
    unsigned auxEraseCallbacks = 0, auxPrintCallbacks = 0;
    unsigned auxAllocationAttempts = 0, auxAllocationSuccesses = 0;
    unsigned auxAllocationMatched = 0, auxAllocationDimMismatch = 0;
    HBITMAP auxAllocationBitmap = nullptr;
    bool auxDcSeen = false;
    HDC paintDcs[livePaintMaxPasses]{};
    RECT paintRects[livePaintMaxPasses]{};
    int mapMode = 0;
    POINT origin{};
    unsigned paintMessages = 0, eraseMessages = 0, eraseAfterReplay = 0;
    HRGN expectedDamage = nullptr;
    RECT client{};
    RECT expectedBox{};
    LivePaintChild children[livePaintMaxChildren];
    unsigned childCount = 0;
    unsigned backgroundCount = 0;
    POINT background[livePaintMaxBackground]{};
    POINT gap{};
    POINT outside{};
    unsigned allocationAttempts = 0, allocationSuccesses = 0, allocationDenials = 0;
    HBITMAP allocated = nullptr, bufferBitmap = nullptr;
    unsigned copyAttempts = 0, copySuccesses = 0, copyDenials = 0;
    bool copyRecorded = false;
    RECT copyRect{};
    int copySourceX = 0, copySourceY = 0;
    DWORD copyRop = 0;
    HBITMAP copySource = nullptr;
    unsigned fillAttempts = 0, fillSuccesses = 0, fillDenials = 0;
    bool fillRecorded = false;
    RECT fillRect{};
    bool deniedFillDamage = false;
    unsigned invalidateCalls = 0, eraseInvalidates = 0, replays = 0;
    unsigned allocationDenialBudget = 0, copyDenialBudget = 0, fillDenialBudget = 0;
    LivePaintClip clip;
};
thread_local LivePaintProbe* activeLivePaintProbe = nullptr;
using BeginPaintFn = HDC(WINAPI*)(HWND, PAINTSTRUCT*);
using EndPaintFn = BOOL(WINAPI*)(HWND, const PAINTSTRUCT*);
using FillRectFn = int(WINAPI*)(HDC, const RECT*, HBRUSH);
using InvalidateRgnFn = BOOL(WINAPI*)(HWND, HRGN, BOOL);
BeginPaintFn originalBeginPaint = nullptr;
EndPaintFn originalEndPaint = nullptr;
FillRectFn originalFillRect = nullptr;
InvalidateRgnFn originalInvalidateRgn = nullptr;
// Runs inside the matched production draw call, before it is forwarded or denied:
// the BeginPaint DC clip is already the live client minus immediate visible children
// intersected with the real update region. -1 from RectVisible/PtVisible is failure.
void ObserveLiveClip(LivePaintProbe& probe, HDC target) {
    if (probe.clip.recorded) return;
    probe.clip.recorded = true;
    probe.clip.drawDcLive = WindowFromDC(target) == probe.window && GetObjectType(target) == OBJ_DC;
    probe.clip.childRects = probe.childCount;
    for (unsigned i = 0; i < probe.childCount; ++i) {
        RECT rect = probe.children[i].client;
        probe.clip.childVisible[i] = RectVisible(target, &rect);
        if (probe.clip.childVisible[i] == FALSE) ++probe.clip.clippedChildren;
    }
    probe.clip.backgroundPoints = probe.backgroundCount;
    for (unsigned i = 0; i < probe.backgroundCount; ++i) {
        probe.clip.backgroundPtVisible[i] = PtVisible(target, probe.background[i].x, probe.background[i].y);
        if (probe.clip.backgroundPtVisible[i] == TRUE) ++probe.clip.backgroundVisible;
    }
    probe.clip.gap = probe.gap;
    probe.clip.gapVisible = PtVisible(target, probe.gap.x, probe.gap.y);
    probe.clip.outside = probe.outside;
    probe.clip.outsideVisible = PtVisible(target, probe.outside.x, probe.outside.y);
}
// Current ownership of a live production draw target. The first real matching
// production paint lease must still be open, and WindowFromDC/GetObjectType are
// re-queried now instead of trusting the cached BeginPaint fact: a numeric HDC
// reused after EndPaint is not the production paint DC. The caller LastError is
// saved and restored around these queries; the caller still captures its own
// incoming LastError before forwarding or denying the original call.
bool LiveOwnedPaintDc(LivePaintProbe& live, HDC target) {
    if (!live.leaseActive || target != live.paintDc) return false;
    const DWORD saved = GetLastError();
    const bool owned = WindowFromDC(target) == live.window && GetObjectType(target) == OBJ_DC;
    SetLastError(saved);
    return owned;
}
HBITMAP WINAPI ObservedCreateDibSection(HDC target, const BITMAPINFO* info, UINT usage, void** bits, HANDLE section, DWORD offset) {
    auto* live = activeLivePaintProbe;
    if (live && live->active) {
        const DWORD incoming = GetLastError();
        if (GetCurrentThreadId() == live->thread) {
            if (LiveOwnedPaintDc(*live, target)) {
                ++live->allocationAttempts;
                if (live->allocationDenialBudget > 0) {
                    --live->allocationDenialBudget;
                    ++live->allocationDenials;
                    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                    return nullptr;
                }
                SetLastError(incoming);
                const HBITMAP bitmap = originalCreateDibSection(target, info, usage, bits, section, offset);
                const DWORD returned = GetLastError();
                if (bitmap) { ++live->allocationSuccesses; live->allocated = bitmap; live->bufferBitmap = bitmap; }
                SetLastError(returned);
                return bitmap;
            }
            if (live->auxDepth > 0 && target == live->auxDc) {
                // Auxiliary same-dialog supplied-DC callback after the first paint
                // lease closed. Observation only: never a denial, never a first-pass
                // counter. Update the known production buffer only for a successful
                // actual allocation matching the dialog client size and 32-bit DIB.
                ++live->auxAllocationAttempts;
                SetLastError(incoming);
                const HBITMAP bitmap = originalCreateDibSection(target, info, usage, bits, section, offset);
                const DWORD returned = GetLastError();
                if (bitmap) {
                    ++live->auxAllocationSuccesses;
                    const LONG expectedWidth = live->client.right - live->client.left;
                    const LONG expectedHeight = live->client.bottom - live->client.top;
                    const bool expectedDib = info && info->bmiHeader.biPlanes == 1 &&
                        info->bmiHeader.biBitCount == 32 && info->bmiHeader.biCompression == BI_RGB &&
                        info->bmiHeader.biWidth == expectedWidth &&
                        info->bmiHeader.biHeight == -expectedHeight;
                    if (expectedDib) {
                        ++live->auxAllocationMatched;
                        live->auxAllocationBitmap = bitmap;
                        live->bufferBitmap = bitmap;
                    } else {
                        ++live->auxAllocationDimMismatch;
                    }
                }
                SetLastError(returned);
                return bitmap;
            }
        }
        SetLastError(incoming);
    }
    auto* probe = activePrintClientProbe;
    if (probe && GetCurrentThreadId() == probe->thread && probe->active && target == probe->target) {
        ++probe->allocationAttempts;
        if (probe->failAllocation) {
            ++probe->allocationFailures;
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        const HBITMAP bitmap = originalCreateDibSection(target, info, usage, bits, section, offset);
        if (bitmap) { ++probe->allocationSuccesses; probe->allocated = bitmap; }
        return bitmap;
    }
    return originalCreateDibSection(target, info, usage, bits, section, offset);
}
BOOL WINAPI ObservedBitBlt(HDC target, int x, int y, int width, int height, HDC source, int sourceX, int sourceY, DWORD operation) {
    auto* live = activeLivePaintProbe;
    if (live && live->active) {
        const DWORD incoming = GetLastError();
        if (GetCurrentThreadId() == live->thread && LiveOwnedPaintDc(*live, target)) {
            const HBITMAP sourceBitmap = reinterpret_cast<HBITMAP>(GetCurrentObject(source, OBJ_BITMAP));
            if (live->bufferBitmap && sourceBitmap == live->bufferBitmap) {
                ++live->copyAttempts;
                if (!live->copyRecorded) {
                    live->copyRecorded = true;
                    live->copyRect = RECT{x, y, x + width, y + height};
                    live->copySourceX = sourceX;
                    live->copySourceY = sourceY;
                    live->copyRop = operation;
                    live->copySource = sourceBitmap;
                    ObserveLiveClip(*live, target);
                }
                const bool expectedDamage = x == live->expectedBox.left && y == live->expectedBox.top &&
                    width == live->expectedBox.right - live->expectedBox.left &&
                    height == live->expectedBox.bottom - live->expectedBox.top;
                if (live->copyDenialBudget > 0 && expectedDamage) {
                    --live->copyDenialBudget;
                    ++live->copyDenials;
                    SetLastError(ERROR_GEN_FAILURE);
                    return FALSE;
                }
                SetLastError(incoming);
                const BOOL result = originalBitBlt(target, x, y, width, height, source, sourceX, sourceY, operation);
                const DWORD returned = GetLastError();
                if (result) ++live->copySuccesses;
                SetLastError(returned);
                return result;
            }
        }
        // Unmatched live target or unmatched production source: the probe own
        // queries above can change LastError, so restore it before forwarding.
        SetLastError(incoming);
    }
    auto* probe = activePrintClientProbe;
    if (probe && GetCurrentThreadId() == probe->thread && probe->active && target == probe->target) {
        ++probe->copyAttempts;
        probe->source = reinterpret_cast<HBITMAP>(GetCurrentObject(source, OBJ_BITMAP));
        if (probe->failCopy) {
            ++probe->copyFailures;
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        probe->copied = originalBitBlt(target, x, y, width, height, source, sourceX, sourceY, operation);
        if (probe->copied) ++probe->copySuccesses;
        return probe->copied;
    }
    return originalBitBlt(target, x, y, width, height, source, sourceX, sourceY, operation);
}
HDC WINAPI ObservedBeginPaint(HWND window, PAINTSTRUCT* paint) {
    auto* live = activeLivePaintProbe;
    if (!live || !live->active || GetCurrentThreadId() != live->thread || window != live->window)
        return originalBeginPaint(window, paint);
    const DWORD incoming = GetLastError();
    SetLastError(incoming);
    const HDC target = originalBeginPaint(window, paint);
    const DWORD returned = GetLastError();
    const unsigned pass = live->beginPaintCalls++;
    if (target && paint) {
        if (pass < livePaintMaxPasses) {
            live->paintDcs[pass] = target;
            live->paintRects[pass] = paint->rcPaint;
        }
        if (pass == 0) {
            // First real production BeginPaint: record it, then arm the one-shot
            // production paint lease after the real call returned. The lease gates
            // every live CreateDIBSection/BitBlt/FillRect production-draw match.
            live->paintDc = target;
            live->paintDcLive = WindowFromDC(target) == window && GetObjectType(target) == OBJ_DC;
            live->mapMode = GetMapMode(target);
            GetViewportOrgEx(target, &live->origin);
            live->firstPaintSeen = true;
            live->leaseActive = true;
        } else {
            // Later native-fallback BeginPaint is recorded only. It must not
            // rearm injection for the first production pass; the lease stays closed.
            live->laterBeginPaintSeen = true;
        }
    } else {
        live->beginPaintFailed = true;
    }
    SetLastError(returned);
    return target;
}
BOOL WINAPI ObservedEndPaint(HWND window, const PAINTSTRUCT* paint) {
    auto* live = activeLivePaintProbe;
    if (!live || !live->active || GetCurrentThreadId() != live->thread || window != live->window)
        return originalEndPaint(window, paint);
    const DWORD incoming = GetLastError();
    ++live->endPaintCalls;
    if (live->leaseActive && live->firstPaintSeen) {
        // The first EndPaint on this exact dialog after the first BeginPaint is the
        // production EndPaint. Revoke the lease BEFORE forwarding it: the real
        // EndPaint can synchronously release this DC, and child/system painting can
        // then reuse the same numeric HDC before UpdateWindow returns. The handle
        // match is recorded and asserted later, so a mismatch fails explicitly
        // instead of leaking injection into later work.
        live->endPaintHdcMatched = paint && paint->hdc == live->paintDc;
        live->leaseActive = false;
        live->firstEndPaintSeen = true;
        live->leaseClosed = true;
    }
    SetLastError(incoming);
    const BOOL result = originalEndPaint(window, paint);
    const DWORD returned = GetLastError();
    SetLastError(returned);
    return result;
}
int WINAPI ObservedFillRect(HDC target, const RECT* rect, HBRUSH brush) {
    auto* live = activeLivePaintProbe;
    if (live && live->active) {
        const DWORD incoming = GetLastError();
        if (GetCurrentThreadId() == live->thread && LiveOwnedPaintDc(*live, target)) {
            ++live->fillAttempts;
            if (!live->fillRecorded) {
                live->fillRecorded = true;
                if (rect) live->fillRect = *rect;
                ObserveLiveClip(*live, target);
            }
            const bool expectedDamage = rect && EqualRect(rect, &live->expectedBox);
            if (live->fillDenialBudget > 0 && expectedDamage) {
                --live->fillDenialBudget;
                ++live->fillDenials;
                live->deniedFillDamage = true;
                SetLastError(ERROR_GEN_FAILURE);
                return 0;
            }
            SetLastError(incoming);
            const int result = originalFillRect(target, rect, brush);
            const DWORD returned = GetLastError();
            if (result != 0) ++live->fillSuccesses;
            SetLastError(returned);
            return result;
        }
        SetLastError(incoming);
    }
    return originalFillRect(target, rect, brush);
}
BOOL WINAPI ObservedInvalidateRgn(HWND window, HRGN region, BOOL erase) {
    auto* live = activeLivePaintProbe;
    if (!live || !live->active || GetCurrentThreadId() != live->thread || window != live->window)
        return originalInvalidateRgn(window, region, erase);
    const DWORD incoming = GetLastError();
    ++live->invalidateCalls;
    if (erase) {
        ++live->eraseInvalidates;
        if (region && EqualRgn(live->expectedDamage, region) == TRUE) ++live->replays;
    }
    SetLastError(incoming);
    const BOOL result = originalInvalidateRgn(window, region, erase);
    const DWORD returned = GetLastError();
    SetLastError(returned);
    return result;
}
unsigned livePaintInitialDialogs = 0, livePaintStageExecutions = 0, livePaintNativeReplays = 0;
unsigned livePaintClipStages = 0, livePaintClippedChildren = 0, livePaintExposedPoints = 0, livePaintGapPoints = 0;
struct LiveControlEntry {
    HWND window = nullptr;
    int id = 0;
    LONG_PTR style = 0, exstyle = 0;
    RECT client{};
    bool visible = false;
};
struct LiveSnapshot {
    unsigned children = 0;
    LiveControlEntry entries[livePaintMaxChildren];
    RECT client{}, frame{};
    HWND focus = nullptr;
    LONG_PTR context = 0;
};
bool LiveSnapshotCapture(HWND dialog, LiveSnapshot& snapshot) {
    snapshot = LiveSnapshot{};
    if (!GetClientRect(dialog, &snapshot.client) || !GetWindowRect(dialog, &snapshot.frame)) return false;
    snapshot.focus = GetFocus();
    snapshot.context = GetWindowLongPtrW(dialog, DWLP_USER);
    for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (snapshot.children >= livePaintMaxChildren) return false;
        LiveControlEntry& entry = snapshot.entries[snapshot.children];
        RECT rect{};
        if (!GetWindowRect(child, &rect) || !MapRect(nullptr, dialog, rect)) return false;
        entry.window = child;
        entry.id = GetDlgCtrlID(child);
        entry.style = GetWindowLongPtrW(child, GWL_STYLE);
        entry.exstyle = GetWindowLongPtrW(child, GWL_EXSTYLE);
        entry.client = rect;
        entry.visible = IsWindowVisible(child) != FALSE;
        ++snapshot.children;
    }
    return true;
}
bool LiveSnapshotSame(const LiveSnapshot& before, const LiveSnapshot& after) {
    if (before.children != after.children || !EqualRect(&before.client, &after.client) ||
        !EqualRect(&before.frame, &after.frame) || before.focus != after.focus ||
        before.context != after.context) return false;
    for (unsigned i = 0; i < before.children; ++i) {
        const LiveControlEntry& a = before.entries[i];
        const LiveControlEntry& b = after.entries[i];
        if (a.window != b.window || a.id != b.id || a.style != b.style || a.exstyle != b.exstyle ||
            a.visible != b.visible || !EqualRect(&a.client, &b.client)) return false;
    }
    return true;
}
bool LiveCoversRect(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.top >= outer.top &&
        inner.right <= outer.right && inner.bottom <= outer.bottom;
}
bool LivePointInChild(const LivePaintProbe& probe, POINT point) {
    for (unsigned i = 0; i < probe.childCount; ++i) {
        const RECT& rect = probe.children[i].client;
        if (point.x >= rect.left && point.x < rect.right &&
            point.y >= rect.top && point.y < rect.bottom) return true;
    }
    return false;
}
unsigned printClientInitialDialogs = 0, printClientStageExecutions = 0;
void ProbePrintClient(HWND dialog, const std::wstring& stem) {
    Check(scenario && dialog && IsWindowVisible(dialog) &&
        GetWindowThreadProcessId(dialog, nullptr) == GetCurrentThreadId(),
        "WM_PRINTCLIENT probe targets the real visible initial dialog on this UI thread");
    RECT client{}, windowBefore{};
    Check(GetClientRect(dialog, &client) && GetWindowRect(dialog, &windowBefore),
        "read the real initial dialog client and window rectangles");
    const int width = client.right - client.left, height = client.bottom - client.top;
    Check(width > 20 && height > 20 && width < 16384 && height < 16384,
        "WM_PRINTCLIENT probe requires a sensible positive dialog client size");
    const HWND focusBefore = GetFocus();
    const int bitmapWidth = width + 16, bitmapHeight = height + 16;
    HDC windowDc = GetDC(dialog);
    Check(windowDc != nullptr, "acquire a display DC for the probe DIB");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmapWidth;
    info.bmiHeader.biHeight = -bitmapHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const HDC memory = CreateCompatibleDC(windowDc);
    ReleaseDC(dialog, windowDc);
    Check(bitmap && pixels, "allocate the probe DIB before any GDI observation");
    Check(memory != nullptr, "create the probe memory DC before any GDI observation");
    const HGDIOBJ originalBitmap = SelectObject(memory, bitmap);
    Check(originalBitmap != nullptr && originalBitmap != HGDI_ERROR, "select the probe DIB into the probe memory DC");
    const HGDIOBJ originalFont = GetCurrentObject(memory, OBJ_FONT);
    Check(originalFont != nullptr, "snapshot the probe DC selected font");
    POINT originalOrigin{};
    Check(GetViewportOrgEx(memory, &originalOrigin), "snapshot the probe DC viewport origin");
    const int originalMapMode = GetMapMode(memory);
    Check(originalMapMode != 0, "snapshot the probe DC mapping mode");
    const HRGN originalClip = CreateRectRgn(0, 0, 0, 0);
    Check(originalClip != nullptr, "create the probe DC original clip snapshot");
    const int originalClipType = GetClipRgn(memory, originalClip);
    Check(originalClipType >= 0, "snapshot the probe DC clip region");
    Check(SetMapMode(memory, MM_TEXT) != 0, "select MM_TEXT for the probe memory DC");
    POINT previousOrigin{};
    Check(SetViewportOrgEx(memory, 3, 5, &previousOrigin) && previousOrigin.x == originalOrigin.x &&
        previousOrigin.y == originalOrigin.y, "set the nonzero probe viewport origin");
    Check(SelectClipRgn(memory, nullptr) != ERROR, "clear any inherited probe clip region");
    Check(IntersectClipRect(memory, client.left + 7, client.top + 9, client.right - 7, client.bottom - 9) != ERROR,
        "intersect the probe logical clip with the required shifted client rectangle");
    const RECT shiftedClip{client.left + 10, client.top + 14, client.right - 4, client.bottom - 4};
    Check(shiftedClip.right > shiftedClip.left && shiftedClip.bottom > shiftedClip.top,
        "the shifted probe clip has a positive drawable area");
    const HGDIOBJ probeBitmap = GetCurrentObject(memory, OBJ_BITMAP);
    const HGDIOBJ probeFont = GetCurrentObject(memory, OBJ_FONT);
    POINT probeOrigin{};
    Check(reinterpret_cast<HBITMAP>(probeBitmap) == bitmap && probeFont == originalFont &&
        GetViewportOrgEx(memory, &probeOrigin) && probeOrigin.x == 3 && probeOrigin.y == 5,
        "the probe DC keeps its selected bitmap, font and shifted origin");
    const HRGN expectedClip = CreateRectRgn(0, 0, 0, 0);
    Check(expectedClip != nullptr && GetClipRgn(memory, expectedClip) == 1,
        "snapshot the exact probe DC clip region");
    const COLORREF face = GetSysColor(COLOR_3DFACE);
    const DWORD facePixel = (DWORD(GetRValue(face)) << 16) | (DWORD(GetGValue(face)) << 8) | DWORD(GetBValue(face));
    const size_t pixelCount = size_t(bitmapWidth) * size_t(bitmapHeight);
    struct Pixels { unsigned inside = 0, faceMismatch = 0, outside = 0, magentaMismatch = 0; };
    auto Analyze = [&]() {
        Pixels result;
        const DWORD* values = static_cast<const DWORD*>(pixels);
        for (int y = 0; y < bitmapHeight; ++y) for (int x = 0; x < bitmapWidth; ++x) {
            const DWORD pixel = values[size_t(y) * size_t(bitmapWidth) + size_t(x)] & 0x00ffffffu;
            if (x >= shiftedClip.left && x < shiftedClip.right && y >= shiftedClip.top && y < shiftedClip.bottom) {
                ++result.inside;
                if (pixel != facePixel) ++result.faceMismatch;
            } else {
                ++result.outside;
                if (pixel != (0x00ff00ffu & 0x00ffffffu)) ++result.magentaMismatch;
            }
        }
        return result;
    };
    enum class Fault { None, Allocation, Copy };
    struct Stage { const wchar_t* name; bool invalidate; Fault fault; };
    const Stage stages[]{
        {L"cold", true, Fault::None},
        {L"warm", false, Fault::None},
        {L"allocation-denial", true, Fault::Allocation},
        {L"recovery", false, Fault::None},
        {L"copy-denial", false, Fault::Copy},
        {L"final-recovery", false, Fault::None},
    };
    constexpr int stageCount = 6;
    HBITMAP coldSource = nullptr, recoveredSource = nullptr;
    ++printClientInitialDialogs;
    for (int index = 0; index < stageCount; ++index) {
        const Stage& stage = stages[index];
        Check(GdiFlush() != FALSE, "flush probe GDI state before the sentinel reset");
        std::fill_n(static_cast<DWORD*>(pixels), pixelCount, DWORD(0x00ff00ff));
        if (stage.invalidate) SendMessageW(dialog, WM_SYSCOLORCHANGE, 0, 0);
        PrintClientProbe probe;
        probe.thread = GetCurrentThreadId();
        probe.target = memory;
        probe.failAllocation = stage.fault == Fault::Allocation;
        probe.failCopy = stage.fault == Fault::Copy;
        probe.active = true;
        activePrintClientProbe = &probe;
        SendMessageW(dialog, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
        probe.active = false;
        activePrintClientProbe = nullptr;
        const BOOL flushed = GdiFlush();
        ++printClientStageExecutions;
        const unsigned allocationAttempts = probe.allocationAttempts;
        const unsigned allocationSuccesses = probe.allocationSuccesses;
        const unsigned allocationFailures = probe.allocationFailures;
        const unsigned copyAttempts = probe.copyAttempts;
        const unsigned copySuccesses = probe.copySuccesses;
        const unsigned copyFailures = probe.copyFailures;
        const HBITMAP source = probe.source;
        const HBITMAP allocated = probe.allocated;
        const BOOL copied = probe.copied;
        POINT originAfter{};
        const HGDIOBJ bitmapAfter = GetCurrentObject(memory, OBJ_BITMAP);
        const HGDIOBJ fontAfter = GetCurrentObject(memory, OBJ_FONT);
        const HRGN clipAfter = CreateRectRgn(0, 0, 0, 0);
        const int clipAfterType = clipAfter ? GetClipRgn(memory, clipAfter) : -1;
        const bool clipSame = clipAfter != nullptr && clipAfterType == 1 && EqualRgn(expectedClip, clipAfter) != FALSE;
        if (clipAfter) DeleteObject(clipAfter);
        const bool viewportSame = GetViewportOrgEx(memory, &originAfter) != FALSE &&
            originAfter.x == probeOrigin.x && originAfter.y == probeOrigin.y;
        const bool bitmapSame = bitmapAfter == probeBitmap;
        const bool fontSame = fontAfter == probeFont;
        const Pixels observed = Analyze();
        events << "wm_printclient stage=" << index << " name=" << Utf8(stage.name)
            << " capture=" << Utf8(stem) << " hwnd=" << reinterpret_cast<UINT_PTR>(dialog)
            << " local_invalidate=" << stage.invalidate
            << " allocation_attempts=" << allocationAttempts
            << " allocation_successes=" << allocationSuccesses
            << " allocation_denials=" << allocationFailures
            << " copy_attempts=" << copyAttempts << " copy_successes=" << copySuccesses
            << " copy_denials=" << copyFailures
            << " allocated_bitmap=" << reinterpret_cast<UINT_PTR>(allocated)
            << " source_bitmap=" << reinterpret_cast<UINT_PTR>(source)
            << " copied=" << copied
            << " inside_pixels=" << observed.inside << " face_mismatches=" << observed.faceMismatch
            << " outside_pixels=" << observed.outside << " magenta_mismatches=" << observed.magentaMismatch
            << " viewport_changed=" << !viewportSame << " clip_changed=" << !clipSame
            << " bitmap_changed=" << !bitmapSame << " font_changed=" << !fontSame << '\n';
        events.flush();
        Check(flushed != FALSE, "flush real WM_PRINTCLIENT GDI output before inspection");
        Check(viewportSame && clipSame && bitmapSame && fontSame,
            "WM_PRINTCLIENT leaves the caller viewport, clip region, selected bitmap and font unchanged");
        Check(observed.inside > 0 && observed.faceMismatch == 0 && observed.magentaMismatch == 0,
            "WM_PRINTCLIENT paints COLOR_3DFACE inside the shifted clipped client and leaves magenta outside");
        if (index == 0) {
            Check(allocationAttempts == 1 && allocationSuccesses == 1 && allocationFailures == 0 &&
                copyAttempts == 1 && copySuccesses == 1 && copyFailures == 0,
                "cold WM_PRINTCLIENT reallocates exactly once, copies exactly once and injects no fault");
            Check(allocated != nullptr && source != nullptr && source == allocated,
                "cold WM_PRINTCLIENT buffered copy uses the actual newly allocated production bitmap");
            coldSource = source;
        } else if (index == 1) {
            Check(allocationAttempts == 0 && allocationSuccesses == 0 && allocationFailures == 0 &&
                copyAttempts == 1 && copySuccesses == 1 && copyFailures == 0,
                "warm WM_PRINTCLIENT reuses the production buffer and performs exactly one buffered copy");
            Check(source != nullptr && source == coldSource,
                "warm WM_PRINTCLIENT copies from the same production source bitmap as the cold stage");
        } else if (index == 2) {
            Check(allocationAttempts == 1 && allocationFailures == 1 && allocationSuccesses == 0 &&
                copyAttempts == 0 && copyFailures == 0,
                "allocation denial fails exactly one CreateDIBSection and performs no buffered copy");
            Check(allocated == nullptr && source == nullptr,
                "allocation denial exposes no production bitmap to copy from");
        } else if (index == 3) {
            Check(allocationAttempts == 1 && allocationSuccesses == 1 && allocationFailures == 0 &&
                copyAttempts == 1 && copySuccesses == 1 && copyFailures == 0,
                "recovery reallocates exactly once and performs exactly one buffered copy");
            Check(source != nullptr && source == allocated,
                "recovery copies from the actual newly allocated production bitmap");
            recoveredSource = source;
        } else if (index == 4) {
            Check(allocationAttempts == 0 && allocationFailures == 0 &&
                copyAttempts == 1 && copyFailures == 1 && copySuccesses == 0,
                "copy denial fails exactly one BitBlt without reallocating the production buffer");
            Check(source != nullptr, "copy denial records the attempted production source bitmap");
        } else {
            Check(allocationAttempts == 0 && allocationFailures == 0 &&
                copyAttempts == 1 && copySuccesses == 1 && copyFailures == 0,
                "final recovery reuses the cached production buffer for exactly one buffered copy");
            Check(source != nullptr && source == recoveredSource,
                "final recovery copies from the same recovered production source bitmap");
        }
    }
    Check(GetFocus() == focusBefore, "WM_PRINTCLIENT probe leaves the dialog focus unchanged");
    RECT clientAfter{}, windowAfter{};
    Check(GetClientRect(dialog, &clientAfter) && EqualRect(&clientAfter, &client),
        "WM_PRINTCLIENT probe leaves the dialog client dimensions unchanged");
    Check(GetWindowRect(dialog, &windowAfter) && EqualRect(&windowAfter, &windowBefore),
        "WM_PRINTCLIENT probe leaves the dialog window rectangle unchanged");
    const HGDIOBJ restored = SelectObject(memory, originalBitmap);
    Check(restored != nullptr && restored != HGDI_ERROR, "restore the probe DC original bitmap");
    Check(SetMapMode(memory, originalMapMode) != 0, "restore the probe DC mapping mode");
    Check(SetViewportOrgEx(memory, originalOrigin.x, originalOrigin.y, nullptr) != FALSE,
        "restore the probe DC viewport origin");
    Check(SelectClipRgn(memory, originalClipType == 1 ? originalClip : nullptr) != ERROR,
        "restore the probe DC clip region");
    Check(GetCurrentObject(memory, OBJ_BITMAP) == originalBitmap && GetCurrentObject(memory, OBJ_FONT) == originalFont,
        "probe DC restored to its original selected objects");
    DeleteObject(expectedClip);
    DeleteObject(originalClip);
    DeleteObject(bitmap);
    Check(DeleteDC(memory) != FALSE, "release the probe memory DC");
    Check(activePrintClientProbe == nullptr,
        "WM_PRINTCLIENT probe leaves no active GDI observation");
}

// Real live WM_PAINT probe: seven stages on the actual dialog HWND. Each stage
// invalidates a complex two-disjoint-rect damage with FALSE erase, verifies the
// actual update region, then lets UpdateWindow dispatch the real production paint.
// Faults only deny the matched production GDI calls; the rest of the stage shows
// the direct-fill recovery or the production native replay. This is GDI clip/call
// evidence on the real dialog DC, not DWM final-pixel proof.
void ProbeLivePaint(HWND dialog, const std::wstring& stem) {
    Check(scenario && dialog && IsWindowVisible(dialog) &&
        GetWindowThreadProcessId(dialog, nullptr) == GetCurrentThreadId(),
        "live WM_PAINT probe targets the real visible initial dialog on this UI thread");
    Check(activePrintClientProbe == nullptr,
        "live WM_PAINT probe starts with the WM_PRINTCLIENT observation inactive");
    RECT client{};
    Check(GetClientRect(dialog, &client), "read the real live dialog client rectangle");
    const int width = client.right - client.left, height = client.bottom - client.top;
    Check(width >= 64 && height >= 64 && width < 16384 && height < 16384,
        "live WM_PAINT probe requires a bounded positive dialog client size");
    RECT first{4, 4, width / 2, height / 2};
    RECT second{width / 2 + 8, height / 2 + 8, width - 4, height - 4};
    Check(first.right > first.left + 8 && first.bottom > first.top + 8 &&
        second.right > second.left && second.bottom > second.top &&
        second.right < width && second.bottom < height,
        "the two live damage rectangles are positive client rectangles strictly inside the dialog");
    RECT overlap{};
    Check(!IntersectRect(&overlap, &first, &second), "the two live damage rectangles are disjoint");
    const RECT expectedBox{first.left, first.top, second.right, second.bottom};
    HRGN expected = CreateRectRgn(0, 0, 0, 0);
    HRGN firstRegion = CreateRectRgnIndirect(&first);
    HRGN secondRegion = CreateRectRgnIndirect(&second);
    HRGN actual = CreateRectRgn(0, 0, 0, 0);
    Check(expected && firstRegion && secondRegion && actual,
        "allocate the preallocated live damage, update-region and replay facts");
    const int combined = CombineRgn(expected, firstRegion, secondRegion, RGN_OR);
    DeleteObject(firstRegion);
    DeleteObject(secondRegion);
    RECT regionBox{};
    Check(combined == COMPLEXREGION && GetRgnBox(expected, &regionBox) != ERROR &&
        EqualRect(&regionBox, &expectedBox),
        "the constructed live damage is a complex two-rect region with the expected bounding box");
    LivePaintProbe probe;
    probe.thread = GetCurrentThreadId();
    probe.window = dialog;
    probe.expectedDamage = expected;
    probe.client = client;
    probe.expectedBox = expectedBox;
    probe.gap = POINT{width / 2 + 4, height / 2 + 4};
    probe.outside = POINT{expectedBox.right + 1, expectedBox.bottom + 1};
    POINT gapPoint = probe.gap, outsidePoint = probe.outside;
    Check(PtInRect(&expectedBox, gapPoint) && !PtInRegion(expected, probe.gap.x, probe.gap.y),
        "the non-damaged gap point is inside the damage bounding box and outside both rectangles");
    Check(!PtInRect(&expectedBox, outsidePoint) && probe.outside.x < width && probe.outside.y < height,
        "the outside point lies beyond the damage bounding box but inside the dialog client");
    // Immediate visible child rectangles, read from the real HWNDs and mapped with
    // the same zero/last-error semantics before any observation is active.
    for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        Check(probe.childCount < livePaintMaxChildren, "bounded immediate visible child list");
        RECT rect{};
        Check(GetWindowRect(child, &rect), "read a real immediate visible child rectangle");
        Check(MapRect(nullptr, dialog, rect), "map the real child rectangle into dialog client coordinates");
        probe.children[probe.childCount].window = child;
        probe.children[probe.childCount].client = rect;
        ++probe.childCount;
    }
    Check(probe.childCount > 0, "the live dialog exposes immediate visible children");
    unsigned damagedChildren = 0;
    for (unsigned i = 0; i < probe.childCount; ++i) {
        RECT intersection{};
        if (IntersectRect(&intersection, &probe.children[i].client, &first) ||
            IntersectRect(&intersection, &probe.children[i].client, &second)) ++damagedChildren;
    }
    Check(damagedChildren > 0, "the constructed live damage includes real visible child areas");
    const RECT areas[2]{first, second};
    for (unsigned areaIndex = 0; areaIndex < 2 && probe.backgroundCount < livePaintMaxBackground; ++areaIndex) {
        const RECT& area = areas[areaIndex];
        bool found = false;
        for (int y = area.top + 1; y < area.bottom && !found; y += 3) {
            for (int x = area.left + 1; x < area.right; ++x) {
                const POINT point{x, y};
                if (!PtInRegion(expected, point.x, point.y) || LivePointInChild(probe, point)) continue;
                probe.background[probe.backgroundCount++] = point;
                found = true;
                break;
            }
        }
    }
    Check(probe.backgroundCount >= 1,
        "the constructed damage exposes real parent background between immediate children");
    LiveSnapshot before;
    Check(LiveSnapshotCapture(dialog, before),
        "snapshot the real dialog frame, focus, business context and immediate control structure");
    events << "wm_paint geometry capture=" << Utf8(stem) << " hwnd=" << reinterpret_cast<UINT_PTR>(dialog)
        << " client=" << Utf8(RectText(client))
        << " damage_first=" << Utf8(RectText(first)) << " damage_second=" << Utf8(RectText(second))
        << " damage_bbox=" << Utf8(RectText(expectedBox))
        << " gap=" << probe.gap.x << ',' << probe.gap.y
        << " outside=" << probe.outside.x << ',' << probe.outside.y
        << " visible_children=" << probe.childCount << " damaged_children=" << damagedChildren
        << " background_points=" << probe.backgroundCount;
    for (unsigned i = 0; i < probe.backgroundCount; ++i)
        events << " background" << i << '=' << probe.background[i].x << ',' << probe.background[i].y;
    events << '\n';
    events.flush();
    enum class LiveFault { None, Allocation, Copy, CopyAndFill };
    struct LiveStage { const wchar_t* name; bool invalidateCache; LiveFault fault; };
    const LiveStage stages[livePaintStageCount]{
        {L"cold", true, LiveFault::None},
        {L"warm", false, LiveFault::None},
        {L"allocation-denial", true, LiveFault::Allocation},
        {L"recovery", false, LiveFault::None},
        {L"copy-denial", false, LiveFault::Copy},
        {L"native-replay", false, LiveFault::CopyAndFill},
        {L"final-recovery", false, LiveFault::None},
    };
    HBITMAP coldSource = nullptr, recoveredSource = nullptr;
    // Observed stage-2 auxiliary recovery: a real same-dialog supplied-DC callback
    // can recreate the production PanelBuffer after the first-pass allocation was
    // denied. Stage 3 uses exactly one observed creation, never an inferred one.
    unsigned auxRecoveryAllocations = 0;
    HBITMAP auxRecoverySource = nullptr;
    ++livePaintInitialDialogs;
    for (unsigned index = 0; index < livePaintStageCount; ++index) {
        const LiveStage& stage = stages[index];
        // Settle pending dialog paint with observation off and no injected fault.
        Check(UpdateWindow(dialog) != FALSE, "settle pending dialog paint before the live WM_PAINT stage");
        for (int settle = 0; settle < 3; ++settle) {
            const int pending = GetUpdateRgn(dialog, actual, FALSE);
            if (pending == NULLREGION) break;
            Check(pending != ERROR, "read the pending dialog update region before the live stage");
            Check(UpdateWindow(dialog) != FALSE, "finish settling pending dialog paint before the live stage");
        }
        const int pendingType = GetUpdateRgn(dialog, actual, FALSE);
        events << "wm_paint_settle stage=" << index << " capture=" << Utf8(stem)
            << " pending_before=" << pendingType << " cache_invalidate=" << stage.invalidateCache << '\n';
        events.flush();
        Check(pendingType == NULLREGION, "no pending dialog damage remains before the live WM_PAINT stage");
        if (stage.invalidateCache) SendMessageW(dialog, WM_SYSCOLORCHANGE, 0, 0);
        Check(InvalidateRgn(dialog, expected, FALSE) != FALSE,
            "invalidate the real dialog with the constructed complex two-rect damage");
        const int regionType = GetUpdateRgn(dialog, actual, FALSE);
        const bool damageEqual = regionType == COMPLEXREGION && EqualRgn(actual, expected) != FALSE;
        events << "wm_paint_damage stage=" << index << " capture=" << Utf8(stem)
            << " update_type=" << regionType << " damage_equal=" << damageEqual << '\n';
        events.flush();
        Check(damageEqual,
            "the real dialog update region is complex and exactly equals the constructed two-rect damage");
        probe.active = false;
        probe.paintDc = nullptr;
        probe.paintDcLive = false;
        probe.beginPaintFailed = false;
        probe.beginPaintCalls = 0;
        probe.firstPaintSeen = false;
        probe.leaseActive = false;
        probe.firstEndPaintSeen = false;
        probe.leaseClosed = false;
        probe.endPaintHdcMatched = false;
        probe.laterBeginPaintSeen = false;
        probe.endPaintCalls = 0;
        probe.auxDc = nullptr;
        probe.auxDepth = 0;
        probe.auxEraseCallbacks = probe.auxPrintCallbacks = 0;
        probe.auxAllocationAttempts = probe.auxAllocationSuccesses = 0;
        probe.auxAllocationMatched = probe.auxAllocationDimMismatch = 0;
        probe.auxAllocationBitmap = nullptr;
        probe.auxDcSeen = false;
        for (unsigned pass = 0; pass < livePaintMaxPasses; ++pass) {
            probe.paintDcs[pass] = nullptr;
            probe.paintRects[pass] = RECT{};
        }
        probe.mapMode = 0;
        probe.origin = POINT{};
        probe.paintMessages = probe.eraseMessages = probe.eraseAfterReplay = 0;
        probe.allocationAttempts = probe.allocationSuccesses = probe.allocationDenials = 0;
        probe.allocated = nullptr;
        probe.copyAttempts = probe.copySuccesses = probe.copyDenials = 0;
        probe.copyRecorded = false;
        probe.copyRect = RECT{};
        probe.copySourceX = probe.copySourceY = 0;
        probe.copyRop = 0;
        probe.copySource = nullptr;
        probe.fillAttempts = probe.fillSuccesses = probe.fillDenials = 0;
        probe.fillRecorded = false;
        probe.fillRect = RECT{};
        probe.deniedFillDamage = false;
        probe.invalidateCalls = probe.eraseInvalidates = probe.replays = 0;
        probe.clip = LivePaintClip{};
        probe.allocationDenialBudget = stage.fault == LiveFault::Allocation ? 1u : 0u;
        probe.copyDenialBudget =
            (stage.fault == LiveFault::Copy || stage.fault == LiveFault::CopyAndFill) ? 1u : 0u;
        probe.fillDenialBudget = stage.fault == LiveFault::CopyAndFill ? 1u : 0u;
        probe.active = true;
        activeLivePaintProbe = &probe;
        const BOOL updated = UpdateWindow(dialog);
        probe.active = false;
        activeLivePaintProbe = nullptr;
        const BOOL flushed = GdiFlush();
        const int afterType = GetUpdateRgn(dialog, actual, FALSE);
        LiveSnapshot after;
        const bool snapshotOk = LiveSnapshotCapture(dialog, after);
        const bool snapshotSame = snapshotOk && LiveSnapshotSame(before, after);
        const unsigned beginPaintCalls = probe.beginPaintCalls;
        const bool paintDcLive = probe.paintDcLive;
        const bool beginPaintFailed = probe.beginPaintFailed;
        const HDC paintDc = probe.paintDc;
        const bool firstPaintSeen = probe.firstPaintSeen;
        const bool leaseActiveAfter = probe.leaseActive;
        const bool firstEndPaintSeen = probe.firstEndPaintSeen;
        const bool leaseClosed = probe.leaseClosed;
        const bool endPaintHdcMatched = probe.endPaintHdcMatched;
        const bool laterBeginPaintSeen = probe.laterBeginPaintSeen;
        const unsigned endPaintCalls = probe.endPaintCalls;
        const unsigned auxDepthAfter = probe.auxDepth;
        const bool auxDcSeen = probe.auxDcSeen;
        const unsigned auxEraseCallbacks = probe.auxEraseCallbacks;
        const unsigned auxPrintCallbacks = probe.auxPrintCallbacks;
        const unsigned auxAllocationAttempts = probe.auxAllocationAttempts;
        const unsigned auxAllocationSuccesses = probe.auxAllocationSuccesses;
        const unsigned auxAllocationMatched = probe.auxAllocationMatched;
        const unsigned auxAllocationDimMismatch = probe.auxAllocationDimMismatch;
        const HBITMAP auxAllocationBitmap = probe.auxAllocationBitmap;
        const RECT firstPaintRect = probe.paintRects[0];
        const int mapMode = probe.mapMode;
        const POINT origin = probe.origin;
        const unsigned paintMessages = probe.paintMessages, eraseMessages = probe.eraseMessages;
        const unsigned eraseAfterReplay = probe.eraseAfterReplay;
        const unsigned allocationAttempts = probe.allocationAttempts;
        const unsigned allocationSuccesses = probe.allocationSuccesses;
        const unsigned allocationDenials = probe.allocationDenials;
        const HBITMAP allocated = probe.allocated;
        const unsigned copyAttempts = probe.copyAttempts, copySuccesses = probe.copySuccesses;
        const unsigned copyDenials = probe.copyDenials;
        const bool copyRecorded = probe.copyRecorded;
        const RECT copyRect = probe.copyRect;
        const int copySourceX = probe.copySourceX, copySourceY = probe.copySourceY;
        const DWORD copyRop = probe.copyRop;
        const HBITMAP copySource = probe.copySource;
        const unsigned fillAttempts = probe.fillAttempts, fillSuccesses = probe.fillSuccesses;
        const unsigned fillDenials = probe.fillDenials;
        const bool fillRecorded = probe.fillRecorded;
        const RECT fillRect = probe.fillRect;
        const bool deniedFillDamage = probe.deniedFillDamage;
        const unsigned invalidateCalls = probe.invalidateCalls, eraseInvalidates = probe.eraseInvalidates;
        const unsigned replays = probe.replays;
        const LivePaintClip clip = probe.clip;
        events << "wm_paint stage=" << index << " name=" << Utf8(stage.name) << " capture=" << Utf8(stem)
            << " hwnd=" << reinterpret_cast<UINT_PTR>(dialog)
            << " cache_invalidate=" << stage.invalidateCache
            << " beginpaint=" << beginPaintCalls << " beginpaint_failed=" << beginPaintFailed
            << " paint_dc=" << reinterpret_cast<UINT_PTR>(paintDc)
            << " paint_dc_live=" << paintDcLive
            << " first_paint_seen=" << firstPaintSeen
            << " lease_closed=" << leaseClosed << " lease_active_after=" << leaseActiveAfter
            << " first_endpaint_seen=" << firstEndPaintSeen
            << " endpaint_hdc_matched=" << endPaintHdcMatched
            << " endpaint_calls=" << endPaintCalls
            << " later_beginpaint_seen=" << laterBeginPaintSeen
            << " map_mode=" << mapMode
            << " origin=" << origin.x << ',' << origin.y
            << " wm_paint=" << paintMessages << " erase=" << eraseMessages
            << " erase_after_replay=" << eraseAfterReplay
            << " paint_rect=" << Utf8(RectText(firstPaintRect))
            << " expected_bbox=" << Utf8(RectText(expectedBox))
            << " allocation_attempts=" << allocationAttempts
            << " allocation_successes=" << allocationSuccesses
            << " allocation_denials=" << allocationDenials
            << " allocated_bitmap=" << reinterpret_cast<UINT_PTR>(allocated)
            << " aux_depth_after=" << auxDepthAfter << " aux_dc_seen=" << auxDcSeen
            << " aux_erase_callbacks=" << auxEraseCallbacks
            << " aux_print_callbacks=" << auxPrintCallbacks
            << " aux_alloc_attempts=" << auxAllocationAttempts
            << " aux_alloc_successes=" << auxAllocationSuccesses
            << " aux_alloc_matched=" << auxAllocationMatched
            << " aux_alloc_dim_mismatch=" << auxAllocationDimMismatch
            << " aux_alloc_bitmap=" << reinterpret_cast<UINT_PTR>(auxAllocationBitmap)
            << " copy_attempts=" << copyAttempts << " copy_successes=" << copySuccesses
            << " copy_denials=" << copyDenials
            << " copy_source=" << reinterpret_cast<UINT_PTR>(copySource)
            << " copy_rect=" << (copyRecorded ? Utf8(RectText(copyRect)) : std::string("n/a"))
            << " copy_src=" << copySourceX << ',' << copySourceY
            << " copy_rop=" << copyRop
            << " fill_attempts=" << fillAttempts << " fill_successes=" << fillSuccesses
            << " fill_denials=" << fillDenials
            << " fill_rect=" << (fillRecorded ? Utf8(RectText(fillRect)) : std::string("n/a"))
            << " denied_fill_damage=" << deniedFillDamage
            << " invalidate_calls=" << invalidateCalls << " erase_invalidates=" << eraseInvalidates
            << " replays=" << replays
            << " clipped_children=" << clip.clippedChildren << '/' << clip.childRects
            << " background_visible=" << clip.backgroundVisible << '/' << probe.backgroundCount
            << " gap_visible=" << clip.gapVisible << " outside_visible=" << clip.outsideVisible
            << " draw_dc_live=" << clip.drawDcLive
            << " update_after=" << afterType << " snapshot_same=" << snapshotSame << '\n';
        events.flush();
        if (index == 2) {
            // Stage-2 auxiliary recovery evidence, logged after the first pass. Zero is
            // a real observation: stage 3 must then show its own first-pass creation.
            events << "wm_paint_aux_recovery stage=2 capture=" << Utf8(stem)
                << " aux_erase_callbacks=" << auxEraseCallbacks
                << " aux_print_callbacks=" << auxPrintCallbacks
                << " aux_alloc_attempts=" << auxAllocationAttempts
                << " aux_alloc_successes=" << auxAllocationSuccesses
                << " aux_alloc_matched=" << auxAllocationMatched
                << " aux_alloc_bitmap=" << reinterpret_cast<UINT_PTR>(auxAllocationBitmap)
                << " first_pass_allocation_attempts=" << allocationAttempts
                << " first_pass_allocation_successes=" << allocationSuccesses
                << " first_pass_allocation_denials=" << allocationDenials
                << '\n';
            events.flush();
        }
        ++livePaintStageExecutions;
        if (clip.recorded && clip.childRects == probe.childCount &&
            clip.clippedChildren == probe.childCount) {
            ++livePaintClipStages;
            livePaintClippedChildren += clip.clippedChildren;
        }
        livePaintExposedPoints += clip.backgroundVisible;
        if (clip.gapVisible == FALSE) ++livePaintGapPoints;
        Check(updated != FALSE, "UpdateWindow delivers the real live WM_PAINT damage");
        Check(flushed != FALSE, "flush live WM_PAINT GDI output before inspection");
        Check(afterType == NULLREGION,
            "each live stage validates the real damage without a leftover update region");
        Check(snapshotOk && snapshotSame,
            "live dialog frame, focus, business context and immediate control structure stay unchanged");
        Check(beginPaintCalls >= 1 && !beginPaintFailed && paintDcLive,
            "each live stage observes a real BeginPaint live dialog DC on the exact fixture HWND");
        Check(firstPaintSeen && firstEndPaintSeen && leaseClosed && endPaintHdcMatched && !leaseActiveAfter,
            "the first live production paint lease closes at its matching EndPaint before any forwarding");
        Check(auxDepthAfter == 0,
            "auxiliary same-dialog observation scope unwinds before stage verdicts");
        Check(paintMessages == 1, "each live stage receives exactly one target WM_PAINT message");
        Check(clip.recorded && clip.childRects == probe.childCount &&
            clip.clippedChildren == probe.childCount && clip.drawDcLive,
            "live paint clips every immediate visible child rectangle on the real dialog DC");
        Check(clip.backgroundVisible >= 1,
            "live paint keeps at least one damaged parent background point visible via PtVisible");
        Check(clip.gapVisible == FALSE && clip.outsideVisible == FALSE,
            "live paint clips the non-damaged gap and a point outside the damage box");
        Check(EqualRect(&firstPaintRect, &expectedBox),
            "BeginPaint reports the constructed damage bounding box as the real rcPaint");
        Check(mapMode == MM_TEXT && origin.x == 0 && origin.y == 0,
            "BeginPaint keeps MM_TEXT and the default origin so PtVisible/RectVisible use logical client coordinates");
        if (copyRecorded) {
            Check(LiveCoversRect(firstPaintRect, copyRect) && EqualRect(&copyRect, &expectedBox),
                "live buffered copy destination stays inside rcPaint and matches the constructed damage box");
            Check(copySourceX == copyRect.left && copySourceY == copyRect.top && copyRop == SRCCOPY,
                "live buffered copy uses the same source coordinates and SRCCOPY");
        }
        if (fillRecorded)
            Check(EqualRect(&fillRect, &expectedBox),
                "live direct FillRect matches the constructed damage rectangle");
        if (index == 0) {
            Check(allocationAttempts == 1 && allocationSuccesses == 1 && allocationDenials == 0,
                "cold live paint reallocates the production buffer exactly once");
            Check(allocated != nullptr && copyRecorded && copySource == allocated,
                "cold live buffered copy uses the actual newly allocated production bitmap");
            Check(copyAttempts == 1 && copySuccesses == 1 && copyDenials == 0 && fillAttempts == 0,
                "cold live paint performs exactly one real buffered copy and no direct fill");
            Check(replays == 0 && beginPaintCalls == 1,
                "cold live paint performs one BeginPaint pass and no native replay");
            coldSource = copySource;
        } else if (index == 1) {
            Check(allocationAttempts == 0 && allocationSuccesses == 0 && allocationDenials == 0,
                "warm live paint reuses the cached production buffer");
            Check(copyAttempts == 1 && copySuccesses == 1 && copyDenials == 0 && fillAttempts == 0,
                "warm live paint performs exactly one real buffered copy and no direct fill");
            Check(copyRecorded && copySource == coldSource,
                "warm live buffered copy comes from the same production source bitmap as the cold stage");
            Check(replays == 0 && beginPaintCalls == 1,
                "warm live paint performs one BeginPaint pass and no native replay");
        } else if (index == 2) {
            Check(allocationAttempts == 1 && allocationDenials == 1 && allocationSuccesses == 0 &&
                allocated == nullptr,
                "allocation denial fails exactly one matched production CreateDIBSection");
            Check(copyAttempts == 0 && copySuccesses == 0,
                "allocation denial exposes no buffered copy");
            Check(fillAttempts == 1 && fillSuccesses == 1 && fillDenials == 0 && fillRecorded &&
                EqualRect(&fillRect, &expectedBox),
                "allocation denial is covered by exactly one real direct FillRect for the constructed damage");
            Check(replays == 0 && beginPaintCalls == 1,
                "allocation denial performs one BeginPaint pass and no native replay");
            Check(auxAllocationMatched <= 1 &&
                (auxAllocationMatched == 0 || auxAllocationBitmap != nullptr),
                "stage-2 auxiliary recovery records at most one observed production buffer creation");
            auxRecoveryAllocations = auxAllocationMatched;
            auxRecoverySource = auxAllocationBitmap;
        } else if (index == 3) {
            // A same-dialog supplied-DC erase after stage 2's denied first pass can
            // recreate the production PanelBuffer before stage 3. Exactly one real
            // observed creation must be the recovery source; never an inference.
            const bool auxiliaryRecovered = auxRecoveryAllocations == 1;
            if (auxiliaryRecovered) {
                Check(allocationAttempts == 0 && allocationSuccesses == 0 && allocationDenials == 0,
                    "recovery reuses the observed auxiliary-created buffer with no first-pass allocation");
            } else {
                Check(allocationAttempts == 1 && allocationSuccesses == 1 && allocationDenials == 0,
                    "recovery performs exactly one observed first-pass allocation when no auxiliary creation was observed");
            }
            Check(auxRecoveryAllocations + allocationSuccesses == 1,
                "recovery has exactly one observed successful creation, either stage-2 auxiliary or stage-3 first pass");
            Check(copyAttempts == 1 && copySuccesses == 1 && copyDenials == 0 && fillAttempts == 0,
                "recovery performs exactly one real buffered copy and no direct fill");
            Check(copyRecorded && copySource != nullptr &&
                copySource == (auxiliaryRecovered ? auxRecoverySource : allocated),
                "recovery copies from the actual observed recovery creation bitmap");
            Check(replays == 0 && beginPaintCalls == 1,
                "recovery performs one BeginPaint pass and no native replay");
            recoveredSource = copySource;
        } else if (index == 4) {
            Check(allocationAttempts == 0 && allocationDenials == 0,
                "copy denial does not reallocate the production buffer");
            Check(copyAttempts == 1 && copyDenials == 1 && copySuccesses == 0 && copyRecorded &&
                EqualRect(&copyRect, &expectedBox),
                "copy denial fails exactly one matched production BitBlt for the constructed damage");
            Check(copySource == recoveredSource,
                "copy denial records the recovered production source bitmap");
            Check(fillAttempts == 1 && fillSuccesses == 1 && fillDenials == 0 && fillRecorded &&
                EqualRect(&fillRect, &expectedBox),
                "copy denial is covered by exactly one real direct FillRect");
            Check(replays == 0 && beginPaintCalls == 1,
                "copy denial performs one BeginPaint pass and no native replay");
        } else if (index == 5) {
            Check(allocationAttempts == 0 && allocationDenials == 0,
                "native replay does not reallocate the production buffer");
            Check(copyDenials == 1 && copySuccesses == 0 && copyRecorded &&
                EqualRect(&copyRect, &expectedBox) && copySource == recoveredSource,
                "native replay denies exactly the first matched production buffered copy");
            Check(fillDenials == 1 && deniedFillDamage && fillRecorded &&
                EqualRect(&fillRect, &expectedBox),
                "native replay denies exactly the first matched direct FillRect for the same damage");
            Check(replays == 1 && eraseInvalidates == 1 && invalidateCalls == 1,
                "native replay restores exactly one erase-enabled invalidation equal to the original damage");
            Check(eraseAfterReplay >= 1 && eraseMessages >= 1,
                "native fallback receives a real WM_ERASEBKGND after the replay invalidation");
            ++livePaintNativeReplays;
        } else {
            Check(allocationAttempts == 0 && allocationDenials == 0,
                "final recovery reuses the cached production buffer");
            Check(copyAttempts == 1 && copySuccesses == 1 && copyDenials == 0 && fillAttempts == 0,
                "final recovery restores exactly one successful buffered copy and no direct fill");
            Check(copyRecorded && copySource == recoveredSource,
                "final recovery copies from the same recovered production bitmap");
            Check(replays == 0 && beginPaintCalls == 1,
                "final recovery performs one BeginPaint pass and no native replay");
        }
    }
    Check(activeLivePaintProbe == nullptr, "live WM_PAINT probe leaves no active observation");
    Check(coldSource != nullptr && recoveredSource != nullptr,
        "live WM_PAINT probe observed real cold and recovered production buffer sources");
    DeleteObject(actual);
    DeleteObject(expected);
}

void Capture(HWND dialog, const std::wstring& suffix) {
    RequireDesktop();
    Check(IsWindowVisible(dialog) && MonitorFromWindow(dialog, MONITOR_DEFAULTTONULL) == scenario->monitor->handle,
        "capture a real visible modal dialog on its requested non-input monitor");
    const UINT dpi = windowDpi(dialog);
    const int aware = awareness(windowContext(dialog));
    Check(dpi > 0 && aware == (scenario->native ? 2 : 0), "observe requested real window DPI awareness");
    const int scale = scenario->monitor->scale;
    if (scale == 175) { observed175 = true; if (scenario->native && dpi == 168) legacyShell175Pair = true; }
    // Raw observations only: these Shell-percentage diagnostics never decide
    // effective 175% coverage; the per-monitor ledger below does.
    if (scenario->native && dpi == 168) native168Observed = true;
    const auto stem = scenario->prefix + L"-" + suffix;
    const DpiSample dpiSample = CaptureDpiEvidence(dialog, stem);
    RecordCoverage(dpiSample, stem);
    // Real production WM_PRINTCLIENT buffered-copy contract, initial captures only.
    // The live WM_PAINT damage/clip/replay probe runs after it on the same initial
    // HWND; later edited captures and all following operations stay unchanged.
    if (!checkInterfaceScale && suffix.size() >= 7 && suffix.compare(suffix.size() - 7, 7, L"initial") == 0) {
        ProbePrintClient(dialog, stem);
        ProbeLivePaint(dialog, stem);
    }
    std::ofstream csv(root / (stem + L".csv"), std::ios::binary);
    csv << "kind,hwnd,id,class,text,screen_left,screen_top,screen_right,screen_bottom,dialog_left,dialog_top,dialog_right,dialog_bottom,client_left,client_top,client_right,client_bottom,font_handle,font_face,selected_face,font_height,font_width,font_weight,italic,charset,text_width,text_height,window_dpi,awareness,monitor_scale_percent,visible,enabled,style,exstyle,inside_dialog,overlapping_siblings\n";
    std::vector<HWND> children;
    EnumChildWindows(dialog, [](HWND child, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(child); return TRUE;
    }, reinterpret_cast<LPARAM>(&children));
    const bool naming = GetDlgItem(dialog, as::Pattern) != nullptr;
    Check(children.size() == (naming ? 15u : 16u), "enumerate every control in the real dialog resource");
    const std::vector<int> required = naming ? std::vector<int>{as::Pattern, as::Preview, as::PatternLabel,
        as::PreviewLabel, as::TokenFirst, as::TokenFirst+1, as::TokenFirst+2, as::TokenFirst+3,
        as::TokenFirst+4, as::TokenFirst+5, as::TokenFirst+6, as::TokenFirst+7, as::TokenFirst+8, IDOK, IDCANCEL} :
        std::vector<int>{as::Folder, as::Browse, as::Interval, as::Limit, as::Overwrite, as::FolderLabel,
        as::IntervalLabel, as::LimitLabel, as::Explanation, as::Enabled, as::KeepPrevious,
        as::NumberedManual, as::BackupNaming, as::IncrementNaming, IDOK, IDCANCEL};
    for (int id : required) Check(GetDlgItem(dialog, id) && IsWindowVisible(GetDlgItem(dialog, id)),
        "every expected control exists and is visible on private desktop");
    const bool injectCounterexample = counterexample != Counterexample::None && captures == 0;
    if (injectCounterexample) {
        Check(!naming, "geometry counterexample starts from the first real settings dialog capture");
        InjectCounterexample(dialog, stem);
    }
    RECT bounds{}; Check(GetClientRect(dialog, &bounds), "read dialog client bounds");
    if (checkInterfaceScale) {
        RECT outer{}; Check(GetWindowRect(dialog, &outer), "read the scaled modal's actual screen bounds");
        const auto& work = scenario->monitor->work;
        Check(outer.left >= work.left && outer.top >= work.top && outer.right <= work.right && outer.bottom <= work.bottom,
              "the scaled modal remains fully within the actual monitor work area");
        LOGFONTW font{};
        const auto handle = reinterpret_cast<HFONT>(SendDlgItemMessageW(dialog, IDOK, WM_GETFONT, 0, 0));
        Check(handle && GetObjectW(handle, sizeof(font), &font) == sizeof(font), "read the actual scaled action font");
        auto& baseline = scaleBaselines[naming ? 1 : 0];
        if (!baseline.width) baseline = {bounds.right, bounds.bottom, font.lfHeight};
        const int percent = ui_scale::Percent();
        if (percent == 100) {
            Check(bounds.right == baseline.width && bounds.bottom == baseline.height && font.lfHeight == baseline.fontHeight,
                  "100 percent restores the original modal geometry and font");
        } else {
            Check(bounds.right > baseline.width && bounds.bottom > baseline.height &&
                  std::abs(font.lfHeight) > std::abs(baseline.fontHeight),
                  "the application scale enlarges the actual modal, controls and action font");
            Check(bounds.right <= MulDiv(baseline.width, percent, 100) &&
                  bounds.bottom <= MulDiv(baseline.height, percent, 100), "the dialog respects the requested scale cap");
        }
        events << "app-scale=" << percent << " naming=" << naming << " width=" << bounds.right
            << " height=" << bounds.bottom << " font=" << font.lfHeight << '\n';
        events.flush();
    }
    auto rows = children; rows.insert(rows.begin(), dialog);
    for (HWND window : rows) {
        Check(windowDpi(window) == dpi && awareness(windowContext(window)) == aware,
            "every real control shares the observed dialog DPI and awareness");
        RECT screen{}, relative{}, client{};
        Check(GetWindowRect(window, &screen) && GetClientRect(window, &client), "read actual control geometry");
        relative = screen; MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&relative), 2);
        const bool inside = window == dialog || (relative.left >= 0 && relative.top >= 0 &&
            relative.right <= bounds.right && relative.bottom <= bounds.bottom &&
            relative.right > relative.left && relative.bottom > relative.top);
        unsigned overlaps = 0;
        if (window != dialog) for (HWND other : children) if (window != other && GetParent(window) == GetParent(other)) {
            RECT otherRect{}, intersection{}; GetWindowRect(other, &otherRect);
            if (IntersectRect(&intersection, &screen, &otherRect)) ++overlaps;
        }
        if (!inside || overlaps) ++geometryFindings;
        const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        LOGFONTW log{}; Check(font && GetObjectW(font, sizeof log, &log) == sizeof log, "record real WM_GETFONT LOGFONT");
        HDC dc = GetDC(window); Check(dc != nullptr, "measure text using control DC");
        const auto previous = SelectObject(dc, font); SIZE extent{};
        wchar_t selectedFace[LF_FACESIZE]{};
        Check(GetTextFaceW(dc, LF_FACESIZE, selectedFace) > 0, "record actual selected font face");
        const auto text = FullText(window);
        Check(GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &extent), "measure actual localized control text");
        SelectObject(dc, previous); ReleaseDC(window, dc);
        wchar_t cls[128]{}; GetClassNameW(window, cls, 128);
        if (checkInterfaceScale && window != dialog) {
            const POINT center{(relative.left + relative.right) / 2, (relative.top + relative.bottom) / 2};
            Check(ChildWindowFromPointEx(dialog, center, CWP_SKIPINVISIBLE) == window,
                  "the scaled native control is hit at the center of its actual rectangle");
            if (!_wcsicmp(cls, L"Static") || !_wcsicmp(cls, L"Button"))
                Check(extent.cx <= client.right && extent.cy <= client.bottom, "localized control text fits the scaled native geometry");
        }
        csv << (window == dialog ? "dialog" : "control") << ',' << reinterpret_cast<UINT_PTR>(window) << ','
            << GetDlgCtrlID(window) << ',' << Csv(cls) << ',' << Csv(text) << ',';
        RectFields(csv, screen); csv << ','; RectFields(csv, relative); csv << ','; RectFields(csv, client);
        csv << ',' << reinterpret_cast<UINT_PTR>(font) << ',' << Csv(log.lfFaceName) << ',' << Csv(selectedFace) << ',' << log.lfHeight << ','
            << log.lfWidth << ',' << log.lfWeight << ',' << unsigned(log.lfItalic) << ',' << unsigned(log.lfCharSet)
            << ',' << extent.cx << ',' << extent.cy << ',' << windowDpi(window) << ',' << awareness(windowContext(window))
            << ',' << scale << ',' << !!IsWindowVisible(window) << ',' << !!IsWindowEnabled(window) << ','
            << GetWindowLongPtrW(window, GWL_STYLE) << ',' << GetWindowLongPtrW(window, GWL_EXSTYLE)
            << ',' << inside << ',' << overlaps << '\n';
    }
    csv.close(); Check(bool(csv), "write complete per-control UTF-8 evidence");
    Check(RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW),
        "run real dialog and child paint before capture");
    RECT rect{}; GetWindowRect(dialog, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    Check(width > 0 && height > 0 && width < 16384 && height < 16384, "bounded real dialog capture size");
    HDC dc = GetWindowDC(dialog), memory = CreateCompatibleDC(dc);
    Check(dc && memory, "allocate dialog capture DC");
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr; auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Check(bitmap && pixels, "allocate dialog capture DIB");
    const auto old = SelectObject(memory, bitmap);
    std::fill_n(static_cast<DWORD*>(pixels), size_t(width) * height, DWORD(0x00ff00ff));
    // Production WM_PRINT/child painting only. No template copy, raster rescale,
    // fake controls, or claim that this is a physical DWM/desktop screenshot.
    SendMessageW(dialog, WM_PRINT, reinterpret_cast<WPARAM>(memory),
        PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    Check(GdiFlush(), "flush real GDI dialog drawing");
    const auto untouched = std::count_if(static_cast<DWORD*>(pixels), static_cast<DWORD*>(pixels) + size_t(width)*height,
        [](DWORD pixel) { return (pixel & 0xffffff) == 0xff00ff; });
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof header + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + width * height * 4;
    std::ofstream bmp(root / (stem + L".bmp"), std::ios::binary);
    bmp.write(reinterpret_cast<const char*>(&header), sizeof header);
    bmp.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof info.bmiHeader);
    bmp.write(static_cast<const char*>(pixels), size_t(width)*height*4); bmp.close();
    Check(bool(bmp), "write actual dialog BMP");
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(dialog, dc);
    events << "capture=" << Utf8(stem) << " dpi=" << dpi << " awareness=" << aware << " monitor_scale=" << scale
        << " source=production-WM_PRINT simulated_dpi=false untouched_magenta=" << untouched << '\n'; events.flush();
    Check(untouched == 0, "real production WM_PRINT must initialize every captured pixel including transparent child backgrounds");
    ++captures;
    if (injectCounterexample) RestoreCounterexample(dialog);
}
void Click(DialogState& state, int id) {
    RequireDesktop();
    HWND control = GetDlgItem(state.window, id);
    Check(control && IsWindowEnabled(control), "click only enabled controls on the exact expected dialog");
    SetActiveWindow(state.window); // Same thread, private desktop; never SetForegroundWindow/SendInput.
    const auto before = state.commands;
    SendMessageW(control, BM_CLICK, 0, 0);
    Check(state.commands == before + 1 && state.lastCommand == id,
        "BM_CLICK delivered exactly the requested control-origin WM_COMMAND");
}
void Drive(DialogState& state) {
    RequireDesktop();
    HWND dialog = state.window;
    const auto& work = scenario->monitor->work;
    if (!checkInterfaceScale)
        Check(SetWindowPos(dialog, nullptr, work.left + 16, work.top + 16, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE), "place real dialog on actual enumerated monitor");
    if (state.naming) {
        auto* context = reinterpret_cast<as::NamingContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
        Check(context && context->app == scenario->context.app && context->backup == scenario->backup,
            "nested naming dialog uses the real production context");
        Check(FullText(dialog) == (scenario->backup ? as::Text(context->app, L"备份命名", L"Backup naming", L"バックアップ名") :
            as::Text(context->app, L"增量命名", L"Increment naming", L"増分名")), "actual naming caption follows requested UI language");
        const auto tag = std::wstring(scenario->backup ? L"backup" : L"increment") +
            (scenario->namingSave ? L"-save" : L"-cancel");
        Capture(dialog, tag + L"-initial");
        Check(GetWindowLongPtrW(GetDlgItem(dialog, as::Preview), GWL_STYLE) & ES_READONLY,
            "production naming preview remains read-only");
        const wchar_t* tokens[]{L"[/NAME]", L"[/N]", L"[/YY]", L"[/YYYY]", L"[/MM]", L"[/DD]", L"[/hh]", L"[/mm]", L"[/ss]"};
        for (int i = 0; i < 9; ++i) {
            SetDlgItemTextW(dialog, as::Pattern, L"start_end");
            SendDlgItemMessageW(dialog, as::Pattern, EM_SETSEL, 5, 5);
            Click(state, as::TokenFirst + i);
            Check(FullText(GetDlgItem(dialog, as::Pattern)) == std::wstring(L"start") + tokens[i] + L"_end",
                "every real token button inserts at the caret via WM_COMMAND");
        }
        SetDlgItemTextW(dialog, as::Pattern, L"[/unknown]");
        Check(!IsWindowEnabled(GetDlgItem(dialog, IDOK)), "actual EN_CHANGE disables invalid naming confirmation");
        const auto pattern = scenario->namingSave ? Pattern(scenario->backup) : L"discard_[/N]";
        SetDlgItemTextW(dialog, as::Pattern, pattern.c_str());
        Check(IsWindowEnabled(GetDlgItem(dialog, IDOK)), "valid naming re-enables actual OK button");
        Check(FullText(GetDlgItem(dialog, as::Preview)).find(L".pmm") != std::wstring::npos,
            "actual preview expands filename and extension");
        Capture(dialog, tag + L"-edited");
        Click(state, scenario->namingSave ? IDOK : IDCANCEL);
        return;
    }
    Check(FullText(dialog) == as::Text(scenario->context.app, L"自动备份设置", L"Automatic backup settings", L"自動バックアップ設定"),
        "actual settings caption follows requested UI language");
    Check(FullText(GetDlgItem(dialog, as::Folder)) == as::DisplayDirectory(scenario->context.value.directory),
        "actual settings folder displays only the isolated backup path");
    Capture(dialog, L"settings-initial");
    for (bool backup : {true, false}) for (bool save : {true, false}) {
        scenario->backup = backup; scenario->namingSave = save;
        const auto old = backup ? scenario->context.value.backupPattern : scenario->context.value.incrementPattern;
        Click(state, backup ? as::BackupNaming : as::IncrementNaming);
        const auto after = backup ? scenario->context.value.backupPattern : scenario->context.value.incrementPattern;
        Check(after == (save ? Pattern(backup) : old), "nested naming OK/cancel changes only the expected parent draft");
    }
    SetDlgItemInt(dialog, as::Interval, 2, FALSE); SetDlgItemInt(dialog, as::Limit, 7, FALSE);
    for (int id : {as::Enabled, as::KeepPrevious, as::NumberedManual, as::Overwrite}) {
        Check(IsDlgButtonChecked(dialog, id) == BST_UNCHECKED, "baseline checkboxes are independently off");
        Click(state, id);
        Check(IsDlgButtonChecked(dialog, id) == BST_CHECKED, "real checkbox BM_CLICK updates checked state");
    }
    Capture(dialog, L"settings-edited");
    Click(state, scenario->save ? IDOK : IDCANCEL);
}
LRESULT CALLBACK Observe(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& state = *reinterpret_cast<DialogState*>(data);
    // Exact-target message counts for the live WM_PAINT probe. Counting only; every
    // verdict is asserted after UpdateWindow returns and observation is cleared.
    auto* live = activeLivePaintProbe;
    if (live && live->active && live->window == window && live->thread == GetCurrentThreadId()) {
        if (msg == WM_PAINT) {
            ++live->paintMessages;
        } else if (msg == WM_ERASEBKGND) {
            ++live->eraseMessages;
            if (live->replays > 0) ++live->eraseAfterReplay;
        }
    }
    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED && lp &&
        reinterpret_cast<HWND>(lp) == GetDlgItem(window, LOWORD(wp))) {
        ++state.commands;
        state.lastCommand = LOWORD(wp);
        events << "WM_COMMAND hwnd=" << reinterpret_cast<UINT_PTR>(window) << " id=" << LOWORD(wp)
            << " child=" << static_cast<UINT_PTR>(lp) << " notification=BN_CLICKED\n"; events.flush();
    }
    if (msg == WM_DPICHANGED) {
        events << "WM_DPICHANGED source=system (never sent by test) x=" << LOWORD(wp) << " y=" << HIWORD(wp) << '\n';
    }
    if (msg == WM_TIMER && wp == driveTimer) { KillTimer(window, driveTimer); Drive(state); return 0; }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(window, Observe, 1);
    // Observation-only auxiliary same-dialog scope. After the first production paint
    // lease has closed, a supplied-DC WM_ERASEBKGND/WM_PRINTCLIENT callback on this
    // exact fixture HWND may recreate the production PanelBuffer before the next
    // stage. The DC comes from this message WPARAM and is live only while this exact
    // callback is on the stack; nesting saves/restores the previous DC and depth. No
    // heap, log, message, Check or fault happens here; it never changes main matching.
    auto* auxiliary = activeLivePaintProbe;
    const bool auxiliaryScope = auxiliary && auxiliary->active && auxiliary->window == window &&
        auxiliary->thread == GetCurrentThreadId() && auxiliary->firstPaintSeen &&
        auxiliary->leaseClosed && !auxiliary->leaseActive &&
        (msg == WM_ERASEBKGND || msg == WM_PRINTCLIENT) && wp != 0;
    HDC previousAuxDc = nullptr;
    unsigned previousAuxDepth = 0;
    if (auxiliaryScope) {
        previousAuxDc = auxiliary->auxDc;
        previousAuxDepth = auxiliary->auxDepth;
        auxiliary->auxDc = reinterpret_cast<HDC>(wp);
        ++auxiliary->auxDepth;
        auxiliary->auxDcSeen = true;
        if (msg == WM_ERASEBKGND) ++auxiliary->auxEraseCallbacks;
        else ++auxiliary->auxPrintCallbacks;
    }
    const LRESULT result = DefSubclassProc(window, msg, wp, lp);
    if (auxiliaryScope) {
        auxiliary->auxDc = previousAuxDc;
        auxiliary->auxDepth = previousAuxDepth;
    }
    return result;
}
LRESULT CALLBACK Initialized(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && scenario) {
        const auto& message = *reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (message.message == WM_INITDIALOG) {
            const auto proc = reinterpret_cast<DLGPROC>(GetPropW(message.hwnd, L"MMS.OriginalLocalizedDialogProc"));
            DialogState* state = nullptr;
            if (proc == as::DialogProc && GetWindowLongPtrW(message.hwnd, DWLP_USER) == reinterpret_cast<LONG_PTR>(&scenario->context))
                state = &scenario->settings;
            else if (proc == as::NamingProc && GetWindow(message.hwnd, GW_OWNER) == scenario->settings.window)
                state = &scenario->naming;
            if (state) {
                *state = DialogState{message.hwnd, proc == as::NamingProc, 0};
                Check(SetWindowSubclass(message.hwnd, Observe, 1, reinterpret_cast<DWORD_PTR>(state)), "observe exact production dialog messages");
                Check(SetTimer(message.hwnd, driveTimer, 100, nullptr) != 0, "drive after real initialization and first paint");
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
int WINAPI RejectMessageW(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    std::fwprintf(stderr, L"FAIL dialog-layout unexpected MessageBoxW owner=%p flags=%u title=%ls text=%ls\n",
        static_cast<void*>(owner), flags, title ? title : L"", text ? text : L"");
    std::exit(2);
}
int WINAPI RejectMessageA(HWND owner, LPCSTR text, LPCSTR title, UINT flags) {
    std::fprintf(stderr, "FAIL dialog-layout unexpected MessageBoxA owner=%p flags=%u title=%s text=%s\n",
        static_cast<void*>(owner), flags, title ? title : "", text ? text : "");
    std::exit(2);
}
int Run(int argc, wchar_t** argv) {
    // Strict optional-argument parsing: reject duplicates, missing values,
    // unknown values, unknown options and extra positional arguments before
    // PrepareEnvironment or any GUI work.
    bool require175 = false, counterexampleSeen = false;
    bool valid = argc >= 4 && std::wstring(argv[2]) == L"--dialog-layout-only";
    for (int index = 4; valid && index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--require-175") {
            if (require175) valid = false; else require175 = true;
        } else if (argument == L"--interface-scale") {
            if (checkInterfaceScale) valid = false; else checkInterfaceScale = true;
        } else if (argument == L"--geometry-counterexample") {
            if (counterexampleSeen || index + 1 >= argc) { valid = false; break; }
            counterexampleSeen = true;
            const std::wstring value = argv[++index];
            if (value == L"outside") counterexample = Counterexample::Outside;
            else if (value == L"overlap") counterexample = Counterexample::Overlap;
            else valid = false;
        } else valid = false;
    }
    Check(valid && !(checkInterfaceScale && (require175 || counterexampleSeen)),
          "usage: exe NEW-OUTPUT --dialog-layout-only PRIVATE-DESKTOP [--interface-scale|--require-175|--geometry-counterexample outside|overlap]");
    Check(!MMD_ORIGINAL_PROFILE, "dialog layout mode requires the new ABI build");
    PrepareEnvironment(argv[1]); desktopName = argv[3]; BootstrapDesktop();
    events.open(root / L"events.txt", std::ios::binary); Check(bool(events), "open dialog evidence journal");
    events << "desktop=" << Utf8(desktopName) << " station=" << Utf8(ObjectName(GetProcessWindowStation()))
        << " pid=" << GetCurrentProcessId() << " input=false\n";
    events << "geometry_counterexample=" << Utf8(CounterexampleName()) << '\n';
    const auto user = GetModuleHandleW(L"user32.dll");
    setContext = reinterpret_cast<SetContext>(GetProcAddress(user, "SetThreadDpiAwarenessContext"));
    windowDpi = reinterpret_cast<WindowDpi>(GetProcAddress(user, "GetDpiForWindow"));
    windowContext = reinterpret_cast<WindowContext>(GetProcAddress(user, "GetWindowDpiAwarenessContext"));
    awareness = reinterpret_cast<Awareness>(GetProcAddress(user, "GetAwarenessFromDpiAwarenessContext"));
    threadDpiContext = reinterpret_cast<ThreadDpiContext>(GetProcAddress(user, "GetThreadDpiAwarenessContext"));
    logicalToPhysical = reinterpret_cast<LogicalToPhysicalPoint>(GetProcAddress(user, "LogicalToPhysicalPointForPerMonitorDPI"));
    Check(setContext && windowDpi && windowContext && awareness, "real DPI APIs required; unavailable is not a pass");
    wchar_t system[32768]{}; Check(GetSystemDirectoryW(system, 32768), "locate system DPI library");
    const auto shcore = LoadLibraryW((fs::path(system) / L"shcore.dll").c_str());
    monitorScale = shcore ? reinterpret_cast<MonitorScale>(GetProcAddress(shcore, "GetScaleFactorForMonitor")) : nullptr;
    Check(monitorScale != nullptr, "query actual monitor scale independently of DPI virtualization");
    Check(setContext(reinterpret_cast<HANDLE>(INT_PTR(-4))) != nullptr, "enumerate physical monitor work areas in per-monitor-v2 context");
    Check(EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM) -> BOOL {
        MONITORINFOEXW info{}; info.cbSize = sizeof info; int scale = 0;
        Check(GetMonitorInfoW(monitor, &info) && SUCCEEDED(monitorScale(monitor, &scale)), "read available monitor and real scale");
        monitors.push_back({monitor, info.rcWork, info.szDevice, scale}); return TRUE;
    }, 0) && !monitors.empty(), "enumerate only real system monitors");
    for (size_t i = 0; i < monitors.size(); ++i)
        events << "monitor=" << i << " device=" << Utf8(monitors[i].device) << " real_scale=" << monitors[i].scale << '\n';
    // Bind each effective-175 coverage entry to the enumerated real monitor.
    coverage.resize(monitors.size());
    for (size_t i = 0; i < monitors.size(); ++i) {
        coverage[i].handle = monitors[i].handle;
        coverage[i].device = monitors[i].device;
        coverage[i].shellScale = monitors[i].scale;
    }
    Check(setContext(reinterpret_cast<HANDLE>(INT_PTR(-1))) != nullptr, "match production WinMain DPI-unaware startup");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Check(MH_Initialize() == MH_OK, "initialize fail-closed unexpected-message guards");
    Check(MH_CreateHook(reinterpret_cast<void*>(&CreateDIBSection), reinterpret_cast<void*>(&ObservedCreateDibSection),
        reinterpret_cast<void**>(&originalCreateDibSection)) == MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&BitBlt), reinterpret_cast<void*>(&ObservedBitBlt),
        reinterpret_cast<void**>(&originalBitBlt)) == MH_OK,
        "install narrowly scoped WM_PRINTCLIENT GDI buffer hooks before any probe");
    Check(MH_CreateHook(reinterpret_cast<void*>(&BeginPaint), reinterpret_cast<void*>(&ObservedBeginPaint),
        reinterpret_cast<void**>(&originalBeginPaint)) == MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&EndPaint), reinterpret_cast<void*>(&ObservedEndPaint),
        reinterpret_cast<void**>(&originalEndPaint)) == MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&FillRect), reinterpret_cast<void*>(&ObservedFillRect),
        reinterpret_cast<void**>(&originalFillRect)) == MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&InvalidateRgn), reinterpret_cast<void*>(&ObservedInvalidateRgn),
        reinterpret_cast<void**>(&originalInvalidateRgn)) == MH_OK,
        "install narrowly scoped live WM_PAINT paint lease, fill and replay observation hooks");
    Check(MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW), reinterpret_cast<void*>(&RejectMessageW), nullptr) == MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&MessageBoxA), reinterpret_cast<void*>(&RejectMessageA), nullptr) == MH_OK &&
        MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "unexpected messages fail; no generalized auto-confirmation");
    auto* app = new MMDApp; g_Block = app;
    { auto initial = std::make_unique<MMDAppState>(); app->state = *initial; }
    app->InitDefaults();
    // These modal dialogs do not require a D3D device. Keep their real resource,
    // factory and procedures on a private desktop with a fixture-owned parent.
    // This does not claim to exercise the main-window GPU startup path.
    WNDCLASSW ownerClass{}; ownerClass.lpfnWndProc = DefWindowProcW;
    ownerClass.hInstance = GetModuleHandleW(nullptr); ownerClass.lpszClassName = L"MMSBackupLayoutOwner";
    Check(RegisterClassW(&ownerClass) != 0, "register the isolated dialog owner");
    const auto menu = CreateMenu(), help = CreatePopupMenu();
    Check(menu && help && AppendMenuW(help, MF_STRING, 201, L"About") &&
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"Help"),
        "provide an isolated Help owner; settings and naming still use real dialog resources");
    const auto owner = CreateWindowW(ownerClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 960, 720, nullptr, menu, ownerClass.hInstance, nullptr);
    Check(owner != nullptr, "create the private dialog owner without a GPU");
    app->Hwnd() = owner; app->HInstance() = ownerClass.hInstance;
    as::Initialize(app);
    const auto hook = SetWindowsHookExW(WH_CALLWNDPROCRET, Initialized, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "observe production modal dialog initialization on this thread only");
    const auto passes = checkInterfaceScale ? std::vector<std::pair<bool, int>>{{false,100}, {false,125}, {false,200}, {false,100}}
                                            : std::vector<std::pair<bool, int>>{{false,100}, {true,100}};
    unsigned pass = 0;
    for (const auto& [native, percent] : passes) {
        ++pass;
        if (checkInterfaceScale) {
            Check(system_settings::WriteSection("uiScaling", "{\"percent\":" + std::to_string(percent) + "}") ==
                      system_settings::Result::Ok, "store the selected application scale in the isolated installation");
            ui_scale::ApplyAtStartup();
            Check(ui_scale::Percent() == percent, "load the persisted application scale");
        }
        Check(setContext(reinterpret_cast<HANDLE>(INT_PTR(native ? -4 : -1))) != nullptr, "select actual OS-supported dialog DPI context");
        // Work-area coordinates must be queried in the same awareness context
        // as SetWindowPos; no synthetic WM_DPICHANGED or requested DPI numbers.
        for (auto monitor : monitors) {
            if (checkInterfaceScale && monitor.handle != monitors.front().handle) continue;
            MONITORINFO info{sizeof info}; Check(GetMonitorInfoW(monitor.handle, &info), "read context-correct monitor coordinates");
            monitor.work = info.rcWork;
            for (int language : {0, 1, 2}) for (bool save : {false, true}) {
                if (checkInterfaceScale && save != (percent == 200)) continue;
                app->EnglishUI() = static_cast<unsigned char>(language); as::RefreshMenu(app);
                as::Settings baseline; baseline.directory = (root / L"backups").wstring(); Configure(app, baseline);
                const auto before = Bytes(root / L"autosave.ini");
                Scenario current; current.context = {app, as::Current()}; current.monitor = &monitor;
                current.save = save; current.native = native; current.language = language;
                current.prefix = std::wstring(native ? L"native" : L"product-unaware") + L"-monitor-" +
                    std::to_wstring(std::find_if(monitors.begin(), monitors.end(), [&](const Monitor& m) { return m.handle == monitor.handle; }) - monitors.begin()) +
                    L"-lang-" + std::to_wstring(language) + (save ? L"-save" : L"-cancel");
                if (checkInterfaceScale)
                    current.prefix = L"app-" + std::to_wstring(percent) + L"-pass-" + std::to_wstring(pass) + L"-" + current.prefix;
                scenario = &current; RequireDesktop();
                const auto result = UiDialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(as::kDialog),
                    app->MainWindow(), as::DialogProc, reinterpret_cast<LPARAM>(&current.context));
                scenario = nullptr;
                Check(result == (save ? IDOK : IDCANCEL), "real modal settings returns expected save/cancel result");
                if (save) {
                    auto expected = baseline; expected.enabled = expected.keepPrevious = expected.numberedManualVersions = expected.overwriteOldest = true;
                    expected.intervalMinutes = 2; expected.maxBackups = 7;
                    expected.backupPattern = Pattern(true); expected.incrementPattern = Pattern(false);
                    Check(Same(as::Current(), expected) && Same(as::LoadSettings(root / L"autosave.ini"), expected),
                        "real OK commits every setting and both naming patterns through production persistence");
                } else Check(Same(as::Current(), baseline) && Bytes(root / L"autosave.ini") == before,
                    "outer Cancel discards all control and nested naming edits without changing stored bytes");
            }
        }
    }
    // 2 DPI branches x 3 languages x 2 outer results x 5 initial dialogs (settings
    // plus four nested naming dialogs) per enumerated monitor; six real stages each.
    const unsigned expectedPrintClientDialogs = checkInterfaceScale ? 0u : unsigned(monitors.size()) * 60u;
    Check(printClientInitialDialogs == expectedPrintClientDialogs &&
        printClientStageExecutions == expectedPrintClientDialogs * 6u,
        "all real initial dialogs complete the six-stage WM_PRINTCLIENT buffer probe");
    // 2 DPI branches x 3 languages x 2 outer results x 5 initial dialogs per
    // enumerated monitor, each live dialog running seven sequential WM_PAINT stages.
    const unsigned expectedLivePaintDialogs = expectedPrintClientDialogs;
    Check(livePaintInitialDialogs == expectedLivePaintDialogs &&
        livePaintStageExecutions == expectedLivePaintDialogs * livePaintStageCount,
        "all real initial dialogs complete the seven-stage live WM_PAINT damage, clip and replay probe");
    Check(livePaintNativeReplays == expectedLivePaintDialogs,
        "every real initial dialog exercises exactly one real native WM_PAINT replay stage");
    Check(livePaintClipStages == expectedLivePaintDialogs * livePaintStageCount,
        "every live stage observes all immediate visible child rectangles clipped by the live dialog DC");
    Check(livePaintGapPoints == expectedLivePaintDialogs * livePaintStageCount,
        "every live stage observes the non-damaged gap point clipped");
    Check(livePaintExposedPoints >= expectedLivePaintDialogs * livePaintStageCount,
        "every live stage observes at least one exposed damaged parent background point");
    const unsigned effective175Monitors = Effective175MonitorCount();
    const std::string effectiveCoverage = EffectiveCoverageReport(effective175Monitors);
    events << effectiveCoverage;
    UnhookWindowsHookEx(hook);
    Check(setContext(reinterpret_cast<HANDLE>(INT_PTR(-1))) != nullptr, "restore production context for app shutdown");
    RequireDesktop(); as::Shutdown();
    SetMenu(owner, nullptr); DestroyMenu(menu); Check(DestroyWindow(owner) != FALSE, "destroy only the fixture dialog owner");
    app->Hwnd() = nullptr; UnregisterClassW(ownerClass.lpszClassName, ownerClass.hInstance);
    ShutdownCleanup(app); g_Block = nullptr; delete app;
    events << "counterexample requested=" << Utf8(CounterexampleName()) << " injected=" << counterexampleInjections
        << " restored=" << counterexampleRestores << " geometry_findings=" << geometryFindings << '\n';
    events.flush();
    events.close(); Check(bool(events), "finish complete production dialog message journal");
    std::ofstream summary(root / L"summary.txt");
    summary << "functional_checks=passed\ncaptures=" << captures
        << "\nwm_printclient_initial_dialogs=" << printClientInitialDialogs
        << "\nwm_printclient_probe_stages=" << printClientStageExecutions
        << "\nwm_printclient_initial_dialogs_expected=" << expectedPrintClientDialogs
        << "\nwm_printclient_probe_stages_expected=" << expectedPrintClientDialogs * 6u
        << "\nwm_paint_initial_dialogs=" << livePaintInitialDialogs
        << "\nwm_paint_probe_stages=" << livePaintStageExecutions
        << "\nwm_paint_initial_dialogs_expected=" << expectedLivePaintDialogs
        << "\nwm_paint_probe_stages_expected=" << expectedLivePaintDialogs * livePaintStageCount
        << "\nwm_paint_native_replay_stages=" << livePaintNativeReplays
        << "\nwm_paint_stages_with_clipped_children=" << livePaintClipStages
        << "\nwm_paint_clipped_child_rect_observations=" << livePaintClippedChildren
        << "\nwm_paint_exposed_background_point_observations=" << livePaintExposedPoints
        << "\nwm_paint_gap_clipped_observations=" << livePaintGapPoints
        << "\nwm_paint_stages=cold,warm,allocation-denial,recovery,copy-denial,native-replay,final-recovery"
        << "\nwm_paint_evidence=live BeginPaint/rcPaint/BitBlt/FillRect/InvalidateRgn on the real dialog DC; GDI clip/call evidence, not DWM final pixels"
        << "\ngeometry_findings=" << geometryFindings
        // monitor_175_observed and legacy_shell_175_pair_observed are raw Shell
        // diagnostics only; effective coverage comes from the per-monitor ledger.
        << "\nmonitor_175_observed=" << observed175 << "\nnative_window_168dpi_observed=" << native168Observed
        << "\nlegacy_shell_175_pair_observed=" << legacyShell175Pair
        << "\nrequire_175=" << require175
        << "\ngeometry_counterexample=" << Utf8(CounterexampleName())
        << "\ngeometry_counterexample_injected=" << counterexampleInjections
        << "\ngeometry_counterexample_restored=" << counterexampleRestores
        << "\nsimulated_WM_DPICHANGED=false\nvisual_acceptance=pending-main-agent\n"
        << "BMP source is production WM_PRINT, not physical DWM composition. Product-unaware HWND DPI is logical 96; monitor scale is separate.\n"
        << "Rects and text extents use each HWND's awareness coordinate space. Native PMv2 is an extra OS-DPI probe, not the product startup policy.\n"
        << "Only enumerated monitor DPIs were exercised. Other DPI values are unavailable, not passed.\n"
        << "Effective 175% coverage is same-capture system physical mapping: native PMv2 168-DPI identity plus product-unaware 96-DPI 7/4 logical-to-physical rects on one monitor in all three languages. It is not the Shell GetScaleFactorForMonitor percentage, not DWM final composition, and not a real product startup visual acceptance run.\n";
    summary << effectiveCoverage;
    if (counterexampleInjections) summary
        << "geometry_counterexample_capture=" << Utf8(counterexampleCaptureName) << '\n'
        << "geometry_counterexample_control=Folder\n"
        << "geometry_counterexample_before_screen=" << Utf8(RectText(counterexampleOriginalScreen)) << '\n'
        << "geometry_counterexample_before_dialog=" << Utf8(RectText(counterexampleOriginalDialog)) << '\n'
        << "geometry_counterexample_injected_screen=" << Utf8(RectText(counterexampleInjectedScreen)) << '\n'
        << "geometry_counterexample_injected_dialog=" << Utf8(RectText(counterexampleInjectedDialog)) << '\n'
        << "geometry_counterexample_restored_screen=" << Utf8(RectText(counterexampleRestoredScreen)) << '\n'
        << "geometry_counterexample_restored_dialog=" << Utf8(RectText(counterexampleRestoredDialog)) << '\n';
    summary.close(); Check(bool(summary), "write explicit DPI coverage and visual-review limitations");
    // Evidence files are closed before these verdicts. The requested counterexample
    // must have run exactly once; a normal run must stay at zero.
    if (counterexample == Counterexample::None) {
        Check(counterexampleInjections == 0 && counterexampleRestores == 0,
            "normal layout runs never inject a geometry counterexample");
    } else {
        Check(counterexampleInjections == 1 && counterexampleRestores == 1,
            "requested geometry counterexample injected and restored exactly once");
        Check(geometryFindings > 0, "requested geometry counterexample must produce a real geometry finding");
    }
    // The real geometry gate precedes the effective-175% gate, so a combined run
    // still reports a genuine out-of-bounds or same-parent overlap as the failure.
    Check(geometryFindings == 0,
        "every real dialog control stays inside its dialog client area with no same-parent overlap");
    Check(!require175 || effective175Monitors >= 1,
        "--require-175 needs one monitor with complete effective 175% native and product-unaware coverage in all three languages");
    std::printf("PASS dialog-layout functional checks=%u captures=%u geometry-findings=%u; visual review pending; effective_175_monitors=%u legacy_shell_175_pair=%d\n",
        checks, captures, geometryFindings, effective175Monitors, int(legacyShell175Pair));
    return 0;
}
} // namespace backup_layout
