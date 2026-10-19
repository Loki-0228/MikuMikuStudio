// Run the real menu/dialog handlers on hidden Win32 controls. Only the modal
// shell and renderer-dependent selection refresh are replaced.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/text_encoding.hpp"
namespace mikumikustudio { void CmdViewMenu(MMDApp*,HWND,std::uint16_t,std::uint16_t); }
using namespace mikumikustudio;
namespace {
unsigned checks=0;
bool cancel=false;
void Check(bool ok,const char* text) { ++checks; if(!ok){std::printf("FAIL %s\n",text);std::exit(1);} }
std::wstring Label(HWND control,int row,UINT message) {
    wchar_t text[128]{}; SendMessageW(control,message,row,reinterpret_cast<LPARAM>(text));return text;
}
void Verify(HWND dialog) {
    auto* order=static_cast<std::int32_t*>(g_Block->AccessoryOrderArray());
    Check(SendDlgItemMessageW(dialog,628,LB_GETCOUNT,0,0)==3,"every loaded model has a row even when the old combo is incomplete");
    for(int row=1;row<=3;++row) {
        Check(order[row]>0 && order[row]<=255,"order row points to a model slot");
        const auto* model=mdl::Mdl(g_Block->ModelSlot(order[row]-1));
        Check(model && Label(GetDlgItem(dialog,628),row-1,LB_GETTEXT)==text_encoding::ModelLabel(*model,true),"dialog labels follow the repaired model identities");
    }
}
INT_PTR WINAPI Dialog(HINSTANCE,LPCSTR,HWND parent,DLGPROC proc,LPARAM param) {
    HWND dialog=CreateWindowW(L"STATIC",L"order test",WS_POPUP,0,0,200,200,parent,nullptr,nullptr,nullptr);
    HWND list=CreateWindowW(L"LISTBOX",L"",WS_CHILD|LBS_NOTIFY,0,0,100,100,dialog,reinterpret_cast<HMENU>(628),nullptr,nullptr);
    Check(dialog && list,"create hidden dialog controls");
    proc(dialog,WM_INITDIALOG,0,param); Verify(dialog);
    SendMessageW(list,LB_SETCURSEL,0,0);proc(dialog,WM_COMMAND,631,0);Verify(dialog);
    SendMessageW(list,LB_SETCURSEL,2,0);proc(dialog,WM_COMMAND,630,0);Verify(dialog);
    proc(dialog,WM_COMMAND,cancel?2:632,0);
    Check(g_Block->AccessoryOrderArray()==nullptr,"dialog releases its temporary ordering");
    DestroyWindow(dialog);return cancel?2:1;
}
void Selection(MMDApp*) {}
template<class F> void Hook(F target,F replacement) {
    Check(MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(replacement),nullptr)==MH_OK,"install scoped test hook");
}
void Case(bool damaged,bool canceled,bool stale) {
    cancel=canceled;
    auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof app->state);
    g_Block=app.get();app->EnglishUI()=1;
    auto a=std::make_unique<mdl::ModelRecord>(),b=std::make_unique<mdl::ModelRecord>(),c=std::make_unique<mdl::ModelRecord>();
    mdl::ModelRecord* models[]={a.get(),b.get(),c.get()};int slots[]={0,7,254};
    for(int i=0;i<3;++i) {std::strcpy(models[i]->name,i==0?"A":i==1?"B":"C");std::strcpy(models[i]->nameEn,models[i]->name);
        models[i]->comboSelIndex=damaged?(i==2?255:2):i+1;
        app->ModelSlot(slots[i])=reinterpret_cast<unsigned char*>(models[i]);}
    app->state.hwnd=CreateWindowW(L"STATIC",L"model order test",WS_POPUP,0,0,200,200,nullptr,nullptr,nullptr,nullptr);
    for(int id:{436,474,449}) {HWND combo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,0,100,100,app->state.hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
        for(const wchar_t* label:{L"camera",L"old A",L"old B",L"old C"})SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        if(stale)SendMessageW(combo,CB_DELETESTRING,3,0);}
    CmdViewMenu(app.get(),app->state.hwnd,288,0);
    for(int i=0;i<3;++i) {
        if(cancel)Check(models[i]->comboSelIndex==(damaged?(i==2?255:2):i+1),"cancel preserves original model order");
        else for(int id:{436,474,449}) Check(Label(GetDlgItem(app->state.hwnd,id),models[i]->comboSelIndex,CB_GETLBTEXT)==text_encoding::ModelLabel(*models[i],true),"all model selectors match the actual selected model after reorder");
    }
    DestroyWindow(app->state.hwnd);g_Block=nullptr;
}
}
int main() {
    Check(MH_Initialize()==MH_OK,"initialize hooks");
    Hook(UiDialogBoxParamA,Dialog);Hook(ApplyModelComboSelection,Selection);
    Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable test hooks");
    for(bool damaged:{false,true})for(bool canceled:{false,true})for(bool stale:{false,true}) Case(damaged,canceled,stale);
    MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();
    std::printf("model_display_order_tests: %u checks passed\n",checks);
}
