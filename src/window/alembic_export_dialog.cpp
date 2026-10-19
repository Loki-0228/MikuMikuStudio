#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/ui_language.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <CommDlg.h>
#include "mikumikustudio/alembic_export.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/channel_bake.hpp"
#include <set>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

namespace mikumikustudio::alembic_export {
namespace {
enum {First=120,Last,Rate,Selected,Bar,Status,WriteAbc,BakeProject,ClearBake,Objects,Materials,Vertices,BoneVertices,Layers};
struct Row { Selection selection;std::wstring materials,vertices; };
struct Placement {HWND window;RECT rect;};
struct Context {MMDApp* app;bool running=false,cancel=false,initializing=false,bakeDefault=false;int row=-1;std::vector<Row> rows;std::vector<std::uint32_t> layers;
    std::vector<Placement> body;int scrollX=0,scrollY=0,bodyWidth=0,bodyHeight=0,viewportHeight=0;bool layoutReady=false,layingOut=false;};
void Layout(HWND d,Context& c) {
    if(!c.layoutReady||c.layingOut)return;
    c.layingOut=true;
    RECT units{0,0,12,13};MapDialogRect(d,&units);
    const int margin=units.right,bottom=units.bottom,gap=std::max(4,margin/2);
    RECT clear{},run{},close{};
    GetWindowRect(GetDlgItem(d,ClearBake),&clear);GetWindowRect(GetDlgItem(d,IDOK),&run);GetWindowRect(GetDlgItem(d,IDCANCEL),&close);
    const int cw=clear.right-clear.left,rw=run.right-run.left,bw=close.right-close.left,h=run.bottom-run.top;
    RECT client{};bool stacked=false;int buttonTop=0;
    // Scrollbars reduce the client area; settle their visibility before placing controls.
    for(int pass=0;pass<3;++pass){
        GetClientRect(d,&client);
        stacked=client.right<cw+rw+bw+margin*2+gap*2;
        buttonTop=client.bottom-bottom-h;
        c.viewportHeight=std::max(1,buttonTop-gap-(stacked?h+gap:0));
        ShowScrollBar(d,SB_HORZ,c.bodyWidth>client.right);
        ShowScrollBar(d,SB_VERT,c.bodyHeight>c.viewportHeight);
    }
    GetClientRect(d,&client);
    c.scrollX=std::clamp(c.scrollX,0,std::max(0,c.bodyWidth-int(client.right)));
    c.scrollY=std::clamp(c.scrollY,0,std::max(0,c.bodyHeight-c.viewportHeight));
    SCROLLINFO horizontal{sizeof(SCROLLINFO),SIF_RANGE|SIF_PAGE|SIF_POS,0,c.bodyWidth-1,UINT(client.right),c.scrollX};
    SCROLLINFO vertical{sizeof(SCROLLINFO),SIF_RANGE|SIF_PAGE|SIF_POS,0,c.bodyHeight-1,UINT(c.viewportHeight),c.scrollY};
    SetScrollInfo(d,SB_HORZ,&horizontal,TRUE);SetScrollInfo(d,SB_VERT,&vertical,TRUE);
    MoveWindow(GetDlgItem(d,ClearBake),margin,buttonTop-(stacked?h+gap:0),cw,h,TRUE);
    MoveWindow(GetDlgItem(d,IDOK),client.right-margin-bw-gap-rw,buttonTop,rw,h,TRUE);
    MoveWindow(GetDlgItem(d,IDCANCEL),client.right-margin-bw,buttonTop,bw,h,TRUE);
    for(const auto& item:c.body){
        const auto& r=item.rect;const int x=r.left-c.scrollX,y=r.top-c.scrollY,w=r.right-r.left,height=r.bottom-r.top;
        MoveWindow(item.window,x,y,w,height,FALSE);
        // Keep direct dialog children (and their IDs), but clip them above the fixed buttons.
        const int left=std::clamp(-x,0,w),top=std::clamp(-y,0,height);
        const int right=std::clamp(int(client.right)-x,left,w),end=std::clamp(c.viewportHeight-y,top,height);
        HRGN region=CreateRectRgn(left,top,right,end);
        if(!SetWindowRgn(item.window,region,FALSE))DeleteObject(region);
    }
    RedrawWindow(d,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    c.layingOut=false;
}
LRESULT CALLBACK BodyFocus(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
    auto& c=*reinterpret_cast<Context*>(data);
    if(message==WM_SETFOCUS){
        const HWND d=GetParent(window);RECT client{};GetClientRect(d,&client);
        for(const auto& item:c.body)if(item.window==window){
            const auto& r=item.rect;
            if(r.left<c.scrollX)c.scrollX=r.left;else if(r.right>c.scrollX+client.right)c.scrollX=r.right-client.right;
            if(r.top<c.scrollY)c.scrollY=r.top;else if(r.bottom>c.scrollY+c.viewportHeight)c.scrollY=r.bottom-c.viewportHeight;
            Layout(d,c);break;
        }
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,BodyFocus,id);
    return DefSubclassProc(window,message,wp,lp);
}
void FitToWorkArea(HWND d,Context& c){
    RECT client{};GetClientRect(d,&client);c.bodyWidth=client.right;
    for(HWND child=GetWindow(d,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        const int id=GetDlgCtrlID(child);if(id==IDOK||id==IDCANCEL||id==ClearBake)continue;
        RECT r{};GetWindowRect(child,&r);MapWindowPoints(nullptr,d,reinterpret_cast<POINT*>(&r),2);
        c.body.push_back({child,r});c.bodyHeight=std::max(c.bodyHeight,int(r.bottom));
        SetWindowSubclass(child,BodyFocus,1,reinterpret_cast<DWORD_PTR>(&c));
    }
    MONITORINFO monitor{sizeof monitor};
    if(GetMonitorInfoW(MonitorFromWindow(d,MONITOR_DEFAULTTONEAREST),&monitor)){
        RECT window{};GetWindowRect(d,&window);const auto& work=monitor.rcWork;
        const int width=std::min(int(window.right-window.left),int(work.right-work.left)-16);
        const int height=std::min(int(window.bottom-window.top),int(work.bottom-work.top)-16);
        const int x=std::clamp(int(window.left),int(work.left)+8,int(work.right)-width-8);
        const int y=std::clamp(int(window.top),int(work.top)+8,int(work.bottom)-height-8);
        SetWindowPos(d,nullptr,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    c.layoutReady=true;Layout(d,c);
}
const wchar_t* Text(const Context& c,const wchar_t* zh,const wchar_t* en,const wchar_t* ja) {
    return c.app->EnglishUI()==2?zh:c.app->EnglishUI()==1?en:ja;
}
HWND Control(HWND d,const wchar_t* kind,const wchar_t* caption,DWORD style,int id,int x,int y,int w,int h) {
    RECT r{x,y,x+w,y+h};MapDialogRect(d,&r);
    auto child=CreateWindowExW(!wcscmp(kind,L"EDIT")?WS_EX_CLIENTEDGE:0,kind,caption,WS_CHILD|WS_VISIBLE|style,
        r.left,r.top,r.right-r.left,r.bottom-r.top,d,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(child,WM_SETFONT,SendMessageW(d,WM_GETFONT,0,0),TRUE);return child;
}
bool Integer(HWND d,int id,int& number) {
    wchar_t value[64]{};GetDlgItemTextW(d,id,value,64);wchar_t* end=nullptr;const long parsed=wcstol(value,&end,10);
    if(end==value||*end||parsed<0||parsed>frame_limits::maximum)return false;number=int(parsed);return true;
}
std::wstring Wide(const std::string& input) {
    const int n=MultiByteToWideChar(CP_UTF8,0,input.c_str(),-1,nullptr,0);if(n<=0)return L"";
    std::wstring out(n,L'\0');MultiByteToWideChar(CP_UTF8,0,input.c_str(),-1,out.data(),n);out.resize(n-1);return out;
}
std::wstring EditText(HWND d,int id){const auto w=GetDlgItem(d,id);const int n=GetWindowTextLengthW(w);std::wstring s(size_t(n)+1,L'\0');GetWindowTextW(w,s.data(),n+1);s.resize(n);return s;}
void StoreRow(HWND d,Context& c){if(c.row<0||size_t(c.row)>=c.rows.size())return;auto& r=c.rows[c.row];r.materials=EditText(d,Materials);r.vertices=EditText(d,Vertices);r.selection.selectedBoneVertices=IsDlgButtonChecked(d,BoneVertices)==BST_CHECKED;}
void SelectRow(HWND d,Context& c,int row){StoreRow(d,c);c.row=row;if(row<0||size_t(row)>=c.rows.size())return;const auto& r=c.rows[row];SetDlgItemTextW(d,Materials,r.materials.c_str());SetDlgItemTextW(d,Vertices,r.vertices.c_str());CheckDlgButton(d,BoneVertices,r.selection.selectedBoneVertices?BST_CHECKED:BST_UNCHECKED);EnableWindow(GetDlgItem(d,BoneVertices),!r.selection.accessory);}
bool Indices(const std::wstring& text,std::vector<uint32_t>& out){
    std::set<uint32_t> values;const wchar_t* p=text.c_str();
    while(*p){while(iswspace(*p)||*p==L',')++p;if(!*p)break;
        if(!iswdigit(*p))return false;wchar_t* end=nullptr;const unsigned long first=wcstoul(p,&end,10);p=end;while(iswspace(*p))++p;unsigned long last=first;
        if(*p==L'-'){++p;while(iswspace(*p))++p;if(!iswdigit(*p))return false;last=wcstoul(p,&end,10);p=end;}
        if(last<first||last>16000000||last-first>16000000)return false;
        for(uint32_t i=uint32_t(first);i<=last;++i)values.insert(i);
        while(iswspace(*p))++p;if(*p&&*p!=L',')return false;
    }
    out.assign(values.begin(),values.end());return true;
}
void Run(HWND d,Context& c) {
    Options options;
    if(!Integer(d,First,options.firstFrame)||!Integer(d,Last,options.lastFrame)||options.lastFrame<options.firstFrame||
       !Integer(d,Rate,options.framesPerSecond)||options.framesPerSecond<1||options.framesPerSecond>120) {
        MessageBoxW(d,Text(c,L"请输入有效帧范围和 1–120 之间的整数帧率。",L"Enter a valid frame range and an integer frame rate from 1 to 120.",L"有効なフレーム範囲と 1～120 の整数フレームレートを入力してください。"),
            Text(c,L"导出 ABC",L"Export ABC",L"ABC 書き出し"),MB_OK|MB_ICONWARNING);return;
    }
    StoreRow(d,c);options.explicitSelection=true;
    for(size_t i=0;i<c.rows.size();++i)if(ListView_GetCheckState(GetDlgItem(d,Objects),int(i))){
        auto selection=c.rows[i].selection;
        if(!Indices(c.rows[i].materials,selection.materials)||!Indices(c.rows[i].vertices,selection.vertices)){
            SetDlgItemTextW(d,Status,Text(c,L"编号格式：0,2,5-10；留空表示全部。",L"Use indices such as 0,2,5-10; leave blank for all.",L"番号は 0,2,5-10。空欄はすべてです。"));return;
        }
        options.selection.push_back(std::move(selection));
    }
    if(options.selection.empty()){SetDlgItemTextW(d,Status,Text(c,L"请勾选至少一个输出对象。",L"Select at least one output object.",L"出力対象を選択してください。"));return;}
    options.selectedOnly=false; // The explicit checkbox list is the authoritative output set.
    options.writeAbc=IsDlgButtonChecked(d,WriteAbc)==BST_CHECKED;
    options.bakeIntoProject=IsDlgButtonChecked(d,BakeProject)==BST_CHECKED;
    options.mmvLayerCutoff=-1;
    if(!options.writeAbc&&!options.bakeIntoProject){
        SetDlgItemTextW(d,Status,Text(c,L"请至少选择一种输出方式。",L"Choose at least one output.",L"出力先を選択してください。"));return;
    }
    wchar_t file[32768]=L"scene.abc";OPENFILENAMEW dialog{sizeof dialog};dialog.hwndOwner=d;
    dialog.lpstrFilter=L"Alembic (*.abc)\0*.abc\0\0";dialog.lpstrFile=file;dialog.nMaxFile=32768;dialog.lpstrDefExt=L"abc";
    dialog.Flags=OFN_EXPLORER|OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(options.writeAbc&&!GetUiSaveFileNameW(&dialog))return;
    c.running=true;c.cancel=false;
    EnableWindow(GetDlgItem(d,Layers),FALSE);
    for(int id:std::initializer_list<int>{First,Last,Rate,Selected,WriteAbc,BakeProject,ClearBake,IDOK,Objects,Materials,Vertices,BoneVertices})EnableWindow(GetDlgItem(d,id),FALSE);
    SetDlgItemTextW(d,IDCANCEL,Text(c,L"取消导出",L"Cancel export",L"書き出し中止"));
    SendDlgItemMessageW(d,Bar,PBM_SETRANGE32,0,options.lastFrame+1);
    Result result;
    try {
    bake::ScopedUiLock lock(d);
    options.preview=true;
    result=Export(*c.app,file,options,[&](int frame,int last) {
        const auto message=std::wstring(frame<options.firstFrame?Text(c,L"预计算：",L"Preroll: ",L"事前計算："):Text(c,L"正在导出：",L"Exporting: ",L"書き出し中："))+
            std::to_wstring(frame)+L" / "+std::to_wstring(last);
        SetDlgItemTextW(d,Status,message.c_str());SendDlgItemMessageW(d,Bar,PBM_SETPOS,frame+1,0);
        MSG msg{};while(PeekMessageW(&msg,d,0,0,PM_REMOVE)){if(!IsDialogMessageW(d,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
        return !c.cancel;
    });
    } catch(const std::exception& e) {result.error=e.what();}
    catch(...) {result.error="Cannot lock the editing windows for baking";}
    c.running=false;
    EnableWindow(GetDlgItem(d,Layers),c.layers.empty()?FALSE:TRUE);
    for(int id:std::initializer_list<int>{First,Last,Rate,WriteAbc,BakeProject,IDOK,Objects,Materials,Vertices,BoneVertices})EnableWindow(GetDlgItem(d,id),TRUE);
    EnableWindow(GetDlgItem(d,Selected),c.app->SelectedModel()!=nullptr);
    EnableWindow(GetDlgItem(d,ClearBake),!mesh_bake_cache::Empty());
    SetDlgItemTextW(d,IDCANCEL,Text(c,L"关闭",L"Close",L"閉じる"));
    if(result.success) {
        const auto message=std::wstring(result.baked?Text(c,L"网格已烧录到工程。",L"Meshes baked into project. ",L"プロジェクトにベイクしました。"):L"")+std::wstring(result.abcWritten?Text(c,L"ABC 已导出。",L"ABC exported. ",L"ABC を書き出しました。"):L"")+std::to_wstring(result.samples)+
            Text(c,L" 个采样，",L" samples, ",L" サンプル、")+std::to_wstring(result.meshes)+Text(c,L" 个网格。",L" meshes.",L" メッシュ。");
        SetDlgItemTextW(d,Status,message.c_str());
    } else if(result.cancelled)SetDlgItemTextW(d,Status,Text(c,L"已取消，目标文件未更改。",L"Cancelled. The destination file was not changed.",L"中止しました。出力先は変更されていません。"));
    else {
        SetDlgItemTextW(d,Status,result.abcWritten?
            Text(c,L"ABC 已导出，但烧录回工程失败。",L"ABC exported, but project baking failed.",L"ABC 出力は完了しましたが、ベイクに失敗しました。"):
            Text(c,L"执行失败，目标文件未更改。",L"Failed. The destination file was not changed.",L"失敗しました。出力先は変更されていません。"));
        const auto message=std::wstring(Text(c,L"无法导出当前变形网格。\n\n",L"Cannot export the current deformed mesh.\n\n",L"現在の変形メッシュを書き出せません。\n\n"))+Wide(result.error);
        MessageBoxW(d,message.c_str(),Text(c,L"导出 ABC",L"Export ABC",L"ABC 書き出し"),MB_OK|MB_ICONWARNING);
    }
}
INT_PTR CALLBACK Proc(HWND d,UINT message,WPARAM wp,LPARAM lp) {
    auto* c=reinterpret_cast<Context*>(GetWindowLongPtrW(d,DWLP_USER));
    if(message==WM_INITDIALOG) {
        c=reinterpret_cast<Context*>(lp);SetWindowLongPtrW(d,DWLP_USER,lp);
        SetWindowTextW(d,c->bakeDefault?Text(*c,L"烘焙网格",L"Bake meshes",L"メッシュをベイク"):Text(*c,L"导出 ABC",L"Export ABC",L"ABC 書き出し"));
        Control(d,L"STATIC",Text(*c,L"起始帧",L"Start frame",L"開始フレーム"),0,-1,10,12,48,12);
        Control(d,L"EDIT",L"0",WS_TABSTOP|ES_NUMBER,First,60,10,48,14);
        Control(d,L"STATIC",Text(*c,L"结束帧",L"End frame",L"終了フレーム"),0,-1,122,12,48,12);
        Control(d,L"EDIT",L"0",WS_TABSTOP|ES_NUMBER,Last,172,10,48,14);
        unsigned last=0;for(int i=0;i<kModelSlotCount;++i)if(auto* p=c->app->ModelSlot(i))last=std::max(last,mdl::Mdl(p)->maxFrame);
        last=std::min(last,unsigned(frame_limits::maximum));SetDlgItemInt(d,Last,last,FALSE);
        Control(d,L"STATIC",Text(*c,L"采样帧率",L"Sample FPS",L"サンプル FPS"),0,-1,234,12,66,12);
        Control(d,L"EDIT",L"30",WS_TABSTOP|ES_NUMBER,Rate,304,10,48,14);
        Control(d,L"BUTTON",Text(*c,L"只选当前模型",L"Select current model",L"選択モデルを選ぶ"),WS_TABSTOP|BS_PUSHBUTTON,Selected,10,32,136,16);
        EnableWindow(GetDlgItem(d,Selected),c->app->SelectedModel()!=nullptr);
        Control(d,L"STATIC",Text(*c,L"按时间轴的 30 帧/秒读取范围。默认导出全部可见模型和附件，包含变形、UV 和材质分组；不包含材质着色与屏幕后期。",L"Range uses the 30 FPS timeline. Exports visible models and accessories with deformation, UVs and material groups. Shading and screen effects are not included.",L"範囲は 30 FPS の時間軸です。表示モデルとアクセサリの変形、UV、材質グループを書き出します。シェーディングと画面効果は含まれません。"),0,-1,10,54,380,26);
        Control(d,L"BUTTON",Text(*c,L"导出 ABC 文件",L"Export ABC file",L"ABC ファイル出力"),WS_TABSTOP|BS_AUTOCHECKBOX,WriteAbc,10,86,140,14);
        Control(d,L"BUTTON",Text(*c,L"烧录回工程",L"Bake into project",L"プロジェクトにベイク"),WS_TABSTOP|BS_AUTOCHECKBOX,BakeProject,158,86,140,14);
        CheckDlgButton(d,WriteAbc,c->bakeDefault?BST_UNCHECKED:BST_CHECKED);
        CheckDlgButton(d,BakeProject,c->bakeDefault?BST_CHECKED:BST_UNCHECKED);
        Control(d,L"STATIC",Text(*c,L"两项可单独选择，也可同时执行。",L"Select either output or both.",L"どちらか一方、または両方を選択できます。"),0,-1,10,108,222,14);
        Control(d,L"STATIC",Text(*c,L"输出对象（所有模型和附件均可选择）",L"Output objects (all models and accessories)",L"出力対象（モデル・アクセサリ）"),0,-1,10,128,380,12);
        auto list=Control(d,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_NOCOLUMNHEADER,Objects,10,146,180,116);
        SendMessageW(list,CCM_SETUNICODEFORMAT,TRUE,0);
        ListView_SetExtendedListViewStyle(list,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        RECT client{};GetClientRect(list,&client);LVCOLUMNW column{};column.mask=LVCF_WIDTH;column.cx=client.right-24;SendMessageW(list,LVM_INSERTCOLUMNW,0,reinterpret_cast<LPARAM>(&column));
        Control(d,L"STATIC",Text(*c,L"材质编号（留空：全部）",L"Material indices (blank: all)",L"材質番号（空欄：すべて）"),0,-1,202,146,188,12);
        Control(d,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,Materials,202,162,188,14);
        Control(d,L"STATIC",Text(*c,L"顶点编号（留空：全部）",L"Vertex indices (blank: all)",L"頂点番号（空欄：すべて）"),0,-1,202,184,188,12);
        Control(d,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,Vertices,202,200,188,14);
        Control(d,L"BUTTON",Text(*c,L"仅已选骨骼的权重顶点",L"Vertices weighted to selected bones",L"選択ボーンのウェイト頂点のみ"),WS_TABSTOP|BS_AUTOCHECKBOX,BoneVertices,202,222,188,14);
        Control(d,L"STATIC",Text(*c,L"编号从 0 开始，例如 0,2,5-10。\n范围取交集，只写入选中的变化。",L"Indices start at 0, e.g. 0,2,5-10.\nFilters intersect; only selected changes are baked.",L"番号は 0 から（例：0,2,5-10）。\n範囲の共通部分のみベイクします。"),0,-1,202,242,188,24);
        c->initializing=true;
        const auto add=[&](Selection choice,const std::wstring& label){const int i=int(c->rows.size());c->rows.push_back({choice,{},{}});LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=i;item.pszText=const_cast<wchar_t*>(label.c_str());SendMessageW(list,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item));ListView_SetCheckState(list,i,TRUE);};
        for(int i=0;i<kModelSlotCount;++i)if(auto* model=c->app->ModelSlot(i)){Selection choice;choice.slot=i;add(choice,text_encoding::ModelName(*mdl::Mdl(model),UiUsesEnglishNames(c->app->EnglishUI())));}
        for(int i=0;i<255;++i)if(auto* accessory=c->app->AccessorySlot(i)){Selection choice;choice.accessory=true;choice.slot=i;add(choice,L".x  "+std::filesystem::path(accessory->sourcePath).filename().wstring());}
        c->initializing=false;if(!c->rows.empty()){SelectRow(d,*c,0);ListView_SetItemState(list,0,LVIS_SELECTED,LVIS_SELECTED);}
        Control(d,PROGRESS_CLASSW,L"",PBS_SMOOTH,Bar,10,274,380,8);
        Control(d,L"STATIC",L"",0,Status,10,288,380,20);
        Control(d,L"BUTTON",Text(*c,L"执行…",L"Run…",L"実行…"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,236,318,74,18);
        Control(d,L"BUTTON",Text(*c,L"关闭",L"Close",L"閉じる"),WS_TABSTOP,IDCANCEL,316,318,74,18);
        Control(d,L"BUTTON",Text(*c,L"清除网格烧录",L"Clear mesh bake",L"ベイク解除"),WS_TABSTOP,ClearBake,10,318,116,18);
        EnableWindow(GetDlgItem(d,ClearBake),!mesh_bake_cache::Empty());
        FitToWorkArea(d,*c);
        return TRUE;
    }
    if(!c)return FALSE;
    if(message==WM_SIZE){Layout(d,*c);return TRUE;}
    if(((message==WM_VSCROLL||message==WM_HSCROLL)&&lp==0)||message==WM_MOUSEWHEEL){
        const bool horizontal=message==WM_HSCROLL;int& position=horizontal?c->scrollX:c->scrollY;
        RECT client{};GetClientRect(d,&client);
        const int page=horizontal?int(client.right):c->viewportHeight;
        if(message==WM_MOUSEWHEEL)position-=GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA*48;
        else switch(LOWORD(wp)){
            case SB_LINEUP:position-=24;break;case SB_LINEDOWN:position+=24;break;
            case SB_PAGEUP:position-=page;break;case SB_PAGEDOWN:position+=page;break;
            case SB_TOP:position=0;break;case SB_BOTTOM:position=horizontal?c->bodyWidth:c->bodyHeight;break;
            case SB_THUMBTRACK:case SB_THUMBPOSITION:{SCROLLINFO info{sizeof info,SIF_TRACKPOS};GetScrollInfo(d,horizontal?SB_HORZ:SB_VERT,&info);position=info.nTrackPos;break;}
        }
        Layout(d,*c);return TRUE;
    }
    // Disabled controls can still receive queued commands. Only cancellation
    // may change state while the exporter owns the scene.
    if(c->running&&(message==WM_CLOSE||message==WM_COMMAND)) {
        if(message==WM_CLOSE||LOWORD(wp)==IDCANCEL)c->cancel=true;
        return TRUE;
    }
    if(message==WM_COMMAND&&LOWORD(wp)==ClearBake&&!c->running){
        std::vector<const void*> objects;
        for(size_t i=0;i<c->rows.size();++i)if(ListView_GetCheckState(GetDlgItem(d,Objects),int(i))){const auto& row=c->rows[i].selection;objects.push_back(row.accessory?static_cast<const void*>(c->app->AccessorySlot(row.slot)):static_cast<const void*>(c->app->ModelSlot(row.slot)));}
        native_mesh::ClearMeshBakeObjects(c->app,objects);
        EnableWindow(GetDlgItem(d,ClearBake),!mesh_bake_cache::Empty());
        SetDlgItemTextW(d,Status,Text(*c,L"已清除勾选对象的网格缓存。FX 开关键帧保留，可在参数中编辑。",L"Selected mesh caches cleared. FX enable keys are retained and can be edited in Parameters.",L"選択対象のキャッシュを解除しました。FX の有効キーはパラメータで編集できます。"));return TRUE;
    }
    if(message==WM_COMMAND&&LOWORD(wp)==Selected&&HIWORD(wp)==BN_CLICKED){
        for(size_t i=0;i<c->rows.size();++i){const auto& row=c->rows[i];const bool current=!row.selection.accessory&&c->app->ModelSlot(row.selection.slot)==c->app->SelectedModel();ListView_SetCheckState(GetDlgItem(d,Objects),int(i),current);}
        return TRUE;
    }
    if(message==WM_NOTIFY&&!c->running&&!c->initializing){
        const auto* header=reinterpret_cast<const NMHDR*>(lp);
        if(header->idFrom==Objects&&header->code==LVN_ITEMCHANGED){const auto* changed=reinterpret_cast<const NMLISTVIEW*>(lp);if((changed->uChanged&LVIF_STATE)&&(changed->uNewState&LVIS_SELECTED)&&!(changed->uOldState&LVIS_SELECTED))SelectRow(d,*c,changed->iItem);}
    }
    if(message==WM_COMMAND&&LOWORD(wp)==BakeProject&&HIWORD(wp)==BN_CLICKED){
        SetDlgItemTextW(d,Status,Text(*c,L"工程烘焙保存原生网格；区间内回放缓存，区间外继续原生动画。MME 继续着色。",L"Project baking records native geometry. Cached meshes play in range; native animation resumes outside. MME shading remains active.",L"標準の形状をベイク。範囲内はキャッシュを再生し、範囲外は元のアニメーションを再開します。MME 描画は保持します。"));return TRUE;
    }
    if(message==WM_CLOSE||(message==WM_COMMAND&&LOWORD(wp)==IDCANCEL)) {
        if(c->running)c->cancel=true;else EndDialog(d,IDCANCEL);return TRUE;
    }
    if(message==WM_COMMAND&&LOWORD(wp)==IDOK&&!c->running){Run(d,*c);return TRUE;}
    return FALSE;
}
}
void ShowDialogImpl(MMDApp* app,bool bakeDefault) {
    if(bake::UiLocked()||!CanExport(app))return;
    INITCOMMONCONTROLSEX controls{sizeof controls,ICC_PROGRESS_CLASS|ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
    std::vector<WORD> storage(128,0);auto* dialog=reinterpret_cast<DLGTEMPLATE*>(storage.data());
    dialog->style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT;
    dialog->cx=400;dialog->cy=348;
    auto* tail=reinterpret_cast<WORD*>(dialog+1);*tail++=0;*tail++=0;*tail++=0;*tail++=9;
    const wchar_t font[]=L"Microsoft YaHei UI";memcpy(tail,font,sizeof font);
    Context context{app};context.bakeDefault=bakeDefault;DialogBoxIndirectParamW(GetModuleHandleW(nullptr),dialog,app->MainWindow(),Proc,reinterpret_cast<LPARAM>(&context));
}
void ShowDialog(MMDApp* app){ShowDialogImpl(app,false);}
void ShowBakeDialog(MMDApp* app){ShowDialogImpl(app,true);}

}
