// Segmented presentation of the existing combo selections.
// Keep CB_* and CBN_SELCHANGE routes. Interpolation and handle choices commit
// immediately; handle modes are followed by four non-sticky preset actions.
namespace curve_editor_segments {
struct State { HWND tips=nullptr; int hover=-1, focus=-1; std::vector<std::wstring> labels; };
inline bool Target(int id){return id==1007||id==1009||id==1045||id==1047;}
inline int Gap(HWND hwnd){return GetDlgCtrlID(hwnd)==1047?6:0;}
inline RECT Cell(HWND hwnd,int index) {
    RECT r{};GetClientRect(hwnd,&r);
    int count=int(SendMessageW(hwnd,CB_GETCOUNT,0,0));
    if(count<=0)return r;
    int width=r.right-Gap(hwnd);
    r.left=width*index/count;r.right=width*(index+1)/count;
    if(GetDlgCtrlID(hwnd)==1047&&index>=3){r.left+=Gap(hwnd);r.right+=Gap(hwnd);}
    return r;
}
inline int Hit(HWND hwnd,LPARAM lp) {
    POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
    int count=int(SendMessageW(hwnd,CB_GETCOUNT,0,0));
    for(int i=0;i<count;++i){RECT r=Cell(hwnd,i);if(PtInRect(&r,point))return i;}
    return -1;
}
inline void Icon(HDC dc,int id,int index,float x,float y,COLORREF color) {
    using namespace Gdiplus;
    GdiFlush();Graphics g(dc);g.SetSmoothingMode(SmoothingModeAntiAlias);g.TranslateTransform(x,y);
    Pen p(Color(255,GetRValue(color),GetGValue(color),GetBValue(color)),1.4f);
    p.SetStartCap(LineCapRound);p.SetEndCap(LineCapRound);p.SetLineJoin(LineJoinRound);
    SolidBrush fill(Color(255,GetRValue(color),GetGValue(color),GetBValue(color)));
    auto line=[&](float a,float b,float c,float d){g.DrawLine(&p,a,b,c,d);};
    auto dot=[&](float a,float b){g.FillEllipse(&fill,a-1.5f,b-1.5f,3.f,3.f);};
    if(id==1007){
        line(2,1,2,16);line(2,16,17,16);
        if(index==0)g.DrawBezier(&p,3.f,13.f,8.f,14.f,9.f,3.f,16.f,3.f);
        else {line(4,12,8,5);line(8,5,12,5);line(12,5,16,12);}
    }else if(id==1009){
        if(index==0){PointF points[]={{3,1},{3,15},{7,11},{10,17},{13,15},{10,10},{16,10}};g.DrawPolygon(&p,points,7);}
        else {line(9,1,9,17);line(1,9,17,9);line(9,1,6,4);line(9,1,12,4);line(9,17,6,14);line(9,17,12,14);line(1,9,4,6);line(1,9,4,12);line(17,9,14,6);line(17,9,14,12);}
    }else if(id==1045){
        if(index==0){line(2,15,9,15);line(9,15,9,3);line(9,3,17,3);}
        if(index==1)line(2,15,17,3);
        if(index==2)g.DrawBezier(&p,2.f,15.f,12.f,15.f,7.f,3.f,17.f,3.f);
        dot(2,15);dot(17,3);
    }else {
        const int iconIndex = index==2?4:index==3?2:index==4?3:index;
        switch(iconIndex){
        case 0:line(2,3,9,10);line(9,10,17,6);dot(2,3);dot(9,10);dot(17,6);break;
        case 1:line(2,15,16,3);dot(2,15);dot(9,9);dot(16,3);break;
        case 2:line(1,9,17,9);dot(2,9);dot(9,9);dot(16,9);break;
        case 3:line(1,15,9,5);line(9,5,17,11);dot(1,15);dot(9,5);dot(17,11);break;
        case 4:g.DrawBezier(&p,1.f,15.f,7.f,15.f,11.f,3.f,17.f,3.f);line(5,13,13,5);dot(9,9);break;
        case 5:g.DrawBezier(&p,1.f,15.f,2.f,4.f,6.f,4.f,11.f,4.f);line(11,4,17,4);dot(11,4);break;
        case 6:line(1,14,7,14);g.DrawBezier(&p,7.f,14.f,13.f,14.f,15.f,14.f,17.f,3.f);dot(7,14);break;
        }
    }
}
inline void Tips(HWND hwnd,State& state) {
    if(!state.tips)state.tips=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,
        WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,
        hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    for(std::size_t i=0;i<state.labels.size();++i){TOOLINFOW t{sizeof(t)};t.hwnd=hwnd;t.uId=i+1;SendMessageW(state.tips,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&t));}
    int count=int(SendMessageW(hwnd,CB_GETCOUNT,0,0));state.labels.clear();
    for(int i=0;i<count;++i){
        wchar_t label[256]{};SendMessageW(hwnd,CB_GETLBTEXT,i,reinterpret_cast<LPARAM>(label));
        std::wstring description=label;
        if(GetDlgCtrlID(hwnd)==1047){
            const auto language=reinterpret_cast<ULONG_PTR>(GetPropW(hwnd,L"CurveLanguage"));
            const wchar_t* zh[]={L"手柄模式：独立调整两侧。点击立即应用。",L"手柄模式：调整一侧时，两侧保持同一斜率。点击立即应用。",L"手柄模式：根据相邻关键帧自动计算斜率。点击立即应用。",L"预设：将两侧手柄设为水平，只应用一次。",L"预设：将前后两个区间拉直，只应用一次。",L"预设：进入侧水平弯曲，离开侧拉直，只应用一次。",L"预设：离开侧水平弯曲，进入侧拉直，只应用一次。"};
            const wchar_t* en[]={L"Mode: edit each side independently. Applies immediately.",L"Mode: editing either side keeps both slopes aligned. Applies immediately.",L"Mode: calculate slopes from neighbouring keys. Applies immediately.",L"Preset: flatten both handles once.",L"Preset: straighten both adjacent intervals once.",L"Preset: flatten the incoming side and straighten the outgoing side once.",L"Preset: flatten the outgoing side and straighten the incoming side once."};
            const wchar_t* ja[]={L"モード：両側を独立して編集します。即時適用。",L"モード：片側を編集すると両側の傾きを揃えます。即時適用。",L"モード：隣接キーから傾きを自動計算します。即時適用。",L"プリセット：両側のハンドルを一度だけ水平にします。",L"プリセット：前後の区間を一度だけ直線化します。",L"プリセット：入る側を水平に曲げ、出る側を直線化します。",L"プリセット：出る側を水平に曲げ、入る側を直線化します。"};
            if(i<7)description=language==2?zh[i]:language==1?en[i]:ja[i];
        }
        state.labels.push_back(std::move(description));
    }
    for(int i=0;i<count;++i){
        TOOLINFOW t{sizeof(t)};t.hwnd=hwnd;t.uId=i+1;t.uFlags=TTF_SUBCLASS;t.rect=Cell(hwnd,i);
        t.lpszText=state.labels[i].data();SendMessageW(state.tips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&t));
    }
    SendMessageW(state.tips,TTM_SETDELAYTIME,TTDT_INITIAL,350);
    SendMessageW(state.tips,TTM_SETMAXTIPWIDTH,0,400);
}
inline void Paint(HWND hwnd,HDC dc,State& state) {
    using namespace curve_editor_theme;
    RECT all{};GetClientRect(hwnd,&all);Fill(dc,all,shell);
    int count=int(SendMessageW(hwnd,CB_GETCOUNT,0,0));
    int selected=int(SendMessageW(hwnd,CB_GETCURSEL,0,0));
    int id=GetDlgCtrlID(hwnd),saved=SaveDC(dc);
    HFONT font=reinterpret_cast<HFONT>(SendMessageW(hwnd,WM_GETFONT,0,0));if(font)SelectObject(dc,font);
    SetBkMode(dc,TRANSPARENT);
    bool enabled=IsWindowEnabled(hwnd)!=FALSE;
    for(int i=0;i<count;++i){
        RECT r=Cell(hwnd,i);bool active=i==selected && (id!=1047 || i<3);
        Fill(dc,r,active?selection:(i==state.hover?Mix(control,paper,0.6):paper));
        Frame(dc,r,active?accent:border);
        if(active){RECT line{r.left+1,r.bottom-3,r.right-1,r.bottom-1};Fill(dc,line,accent);}
        wchar_t label[256]{};SendMessageW(hwnd,CB_GETLBTEXT,i,reinterpret_cast<LPARAM>(label));
        SIZE extent{};GetTextExtentPoint32W(dc,label,int(wcslen(label)),&extent);
        const int iconSize=18,gap=3;
        bool showLabel=r.right-r.left>=iconSize+gap+extent.cx+4;
        int content=showLabel?iconSize+gap+extent.cx:iconSize;
        int x=r.left+(r.right-r.left-content)/2,y=r.top+(r.bottom-r.top-iconSize)/2;
        COLORREF ink=enabled?text:muted;
        Icon(dc,id,i,float(x),float(y),ink);
        if(showLabel){RECT textRect{x+iconSize+gap,r.top,r.right-2,r.bottom-1};SetTextColor(dc,ink);DrawTextW(dc,label,-1,&textRect,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);}
        if(GetFocus()==hwnd&&i==(state.focus<0?selected:state.focus)){RECT focus=r;InflateRect(&focus,-4,-4);DrawFocusRect(dc,&focus);}
    }
    RestoreDC(dc,saved);
}
inline void Choose(HWND hwnd,int index) {
    if(!IsWindowEnabled(hwnd))return;
    int count=int(SendMessageW(hwnd,CB_GETCOUNT,0,0));if(index<0||index>=count)return;
    SendMessageW(hwnd,CB_SETCURSEL,index,0);InvalidateRect(hwnd,nullptr,FALSE);
    SendMessageW(GetParent(hwnd),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(hwnd),CBN_SELCHANGE),reinterpret_cast<LPARAM>(hwnd));
}
inline LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
    auto& state=*reinterpret_cast<State*>(data);
    if(msg==WM_NCDESTROY){if(state.tips)DestroyWindow(state.tips);RemoveWindowSubclass(hwnd,Proc,id);delete &state;return DefSubclassProc(hwnd,msg,wp,lp);}
    if(msg==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);Paint(hwnd,dc,state);EndPaint(hwnd,&ps);return 0;}
    if(msg==WM_PRINTCLIENT){Paint(hwnd,reinterpret_cast<HDC>(wp),state);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_LBUTTONDOWN){SetFocus(hwnd);state.focus=Hit(hwnd,lp);Choose(hwnd,state.focus);return 0;}
    if(msg==WM_LBUTTONUP||msg==WM_LBUTTONDBLCLK)return 0;
    // Activation is handled on key-down; do not let the native combo perform
    // a second action when TranslateMessage delivers its character message.
    if(msg==WM_CHAR&&(wp==L'\r'||wp==L' '))return 0;
    if(msg==CB_SHOWDROPDOWN)return 0;
    if(msg==WM_MOUSEMOVE){int hover=Hit(hwnd,lp);if(hover!=state.hover){state.hover=hover;InvalidateRect(hwnd,nullptr,FALSE);}TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hwnd,0};TrackMouseEvent(&t);return 0;}
    if(msg==WM_MOUSELEAVE){state.hover=-1;InvalidateRect(hwnd,nullptr,FALSE);return 0;}
    if(msg==WM_KEYDOWN){
        int current=state.focus<0?int(SendMessageW(hwnd,CB_GETCURSEL,0,0)):state.focus;
        int count=int(SendMessageW(hwnd,CB_GETCOUNT,0,0));
        if(wp==VK_LEFT||wp==VK_UP)state.focus=std::max(0,current-1);
        else if(wp==VK_RIGHT||wp==VK_DOWN)state.focus=std::min(count-1,current+1);
        else if(wp==VK_HOME)state.focus=0;
        else if(wp==VK_END)state.focus=count-1;
        else if(wp==VK_SPACE||wp==VK_RETURN||wp==VK_F4)state.focus=current;
        else return DefSubclassProc(hwnd,msg,wp,lp);
        Choose(hwnd,state.focus);return 0;
    }
    if(msg==WM_SYSKEYDOWN&&wp==VK_DOWN)return 0;
    LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
    if(msg==WM_SIZE||msg==CB_ADDSTRING||msg==CB_RESETCONTENT)Tips(hwnd,state);
    if(msg==CB_SETCURSEL||msg==WM_ENABLE||msg==WM_SETFOCUS||msg==WM_KILLFOCUS)InvalidateRect(hwnd,nullptr,FALSE);
    return result;
}
inline void Apply(HWND root) {
    for(int id:{1007,1009,1045,1047}){
        HWND hwnd=GetDlgItem(root,id);if(!hwnd)continue;
        RemoveWindowSubclass(hwnd,curve_editor_theme::ControlProc,0x434f4c52);
        SetWindowSubclass(hwnd,Proc,0x5345474d,reinterpret_cast<DWORD_PTR>(new State));
    }
}
}
