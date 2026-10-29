#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "../src/window/cache_manager_dialog.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
namespace fs=std::filesystem;
namespace cache=mikumikustudio::cache_manager;
namespace ui=mikumikustudio::cache_manager_ui;
unsigned checks=0,failures=0,language=0,stage=0,ticks=0;
fs::path output;
HWND dialog=nullptr;
void Check(bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL %s\n",message);}}
fs::path File(unsigned id){wchar_t hash[65]{};swprintf_s(hash,L"%064x",id);return cache::Root()/L"Textures"/L"texture"/(std::wstring(hash)+L".mtc");}
void Store(unsigned id,unsigned size) {
    const auto path=File(id);cache::WriteReservation write(path,size);Check(bool(write),"fixture reservation");fs::create_directories(path.parent_path());
    std::uint32_t header[19]{};header[0]=0x4354504d;header[1]=2;header[2]=size-76;
    std::ofstream stream(path,std::ios::binary);stream.write(reinterpret_cast<char*>(header),sizeof(header));stream.seekp(size-1);stream.put(0);stream.close();write.Commit();
}
void Fixture() {
    SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",(output/(L"lang-"+std::to_wstring(language))/L"Cache").c_str());
    Store(1,8192);Store(2,12288);Store(3,10240);cache::ResetScene();
    const auto a=output/L"舞台演出"/L"Stage.pmm",b=output/L"另一个工程"/L"Stage.pmm";
    cache::BeginProject(a.c_str());cache::Touch(File(1));cache::Touch(File(2));cache::ProjectOpened(a.c_str());cache::ResetScene();Sleep(2);
    cache::BeginProject(b.c_str());cache::Touch(File(1));cache::Touch(File(3));cache::ProjectOpened(b.c_str());cache::ResetScene();
}
BOOL CALLBACK Find(HWND window,LPARAM){if(GetDlgItem(window,ui::Enabled)&&GetDlgItem(window,ui::Projects))dialog=window;return TRUE;}
BOOL CALLBACK Confirm(HWND window,LPARAM){if(GetDlgItem(window,IDYES)&&GetDlgItem(window,IDNO))PostMessageW(window,WM_COMMAND,IDYES,0);return TRUE;}
void Capture() {
    RECT bounds{};GetWindowRect(dialog,&bounds);int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(dc,bitmap);
    SendMessageW(dialog,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);GdiFlush();
    const auto path=output/(L"dialog-"+std::to_wstring(language)+L".bmp");FILE* file=nullptr;
    if(!_wfopen_s(&file,path.c_str(),L"wb")){BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);header.bfSize=header.bfOffBits+width*height*4;
        fwrite(&header,sizeof(header),1,file);fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,file);fwrite(pixels,4,std::size_t(width)*height,file);fclose(file);}
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
void Geometry() {
    RECT client{};GetClientRect(dialog,&client);
    RECT expected{0,0,360,206};MapDialogRect(dialog,&expected);
    Check(client.right<=expected.right&&client.bottom<=expected.bottom,"compact dialog respects requested size");
    const int controls[]={ui::Enabled,ui::Automatic,ui::Limit,ui::Recent,ui::Projects,ui::Pin,ui::ClearProject,ui::ClearReclaimable,ui::ClearAll,ui::Apply,IDCANCEL};
    for(int id:controls) {
        HWND control=GetDlgItem(dialog,id);RECT rect{};GetWindowRect(control,&rect);MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&rect),2);
        Check(rect.left>=0&&rect.top>=0&&rect.right<=client.right&&rect.bottom<=client.bottom,"control stays inside dialog");
        if(id==ui::Limit||id==ui::Recent||id==ui::Projects)continue;
        wchar_t text[256]{};GetWindowTextW(control,text,256);HDC dc=GetDC(control);auto font=reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0));auto previous=SelectObject(dc,font);
        RECT needed{};DrawTextW(dc,text,-1,&needed,DT_CALCRECT|DT_SINGLELINE|DT_NOPREFIX);
        const int captionWidth=needed.right+(id==ui::Enabled||id==ui::Automatic?24:12);
        if(captionWidth>rect.right-rect.left)std::printf("caption language=%u id=%d required=%ld available=%ld\n",language,id,long(captionWidth),rect.right-rect.left);
        Check(captionWidth<=rect.right-rect.left,"localized button caption fits");
        SelectObject(dc,previous);ReleaseDC(control,dc);
    }
    Check(!GetDlgItem(dialog,ui::Explanation),"footer paragraphs removed");
    RECT summary{},kinds{};GetWindowRect(GetDlgItem(dialog,ui::Summary),&summary);GetWindowRect(GetDlgItem(dialog,ui::Kinds),&kinds);
    Check(summary.top==kinds.top&&summary.right<=kinds.left,"MME and texture usage shares the summary row");
}
BOOL CALLBACK Tooltips(HWND window,LPARAM result) {
    wchar_t name[80]{};GetClassNameW(window,name,80);
    if(std::wstring(name)==TOOLTIPS_CLASSW&&GetWindow(window,GW_OWNER)==dialog)*reinterpret_cast<HWND*>(result)=window;
    return TRUE;
}
void CheckTooltips() {
    HWND tips=nullptr;EnumThreadWindows(GetCurrentThreadId(),Tooltips,reinterpret_cast<LPARAM>(&tips));
    Check(tips&&SendMessageW(tips,TTM_GETTOOLCOUNT,0,0)==4,"automatic option and all cleaning buttons have hover hints");
    for(int id:{ui::Automatic,ui::ClearProject,ui::ClearReclaimable,ui::ClearAll}) {
        wchar_t text[1024]{};TOOLINFOW tool{};tool.cbSize=sizeof(tool);tool.hwnd=dialog;
        tool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(dialog,id));tool.lpszText=text;
        SendMessageW(tips,TTM_GETTEXTW,1024,reinterpret_cast<LPARAM>(&tool));
        Check(text[0]!=0,"localized cleaning tooltip text is present");
        if(id==ui::ClearReclaimable)Check(std::wstring(text).find(L"80%")!=std::wstring::npos,"reclaimable tooltip explains automatic target");
    }
}
void Click(int id){SendDlgItemMessageW(dialog,id,BM_CLICK,0,0);}
void CALLBACK Poll(HWND,UINT,UINT_PTR,DWORD) {
    if(++ticks>180){Check(false,"dialog timed out");if(dialog)PostMessageW(dialog,WM_CLOSE,0,0);return;}
    dialog=nullptr;EnumThreadWindows(GetCurrentThreadId(),Find,0);if(!dialog)return;
    if(stage==10){EnumThreadWindows(GetCurrentThreadId(),Confirm,0);stage=11;return;}
    if(!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
    switch(stage) {
    case 0: {
        Check(IsDlgButtonChecked(dialog,ui::Enabled)==BST_CHECKED&&IsDlgButtonChecked(dialog,ui::Automatic)==BST_CHECKED,"both caching and automatic cleanup start checked");
        Check(ListView_GetItemCount(GetDlgItem(dialog,ui::Projects))==2,"both fixture projects appear");
        wchar_t text[32]{};GetDlgItemTextW(dialog,ui::Limit,text,32);Check(std::wstring(text)==L"4","four GB shown by default");
        Check(GetDlgItemInt(dialog,ui::Recent,nullptr,FALSE)==10,"ten recent projects shown by default");
        Geometry();CheckTooltips();Capture();stage=1;Click(ui::Enabled);Click(ui::Apply);break;
    }
    case 1:Check(!cache::Current().enabled,"disable caching is persisted");Check(!IsWindowEnabled(GetDlgItem(dialog,ui::Automatic)),"automatic cleanup disabled while caching is off");stage=2;Click(ui::Enabled);Click(ui::Apply);break;
    case 2:Check(cache::Current().enabled,"caching can be re-enabled");stage=3;Click(ui::Automatic);Click(ui::Apply);break;
    case 3:Check(!cache::Current().automaticCleanup,"automatic cleanup can be disabled independently");stage=4;Click(ui::Automatic);Click(ui::Apply);break;
    case 4:Check(cache::Current().automaticCleanup,"automatic cleanup can be re-enabled");
        ListView_SetItemState(GetDlgItem(dialog,ui::Projects),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);stage=5;Click(ui::Pin);break;
    case 5: {
        auto view=cache::Inspect();Check(view.projects[0].pinned,"pin button saves project preference");
        Check(IsWindowEnabled(GetDlgItem(dialog,ui::ClearProject)),"selected exclusive cache can be manually cleaned");stage=6;Click(ui::ClearProject);break;
    }
    case 6:Check(!fs::exists(File(3))&&fs::exists(File(1))&&fs::exists(File(2)),"manual project action preserves shared cache");
        SetDlgItemTextW(dialog,ui::Recent,L"1");stage=7;Click(ui::Apply);break;
    case 7:Check(cache::Current().recentProjects==1,"recent project preference is saved");stage=8;Click(ui::ClearReclaimable);break;
    case 8:Check(!fs::exists(File(2))&&fs::exists(File(1)),"manual reclaimable action cleans older exclusive data");stage=10;Click(ui::ClearAll);break;
    case 11:Check(cache::Inspect().totalBytes==0,"confirmed clear-all removes unused disk data");stage=12;PostMessageW(dialog,WM_CLOSE,0,0);break;
    }
}
int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);Check(argc==2,"explicit new output directory");if(argc!=2)return 1;
    output=fs::absolute(argv[1]);Check(!fs::exists(output),"new UI evidence directory");if(fs::exists(output))return 1;fs::create_directories(output);
    SetProcessDPIAware();const auto previous=GetThreadDesktop(GetCurrentThreadId());wchar_t name[80]{};swprintf_s(name,L"MMS-cache-ui-%lu",GetCurrentProcessId());
    const auto desktop=CreateDesktopW(name,nullptr,nullptr,0,GENERIC_ALL,nullptr);Check(desktop&&SetThreadDesktop(desktop),"private inactive desktop");if(!desktop)return 1;
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES|ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"MMS-cache-ui-owner";wc.lpfnWndProc=DefWindowProcW;RegisterClassW(&wc);
    HWND owner=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,1200,800,nullptr,nullptr,wc.hInstance,nullptr);
    auto app=std::make_unique<mikumikustudio::MMDApp>();app->state=mikumikustudio::MMDAppState{};app->Hwnd()=owner;app->HInstance()=wc.hInstance;
    HMENU bar=CreateMenu(),help=CreatePopupMenu();AppendMenuW(help,MF_STRING,201,L"About");AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(help),L"Help");SetMenu(owner,bar);
    for(language=0;language<3;++language) {
        Fixture();stage=ticks=0;app->EnglishUI()=static_cast<unsigned char>(language);ui::RefreshMenu(app.get());
        Check(GetMenuState(help,ui::kCommand,MF_BYCOMMAND)!=UINT(-1),"Help menu exposes cache management");
        SetTimer(owner,77,100,Poll);Check(ui::HandleCommand(app.get(),ui::kCommand),"cache management command opens real dialog");KillTimer(owner,77);
        Check(stage==12,"all native dialog actions completed");cache::Shutdown();
    }
    SetMenu(owner,nullptr);DestroyMenu(bar);DestroyWindow(owner);app->Hwnd()=nullptr;
    UnregisterClassW(wc.lpszClassName,wc.hInstance);SetThreadDesktop(previous);CloseDesktop(desktop);
    std::printf("cache manager UI: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
