#include "repair_core.hpp"
#include "mikudancestudio/plugin_menu.h"
#include <commdlg.h>
#include <atomic>
#include <memory>
#include <string>

namespace {
HMODULE module=nullptr;
HWND host=nullptr;
std::atomic<HWND> dialog{nullptr};
std::atomic<bool> opening{false};
std::atomic<unsigned> language{2};
MmsMenuHostApi api{};
std::atomic<HHOOK> startupHook{nullptr};
std::atomic<bool> registered{false};
constexpr wchar_t kClass[]=L"MMS.CurveRepair.1";
constexpr UINT kAttach=WM_APP+0x6d1;
enum { Project=101, ChooseProject, Reference, ChooseReference, Scan, Preview, Save, Close, Info };
const wchar_t* Text(unsigned lang,const wchar_t* zh,const wchar_t* en,const wchar_t* ja) {
    return lang==0?ja:lang==1?en:zh;
}
struct Ui {
    unsigned lang=2; HFONT font=nullptr; int scale=96;
    std::unique_ptr<repair::Analysis> analysis;
    std::filesystem::path source;
    int px(int n)const{return MulDiv(n,scale,96);}
    const wchar_t* tr(const wchar_t* zh,const wchar_t* en,const wchar_t* ja)const{return Text(lang,zh,en,ja);}
};
std::wstring ControlText(HWND window,int id) {
    const auto control=GetDlgItem(window,id); const auto size=GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(size)+1,L'\0'); GetWindowTextW(control,text.data(),size+1); text.resize(size); return text;
}
std::wstring Wide(const char* text) {
    const int size=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0); if(size<=0)return L"Error";
    std::wstring result(size,L'\0'); MultiByteToWideChar(CP_UTF8,0,text,-1,result.data(),size); result.pop_back();return result;
}
void Layout(HWND window,Ui& ui) {
    RECT r{};GetClientRect(window,&r);const int w=MulDiv(r.right,96,ui.scale),h=MulDiv(r.bottom,96,ui.scale);
    auto move=[&](int id,int x,int y,int width,int height){MoveWindow(GetDlgItem(window,id),ui.px(x),ui.px(y),ui.px(width),ui.px(height),TRUE);};
    move(201,14,17,146,24);move(Project,166,14,w-276,26);move(ChooseProject,w-98,14,84,26);
    move(202,14,55,146,24);move(Reference,166,52,w-276,26);move(ChooseReference,w-98,52,84,26);
    move(Info,14,90,w-144,46);move(Scan,w-126,94,112,30);
    move(Preview,14,146,w-28,h-202);move(Save,14,h-42,220,28);move(Close,w-98,h-42,84,28);
}
void Browse(HWND window,int id,Ui& ui) {
    wchar_t path[32768]{};const auto previous=ControlText(window,id);wcsncpy_s(path,previous.c_str(),_TRUNCATE);
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=window;ofn.lpstrFile=path;ofn.nMaxFile=32768;
    ofn.lpstrFilter=id==Project?L"PMM (*.pmm)\0*.pmm\0\0":L"VMD (*.vmd)\0*.vmd\0\0";
    ofn.lpstrTitle=id==Project?ui.tr(L"选择已保存的工程",L"Select a saved project",L"保存済みのプロジェクトを選択"):ui.tr(L"选择原始镜头 VMD",L"Select the original camera VMD",L"元のカメラ VMD を選択");
    ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
    if(GetOpenFileNameW(&ofn)) SetDlgItemTextW(window,id,path);
}
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto* ui=reinterpret_cast<Ui*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    try {
    if(message==WM_NCCREATE) {ui=static_cast<Ui*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(ui));}
    if(!ui)return DefWindowProcW(window,message,w,l);
    switch(message) {
    case WM_CREATE: {
        HDC dc=GetDC(window);ui->scale=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(window,dc);
        ui->font=CreateFontW(-ui->px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        const auto control=[&](const wchar_t* type,const wchar_t* caption,int id,DWORD style) {
            HWND h=CreateWindowExW(wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,type,caption,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),module,nullptr);
            SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(ui->font),FALSE);return h;
        };
        control(L"STATIC",ui->tr(L"已保存工程",L"Saved project",L"保存済み PMM"),201,0);
        control(L"EDIT",L"",Project,WS_TABSTOP|ES_AUTOHSCROLL);
        control(L"BUTTON",ui->tr(L"浏览…",L"Browse…",L"参照…"),ChooseProject,WS_TABSTOP);
        control(L"STATIC",ui->tr(L"原始 VMD（可选）",L"Original VMD (opt.)",L"元の VMD（任意）"),202,0);
        control(L"EDIT",L"",Reference,WS_TABSTOP|ES_AUTOHSCROLL);
        control(L"BUTTON",ui->tr(L"浏览…",L"Browse…",L"参照…"),ChooseReference,WS_TABSTOP);
        control(L"STATIC",ui->tr(L"先扫描并核对结果，再保存副本。此窗口处理磁盘文件，当前场景不受影响。",L"Scan and review before saving a copy. This tool reads saved files; the current scene is unchanged.",L"スキャン結果を確認してからコピーを保存します。保存済みファイルを処理し、現在のシーンは変更しません。"),Info,0);
        control(L"BUTTON",ui->tr(L"扫描",L"Scan",L"スキャン"),Scan,WS_TABSTOP|BS_DEFPUSHBUTTON);
        const auto preview=control(L"EDIT",L"",Preview,WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL);
        SendMessageW(preview,EM_SETLIMITTEXT,4*1024*1024,0);
        control(L"BUTTON",ui->tr(L"保存修复副本",L"Save repaired copy",L"修復コピーを保存"),Save,WS_TABSTOP);
        control(L"BUTTON",ui->tr(L"关闭",L"Close",L"閉じる"),Close,WS_TABSTOP);
        EnableWindow(GetDlgItem(window,Save),FALSE);Layout(window,*ui);return 0;
    }
    case WM_SIZE:Layout(window,*ui);return 0;
    case WM_GETMINMAXINFO: {
        auto* limits=reinterpret_cast<MINMAXINFO*>(l);limits->ptMinTrackSize={ui->px(660),ui->px(420)};return 0;
    }
    case WM_COMMAND:
        if((LOWORD(w)==Project||LOWORD(w)==Reference)&&HIWORD(w)==EN_CHANGE) {
            ui->analysis.reset();EnableWindow(GetDlgItem(window,Save),FALSE);SetDlgItemTextW(window,Preview,L"");return 0;
        }
        switch(LOWORD(w)) {
        case ChooseProject:Browse(window,Project,*ui);return 0;
        case ChooseReference:Browse(window,Reference,*ui);return 0;
        case IDOK:case Scan: {
            ui->analysis.reset();EnableWindow(GetDlgItem(window,Save),FALSE);
            SetDlgItemTextW(window,Preview,ui->tr(L"正在扫描…",L"Scanning…",L"スキャン中…"));UpdateWindow(window);
            ui->source=std::filesystem::absolute(ControlText(window,Project));const auto vmd=ControlText(window,Reference);
            auto result=repair::Analyze(repair::ReadFile(ui->source),vmd.empty()?repair::Bytes{}:repair::ReadFile(vmd));
            SetDlgItemTextW(window,Preview,repair::Report(result,ui->lang).c_str());
            EnableWindow(GetDlgItem(window,Save),!result.patches.empty());ui->analysis=std::make_unique<repair::Analysis>(std::move(result));return 0;
        }
        case Save:if(ui->analysis) {
            const auto report=L"PMM: "+ui->source.wstring()+L"\r\nVMD: "+ControlText(window,Reference)+L"\r\n\r\n"+repair::Report(*ui->analysis,ui->lang,true);
            const auto output=repair::SaveCopy(*ui->analysis,ui->source,report);
            const auto text=std::wstring(ui->tr(L"修复副本已保存。用 MMS 打开以下文件查看结果：\r\n",L"Copy saved. Open this file in MMS to review the result:\r\n",L"コピーを保存しました。MMS で次のファイルを開いて確認してください：\r\n"))+output.wstring();
            SetDlgItemTextW(window,Preview,(text+L"\r\n\r\n"+repair::Report(*ui->analysis,ui->lang)).c_str());EnableWindow(GetDlgItem(window,Save),FALSE);
            MessageBoxW(window,text.c_str(),ui->tr(L"保存完成",L"Saved",L"保存完了"),MB_OK|MB_ICONINFORMATION);
        }return 0;
        case Close:case IDCANCEL:DestroyWindow(window);return 0;
        }break;
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:dialog=nullptr;if(ui->font)DeleteObject(ui->font);PostQuitMessage(0);return 0;
    }
    } catch(const std::exception& error) {
        const auto text=std::wstring(ui->tr(L"未写入原工程。\r\n详情：",L"The original project was not modified.\r\nDetails: ",L"元のプロジェクトは変更していません。\r\n詳細: "))+Wide(error.what());
        SetDlgItemTextW(window,Preview,text.c_str());MessageBoxW(window,text.c_str(),ui->tr(L"无法完成",L"Unable to complete",L"処理できません"),MB_OK|MB_ICONWARNING);
    } catch(...) {MessageBoxW(window,L"Repair operation failed.",L"MMS Curve Repair",MB_OK|MB_ICONERROR);}
    return DefWindowProcW(window,message,w,l);
}
DWORD WINAPI OpenUi(void*) {
    try {
        Ui ui;ui.lang=language.load();
        WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=WindowProc;cls.hInstance=module;cls.lpszClassName=kClass;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
        RegisterClassExW(&cls);
        HDC dc=GetDC(nullptr);ui.scale=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(nullptr,dc);
        const auto window=CreateWindowExW(WS_EX_CONTROLPARENT,kClass,ui.tr(L"镜头数值修复",L"Camera value repair",L"カメラ数値の修復"),WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,ui.px(820),ui.px(560),host,nullptr,module,&ui);
        dialog=window;
        if(window) {ShowWindow(window,SW_SHOW);SetForegroundWindow(window);MSG msg{};
            while(GetMessageW(&msg,nullptr,0,0)>0) if(!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    }catch(...) {OutputDebugStringW(L"MMS Curve Repair: could not create repair window.\n");}
    dialog=nullptr;opening=false;return 0;
}
const wchar_t* __cdecl Title(void*,std::uint32_t lang) {return Text(lang,L"数值修复",L"Value repair",L"数値修復");}
void __cdecl Populate(void*,HMENU menu,std::uint32_t first,std::uint32_t lang) {
    language=lang;while(GetMenuItemCount(menu)>0)DeleteMenu(menu,0,MF_BYPOSITION);
    AppendMenuW(menu,MF_STRING,first,Text(lang,L"修复已保存的镜头数值…",L"Repair saved camera values…",L"保存済みカメラ数値を修復…"));
}
void __cdecl Command(void*,std::uint32_t offset) {
    if(offset)return;
    if(auto window=dialog.load()){ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);return;}
    if(opening.exchange(true))return;
    HANDLE thread=CreateThread(nullptr,0,OpenUi,nullptr,0,nullptr);if(thread)CloseHandle(thread);else opening=false;
}
LRESULT CALLBACK AttachHook(int code,WPARAM w,LPARAM l) {
    if(code>=0) {
        const auto* msg=reinterpret_cast<CWPSTRUCT*>(l);
        if(msg->hwnd==host&&msg->message==kAttach&&!registered.load()) {
            MmsPluginMenu descriptor{sizeof(descriptor),MMS_MENU_ABI_VERSION,host,"org.mikudancestudio.curve-repair",nullptr,Title,Populate,Command,nullptr};
            if(api.registerMenu(&descriptor))registered=true;
        }
    }
    return CallNextHookEx(startupHook.load(),code,w,l);
}
BOOL CALLBACK FindWindow(HWND window,LPARAM data) {
    DWORD process=0;GetWindowThreadProcessId(window,&process);if(process!=GetCurrentProcessId()||!GetMenu(window))return TRUE;
    wchar_t name[128]{};GetClassNameW(window,name,128);
    if(wcscmp(name,L"Polygon Movie Maker"))return TRUE;
    *reinterpret_cast<HWND*>(data)=window;return FALSE;
}
DWORD WINAPI Start(void*) {
    const auto getter=reinterpret_cast<MmsGetMenuHostApiFn>(GetProcAddress(GetModuleHandleW(nullptr),"MMSGetMenuHostApi"));
    if(!getter)return 0;api.structureSize=sizeof(api);
    if(!getter(MMS_MENU_ABI_VERSION,&api)||api.abiVersion!=MMS_MENU_ABI_VERSION||!api.registerMenu)return 0;
    // The worker runs only after loader initialization. No menu/UI work occurs in DllMain.
    HMODULE pinned=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Start),&pinned);
    for(int attempt=0;attempt<600;++attempt) {
        HWND found=nullptr;EnumWindows(FindWindow,reinterpret_cast<LPARAM>(&found));
        if(found) {host=found;break;}Sleep(100);
    }
    if(!host)return 0;
    startupHook=SetWindowsHookExW(WH_CALLWNDPROC,AttachHook,module,GetWindowThreadProcessId(host,nullptr));
    if(!startupHook.load())return 0;
    for(int i=0;i<100&&!registered.load()&&IsWindow(host);++i) {
        DWORD_PTR ignored=0;SendMessageTimeoutW(host,kAttach,0,0,SMTO_ABORTIFHUNG,200,&ignored);Sleep(100);
    }
    if(auto hook=startupHook.exchange(nullptr))UnhookWindowsHookEx(hook);
    return 0;
}
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        // Keep thread notifications: this DLL links the static C runtime.
        module=instance;
        HANDLE thread=CreateThread(nullptr,0,Start,nullptr,0,nullptr);if(thread)CloseHandle(thread);
    }
    return TRUE;
}
