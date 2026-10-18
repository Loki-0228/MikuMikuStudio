// Native Win32 controls only. No renderer, plugin runtime or user window.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/ui_control_readout.hpp"
#include "slider_readout_spy.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
unsigned checks=0;
void Check(bool okay,const char* label){++checks;if(!okay){std::fprintf(stderr,"FAIL %s\n",label);std::exit(1);}}
}
int main(){
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES};Check(InitCommonControlsEx(&controls)!=FALSE,"initialize native trackbar class");
    const auto parent=CreateWindowExW(0,L"STATIC",L"native readout",WS_OVERLAPPED,0,0,320,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(parent!=nullptr,"create hidden parent");
    const auto slider=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD,0,0,140,30,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    const auto edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD,0,35,140,25,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(slider&&edit,"create actual native trackbar and EDIT");SendMessageW(slider,TBM_SETRANGE,FALSE,MAKELPARAM(0,255));
    {
        slider_readout_probe::Spy sliderSpy,editSpy;Check(sliderSpy.Watch(slider)&&editSpy.Watch(edit),"observe native messages without changing their implementation");
        SendMessageW(slider,TBM_SETPOS,TRUE,255);sliderSpy.Clear();
        for(unsigned i=0;i<10;++i)Check(!mikudancestudio::ui::SetTrackbarPositionIfChanged(slider,256),"repeat256 compares final255");
        Check(!mikudancestudio::ui::SetTrackbarPositionIfChanged(slider,(std::numeric_limits<int>::max)())&&sliderSpy.counts.NoEcho()&&
            SendMessageW(slider,TBM_GETPOS,0,0)==255,"clamped same position sends no redraw request");
        Check(mikudancestudio::ui::SetTrackbarPositionIfChanged(slider,137)&&sliderSpy.counts.positions==1&&sliderSpy.counts.redrawPositions==1&&
            sliderSpy.counts.lastPosition==137&&SendMessageW(slider,TBM_GETPOS,0,0)==137,"changed tick immediately uses native redraw once");
        Check(!mikudancestudio::ui::SetTrackbarPositionIfChanged(slider,137)&&sliderSpy.counts.positions==1,"same changed tick deduplicates");
        sliderSpy.Clear();Check(mikudancestudio::ui::SetTrackbarPositionIfChanged(slider,-999)&&SendMessageW(slider,TBM_GETPOS,0,0)==0&&
            sliderSpy.counts.positions==1&&!mikudancestudio::ui::SetTrackbarPositionIfChanged(slider,-999)&&sliderSpy.counts.positions==1,"lower endpoint is clamped before deduplication");
        SendMessageW(slider,TBM_SETRANGE,FALSE,MAKELPARAM(-100,100));sliderSpy.Clear();
        Check(mikudancestudio::ui::SetTrackbarPositionIfChanged(slider,-53)&&SendMessageW(slider,TBM_GETPOS,0,0)==-53&&
            sliderSpy.counts.positions==1&&sliderSpy.counts.redrawPositions==1,"signed direction range changes normally");
        Check(!mikudancestudio::ui::SetTrackbarPositionIfChanged(slider,-53)&&sliderSpy.counts.positions==1,"negative same tick deduplicates");
        SetWindowTextA(edit,"same");SendMessageW(edit,EM_SETMODIFY,TRUE,0);SendMessageW(edit,EM_SETSEL,1,3);editSpy.Clear();
        for(unsigned i=0;i<10;++i)Check(!mikudancestudio::ui::SetReadoutTextIfChangedA(edit,"same"),"identical text does not write native edit");
        DWORD start=0,end=0;SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
        Check(editSpy.counts.NoEcho()&&start==1&&end==3&&SendMessageW(edit,EM_GETMODIFY,0,0),"same text preserves native modify and selection");
        Check(mikudancestudio::ui::SetReadoutTextIfChangedA(edit,"different")&&editSpy.counts.textSets==1,"changed text writes once");
        char actual[32]{};GetWindowTextA(edit,actual,sizeof(actual));
        Check(!std::strcmp(actual,"different")&&!mikudancestudio::ui::SetReadoutTextIfChangedA(edit,"different")&&editSpy.counts.textSets==1,"actual new text is retained without repeat writes");
        Check(!mikudancestudio::ui::SetTrackbarPositionIfChanged(nullptr,12)&&!mikudancestudio::ui::SetReadoutTextIfChangedA(nullptr,"text"),"missing controls remain harmless");
    }
    DestroyWindow(parent);std::printf("PASS native readout message deduplication: %u checks (visual flicker not assessed)\n",checks);return 0;
}
