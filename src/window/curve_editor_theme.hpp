// The host supplies its loaded color.txt palette through Session. No second
// file read or current-directory dependency is introduced by the editor.
#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")

namespace curve_editor_theme {
inline COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB(int(GetRValue(a)*(1-t)+GetRValue(b)*t),
               int(GetGValue(a)*(1-t)+GetGValue(b)*t),
               int(GetBValue(a)*(1-t)+GetBValue(b)*t));
}
inline COLORREF shell=RGB(247,248,250), text=RGB(51,60,73), paper=RGB(255,255,255);
inline COLORREF control=RGB(241,244,248), border=RGB(171,180,194), muted=RGB(108,116,130);
inline COLORREF ruler=RGB(246,247,250), grid=RGB(234,237,242), zero=RGB(203,210,220);
inline COLORREF selection=RGB(219,230,243), accent=RGB(61,132,219), accentText=RGB(255,255,255);
inline COLORREF arm=RGB(175,182,193), track=RGB(240,243,247), grip=RGB(125,154,192);
inline COLORREF error=RGB(168,0,0);
inline bool loaded=false;
inline HBRUSH paperBrush=nullptr;

inline void Load(const mikumikustudio::curve_editor::Theme& theme) {
    if(!theme.enabled)return;
    const auto& colors=theme.colors;text=theme.text;loaded=true;
    shell=Mix(colors[0],colors[1],0.5);
    control=Mix(colors[2],colors[3],0.5);
    paper=colors[22];ruler=Mix(colors[27],colors[28],0.5);
    // Existing MMD row meanings: [30] current-frame line, [33] timeline text,
    // [34] selected timeline text. The enum names alone are misleading here.
    grid=Mix(paper,colors[31],0.32);zero=colors[31];
    border=Mix(control,text,0.32);muted=Mix(shell,text,0.66);
    selection=Mix(colors[25],colors[26],0.5);accent=colors[30];
    accentText=(GetRValue(accent)*299+GetGValue(accent)*587+GetBValue(accent)*114)>145000
        ?RGB(20,24,30):RGB(255,255,255);
    arm=Mix(paper,text,0.55);track=colors[23];grip=Mix(control,text,0.50);
    error=Mix(shell,RGB(255,100,110),0.9);
    if(paperBrush)DeleteObject(paperBrush);
    paperBrush=CreateSolidBrush(paper);
}
inline void Fill(HDC dc,const RECT& r,COLORREF c) {
    HBRUSH b=CreateSolidBrush(c);FillRect(dc,&r,b);DeleteObject(b);
}
inline void Frame(HDC dc,const RECT& r,COLORREF c) {
    HBRUSH b=CreateSolidBrush(c);FrameRect(dc,&r,b);DeleteObject(b);
}
inline COLORREF Curve(COLORREF color) {
    const bool dark=GetRValue(paper)+GetGValue(paper)+GetBValue(paper)<384;
    return dark?Mix(color,RGB(255,255,255),0.20):color;
}
inline void PaintEditFrame(HWND hwnd,HDC dc) {
    RECT r{};GetWindowRect(hwnd,&r);OffsetRect(&r,-r.left,-r.top);
    Frame(dc,r,GetFocus()==hwnd?accent:border);InflateRect(&r,-1,-1);Frame(dc,r,paper);
}
inline LRESULT CALLBACK EditProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,EditProc,id);
    const LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
    if(msg==WM_NCPAINT){HDC dc=GetWindowDC(hwnd);PaintEditFrame(hwnd,dc);ReleaseDC(hwnd,dc);}
    if(msg==WM_PRINT&&(lp&PRF_NONCLIENT))PaintEditFrame(hwnd,reinterpret_cast<HDC>(wp));
    return result;
}

// Keep standard Win32 input/command processing. Only client painting changes.
inline void PaintControl(HWND hwnd,HDC dc,bool combo) {
    RECT r{};GetClientRect(hwnd,&r);int saved=SaveDC(dc);
    HFONT font=reinterpret_cast<HFONT>(SendMessageW(hwnd,WM_GETFONT,0,0));
    if(font)SelectObject(dc,font);
    bool enabled=IsWindowEnabled(hwnd)!=FALSE;
    LONG style=GetWindowLongW(hwnd,GWL_STYLE);
    LRESULT state=combo?0:SendMessageW(hwnd,BM_GETSTATE,0,0);
    bool checked=!combo&&SendMessageW(hwnd,BM_GETCHECK,0,0)==BST_CHECKED;
    bool checkbox=!combo&&(style&BS_TYPEMASK)==BS_AUTOCHECKBOX&&!(style&BS_PUSHLIKE);
    Fill(dc,r,checkbox?shell:(combo?paper:((state&BST_PUSHED)||checked?selection:control)));
    if(!checkbox)Frame(dc,r,GetFocus()==hwnd?accent:border);
    RECT label=r;label.left+=6;label.right-=6;
    const COLORREF foreground=enabled?text:muted;
    if(checkbox) {
        RECT box{r.left+2,(r.bottom-12)/2,r.left+14,(r.bottom+12)/2};
        Fill(dc,box,paper);Frame(dc,box,border);
        if(checked) {
            HPEN p=CreatePen(PS_SOLID,2,accent);auto old=SelectObject(dc,p);
            MoveToEx(dc,box.left+2,box.top+6,nullptr);LineTo(dc,box.left+5,box.bottom-2);LineTo(dc,box.right-2,box.top+2);
            SelectObject(dc,old);DeleteObject(p);
        }
        label.left=16;label.right=r.right;
    }
    if(combo) {
        int x=r.right-12,y=r.bottom/2;
        POINT points[]={{x-3,y-2},{x+3,y-2},{x,y+2}};
        HBRUSH b=CreateSolidBrush(foreground);auto old=SelectObject(dc,b);
        auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));Polygon(dc,points,3);
        SelectObject(dc,oldPen);SelectObject(dc,old);DeleteObject(b);label.right-=18;
    }
    wchar_t caption[256]{};GetWindowTextW(hwnd,caption,256);
    if(combo) {
        int item=int(SendMessageW(hwnd,CB_GETCURSEL,0,0));
        LRESULT length=SendMessageW(hwnd,CB_GETLBTEXTLEN,item,0);
        if(item>=0&&length>=0&&length<256)SendMessageW(hwnd,CB_GETLBTEXT,item,reinterpret_cast<LPARAM>(caption));
    }
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,foreground);
    const int controlId=GetDlgCtrlID(hwnd);
    if(controlId==1022||controlId==1023||controlId==1026||controlId==1027){
        const bool previous=controlId==1022||controlId==1026,key=controlId==1026||controlId==1027;
        const int scale=std::max(1,int(r.bottom-r.top)/26),half=5*scale;
        int cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;
        HPEN pen=CreatePen(PS_SOLID,2*scale,foreground);auto old=SelectObject(dc,pen);
        if(key){
            const int bar=cx+(previous?-6:6)*scale;
            MoveToEx(dc,bar,cy-half-1,nullptr);LineTo(dc,bar,cy+half+2);
            cx+=(previous?2:-2)*scale;
        }
        const int dx=(previous?1:-1)*3*scale;
        POINT arrow[]={{cx+dx,cy-half},{cx-dx,cy},{cx+dx,cy+half}};
        Polyline(dc,arrow,3);SelectObject(dc,old);DeleteObject(pen);
    }else if(controlId==1058 && wcslen(caption)==2){
        TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);
        const int line=metrics.tmHeight,top=(r.bottom-line*2)/2;
        RECT upper{0,top,r.right,top+line},lower{0,top+line,r.right,top+line*2};
        DrawTextW(dc,caption,1,&upper,DT_CENTER|DT_SINGLELINE|DT_NOPREFIX);
        DrawTextW(dc,caption+1,1,&lower,DT_CENTER|DT_SINGLELINE|DT_NOPREFIX);
    }else{
        DrawTextW(dc,caption,-1,&label,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX|
            (combo||checkbox?DT_LEFT:DT_CENTER));
    }
    if(GetFocus()==hwnd){RECT focus=r;InflateRect(&focus,-3,-3);DrawFocusRect(dc,&focus);}
    RestoreDC(dc,saved);
}
inline LRESULT CALLBACK ControlProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR combo) {
    if(msg==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);PaintControl(hwnd,dc,combo!=0);EndPaint(hwnd,&ps);return 0;}
    if(msg==WM_PRINTCLIENT){PaintControl(hwnd,reinterpret_cast<HDC>(wp),combo!=0);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,ControlProc,id);
    LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
    if(msg==WM_ENABLE||msg==WM_SETFOCUS||msg==WM_KILLFOCUS||msg==BM_SETSTATE||msg==BM_SETCHECK||msg==CB_SETCURSEL)
        InvalidateRect(hwnd,nullptr,FALSE);
    return result;
}
inline BOOL CALLBACK ApplyControl(HWND hwnd,LPARAM) {
    wchar_t name[64]{};GetClassNameW(hwnd,name,64);
    if(wcscmp(name,L"Button")==0||wcscmp(name,L"ComboBox")==0) {
        SetWindowTheme(hwnd,L"",L"");
        SetWindowSubclass(hwnd,ControlProc,0x434f4c52,wcscmp(name,L"ComboBox")==0);
    }
    if(wcscmp(name,L"Edit")==0)SetWindowSubclass(hwnd,EditProc,0x434f4c45,0);
    if(wcscmp(name,L"SysTreeView32")==0)SetWindowTheme(hwnd,L"",L"");
    return TRUE;
}
inline void Apply(HWND root) {if(loaded)EnumChildWindows(root,ApplyControl,0);}
inline void Release(){if(paperBrush){DeleteObject(paperBrush);paperBrush=nullptr;}}
}
