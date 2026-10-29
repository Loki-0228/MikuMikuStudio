#pragma once
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/menu_initial_paint.hpp"
#include "mikumikustudio/effect_display.hpp"
#include <commctrl.h>
#include <algorithm>
#include <vector>

// Exercise native popup painting on an inactive desktop, before hover and
// without WM_PRINT, which would request a second paint and hide this defect.
namespace menu_first_paint {
using namespace mikumikustudio;
inline MMDApp* app=nullptr;
inline HMENU expected=nullptr;
inline HWND popup=nullptr;
inline unsigned checks=0,failures=0;
inline bool curveControls=true;
inline bool injectBlank=false,firstIdle=false;
inline unsigned idleChecks=0;
inline bool delayedBlankMode=false,noIdleMode=false,slowInitMode=false;
inline bool editorMode=false;
inline BOOL CALLBACK FindPopup(HWND hwnd,LPARAM) {
    wchar_t name[32]{};GetClassNameW(hwnd,name,32);
    if(std::wcscmp(name,L"#32768"))return TRUE;
    MENUBARINFO menu{sizeof(menu)};
    if(GetMenuBarInfo(hwnd,OBJID_CLIENT,0,&menu) && menu.hMenu==expected)popup=hwnd;
    return TRUE;
}
inline void Capture(bool expectText=true) {
    popup=nullptr;EnumThreadWindows(GetCurrentThreadId(),FindPopup,0);
    if(!popup){++failures;std::puts("FAIL no native popup");return;}
    RECT bounds{};GetWindowRect(popup,&bounds);
    const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    HDC source=GetWindowDC(popup),dc=CreateCompatibleDC(source);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* raw=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&raw,nullptr,0);
    if(!source||!dc||!bitmap||!raw){++failures;return;}
    const auto old=SelectObject(dc,bitmap);
    BitBlt(dc,0,0,width,height,source,0,0,SRCCOPY);GdiFlush();
    const auto* pixels=static_cast<const DWORD*>(raw);
    unsigned tested=0,blank=0;
    for(int position=0;position<GetMenuItemCount(expected);++position) {
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_FTYPE|MIIM_STATE;
        RECT row{};wchar_t text[512]{};
        GetMenuItemInfoW(expected,position,TRUE,&item);
        if((item.fType&MFT_SEPARATOR) || !GetMenuStringW(expected,position,text,512,MF_BYPOSITION) ||
           !GetMenuItemRect(app->MainWindow(),expected,position,&row))continue;
        OffsetRect(&row,-bounds.left,-bounds.top);
        unsigned ink=0,area=0;
        for(int y=std::max(0,int(row.top)+2);y<std::min(height,int(row.bottom)-2);++y)
            for(int x=std::max(0,int(row.left)+30);x<std::min(width,int(row.right)-20);++x) {
                const auto rgb=pixels[y*width+x];
                if(((rgb>>16)&255)+((rgb>>8)&255)+(rgb&255)<600)++ink;
                ++area;
            }
        if(!area)continue;
        ++tested;if(ink<4 || ink>area/2)++blank;
        std::printf("row=%d ink=%u area=%u\n",position,ink,area);
    }
    ++checks;
    const bool passed=tested && (expectText ? !blank : blank==tested);
    if(!passed)++failures;
    std::printf("first_paint expected_text=%u rows=%u blank=%u %s\n",expectText,tested,blank,
        passed?"PASS":"FAIL");
    wchar_t directory[1024]{};
    if(GetEnvironmentVariableW(L"MMS_MENU_PAINT_DIR",directory,1024)) {
        wchar_t path[1200]{};swprintf_s(path,L"%ls\\popup-%02u.bmp",directory,checks);
        FILE* file=nullptr;
        if(!_wfopen_s(&file,path,L"wb")) {
            BITMAPFILEHEADER header{};header.bfType=0x4d42;
            header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);
            header.bfSize=header.bfOffBits+width*height*4;
            fwrite(&header,sizeof(header),1,file);fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,file);
            fwrite(pixels,4,std::size_t(width)*height,file);fclose(file);
        }
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(popup,source);
}
inline void Blank(HWND window) {
    RECT all{};GetWindowRect(window,&all);OffsetRect(&all,-all.left,-all.top);
    HDC dc=GetWindowDC(window);
    FillRect(dc,&all,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    GdiFlush();ReleaseDC(window,dc);
}
inline void FinishSample(HWND hwnd) {
    KillTimer(hwnd,2);Capture();
    // The bounded opening paints must finish. Later idle messages must not
    // repaint an already completed popup indefinitely.
    Blank(popup);
    for(int repeat=0;repeat<3;++repeat)
        if(editorMode)SendMessageW(hwnd,WM_ENTERIDLE,MSGF_MENU,reinterpret_cast<LPARAM>(popup));
        else MainWndProc(hwnd,WM_ENTERIDLE,MSGF_MENU,reinterpret_cast<LPARAM>(popup));
    ++idleChecks;Capture(false);EndMenu();
}
inline LRESULT CALLBACK Owner(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_INITMENUPOPUP && slowInitMode){const auto result=MainWndProc(hwnd,message,wp,lp);Sleep(280);return result;}
    if(message==WM_TIMER && wp==2){FinishSample(hwnd);return 0;}
    if(message==WM_TIMER && wp==3){KillTimer(hwnd,3);Blank(popup);Capture(false);return 0;}
    if(message==WM_TIMER && wp==1){++failures;std::puts("FAIL popup sample timed out");EndMenu();return 0;}
    if(message==WM_ENTERIDLE && wp==MSGF_MENU) {
        if(editorMode) {
            if(firstIdle) {
                firstIdle=false;Blank(reinterpret_cast<HWND>(lp));Capture(false);
                SetTimer(hwnd,3,100,nullptr);SetTimer(hwnd,2,360,nullptr);
            }
            return DefWindowProcW(hwnd,message,wp,lp);
        }
        if(noIdleMode) {
            if(firstIdle){firstIdle=false;Blank(reinterpret_cast<HWND>(lp));Capture(false);SetTimer(hwnd,2,360,nullptr);}
            return 0;
        }
        if(firstIdle) {
            firstIdle=false;
            if(injectBlank)Blank(reinterpret_cast<HWND>(lp));
            const auto result=MainWndProc(hwnd,message,wp,lp);
            std::printf("injected_blank=%u delayed=%u\n",injectBlank,delayedBlankMode);Capture();
            if(delayedBlankMode){SetTimer(hwnd,3,100,nullptr);SetTimer(hwnd,2,360,nullptr);return result;}
            Blank(reinterpret_cast<HWND>(lp));
            for(int repeat=0;repeat<3;++repeat)MainWndProc(hwnd,message,wp,lp);
            ++idleChecks;Capture(false);EndMenu();return result;
        }
    }
    switch(message) {
    case WM_TIMER:case WM_INITMENU:case WM_INITMENUPOPUP:case WM_UNINITMENUPOPUP:
    case WM_ENTERMENULOOP:case WM_EXITMENULOOP:case WM_MENUSELECT:case WM_ENTERIDLE:
        return editorMode ? DefWindowProcW(hwnd,message,wp,lp) : MainWndProc(hwnd,message,wp,lp);
    default:return DefWindowProcW(hwnd,message,wp,lp);
    }
}
inline int Run(bool delayed=false,bool noIdle=false,bool slow=false,bool editor=false) {
    delayedBlankMode=delayed;noIdleMode=noIdle;slowInitMode=slow;
    editorMode=editor;
    std::setvbuf(stdout,nullptr,_IONBF,0);SetProcessDPIAware();
    const auto previous=GetThreadDesktop(GetCurrentThreadId());
    wchar_t desktopName[80]{};swprintf_s(desktopName,L"MMS-first-menu-%lu",GetCurrentProcessId());
    auto desktop=CreateDesktopW(desktopName,nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop || !SetThreadDesktop(desktop)){std::puts("FAIL private desktop");return 1;}
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"MMS-first-menu";
    wc.lpfnWndProc=Owner;wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&wc);
    auto storage=std::make_unique<MMDApp>();app=storage.get();app->state=MMDAppState{};
    g_Block=app;app->EnglishUI()=2;app->CameraMode()=1;
    GetCurrentDirectoryW(MAX_PATH,app->ExeDir());
    HWND owner=CreateWindowW(wc.lpszClassName,L"Menu first paint",WS_OVERLAPPEDWINDOW,
        0,0,1200,800,nullptr,nullptr,wc.hInstance,nullptr);
    app->Hwnd()=owner;
    if(editorMode){InstallEffectListDisplay(owner,app);InstallEffectListDisplay(owner,app);}
    HMENU bar=LoadMenuW(wc.hInstance,L"SAMPLE02E");SetMenu(owner,bar);
    HMODULE mmeResources=nullptr;
    wchar_t mmePath[32768]{};
    if(editorMode && GetEnvironmentVariableW(L"MMS_MENU_MME_DLL",mmePath,32768)) {
        mmeResources=LoadLibraryExW(mmePath,nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        auto mapping=mmeResources ? LoadMenuW(mmeResources,MAKEINTRESOURCEW(110)) : nullptr;
        if(!mapping && mmeResources)mapping=LoadMenuW(mmeResources,MAKEINTRESOURCEW(50110));
        if(!mapping){std::puts("FAIL original MME menu resource");return 1;}
        SetMenu(owner,mapping);DestroyMenu(bar);bar=mapping;
    } else {RefreshMenuLanguage(app);curve_host::CreateControls(app,owner);}
    ShowWindow(owner,SW_SHOWNOACTIVATE);UpdateWindow(owner);
    for(bool blank:{false,true})for(int position=0;position<GetMenuItemCount(bar);++position) {
        expected=GetSubMenu(bar,position);if(!expected)continue;
        injectBlank=blank;firstIdle=true;
        std::printf("opening top=%d inject=%u delayed=%u no_idle=%u\n",position,injectBlank,delayedBlankMode,noIdleMode);
        const auto before=checks;
        SetTimer(owner,1,2000,nullptr);
        // Animation requires a composed active desktop. Keep these pixel
        // assertions on the menu's own GDI backing on the inactive desktop.
        TrackPopupMenuEx(expected,TPM_RETURNCMD|TPM_NOANIMATION,20,80,owner,nullptr);
        KillTimer(owner,1);KillTimer(owner,2);KillTimer(owner,3);
        const unsigned expectedSamples=noIdleMode?3:delayedBlankMode?4:2;
        if(checks!=before+expectedSamples){++failures;std::puts("FAIL initial popup was not sampled");}
    }
    SetMenu(owner,nullptr);DestroyWindow(owner);app->Hwnd()=nullptr;g_Block=nullptr;app=nullptr;
    DestroyMenu(bar);UnregisterClassW(wc.lpszClassName,wc.hInstance);
    if(mmeResources)FreeLibrary(mmeResources);
    SetThreadDesktop(previous);CloseDesktop(desktop);
    std::printf("menu first paint: %u checks, %u completed-opening checks, %u failures\n",checks,idleChecks,failures);
    return failures?1:0;
}
}
