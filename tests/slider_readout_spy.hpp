#pragma once
#include <Windows.h>
#include <CommCtrl.h>

// Observation only: every message is forwarded to the real native control.
// Reset after the test moves the thumb, before invoking the production entry.
namespace slider_readout_probe {
struct Counts {
    unsigned positions=0,redrawPositions=0,textSets=0,textReplaces=0,selections=0,paints=0,erases=0;
    LPARAM lastPosition=0;
    unsigned TextWrites() const noexcept {return textSets+textReplaces;}
    bool NoEcho() const noexcept {return !positions&&!TextWrites()&&!selections;}
};
struct Spy {
    HWND window=nullptr;
    Counts counts{};
    Spy()=default;
    Spy(const Spy&)=delete;
    Spy& operator=(const Spy&)=delete;
    bool Watch(HWND value) noexcept {
        if(window||!value)return false;
        if(!SetWindowSubclass(value,Observe,reinterpret_cast<UINT_PTR>(this),reinterpret_cast<DWORD_PTR>(this)))return false;
        window=value;return true;
    }
    void Clear() noexcept {counts={};}
    ~Spy(){if(window&&IsWindow(window))RemoveWindowSubclass(window,Observe,reinterpret_cast<UINT_PTR>(this));}
    static LRESULT CALLBACK Observe(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto& counts=reinterpret_cast<Spy*>(data)->counts;
        if(message==TBM_SETPOS){++counts.positions;if(wp)++counts.redrawPositions;counts.lastPosition=lp;}
        else if(message==WM_SETTEXT)++counts.textSets;
        else if(message==EM_REPLACESEL)++counts.textReplaces;
        else if(message==EM_SETSEL)++counts.selections;
        else if(message==WM_PAINT)++counts.paints;
        else if(message==WM_ERASEBKGND)++counts.erases;
        return DefSubclassProc(window,message,wp,lp);
    }
};
}
