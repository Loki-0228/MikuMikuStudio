#include "mikudancestudio/system_settings_ui.hpp"
#include "mikudancestudio/system_settings.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "../plugins/history_runtime.hpp"
#include "../plugins/json.hpp"
#include "../plugins/shared_history.hpp"
#include <Windows.h>
#include <algorithm>

namespace mikudancestudio::system_settings_ui {
namespace {
const wchar_t* Text(MMDApp* app,const wchar_t* zh,const wchar_t* en,const wchar_t* ja) {
    return app->EnglishUI()==2?zh:app->EnglishUI()?en:ja;
}
using Mode=plugins::SharedHistory::RecordingMode;
struct State { MMDApp* app; HistoryPreferences preference; };
bool Number(HWND dialog,int id,unsigned maximum,unsigned& result) {
    wchar_t text[16]{};const int length=GetWindowTextLengthW(GetDlgItem(dialog,id));
    if(length<1||length>=16)return false;
    GetDlgItemTextW(dialog,id,text,16);unsigned value=0;
    for(int i=0;i<length;++i){if(text[i]<L'0'||text[i]>L'9'||value>(maximum-unsigned(text[i]-L'0'))/10)return false;
        value=value*10+unsigned(text[i]-L'0');}
    if(!value||value>maximum)return false;result=value;return true;
}
HWND Control(HWND dialog,const wchar_t* cls,const wchar_t* text,DWORD style,int id,int x,int y,int w,int h) {
    const auto child=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,dialog,
        reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    return child;
}
INT_PTR CALLBACK Procedure(HWND dialog,UINT message,WPARAM wp,LPARAM lp) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(message==WM_INITDIALOG) {
        state=reinterpret_cast<State*>(lp);SetWindowLongPtrW(dialog,DWLP_USER,lp);
        auto* app=state->app;
        SetWindowTextW(dialog,Text(app,L"系统设置",L"System settings",L"システム設定"));
        RECT area{};GetClientRect(dialog,&area);const int width=area.right;
        Control(dialog,L"STATIC",Text(app,L"历史记录模式",L"History recording mode",L"履歴記録モード"),0,-1,16,12,width-32,22);
        Control(dialog,L"BUTTON",Text(app,L"记录全部编辑操作",L"Record all editing operations",L"すべての編集操作を記録"),
            BS_AUTORADIOBUTTON|WS_GROUP|WS_TABSTOP,101,20,38,width-40,24);
        Control(dialog,L"STATIC",Text(app,L"对已支持的编辑操作，姿态、参数调整也记录撤销。",L"Includes pose and parameter adjustments for supported editing operations.",L"対応する編集操作では、ポーズやパラメータの調整も記録します。"),0,-1,38,64,width-54,28);
        Control(dialog,L"BUTTON",Text(app,L"仅记录显式提交",L"Record explicit commits only",L"確定した操作のみ記録"),
            BS_AUTORADIOBUTTON|WS_TABSTOP,102,20,94,width-40,24);
        Control(dialog,L"STATIC",Text(app,L"登记、导入、删除等提交操作产生撤销步骤。",L"Key registration, imports, deletions and other commits create undo steps.",L"登録、読込、削除などの確定操作を記録します。"),0,-1,38,120,width-54,40);
        Control(dialog,L"STATIC",Text(app,L"最大保留步数",L"Maximum history steps",L"最大履歴数"),0,-1,20,166,180,22);
        Control(dialog,L"EDIT",std::to_wstring(state->preference.steps).c_str(),ES_NUMBER|WS_BORDER|WS_TABSTOP,103,210,164,110,24);
        Control(dialog,L"STATIC",Text(app,L"历史内存预算（MB）",L"History memory budget (MB)",L"履歴メモリ上限（MB）"),0,-1,20,200,185,22);
        Control(dialog,L"EDIT",std::to_wstring(state->preference.memoryMiB).c_str(),ES_NUMBER|WS_BORDER|WS_TABSTOP,104,210,198,110,24);
        Control(dialog,L"BUTTON",Text(app,L"按本机计算建议值",L"Suggest limits for this PC",L"このPCの推奨値を計算"),BS_PUSHBUTTON|WS_TABSTOP,105,20,234,240,26);
        Control(dialog,L"STATIC",Text(app,L"达到上限时回收最旧记录。预算不代表程序总内存。",L"Oldest records are retired at the limit. This is not a total process memory cap.",L"上限に達すると古い履歴を破棄します。アプリ全体のメモリ上限ではありません。"),0,-1,20,270,width-40,42);
        Control(dialog,L"BUTTON",Text(app,L"保存",L"Save",L"保存"),BS_DEFPUSHBUTTON|WS_TABSTOP,IDOK,width-184,318,76,26);
        Control(dialog,L"BUTTON",Text(app,L"取消",L"Cancel",L"キャンセル"),BS_PUSHBUTTON|WS_TABSTOP,IDCANCEL,width-96,318,76,26);
        CheckRadioButton(dialog,101,102,state->preference.recordAdjustments?101:102);return TRUE;
    }
    if(!state)return FALSE;
    if(message==WM_CLOSE){EndDialog(dialog,IDCANCEL);return TRUE;}
    if(message==WM_COMMAND&&LOWORD(wp)==IDCANCEL){EndDialog(dialog,IDCANCEL);return TRUE;}
    if(message==WM_COMMAND&&LOWORD(wp)==105) {
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
        const unsigned budget=GlobalMemoryStatusEx(&memory)?unsigned(std::clamp<ULONGLONG>(memory.ullAvailPhys/(16ull*1024*1024),16,256)):64;
        auto* history=plugins::NativeHistory(state->app);
        const auto count=history?history->UndoCount()+history->RedoCount():0;
        const auto average=count?std::max<std::uint64_t>(1,history->RetainedBytes()/count):2ull*1024*1024;
        const unsigned steps=unsigned(std::clamp<std::uint64_t>(std::uint64_t(budget)*1024*1024/average,1,100000));
        SetDlgItemInt(dialog,103,steps,FALSE);SetDlgItemInt(dialog,104,budget,FALSE);return TRUE;
    }
    if(message==WM_COMMAND&&LOWORD(wp)==IDOK) {
        const bool record=IsDlgButtonChecked(dialog,101)==BST_CHECKED;
        unsigned steps=0,memoryMiB=0;
        if(!Number(dialog,103,100000,steps)||!Number(dialog,104,16384,memoryMiB)||memoryMiB<16) {
            MessageBoxW(dialog,Text(state->app,L"步数范围为 1 至 100000，内存预算范围为 16 至 16384 MB。",L"Steps: 1–100000. Memory budget: 16–16384 MB.",L"履歴数は1～100000、メモリ上限は16～16384 MBです。"),L"MMS",MB_OK|MB_ICONINFORMATION);return TRUE;
        }
        auto* history=plugins::NativeHistory(state->app);
        const auto next=record?Mode::AllOperations:Mode::ExplicitOnly;
        const std::string body=std::string("{\"recordAdjustments\":")+(record?"true":"false")+",\"steps\":"+
            std::to_string(steps)+",\"memoryMiB\":"+std::to_string(memoryMiB)+"}";
        const auto persist=[&]()->MmsResult{return system_settings::WriteSection("history",body)==system_settings::Result::Ok?MMS_OK:MMS_E_FAILED;};
        const auto saved=history?history->Configure(next,steps,std::uint64_t(memoryMiB)*1024*1024,persist):persist();
        if(saved!=MMS_OK) {
            MessageBoxW(dialog,Text(state->app,L"设置未能保存，已保留原设置。",L"The setting could not be saved. The previous setting was kept.",L"設定を保存できなかったため、元の設定を保持しました。"),
                Text(state->app,L"系统设置",L"System settings",L"システム設定"),MB_OK|MB_ICONERROR);return TRUE;
        }
        if(history)history->Collect();
        EndDialog(dialog,IDOK);return TRUE;
    }
    return FALSE;
}
}
HistoryPreferences HistoryPreference() noexcept {
    HistoryPreferences preference;
    try {
        std::string bytes;
        if(system_settings::ReadSection("history",bytes)!=system_settings::Result::Ok)return preference;
        const auto section=plugins::Json::Parse(bytes);
        const auto field=section.object.find("recordAdjustments");
        preference.recordAdjustments=field!=section.object.end()&&field->second.kind==plugins::Json::Boolean&&field->second.text=="true";
        const auto steps=section.object.find("steps"),memory=section.object.find("memoryMiB");
        if(steps!=section.object.end()){const auto value=steps->second.UInt();if(value>=1&&value<=100000)preference.steps=value;}
        if(memory!=section.object.end()){const auto value=memory->second.UInt();if(value>=16&&value<=16384)preference.memoryMiB=value;}
    } catch(...) {}
    return preference;
}
bool RecordAdjustmentsPreference() noexcept {return HistoryPreference().recordAdjustments;}
void EnsureItem(MMDApp* app) {
    if(!app||!app->MainWindow())return;
    const auto bar=GetMenu(app->MainWindow());if(!bar)return;
    for(int i=0;i<GetMenuItemCount(bar);++i) {
        const auto popup=GetSubMenu(bar,i);if(!popup||GetMenuState(popup,201,MF_BYCOMMAND)==UINT(-1))continue;
        const auto label=Text(app,L"系统设置…",L"System settings…",L"システム設定…");
        if(GetMenuState(popup,kCommand,MF_BYCOMMAND)==UINT(-1))InsertMenuW(popup,201,MF_BYCOMMAND|MF_STRING,kCommand,label);
        else ModifyMenuW(popup,kCommand,MF_BYCOMMAND|MF_STRING,kCommand,label);
        break;
    }
}
bool HandleCommand(MMDApp* app,unsigned command) {
    if(command!=kCommand||!app)return false;
    if(GetCapture())return true;
    State state{app,HistoryPreference()};
    // No resource DLL or new Windows dialog API is required.
    struct {DLGTEMPLATE dialog;WORD menu,windowClass,title;} layout{};
    layout.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;
    layout.dialog.dwExtendedStyle=WS_EX_DLGMODALFRAME;layout.dialog.cx=310;layout.dialog.cy=236;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&layout.dialog,app->MainWindow(),Procedure,reinterpret_cast<LPARAM>(&state));
    return true;
}
}
