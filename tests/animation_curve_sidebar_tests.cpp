#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include "mikumikustudio/animation_curve_editor.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ce=mikumikustudio::curve_editor;
namespace ac=mikumikustudio::animation_curve;
void Check(bool condition,const char* text){if(!condition){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
RECT Bounds(HWND window,HWND parent){RECT r{};GetWindowRect(window,&r);MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&r),2);return r;}
SCROLLINFO Scroll(HWND tree){SCROLLINFO s{sizeof(s),SIF_ALL};GetScrollInfo(tree,SB_VERT,&s);return s;}
int main(){
    std::vector<ce::Channel> channels;
    for(int i=0;i<80;++i){ce::Channel channel;channel.id="bone/"+std::to_string(i);channel.label=L"骨骼通道 "+std::to_wstring(i);
        channel.collections={{"bone",L"长通道列表"}};channel.selected=true;
        ac::Key first,last;last.frame=30;last.value=i+1;channel.track.keys={first,last};channels.push_back(channel);}
    int writes=0;ce::Source source;source.id="sidebar";source.label=L"骨骼";source.initialExpandedCollection="bone";
    source.read=[&](auto& output,auto&){output=channels;return true;};
    source.apply=[&](const auto& output,auto&){++writes;channels=output;return true;};
    ce::Session session;session.id="sidebar";session.language=2;session.sources.push_back(source);
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(window!=nullptr,"editor opens");
    SetWindowPos(window,nullptr,-32000,-32000,1200,820,SWP_NOZORDER|SWP_NOACTIVATE);
    ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
    HWND tree=GetDlgItem(window,1015),bar=GetDlgItem(window,1110),splitter=GetDlgItem(window,1111);
    Check(tree&&bar&&splitter,"sidebar controls exist");
    TreeView_SelectSetFirstVisible(tree,TreeView_GetRoot(tree));
    auto before=Scroll(tree);Check(before.nMax>int(before.nPage),"overflowing tree scrolls");
    Check(IsWindowVisible(bar),"themed scrollbar visible with overflow");
    RECT r{};GetClientRect(bar,&r);const int x=r.right/2;
    const auto selected=TreeView_GetSelection(tree);
    SendMessageW(bar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,r.bottom-r.right/2));
    SendMessageW(bar,WM_LBUTTONUP,0,MAKELPARAM(x,r.bottom-r.right/2));
    Check(Scroll(tree).nPos>before.nPos,"down arrow scrolls native tree");
    SendMessageW(bar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,r.bottom-r.right/2));
    SendMessageW(bar,WM_CANCELMODE,0,0);Check(GetCapture()!=bar,"scroll cancellation releases mouse capture");
    before=Scroll(tree);SendMessageW(bar,WM_MOUSEWHEEL,MAKEWPARAM(0,short(-WHEEL_DELTA)),0);
    Check(Scroll(tree).nPos>before.nPos,"wheel over custom scrollbar scrolls native tree");
    TreeView_SelectSetFirstVisible(tree,TreeView_GetRoot(tree));
    SendMessageW(bar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,r.right+3));
    SendMessageW(bar,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,r.bottom-r.right-2));
    SendMessageW(bar,WM_LBUTTONUP,0,MAKELPARAM(x,r.bottom-r.right-2));
    auto bottom=Scroll(tree);Check(bottom.nPos==bottom.nMax-int(bottom.nPage)+1,"thumb drag reaches last page");
    Check(TreeView_GetSelection(tree)==selected,"scrolling preserves active channel");
    SendMessageW(tree,WM_VSCROLL,SB_TOP,0);before=Scroll(tree);
    SendMessageW(bar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,r.bottom/2));
    SendMessageW(bar,WM_LBUTTONUP,0,MAKELPARAM(x,r.bottom/2));
    Check(Scroll(tree).nPos>before.nPos,"track click pages the native tree");
    auto original=Bounds(tree,window),header=Bounds(GetDlgItem(window,1009),window);
    const auto originalCanvas=Bounds(GetDlgItem(window,1020),window);
    SendMessageW(splitter,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(4,15));
    for(int target:{114,-100,80}){
        SendMessageW(splitter,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(target,15));
        auto heldTree=Bounds(tree,window),heldCanvas=Bounds(GetDlgItem(window,1020),window);
        Check(EqualRect(&original,&heldTree)&&EqualRect(&originalCanvas,&heldCanvas),"drag keeps sidebar and plot stationary until release");
    }
    SendMessageW(splitter,WM_LBUTTONUP,0,MAKELPARAM(114,15));
    auto wide=Bounds(tree,window),canvas=Bounds(GetDlgItem(window,1020),window);
    Check(wide.right>original.right+50,"drag widens left panel");
    Check(!GetUpdateRect(window,nullptr,FALSE)&&!GetUpdateRect(tree,nullptr,FALSE)&&
          !GetUpdateRect(GetDlgItem(window,1020),nullptr,FALSE),"release completes parent, tree and plot repaint");
    Check(canvas.left>wide.right&&canvas.right-canvas.left>=550,"canvas follows divider with usable width");
    auto newHeader=Bounds(GetDlgItem(window,1009),window);
    Check(EqualRect(&header,&newHeader),"toolbar remains fixed while divider moves");
    SendMessageW(splitter,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(4,15));
    SendMessageW(splitter,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(-1000,15));
    auto held=Bounds(tree,window);Check(EqualRect(&held,&wide),"pending shrink leaves controls in place");
    SendMessageW(splitter,WM_KEYDOWN,VK_ESCAPE,0);
    auto restored=Bounds(tree,window);Check(restored.right==wide.right,"Escape discards pending width");
    SendMessageW(splitter,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(4,15));
    SendMessageW(splitter,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(-90,15));
    SetCapture(tree);ReleaseCapture();
    SendMessageW(splitter,WM_LBUTTONUP,0,MAKELPARAM(-90,15));
    Check(Bounds(tree,window).right==wide.right,"lost capture discards pending width and late release");
    SetWindowPos(window,nullptr,0,0,840,640,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    canvas=Bounds(GetDlgItem(window,1020),window);Check(canvas.right-canvas.left>=550,"narrow window clamps divider");
    SetWindowPos(window,nullptr,0,0,1200,820,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    Check(Bounds(tree,window).right==wide.right,"widening window restores preferred sidebar width");
    SendMessageW(splitter,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(4,15));
    SendMessageW(splitter,WM_LBUTTONUP,0,MAKELPARAM(-1000,15));
    auto narrow=Bounds(tree,window);Check(narrow.right-narrow.left==180,"release clamps shrink to usable width");
    SendMessageW(splitter,WM_LBUTTONDBLCLK,0,0);Check(Bounds(tree,window).right==original.right,"double click resets width");
    SendMessageW(splitter,WM_KEYDOWN,VK_RIGHT,0);Check(Bounds(tree,window).right>original.right,"keyboard resizes divider");
    SetWindowPos(window,nullptr,0,0,1200,599,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    for(int i=0;i<30;++i)SendMessageW(splitter,WM_KEYDOWN,VK_RIGHT,0);
    canvas=Bounds(GetDlgItem(window,1020),window);auto frame=Bounds(GetDlgItem(window,1041),window);
    Check(canvas.bottom<frame.top,"short wide window keeps canvas clear of parameter strip");
    auto mini=Bounds(GetDlgItem(window,1031),window);
    Check(mini.bottom<=Bounds(GetDlgItem(window,1100),window).top,"short window keeps preview clear of coordinate fields");
    TreeView_Expand(tree,TreeView_GetRoot(tree),TVE_COLLAPSE);
    Check(!IsWindowVisible(bar),"collapsed short list hides scrollbar");
    TreeView_Expand(tree,TreeView_GetRoot(tree),TVE_EXPAND);
    Check(IsWindowVisible(bar),"expanding restores scrollbar");
    Check(writes==0,"navigation and layout never write animation data");
    ce::CloseOwned(nullptr);
    std::puts("animation_curve_sidebar_tests: arrows, wheel, thumb, paging, selection, resizing, bounds, cancel, reset, collapse and zero writes passed");
}
