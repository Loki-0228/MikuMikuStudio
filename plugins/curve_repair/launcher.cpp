#include <Windows.h>
#include <TlHelp32.h>
#include <commdlg.h>
#include <commctrl.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {
constexpr wchar_t kClass[]=L"MMS.CurveRepair.Launcher";
constexpr int kPath=301,kBrowse=302,kLaunch=303,kStatus=304;
HWND windowHandle=nullptr;HFONT fontHandle=nullptr;int dpi=96;
std::wstring Error(DWORD code) {
    wchar_t* buffer=nullptr;
    const auto length=FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,code,MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US),reinterpret_cast<wchar_t*>(&buffer),0,nullptr);
    std::wstring result=length&&buffer?std::wstring(buffer,length):L"Windows error "+std::to_wstring(code);
    if(buffer)LocalFree(buffer);while(!result.empty()&&(result.back()==L'\r'||result.back()==L'\n'))result.pop_back();return result;
}
[[noreturn]]void WinFail(const wchar_t* operation) {
    const auto message=std::wstring(operation)+L": "+Error(GetLastError());
    const auto count=WideCharToMultiByte(CP_UTF8,0,message.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string utf8(count>0?static_cast<std::size_t>(count):1,'\0');
    if(count>0){WideCharToMultiByte(CP_UTF8,0,message.c_str(),-1,utf8.data(),count,nullptr,nullptr);utf8.pop_back();}
    throw std::runtime_error(std::move(utf8));
}
std::wstring EditText(int id) {
    auto edit=GetDlgItem(windowHandle,id);const auto n=GetWindowTextLengthW(edit);
    std::wstring value(static_cast<std::size_t>(n)+1,L'\0');GetWindowTextW(edit,value.data(),n+1);value.resize(n);return value;
}
bool IsTarget(const std::filesystem::path& wanted,const wchar_t* actual) {
    std::error_code error;const auto target=std::filesystem::weakly_canonical(wanted,error);
    if(error)return false;const auto found=std::filesystem::weakly_canonical(actual,error);
    return !error&&_wcsicmp(target.c_str(),found.c_str())==0;
}
MODULEENTRY32W Module(HANDLE process,const wchar_t* name) {
    // A newly created process has no loader module list while its primary
    // thread is suspended. Even after resuming, snapshots can be transiently
    // incomplete while Windows initializes the process or loads a DLL.
    const DWORD pid=GetProcessId(process);
    for(int attempt=0;attempt<100;++attempt) {
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
        if(snapshot!=INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);bool found=false;
            if(Module32FirstW(snapshot,&entry))do{if(_wcsicmp(entry.szModule,name)==0){found=true;break;}entry.dwSize=sizeof(entry);}while(Module32NextW(snapshot,&entry));
            CloseHandle(snapshot);if(found)return entry;
        } else {
            const DWORD error=GetLastError();
            if(error!=ERROR_BAD_LENGTH&&error!=ERROR_PARTIAL_COPY) {SetLastError(error);WinFail(L"Cannot inspect MMS modules");}
        }
        if(WaitForSingleObject(process,50)!=WAIT_TIMEOUT)throw std::runtime_error("MMS exited before the plug-in could be loaded.");
    }
    throw std::runtime_error("Cannot locate system module in MMS.");
}
HMODULE LocalModuleContaining(const void* address) {
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,GetCurrentProcessId());
    if(snapshot==INVALID_HANDLE_VALUE)WinFail(L"Cannot inspect launcher modules");
    const auto target=reinterpret_cast<std::uintptr_t>(address);HMODULE result=nullptr;
    if(Module32FirstW(snapshot,&entry))do{
        const auto base=reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        if(target>=base&&target-base<entry.modBaseSize){result=reinterpret_cast<HMODULE>(entry.modBaseAddr);break;}
        entry.dwSize=sizeof(entry);
    }while(Module32NextW(snapshot,&entry));
    CloseHandle(snapshot);return result;
}
HMODULE WaitModule(HANDLE process,const std::filesystem::path& wanted) {
    for(int i=0;i<100;++i) {
        const DWORD pid=GetProcessId(process);HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
        if(snapshot!=INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
            if(Module32FirstW(snapshot,&entry))do{
                if(IsTarget(wanted,entry.szExePath)){const auto result=reinterpret_cast<HMODULE>(entry.modBaseAddr);CloseHandle(snapshot);return result;}
                entry.dwSize=sizeof(entry);
            }while(Module32NextW(snapshot,&entry));
            CloseHandle(snapshot);
        }
        Sleep(100);
    }
    return nullptr;
}
void Inject(HANDLE process,const std::filesystem::path& dll) {
    std::wstring full=std::filesystem::absolute(dll).wstring();const auto bytes=(full.size()+1)*sizeof(wchar_t);
    LPVOID remote=VirtualAllocEx(process,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!remote)WinFail(L"Cannot allocate MMS loader memory");
    HANDLE thread=nullptr;
    try {
        SIZE_T written=0;if(!WriteProcessMemory(process,remote,full.c_str(),bytes,&written)||written!=bytes)WinFail(L"Cannot pass the plug-in path to MMS");
        const auto localLoad=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
        if(!localLoad)throw std::runtime_error("Cannot locate the Windows library loader.");
        const auto owning=LocalModuleContaining(reinterpret_cast<const void*>(localLoad));
        if(!owning)throw std::runtime_error("Cannot locate the Windows library loader.");
        wchar_t modulePath[MAX_PATH]{};
        if(!GetModuleFileNameW(owning,modulePath,MAX_PATH))WinFail(L"Cannot resolve the Windows library loader");
        const auto moduleName=std::filesystem::path(modulePath).filename().wstring();
        const auto localBase=reinterpret_cast<std::uintptr_t>(owning);const auto localProc=reinterpret_cast<std::uintptr_t>(localLoad);
        const auto remoteModule=Module(process,moduleName.c_str());const auto offset=localProc-localBase;
        if(offset>=remoteModule.modBaseSize)throw std::runtime_error("The Windows library loader address is invalid.");
        const auto remoteProc=reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<std::uintptr_t>(remoteModule.modBaseAddr)+offset);
        thread=CreateRemoteThread(process,nullptr,0,remoteProc,remote,0,nullptr);
        if(!thread)WinFail(L"MMS plug-in load was denied");
        if(WaitForSingleObject(thread,30000)!=WAIT_OBJECT_0) {
            TerminateProcess(process,1);WaitForSingleObject(thread,5000);
            throw std::runtime_error("MMS plug-in load timed out.");
        }
        DWORD result=0;if(!GetExitCodeThread(thread,&result))WinFail(L"Cannot read the MMS plug-in load result");
        if(!result||!WaitModule(process,dll))throw std::runtime_error("MMS did not load the plug-in.");
        CloseHandle(thread);thread=nullptr;
    }catch(...) {if(thread)CloseHandle(thread);VirtualFreeEx(process,remote,0,MEM_RELEASE);throw;}
    VirtualFreeEx(process,remote,0,MEM_RELEASE);
}
std::filesystem::path PluginPath() {
    wchar_t own[32768]{};const auto n=GetModuleFileNameW(nullptr,own,32768);
    if(!n||n>=32768)WinFail(L"Cannot resolve the launcher directory");
    return std::filesystem::path(own).parent_path()/L"MMSCurveRepair.dll";
}
void Launch() {
    const auto host=std::filesystem::path(EditText(kPath));const auto dll=PluginPath();
    if(host.empty()||!std::filesystem::exists(host))throw std::runtime_error("Select the current MMS executable first.");
    if(!std::filesystem::exists(dll))throw std::runtime_error("MMSCurveRepair.dll must be beside this launcher.");
    auto hostPath=std::filesystem::absolute(host).wstring();STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(hostPath.c_str(),nullptr,nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT,
        nullptr,std::filesystem::path(hostPath).parent_path().c_str(),&startup,&pi))WinFail(L"Cannot start MMS");
    try {
        // Let Windows initialize the child's loader before looking up the
        // remote LoadLibraryW address. The DLL waits for the MMS menu itself.
        if(ResumeThread(pi.hThread)==static_cast<DWORD>(-1))WinFail(L"Cannot resume MMS");
        Inject(pi.hProcess,dll);
    }catch(...) {TerminateProcess(pi.hProcess,1);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);throw;}
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);DestroyWindow(windowHandle);
}
void Browse() {
    wchar_t path[32768]{};const auto current=EditText(kPath);wcsncpy_s(path,current.c_str(),_TRUNCATE);
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=windowHandle;ofn.lpstrFile=path;ofn.nMaxFile=32768;
    ofn.lpstrFilter=L"MikuDanceStudio (*.exe)\0MikuMikuDance.exe\0All executable files (*.exe)\0*.exe\0\0";
    ofn.lpstrTitle=L"Select the current MMS executable";
    ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
    if(GetOpenFileNameW(&ofn))SetDlgItemTextW(windowHandle,kPath,path);
}
void Layout(HWND hwnd) {
    RECT r{};GetClientRect(hwnd,&r);auto px=[](int n){return MulDiv(n,dpi,96);};
    MoveWindow(GetDlgItem(hwnd,300),px(16),px(17),px(104),px(24),TRUE);
    MoveWindow(GetDlgItem(hwnd,kPath),px(126),px(14),r.right-px(234),px(27),TRUE);
    MoveWindow(GetDlgItem(hwnd,kBrowse),r.right-px(96),px(14),px(80),px(27),TRUE);
    MoveWindow(GetDlgItem(hwnd,kStatus),px(16),px(53),r.right-px(32),px(50),TRUE);
    MoveWindow(GetDlgItem(hwnd,kLaunch),r.right-px(212),r.bottom-px(45),px(104),px(30),TRUE);
    MoveWindow(GetDlgItem(hwnd,IDCANCEL),r.right-px(96),r.bottom-px(45),px(80),px(30),TRUE);
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
    case WM_CREATE: {
        HDC dc=GetDC(hwnd);dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(hwnd,dc);
        fontHandle=CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        auto add=[&](const wchar_t* cls,const wchar_t* value,int id,DWORD style) {
            auto child=CreateWindowExW(!wcscmp(cls,L"EDIT")?WS_EX_CLIENTEDGE:0,cls,value,WS_CHILD|WS_VISIBLE|style,
                0,0,1,1,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(fontHandle),FALSE);return child;
        };
        add(L"STATIC",L"MMS executable",300,0);
        auto path=add(L"EDIT",L"",kPath,WS_TABSTOP|ES_AUTOHSCROLL);
        add(L"BUTTON",L"Browse...",kBrowse,WS_TABSTOP);
        add(L"STATIC",L"The launcher loads the plug-in into MMS before starting it. The MMS executable stays intact.",kStatus,0);
        add(L"BUTTON",L"Start MMS",kLaunch,WS_TABSTOP|BS_DEFPUSHBUTTON);
        add(L"BUTTON",L"Close",IDCANCEL,WS_TABSTOP);
        const auto candidate=PluginPath().parent_path()/L"MikuMikuDance.exe";
        if(std::filesystem::exists(candidate))SetWindowTextW(path,candidate.c_str());
        Layout(hwnd);return 0;
    }
    case WM_SIZE:Layout(hwnd);return 0;
    case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize={MulDiv(520,dpi,96),MulDiv(168,dpi,96)};return 0;
    case WM_COMMAND:
        if(LOWORD(w)==kBrowse){Browse();return 0;}
        if(LOWORD(w)==kLaunch) {
            EnableWindow(GetDlgItem(hwnd,kLaunch),FALSE);
            SetWindowTextW(GetDlgItem(hwnd,kStatus),L"Starting MMS and loading the plug-in...");UpdateWindow(hwnd);
            try {Launch();}catch(const std::exception& e){
                const int count=MultiByteToWideChar(CP_UTF8,0,e.what(),-1,nullptr,0);
                std::wstring detail(count>0?static_cast<std::size_t>(count):8,L'\0');
                if(count>0){MultiByteToWideChar(CP_UTF8,0,e.what(),-1,detail.data(),count);detail.pop_back();}
                else detail=L"Plug-in loading failed.";
                SetWindowTextW(GetDlgItem(hwnd,kStatus),detail.c_str());
                MessageBoxW(hwnd,detail.c_str(),L"Plug-in was not loaded",MB_OK|MB_ICONERROR);
                EnableWindow(GetDlgItem(hwnd,kLaunch),TRUE);
            }
            return 0;
        }
        if(LOWORD(w)==IDCANCEL)DestroyWindow(hwnd);return 0;
    case WM_CLOSE:DestroyWindow(hwnd);return 0;
    case WM_DESTROY:if(fontHandle)DeleteObject(fontHandle);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    SetProcessDPIAware();
    ULONG languages=0;SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME,L"en-US\0",&languages);
    HDC dc=GetDC(nullptr);dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(nullptr,dc);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=WindowProc;cls.hInstance=instance;cls.lpszClassName=kClass;
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    if(!RegisterClassExW(&cls))return 2;
    constexpr DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    RECT bounds{0,0,MulDiv(640,dpi,96),MulDiv(164,dpi,96)};
    AdjustWindowRectEx(&bounds,style,FALSE,WS_EX_CONTROLPARENT);
    windowHandle=CreateWindowExW(WS_EX_CONTROLPARENT,kClass,L"MMS Curve Repair Launcher",
        style,CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,instance,nullptr);
    if(!windowHandle)return 3;
    ShowWindow(windowHandle,show);UpdateWindow(windowHandle);MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(windowHandle,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    return static_cast<int>(msg.wParam);
}
