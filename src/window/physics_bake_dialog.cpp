#include "baking_menu_commands.hpp"
#include "physics_settings_controls.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/ui_translation.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <CommDlg.h>
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/alembic_export.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <cwchar>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mikumikustudio::bake {
namespace {
constexpr int Start=100,End=101,Last=102,Keys=104,Selected=105,Presets=106,Advanced=107,Details=108,
    Bar=109,Status=110,Name=126,Save=127,Load=128,
    ClearCurrent=130,ClearAll=131,ClearNote=132,Info=133,
    Step=140,Channels=141,ChannelSummary=142,Preview=145,
    GroupList=160,FieldList=161,FieldAll=162,FieldNone=163,ChannelNote=164,GroupLabel=165,GroupAll=167;
enum class BakeKind { Keyframes, Physics };
constexpr int OptionsRow(BakeKind kind) { return kind==BakeKind::Physics?152:66; }
constexpr int MainHeight(BakeKind kind) { return OptionsRow(kind)+118; }
bool previewEnabled = true;
struct Context {
    MMDApp* app{};Request request;bool advanced=false,details=false,running=false,cancel=false,clear=false,channels=false;
    physics_settings_controls::Editor controls;
    BakeKind kind=BakeKind::Keyframes;
    bool sceneKeyframes=false;
    HWND tooltip{};std::map<int,std::wstring> tips;
    std::vector<channel_bake::Group> groups;
    int shown=-1;
};
void EnableInputs(HWND d,Context& c,bool enabled);
bool IsPhysics(const Context& c) { return c.kind==BakeKind::Physics; }
bool IsModel(const Context& c) { return c.request.channelScope==channel_bake::Scope::Model; }
const wchar_t* Text(const Context& c,const wchar_t* zh,const wchar_t* en,const wchar_t* ja) {
    return c.app->EnglishUI()==kUiChinese?zh:c.app->EnglishUI()==kUiEnglish?en:ja;
}
std::wstring Value(HWND dlg,int id) {
    HWND edit=GetDlgItem(dlg,id);int n=GetWindowTextLengthW(edit);
    std::wstring text(n+1,L'\0');GetWindowTextW(edit,text.data(),n+1);text.resize(n);return text;
}
void Set(HWND dlg,int id,const std::wstring& value){SetDlgItemTextW(dlg,id,value.c_str());}
bool Integer(HWND dlg,int id,int& value){char text[256]{};GetDlgItemTextA(dlg,id,text,sizeof text);return frame_limits::Parse(text,value);}
HWND Control(HWND dlg,const wchar_t* kind,const wchar_t* text,DWORD style,int id,int x,int y,int w,int h) {
    RECT r{x,y,x+w,y+h};MapDialogRect(dlg,&r);
    auto child=CreateWindowExW(wcscmp(kind,L"EDIT")==0?WS_EX_CLIENTEDGE:0,kind,text,
        WS_CHILD|WS_VISIBLE|style,r.left,r.top,r.right-r.left,r.bottom-r.top,dlg,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(child,WM_SETFONT,SendMessageW(dlg,WM_GETFONT,0,0),TRUE);return child;
}
void Label(HWND d,const wchar_t* t,int x,int y,int w,int h=12){Control(d,L"STATIC",t,0,-1,x,y,w,h);}
void Tip(HWND d,Context& c,int id,const wchar_t* text) {
    c.tips[id]=text;TOOLINFOW info{sizeof info};info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;info.hwnd=d;
    info.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(d,id));info.lpszText=c.tips[id].data();
    SendMessageW(c.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
}
std::wstring Wide(const std::string& input) {
    if(input.empty())return std::wstring();
    const int n=MultiByteToWideChar(CP_UTF8,0,input.c_str(),-1,nullptr,0);
    if(n<=0)return std::wstring();
    std::wstring out(size_t(n),L'\0');MultiByteToWideChar(CP_UTF8,0,input.c_str(),-1,out.data(),n);
    out.resize(size_t(n)-1);return out;
}
const wchar_t* MainTitle(const Context& c) {
    return IsPhysics(c)?Text(c,L"物理烧录",L"Physics baking",L"物理焼き込み")
                       :Text(c,L"关键帧烧录",L"Keyframe baking",L"キー焼き込み");
}
void ErrorBox(HWND dlg,Context& c,const wchar_t* message){MessageBoxW(dlg,message,MainTitle(c),MB_OK|MB_ICONWARNING);}
// ---- channel selection -----------------------------------------------------
// The list of selectable channels follows the page the main window shows.  The
// model page offers the selected model's own tracks only, so a bake started
// there can never reach another object in the scene; the camera/light/accessory
// page offers the scene-wide tracks and every loaded .x object.
//
// The bone channel is a channel of bake::Compute/Commit, not of this module:
// the bone pools are written there and the selection entry only carries the
// scope.  Its sub-parameters are the bones themselves (field i is bone i), so
// the picker selects individual bones instead of only switching the model on or
// off.  Whole means every bone of the model, Selected the scene's own bone
// selection (the existing selectedOnly semantics) and Explicit the ticked list;
// a checked source is therefore never widened to the whole model.
const channel_bake::Group* BonesGroup(const std::vector<channel_bake::Group>& groups) {
    for(const auto& group:groups)if(group.kind==channel_bake::Kind::Bones)return &group;
    return nullptr;
}
std::vector<int> CheckedBones(const Context& c,const channel_bake::Group& bones) {
    std::vector<int> checked;
    const auto* choice=c.request.channels.Find(channel_bake::Kind::Bones,0);
    for(std::size_t i=0;i<bones.fields.size();++i){
        const bool on=!choice||choice->fields.empty()||
                      (i<choice->fields.size()?choice->fields[i]:true);
        if(on)checked.push_back(static_cast<int>(i));
    }
    return checked;
}
// Writes what the picker shows into the entry as an explicit scope: the whole
// model when every bone is ticked, the scene's selection when the keyframe
// settings ask for it, and the ticked list otherwise.
void DeriveBoneScope(Context& c,const channel_bake::Group& bones) {
    auto& choice=c.request.channels.Touch(channel_bake::Kind::Bones,0,bones.fields.size());
    const auto checked=CheckedBones(c,bones);
    if(checked.size()==bones.fields.size()){
        choice.coverage=c.request.selectedOnly?channel_bake::Coverage::Selected
                                              :channel_bake::Coverage::Whole;
        choice.indices.clear();
        return;
    }
    choice.coverage=channel_bake::Coverage::Explicit;
    choice.indices=checked;
}
bool GroupIncluded(const Context& c,const channel_bake::Group& group) {
    return c.request.channels.Active(group.kind,group.slot);
}
void SetIncluded(Context& c,const channel_bake::Group& group,bool on) {
    if(on) {
        c.request.channels.Touch(group.kind,group.slot,group.fields.size());
        if(group.kind==channel_bake::Kind::Bones)DeriveBoneScope(c,group);
    }else c.request.channels.Remove(group.kind,group.slot);
}
void RefreshChannels(HWND d,Context& c) {
    const bool scenePhysicsOnly=IsPhysics(c)&&!IsModel(c)&&!c.sceneKeyframes;
    if(scenePhysicsOnly){Set(d,ChannelSummary,L"");return;}
    Set(d,ChannelSummary,channel_bake::Summary(*c.app,c.request.channelScope,c.groups,c.request.channels));
}
// The field list of the shown entry, and the write-back of both lists into the
// request.  Selecting an entry includes it; the field list is its
// sub-parameters, and an entry whose fields are all cleared is dropped.
void ShowFields(HWND d,Context& c,int index) {
    HWND fields=GetDlgItem(d,FieldList);
    SendMessageW(fields,LB_RESETCONTENT,0,0);
    c.shown=index;
    if(index<0||index>=static_cast<int>(c.groups.size()))return;
    const auto& group=c.groups[index];
    for(const auto& field:group.fields)SendMessageW(fields,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(field.label.c_str()));
    const auto* choice=c.request.channels.Find(group.kind,group.slot);
    const bool included=GroupIncluded(c,group);
    for(std::size_t i=0;i<group.fields.size();++i){
        bool on=included;
        if(on&&choice)on=choice->fields.empty()||i>=choice->fields.size()||choice->fields[i];
        SendMessageW(fields,LB_SETSEL,on?TRUE:FALSE,static_cast<LPARAM>(i));
    }
}
void SyncFields(HWND d,Context& c,bool includeGroup=true) {
    if(c.shown<0||c.shown>=static_cast<int>(c.groups.size()))return;
    const auto& group=c.groups[c.shown];
    HWND fields=GetDlgItem(d,FieldList);
    const int selected=static_cast<int>(SendMessageW(fields,LB_GETSELCOUNT,0,0));
    // The bone entry's field list is the per-bone picker, so it writes its own
    // mask and then derives the scope from it.
    if(group.kind==channel_bake::Kind::Bones){
        if(selected<=0){
            // No bone ticked: remove the channel without changing the dialog mode.
            SendMessageW(GetDlgItem(d,GroupList),LB_SETSEL,FALSE,static_cast<LPARAM>(c.shown));
            c.request.channels.Remove(channel_bake::Kind::Bones,0);
            return;
        }
        if(includeGroup)SendMessageW(GetDlgItem(d,GroupList),LB_SETSEL,TRUE,static_cast<LPARAM>(c.shown));
        auto& choice=c.request.channels.Touch(channel_bake::Kind::Bones,0,group.fields.size());
        choice.fields.assign(group.fields.size(),false);
        std::vector<int> indices(static_cast<std::size_t>(selected));
        SendMessageW(fields,LB_GETSELITEMS,static_cast<WPARAM>(selected),reinterpret_cast<LPARAM>(indices.data()));
        for(const int index:indices)
            if(index>=0&&index<static_cast<int>(choice.fields.size()))choice.fields[static_cast<std::size_t>(index)]=true;
        DeriveBoneScope(c,group);
        return;
    }
    // Clearing every sub-parameter drops the channel, and the channel list says
    // so: the two lists must never disagree about what the bake includes.
    if(selected<=0){
        SendMessageW(GetDlgItem(d,GroupList),LB_SETSEL,FALSE,static_cast<LPARAM>(c.shown));
        c.request.channels.Remove(group.kind,group.slot);
        return;
    }
    if(includeGroup)SendMessageW(GetDlgItem(d,GroupList),LB_SETSEL,TRUE,static_cast<LPARAM>(c.shown));
    auto& choice=c.request.channels.Touch(group.kind,group.slot,group.fields.size());
    choice.fields.assign(group.fields.size(),false);
    std::vector<int> indices(static_cast<std::size_t>(selected));
    SendMessageW(fields,LB_GETSELITEMS,static_cast<WPARAM>(selected),reinterpret_cast<LPARAM>(indices.data()));
    for(const int index:indices)
        if(index>=0&&index<static_cast<int>(choice.fields.size()))choice.fields[static_cast<std::size_t>(index)]=true;
}
void SyncGroups(HWND d,Context& c) {
    HWND groups=GetDlgItem(d,GroupList);
    const int count=static_cast<int>(SendMessageW(groups,LB_GETCOUNT,0,0));
    for(int i=0;i<count&&i<static_cast<int>(c.groups.size());++i)
        SetIncluded(c,c.groups[i],SendMessageW(groups,LB_GETSEL,static_cast<WPARAM>(i),0)>0);
}
const wchar_t* SettingMessage(Context& c,SettingsError e) {
    switch(e){
    case SettingsError::InvalidErp:return Text(c,L"ERP 必须是 0 到 1 之间的有限数值。",L"ERP must be a finite value from 0 to 1.",L"ERP は 0～1 の有限数値を入力してください。");
    case SettingsError::InvalidCfm:return Text(c,L"CFM 必须是非负有限数值。",L"CFM must be finite and nonnegative.",L"CFM は非負の有限数値を入力してください。");
    case SettingsError::InvalidSubsteps:return Text(c,L"物理子步数必须是 正整数。",L"Physics substeps must be a positive integer.",L"物理サブステップ数は 正の整数を入力してください。");
    case SettingsError::InvalidMultiplier:return Text(c,L"内部计算倍率必须是 正整数。",L"The simulation rate multiplier must be a positive integer.",L"内部計算倍率は 正の整数を入力してください。");
    case SettingsError::InvalidReferenceRate:
    case SettingsError::InvalidBaseRate:return Text(c,L"有效物理帧率须为可表示的正整数。",L"The effective physics rate must be a representable positive integer.",L"有効物理周波数は表現可能な正の整数です。");
    case SettingsError::InvalidIterations:return Text(c,L"迭代次数须为正整数。",L"Iterations must be a positive integer.",L"反復回数は正の整数です。");
    case SettingsError::InvalidName:return Text(c,L"预设名称不得超过 128 个字符或包含控制字符。",L"Use a preset name of up to 128 characters without control characters.",L"プリセット名は制御文字を含まない 128 文字以内にしてください。");
    case SettingsError::IoRead:return Text(c,L"无法读取预设文件。请检查文件路径和读取权限。",L"Cannot read the preset. Check the path and read permissions.",L"プリセットを読み込めません。パスと読み取り権限を確認してください。");
    case SettingsError::IoWrite:return Text(c,L"无法保存预设。请检查目标文件夹的写入权限。原文件未更改。",L"Cannot save the preset. Check write permissions. The original file was not changed.",L"保存できません。書き込み権限を確認してください。元のファイルは変更されていません。");
    case SettingsError::UnsupportedVersion:return Text(c,L"此预设的格式或版本不受支持。",L"This preset format or version is not supported.",L"このプリセット形式またはバージョンには対応していません。");
    case SettingsError::TooLarge:return Text(c,L"预设文件超过 64 KiB 的大小限制。",L"The preset exceeds the 64 KiB size limit.",L"プリセットが 64 KiB の上限を超えています。");
    default:return Text(c,L"预设内容无效。请检查 UTF-8 JSON 格式、重复或缺失字段及参数类型。当前参数未更改。",L"Invalid preset. Check the UTF-8 JSON, duplicate or missing fields, and value types. Current settings were not changed.",L"プリセットが無効です。UTF-8 JSON、重複または不足した項目と型を確認してください。現在の設定は変更されていません。");
    }
}
void FillSettings(HWND d,Context& c) {
    c.controls.Set(c.request.settings);
    Set(d,Name,c.request.settings.name);
}
bool ReadSettings(HWND d,Context& c,Settings& s) {
    std::wstring message;
    if (!c.controls.Read(s,message)) { ErrorBox(d,c,message.c_str()); return false; }
    s.name=Value(d,Name);
    SettingsError error;
    if (!ValidateSettings(s,error)) { ErrorBox(d,c,SettingMessage(c,error)); return false; }
    return true;
}
void PresetFile(HWND d,Context& c,bool save) {
    Settings s;if(save&&!ReadSettings(d,c,s))return;
    wchar_t file[MAX_PATH]=L"physics-preset.json";OPENFILENAMEW ofn{sizeof ofn};ofn.hwndOwner=d;
    ofn.lpstrFilter=L"Physics preset (*.json)\0*.json\0All files (*.*)\0*.*\0\0";ofn.lpstrFile=file;ofn.nMaxFile=MAX_PATH;ofn.lpstrDefExt=L"json";
    ofn.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    ofn.lpstrTitle=save?Text(c,L"保存物理预设",L"Save physics preset",L"物理プリセットを保存"):Text(c,L"读取物理预设",L"Load physics preset",L"物理プリセットを読み込み");
    if(!(save?GetUiSaveFileNameW(&ofn):GetUiOpenFileNameW(&ofn)))return;
    SettingsError error;bool ok=save?SavePreset(file,s,error):LoadPreset(file,s,error);
    if(!ok){ErrorBox(d,c,SettingMessage(c,error));return;}
    c.request.settings=s;FillSettings(d,c);
}
INT_PTR CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
INT_PTR Dialog(HWND owner,Context& c,int height) {
    std::vector<WORD> data((sizeof(DLGTEMPLATE)+1)/2,0);auto* tpl=reinterpret_cast<DLGTEMPLATE*>(data.data());
    tpl->style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT|DS_CENTER;tpl->cx=c.channels?440:380;tpl->cy=static_cast<short>(height);
    data.push_back(0);data.push_back(0);data.push_back(0);data.push_back(9);
    const wchar_t font[]=L"Segoe UI";data.insert(data.end(),font,font+std::size(font));
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<DLGTEMPLATE*>(data.data()),owner,Proc,reinterpret_cast<LPARAM>(&c));
}
void InitAdvanced(HWND d,Context& c) {
    SetWindowTextW(d,Text(c,L"烘焙物理设置",L"Bake physics settings",L"ベイク物理設定"));
    c.controls.Create(d,c.app->EnglishUI(),12,12,353,c.request.settings);
    Label(d,Text(c,L"预设名称",L"Preset name",L"プリセット名"),12,223,80);
    Control(d,L"EDIT",c.request.settings.name.c_str(),WS_TABSTOP|ES_AUTOHSCROLL,Name,105,220,260,17);
    SendDlgItemMessageW(d,Name,EM_SETLIMITTEXT,128,0);
    Control(d,L"BUTTON",Text(c,L"读取预设…",L"Load preset…",L"読み込み…"),WS_TABSTOP|BS_PUSHBUTTON,Load,12,249,97,20);
    Control(d,L"BUTTON",Text(c,L"保存预设…",L"Save preset…",L"保存…"),WS_TABSTOP|BS_PUSHBUTTON,Save,118,249,97,20);
    Control(d,L"BUTTON",Text(c,L"确定",L"OK",L"OK"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,199,281,78,21);
    Control(d,L"BUTTON",Text(c,L"取消",L"Cancel",L"キャンセル"),WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL,287,281,78,21);
}
const wchar_t* ScopeNote(const Context& c) {
    if(IsPhysics(c))return IsModel(c)
        ?Text(c,L"写入当前模型。场景中的所有模型参与碰撞。",L"Writes to the current model. All scene models participate in collisions.",L"現在のモデルに記録します。全モデルが衝突に参加します。")
        :Text(c,L"烧录场景中所有模型的物理骨骼。",L"Bakes physics-driven bones on all scene models.",L"シーンの全モデルの物理ボーンを焼き込みます。");
    return IsModel(c)
        ?Text(c,L"当前模型：骨骼、表情、显示与 IK。",L"Current model: bones, morphs, visibility and IK.",L"現在のモデル：ボーン・モーフ・表示・IK。")
        :Text(c,L"场景：相机、灯光、自阴影、重力和附件。",L"Scene: camera, light, self shadow, gravity and accessories.",L"シーン：カメラ・照明・セルフ影・重力・アクセサリ。");
}
void InitMain(HWND d,Context& c) {
    SetWindowTextW(d,MainTitle(c));
    Label(d,Text(c,L"起始帧",L"Start frame",L"開始フレーム"),12,16,75);
    Control(d,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,Start,91,13,65,18);
    Label(d,Text(c,L"结束帧",L"End frame",L"終了フレーム"),169,16,75);
    Control(d,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,End,250,13,65,18);
    Control(d,L"BUTTON",Text(c,L"末帧",L"Last",L"最終"),WS_TABSTOP|BS_PUSHBUTTON,Last,325,12,40,20);

    if(IsPhysics(c)){
        Control(d,L"BUTTON",Text(c,L"物理设置",L"Physics settings",L"物理演算設定"),BS_GROUPBOX,-1,12,42,353,62);
        Control(d,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,Presets,24,56,215,75);
        SendDlgItemMessageW(d,Presets,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(Text(c,L"Bullet 默认",L"Bullet defaults",L"Bullet 既定値")));
        SendDlgItemMessageW(d,Presets,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"MMD 60 Hz"));
        SendDlgItemMessageW(d,Presets,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(Text(c,L"自定义 / 已读取预设",L"Custom / loaded preset",L"カスタム / 読み込み済み")));
        SendDlgItemMessageW(d,Presets,CB_SETCURSEL,0,0);
        Control(d,L"BUTTON",Text(c,L"高级设置…",L"Advanced…",L"詳細設定…"),WS_TABSTOP|BS_PUSHBUTTON,Advanced,247,54,106,22);
        Control(d,L"BUTTON",Text(c,L"视窗滚动预览",L"Viewport preview",L"ビューポートプレビュー"),WS_TABSTOP|BS_AUTOCHECKBOX,Preview,24,80,329,16);
        CheckDlgButton(d,Preview,c.request.preview?BST_CHECKED:BST_UNCHECKED);
        Tip(d,c,Preview,Text(c,L"随烘焙进度显示模拟结果。关闭预览不影响烧录结果。",L"Shows simulation samples as baking progresses. Turning preview off does not change the baked result.",L"焼き込みの進行に合わせて計算結果を表示します。無効にしても結果は変わりません。"));
        Tip(d,c,Presets,Text(c,L"Bullet 默认使用 ERP 0.2、CFM 0、1 个子步和 60 Hz。MMD 60 Hz 采用同样频率，不保证与原版运动结果逐位一致。",L"Bullet defaults: ERP 0.2, CFM 0, one substep, 60 Hz. MMD 60 Hz uses the same rate; motion is not guaranteed to match MMD bit for bit.",L"Bullet 既定値：ERP 0.2、CFM 0、サブステップ 1、60 Hz。MMD とのビット単位の一致は保証しません。"));
        Control(d,L"BUTTON",IsModel(c)
            ?Text(c,L"同时重采样骨骼关键帧",L"Also resample bone keys",L"ボーンキーも再サンプリング")
            :Text(c,L"同时重采样场景通道",L"Also resample scene channels",L"シーンチャンネルも再サンプリング"),
            WS_TABSTOP|BS_AUTOCHECKBOX,Keys,12,130,353,16);
    }
    const int y=OptionsRow(c.kind);
    Control(d,L"STATIC",ScopeNote(c),0,Info,12,IsPhysics(c)?111:43,353,16);
    Label(d,Text(c,L"步长",L"Step",L"ステップ"),12,y+4,42);
    Control(d,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,Step,56,y+1,52,18);
    Control(d,L"BUTTON",Text(c,L"通道…",L"Channels…",L"チャンネル…"),WS_TABSTOP|BS_PUSHBUTTON,Channels,120,y,110,22);
    if(IsModel(c))Control(d,L"BUTTON",Text(c,L"骨骼范围…",L"Bone scope…",L"ボーンの範囲…"),
        WS_TABSTOP|BS_PUSHBUTTON,Details,240,y,125,22);
    Control(d,L"STATIC",L"",SS_LEFT,ChannelSummary,12,y+28,353,16);
    Control(d,L"STATIC",L"",SS_LEFT,Status,12,y+50,353,14);
    HWND bar=Control(d,PROGRESS_CLASSW,L"",0,Bar,12,y+66,353,10);
    ShowWindow(bar,SW_HIDE);
    Control(d,L"BUTTON",Text(c,L"开始烧录",L"Start baking",L"焼き込み開始"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,181,y+86,96,23);
    Control(d,L"BUTTON",Text(c,L"取消",L"Cancel",L"キャンセル"),WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL,287,y+86,78,23);
    Set(d,Start,std::to_wstring(c.request.firstFrame));Set(d,End,std::to_wstring(c.request.lastFrame));
    Set(d,Step,std::to_wstring(c.request.step));
    Tip(d,c,Step,Text(c,L"每隔多少帧记录一个关键帧，最小为 1。",L"Frames between recorded keys, at least 1.",L"キーを記録する間隔（最小 1）。"));
    RefreshChannels(d,c);
    EnableInputs(d,c,true);
}
// Channel selection dialog.  Two native multi-select lists: the left one holds
// the channels of the current page, the right one the sub-parameters of the
// entry highlighted on the left.  Nothing is written here; the main dialog
// prepares and commits, so cancelling this dialog changes only the selection.
void InitChannels(HWND d,Context& c) {
    SetWindowTextW(d,Text(c,L"烧录通道",L"Channels to bake",L"焼き込むチャンネル"));
    Control(d,L"STATIC",Text(c,
        L"左栏选择通道，右栏选择该通道的子参数或单根骨骼。",
        L"Choose a channel on the left, then its parameters or individual bones on the right.",
        L"左でチャンネルを、右でパラメータやボーンを選びます。"),
        0,ChannelNote,12,10,416,26);
    Label(d,Text(c,L"通道",L"Channels",L"チャンネル"),12,42,200,12);
    Label(d,Text(c,L"子参数",L"Sub-parameters",L"子パラメータ"),236,42,192,12);
    HWND groups=Control(d,L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_EXTENDEDSEL,
        GroupList,12,56,212,186);
    Control(d,L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_EXTENDEDSEL,
        FieldList,236,56,192,186);
    Control(d,L"BUTTON",Text(c,L"当前全选",L"Select current",L"現在の項目を全選択"),WS_TABSTOP|BS_PUSHBUTTON,FieldAll,236,250,92,21);
    Control(d,L"BUTTON",Text(c,L"全部清除",L"Clear all",L"選択解除"),WS_TABSTOP|BS_PUSHBUTTON,FieldNone,336,250,92,21);
    Control(d,L"STATIC",L"",0,GroupLabel,12,250,212,40);
    Control(d,L"BUTTON",Text(c,L"全选所有通道",L"Select all channels",L"全チャンネルを選択"),WS_TABSTOP|BS_PUSHBUTTON,GroupAll,12,296,150,22);
    Control(d,L"BUTTON",Text(c,L"确定",L"OK",L"OK"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,270,296,76,22);
    Control(d,L"BUTTON",Text(c,L"取消",L"Cancel",L"キャンセル"),WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL,352,296,76,22);
    int caret=-1;
    for(std::size_t i=0;i<c.groups.size();++i){
        SendMessageW(groups,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(c.groups[i].label.c_str()));
        if(GroupIncluded(c,c.groups[i])){
            SendMessageW(groups,LB_SETSEL,TRUE,static_cast<LPARAM>(i));
            if(caret<0)caret=static_cast<int>(i);
        }
    }
    if(caret<0)caret=c.groups.empty()?-1:0;
    if(caret>=0)SendMessageW(groups,LB_SETCARETINDEX,static_cast<WPARAM>(caret),FALSE);
    ShowFields(d,c,caret);
    Set(d,GroupLabel,caret>=0?c.groups[static_cast<std::size_t>(caret)].label:std::wstring());
}
// Scope dialog of "Clear mesh bake...".  Only the scope is chosen here; the
// removal itself is native_mesh::ClearMeshBake, the same call the ABC export
// dialog offers on its own checkbox.  A scope whose cache is empty stays
// disabled instead of silently doing nothing.
void InitClear(HWND d,Context& c) {
    SetWindowTextW(d,Text(c,L"清除网格烧录",L"Clear mesh bake",L"ベイク解除"));
    const bool current=mesh_bake_cache::Has(c.app->SelectedModel());
    Control(d,L"BUTTON",Text(c,L"当前模型",L"Current model",L"現在のモデル"),
        WS_TABSTOP|WS_GROUP|BS_AUTORADIOBUTTON,ClearCurrent,12,14,353,16);
    Control(d,L"BUTTON",Text(c,L"全部对象",L"All objects",L"すべてのオブジェクト"),
        WS_TABSTOP|BS_AUTORADIOBUTTON,ClearAll,12,36,353,16);
    EnableWindow(GetDlgItem(d,ClearCurrent),current?TRUE:FALSE);
    EnableWindow(GetDlgItem(d,ClearAll),mesh_bake_cache::Empty()?FALSE:TRUE);
    CheckDlgButton(d,current?ClearCurrent:ClearAll,BST_CHECKED);
    Control(d,L"STATIC",Text(c,
        L"清除后骨骼、IK、表情与物理重新影响网格；原 FX 保持停用，可手动恢复。",
        L"After clearing, bones, IK, morphs and physics drive the mesh again. The original FX stay disabled and can be restored manually.",
        L"解除するとボーン・IK・モーフ・物理が再びメッシュに反映されます。元の FX は無効のままで、手動で戻せます。"),
        0,ClearNote,12,62,353,34);
    Control(d,L"BUTTON",Text(c,L"清除",L"Clear",L"解除"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,181,110,96,23);
    Control(d,L"BUTTON",Text(c,L"取消",L"Cancel",L"キャンセル"),WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL,287,110,78,23);
    EnableWindow(GetDlgItem(d,IDOK),current||!mesh_bake_cache::Empty()?TRUE:FALSE);
}
void EnableInputs(HWND d,Context& c,bool enabled) {
    const auto enable=[&](int id,bool on){if(HWND child=GetDlgItem(d,id))EnableWindow(child,enabled&&on);};
    for(int id:{Start,End,Last,Step,Preview,Presets,Advanced,IDOK})enable(id,true);
    enable(Keys,IsModel(c)||!c.groups.empty());
    enable(Details,BonesGroup(c.groups)!=nullptr);
    enable(Channels,!c.groups.empty()&&(!IsPhysics(c)||IsModel(c)||c.sceneKeyframes));
}
const wchar_t* BakeError(Context& c,Error e){switch(e){
    case Error::NoBones:return Text(c,L"没有可烧录的骨骼。请检查模型、所选骨骼及烧录选项。",L"No bones to bake. Check the model, bone selection, and bake options.",L"焼き込むボーンがありません。モデル、選択と設定を確認してください。");
    case Error::Capacity:return Text(c,L"关键帧数量超过模型容量。请缩短范围或减少目标骨骼。",L"The model's keyframe capacity would be exceeded. Reduce the range or target bones.",L"キーフレーム数が上限を超えます。範囲またはボーン数を減らしてください。");
    case Error::Allocation:return Text(c,L"内存不足，未写入任何烧录结果。",L"Not enough memory. No baked keys were written.",L"メモリが不足しています。結果は記録されていません。");
    default:return Text(c,L"无法完成烧录。请检查模型与参数。原动作和物理状态未更改。",L"Baking failed. Check the model and settings. The original motion and physics state were not changed.",L"焼き込みに失敗しました。モデルと設定を確認してください。元の動作と物理状態は変更されていません。");}}
void Run(HWND d,Context& c) {
    c.request.keepLayerEffects=false;
    if(!Integer(d,Start,c.request.firstFrame)||!Integer(d,End,c.request.lastFrame)||c.request.firstFrame>c.request.lastFrame) {
        ErrorBox(d,c,Text(c,L"起止帧须为有效的非负整数，起始帧不能大于结束帧。",L"Use valid nonnegative frame numbers, with start no later than end.",L"有効な非負のフレーム番号を入力し、開始を終了以前にしてください。"));return;
    }
    if(!Integer(d,Step,c.request.step)||c.request.step<1||c.request.step>frame_limits::maximum){
        ErrorBox(d,c,Text(c,L"步长须为有效的正整数。",L"Use a valid positive integer step.",L"ステップは有効な正の整数を入力してください。"));return;
    }
    const bool modelPage=IsModel(c);
    c.request.physics=IsPhysics(c);
    const bool scenePhysics=!modelPage&&c.request.physics;
    c.sceneKeyframes=!modelPage&&(!c.request.physics||IsDlgButtonChecked(d,Keys)==BST_CHECKED);
    const auto* bones=modelPage?BonesGroup(c.groups):nullptr;
    const bool bonesIncluded=bones&&GroupIncluded(c,*bones);
    c.request.keyframes=bonesIncluded&&(!c.request.physics||IsDlgButtonChecked(d,Keys)==BST_CHECKED);
    if(c.request.physics&&modelPage&&!bonesIncluded){ErrorBox(d,c,BakeError(c,Error::NoBones));return;}
    if(bonesIncluded)DeriveBoneScope(c,*bones);
    // The picker is resolved into a concrete bone list and handed to Compute, so
    // the bone transaction and the per-bone FX keys cover exactly the ticked
    // bones - physics only included.  An explicit list supersedes the coarse
    // "selected bones only" switch, which would otherwise intersect two
    // different notions of the target set.
    if(bonesIncluded){
        std::vector<int> targets;std::wstring boneMessage;
        const channel_bake::Error resolved=channel_bake::ResolveBones(*c.app,c.request.channels,targets,boneMessage);
        if(resolved!=channel_bake::Error::None){
            ErrorBox(d,c,boneMessage.empty()?channel_bake::ErrorText(*c.app,resolved):boneMessage.c_str());return;
        }
        const auto* choice=c.request.channels.Find(channel_bake::Kind::Bones,0);
        c.request.boneCoverage=choice?choice->coverage:channel_bake::Coverage::Whole;
        if(c.request.boneCoverage==channel_bake::Coverage::Explicit){
            c.request.boneIndices=targets;
            c.request.selectedOnly=false;
        }else{
            c.request.boneIndices.clear();
        }
    }
    const bool channels=(modelPage||c.sceneKeyframes)&&!c.request.channels.Empty();
    if(!c.request.physics&&!c.request.keyframes&&!channels){ErrorBox(d,c,Text(c,L"请至少选择一个通道。",L"Select at least one channel to bake.",L"焼き込むチャンネルを 1 つ以上選択してください。"));return;}
    bool existing=HasExistingKeys(c.app->SelectedModel(),c.request);
    if(scenePhysics){Request all=c.request;all.selectedOnly=false;all.boneCoverage=channel_bake::Coverage::Whole;all.boneIndices.clear();
        existing=false;for(int slot=0;slot<kModelSlotCount;++slot)if(auto* raw=c.app->ModelSlot(slot))existing=HasExistingKeys(raw,all)||existing;
    }
    if(existing) {
        const auto message=std::wstring(scenePhysics?Text(c,L"将覆盖全场景模型物理骨骼在以下范围内的关键帧：",L"Replace physics bone keys on all scene models in this range: ",L"全シーンモデルの物理ボーンで、次の範囲のキーを上書きします："):Text(c,L"将覆盖当前模型目标骨骼在以下范围内的关键帧：",L"Replace keys on the target bones of the current model in this range: ",L"現在のモデルの対象ボーンで、次の範囲のキーを上書きします："))+
            std::to_wstring(c.request.firstFrame)+L" – "+std::to_wstring(c.request.lastFrame);
        if(MessageBoxW(d,message.c_str(),Text(c,L"覆盖关键帧",L"Replace keyframes",L"キーの上書き"),MB_OKCANCEL|MB_ICONWARNING)!=IDOK)return;
    }
    c.running=true;c.cancel=false;EnableInputs(d,c,false);ShowWindow(GetDlgItem(d,Bar),SW_SHOW);
    // The interface is locked for the whole simulation and the commit, like
    // MMD's recording: this dialog stays enabled and keeps Cancel usable while
    // every other window of the thread stops taking input.  Completion,
    // cancellation, failure and an unexpected exception all restore the inputs
    // and c.running through Finish.
    std::unique_ptr<ScopedUiLock> lock;
    const auto Finish=[&]{
        lock.reset();
        c.running=false;EnableInputs(d,c,true);ShowWindow(GetDlgItem(d,Bar),SW_HIDE);
        if(c.request.preview){
            c.app->state.messageSeen=1;
            InvalidateRect(c.app->MainWindow(),nullptr,FALSE);
            if(c.app->FloatingWindow())InvalidateRect(c.app->FloatingWindow(),nullptr,FALSE);
        }
    };
    channel_bake::Plan plan;
    bool allTracksWritten=false;
    try{
        lock=std::make_unique<ScopedUiLock>(d);
        // The channel bake is prepared first: it reads and resamples on private
        // copies and refuses an invalid range, a missing track or a pool that
        // cannot hold the baked keys, so a refused bake never rewrites the
        // tracks that come before the failing one.  The physics simulation then
        // runs on its own private world, exactly as it did before, and the
        // commit order below keeps the bone transaction and the channel
        // transaction recoverable on a failed commit.
        Result result;SceneResult sceneResult;
        const auto progress=[&](int frame,int last){
            SendDlgItemMessageW(d,Bar,PBM_SETRANGE32,0,std::max(1,last));SendDlgItemMessageW(d,Bar,PBM_SETPOS,frame,0);
            const auto prefix=c.request.physics?Text(c,L"模拟帧：",L"Simulating frame: ",L"計算中：")
                                               :Text(c,L"采样帧：",L"Sampling frame: ",L"サンプリング中：");
            Set(d,Status,std::wstring(prefix)+std::to_wstring(frame)+L" / "+std::to_wstring(last));
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
                if(msg.message==WM_QUIT){c.cancel=true;PostQuitMessage(static_cast<int>(msg.wParam));break;}
                if(!IsDialogMessageW(d,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            }return !c.cancel;
        };
        if(channels){
            Set(d,Status,Text(c,L"正在准备通道…",L"Preparing channels…",L"チャンネルを準備中…"));
            channel_bake::Request channelRequest;
            channelRequest.scope=c.request.channelScope;channelRequest.firstFrame=c.request.firstFrame;
            channelRequest.lastFrame=c.request.lastFrame;channelRequest.step=c.request.step;
            channelRequest.selection=c.request.channels;
            std::wstring message;
            const channel_bake::Error prepared=channel_bake::Prepare(*c.app,channelRequest,plan,message);
            if(prepared!=channel_bake::Error::None){
                Finish();ErrorBox(d,c,message.empty()?channel_bake::ErrorText(*c.app,prepared):message.c_str());return;}
        }
        if(c.request.physics||c.request.keyframes){
            if(scenePhysics){sceneResult=ComputeScene(*c.app,c.request,progress);result.error=sceneResult.error;}
            else result=Compute(*c.app,c.request,progress);
        }
        if(result.error==Error::Cancelled){Finish();Set(d,Status,Text(c,L"已取消，原动作未更改。",L"Cancelled. The original motion was not changed.",L"キャンセルしました。元の動作は変更されていません。"));return;}
        if(result.error==Error::LayerRefused){
            Finish();ErrorBox(d,c,result.layerMessage.empty()?Text(c,L"该层组合无法忠实烘焙，已拒绝以免丢失结果。",L"This layer combination cannot be baked faithfully; refused to avoid losing results.",L"この層の組み合わせは正確にベイクできないため中止しました。"):result.layerMessage.c_str());return;}
        if(result.error!=Error::None){Finish();ErrorBox(d,c,BakeError(c,result.error));return;}
        {
            if(plan.Valid()){
                std::wstring message;
                const channel_bake::Error committed=channel_bake::Commit(*c.app,plan,message);
                if(committed!=channel_bake::Error::None){
                    Finish();ErrorBox(d,c,message.empty()?channel_bake::ErrorText(*c.app,committed):message.c_str());return;}
            }
            if(c.request.physics||c.request.keyframes){
                if(!(scenePhysics?CommitScene(*c.app,sceneResult,c.app->CurrentFrame()):Commit(c.app->SelectedModel(),result,c.app->CurrentFrame()))){
                    // The bones could not be staged, so the channel tracks that were
                    // already written go back to their original values.
                    channel_bake::Rollback(*c.app,plan);
                    Finish();ErrorBox(d,c,BakeError(c,Error::Allocation));return;}
            }
        }
        allTracksWritten=true;
        c.app->SceneModified()=1;
        // Replace the last progress line with the completion status.
        Set(d,Status,Text(c,L"烧录完成。",L"Baking complete.",L"焼き込み完了。"));
        if(c.app->SelectedModel())SeekSelectedModelToCurrentFrame(c.app);
        InvalidateRect(c.app->MainWindow(),nullptr,FALSE);
    }
    catch(...){
        if(!allTracksWritten)channel_bake::Rollback(*c.app,plan);
        Finish();
        ErrorBox(d,c,Text(c,L"烧录未能完成：界面锁定或写入过程出错。请检查模型状态后重试。",L"The bake could not be completed: locking the interface or writing the result failed. Check the model state and try again.",L"焼き込みを完了できませんでした。インターフェースのロックまたは書き込みに失敗しました。モデルの状態を確認して再試行してください。"));
        return;
    }
    Finish();
    EndDialog(d,IDOK);
}
INT_PTR CALLBACK Proc(HWND d,UINT msg,WPARAM wp,LPARAM lp) {
    auto* c=reinterpret_cast<Context*>(GetWindowLongPtrW(d,DWLP_USER));
    if(msg==WM_INITDIALOG){c=reinterpret_cast<Context*>(lp);SetWindowLongPtrW(d,DWLP_USER,lp);
        c->tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,d,nullptr,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(c->tooltip,TTM_SETMAXTIPWIDTH,0,520);SendMessageW(c->tooltip,TTM_SETDELAYTIME,TTDT_AUTOPOP,15000);
        if(c->clear)InitClear(d,*c);
        else if(c->advanced)InitAdvanced(d,*c);
        else if(c->channels)InitChannels(d,*c);
        else if(c->details){
            SetWindowTextW(d,Text(*c,L"骨骼范围",L"Bone scope",L"ボーンの範囲"));
            Control(d,L"BUTTON",Text(*c,L"只烧录当前选中的骨骼",L"Bake selected bones only",L"選択したボーンだけを焼き込む"),WS_TABSTOP|BS_AUTOCHECKBOX,Selected,12,15,353,18);
            CheckDlgButton(d,Selected,c->request.selectedOnly?BST_CHECKED:BST_UNCHECKED);
            Label(d,Text(*c,L"在「通道…」中逐根指定骨骼时，使用通道中的选择。",L"An explicit bone list in Channels takes precedence.",L"「チャンネル…」で個別に指定したボーンが優先されます。"),12,43,353,24);
            Control(d,L"BUTTON",Text(*c,L"确定",L"OK",L"OK"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,199,88,78,22);
            Control(d,L"BUTTON",Text(*c,L"取消",L"Cancel",L"キャンセル"),WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL,287,88,78,22);
        }else InitMain(d,*c);return TRUE;}
    if(!c)return FALSE;
    if(!c->clear&&msg==WM_CTLCOLORDLG)return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
    if(!c->clear&&msg==WM_CTLCOLORSTATIC){
        wchar_t cls[16]{};
        GetClassNameW(reinterpret_cast<HWND>(lp),cls,static_cast<int>(std::size(cls)));
        if(!_wcsicmp(cls,L"STATIC")){
            auto dc=reinterpret_cast<HDC>(wp);
            SetTextColor(dc,GetSysColor(COLOR_BTNTEXT));
            SetBkColor(dc,GetSysColor(COLOR_3DFACE));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
        }
    }
    if(msg==WM_CLOSE){if(c->running)c->cancel=true;else EndDialog(d,IDCANCEL);return TRUE;}
    if(c && c->advanced && c->controls.Handle(msg,wp,lp))return TRUE;
    if(msg==WM_COMMAND){const int id=LOWORD(wp),code=HIWORD(wp);
        if(id==IDCANCEL){if(c->running)c->cancel=true;else EndDialog(d,IDCANCEL);return TRUE;}
        if(c->running)return TRUE;
        if(id==Preview&&code==BN_CLICKED&&GetDlgItem(d,Preview)){
            previewEnabled=c->request.preview=IsDlgButtonChecked(d,Preview)==BST_CHECKED;
            return TRUE;
        }
        if(c->clear){
            if(id==IDOK){const bool current=IsDlgButtonChecked(d,ClearCurrent)==BST_CHECKED;
                if(current?mesh_bake_cache::Has(c->app->SelectedModel()):!mesh_bake_cache::Empty()){
                    native_mesh::ClearMeshBake(c->app,current);RefreshMenu(c->app);
                }
                EndDialog(d,IDOK);}
            return TRUE;
        }
        if(c->channels){
            // Two multi-select lists: the left one is the channel list of the
            // current page, the right one the sub-parameters of the entry the
            // left list has the caret on.  Switching entries stores the fields
            // of the entry that was shown, and OK stores the last one.
            if(id==GroupAll){
                // This explicit global action includes every field and removes
                // the selected-bones filter; per-entry edits keep their scope.
                c->request.selectedOnly=false;
                HWND groups=GetDlgItem(d,GroupList);
                for(size_t row=0;row<c->groups.size();++row){
                    const auto& group=c->groups[row];
                    auto& choice=c->request.channels.Touch(group.kind,group.slot,group.fields.size());
                    choice.fields.assign(group.fields.size(),true);
                    choice.coverage=channel_bake::Coverage::Whole;choice.indices.clear();
                    SetIncluded(*c,group,true);
                    SendMessageW(groups,LB_SETSEL,TRUE,static_cast<LPARAM>(row));
                }
                ShowFields(d,*c,c->shown);
                return TRUE;
            }
            if(id==GroupList&&code==LBN_SELCHANGE){
                SyncFields(d,*c,false);
                SyncGroups(d,*c);
                int caret=static_cast<int>(SendMessageW(GetDlgItem(d,GroupList),LB_GETCARETINDEX,0,0));
                if(caret<0||caret>=static_cast<int>(c->groups.size()))caret=c->groups.empty()?-1:0;
                ShowFields(d,*c,caret);
                Set(d,GroupLabel,caret>=0?c->groups[static_cast<std::size_t>(caret)].label:std::wstring());
                return TRUE;
            }
            if(id==FieldAll||id==FieldNone){
                HWND fields=GetDlgItem(d,FieldList);const int count=static_cast<int>(SendMessageW(fields,LB_GETCOUNT,0,0));
                for(int i=0;i<count;++i)SendMessageW(fields,LB_SETSEL,id==FieldAll?TRUE:FALSE,static_cast<LPARAM>(i));
                SyncFields(d,*c);
                return TRUE;
            }
            if(id==IDOK){SyncFields(d,*c);SyncGroups(d,*c);EndDialog(d,IDOK);return TRUE;}
            return TRUE;
        }
        if(c->advanced){
            if(id==Save||id==Load){PresetFile(d,*c,id==Save);return TRUE;}
            if(id==IDOK){Settings settings;if(ReadSettings(d,*c,settings)){c->request.settings=settings;EndDialog(d,IDOK);}return TRUE;}
        }else if(c->details){if(id==IDOK){c->request.selectedOnly=IsDlgButtonChecked(d,Selected)==BST_CHECKED;EndDialog(d,IDOK);return TRUE;}}
        else {
            if(id==Last){Set(d,End,std::to_wstring(channel_bake::SceneLastFrame(*c->app)));return TRUE;}
            if(id==Keys&&code==BN_CLICKED&&IsPhysics(*c)){
                if(!IsModel(*c))c->sceneKeyframes=IsDlgButtonChecked(d,Keys)==BST_CHECKED;
                RefreshChannels(d,*c);
                EnableInputs(d,*c,true);
                return TRUE;
            }
            if(IsPhysics(*c)&&id==Presets&&code==CBN_SELCHANGE){
                const int selected=static_cast<int>(SendDlgItemMessageW(d,Presets,CB_GETCURSEL,0,0));
                if(selected>=0&&selected<2){c->request.settings=BulletDefaults();if(selected==1)c->request.settings.name=L"MMD 60 Hz";}
                return TRUE;
            }
            if((id==Advanced&&IsPhysics(*c))||(id==Details&&IsModel(*c)&&BonesGroup(c->groups))){
                Context child;child.app=c->app;child.request=c->request;child.kind=c->kind;
                child.advanced=id==Advanced;child.details=id==Details;
                if(Dialog(d,child,child.advanced?314:122)==IDOK){
                    c->request=child.request;
                    if(child.advanced)SendDlgItemMessageW(d,Presets,CB_SETCURSEL,2,0);
                    if(const auto* bones=BonesGroup(c->groups);bones&&GroupIncluded(*c,*bones))DeriveBoneScope(*c,*bones);
                    RefreshChannels(d,*c);
                }
                return TRUE;
            }
            if(id==Channels&&IsWindowEnabled(GetDlgItem(d,Channels))){
                Context child;child.app=c->app;child.request=c->request;child.kind=c->kind;child.channels=true;child.groups=c->groups;
                if(Dialog(d,child,330)==IDOK){c->request=child.request;RefreshChannels(d,*c);}
                return TRUE;
            }
            if(id==IDOK){Run(d,*c);return TRUE;}
        }
    }
    if(msg==WM_DESTROY&&c->tooltip){DestroyWindow(c->tooltip);c->tooltip=nullptr;}
    return FALSE;
}
}
// The baking commands share one top-level popup that sits directly to the
// left of the Help popup, in every language.  Help is located through one of its
// stable original commands, never through a fixed index: inserting in front of
// Help is exactly what moves Help one slot to the right.
//
// RefreshMenu runs for each popup opening and for each language change, so it
// recognises its own popup by its menu id, moves it only while it is not already
// in front of Help, and rewrites the captions instead of inserting a copy.
//
// MAINTENANCE: Help used to be top-level index 7, which stops being true as soon
// as this popup moves in front of it.  Every place in the port that needs the
// Help popup therefore resolves it through its About command (201) instead of an
// index: the positional title table and the "enhance model" submenu lookup of
// RefreshMenuLanguage in src/window/enhance_model_io.cpp, the startup state of
// that submenu in src/window/ui_init.cpp, and SetPhysicsMenuState in
// src/window/ui_model_reload.cpp.  Keep new lookups on that command too, because
// GetSubMenu(menu, 7) now returns this popup rather than Help.  No caller parks this popup any more: RefreshMenuLanguage ends by
// calling this function, which leaves the popup in front of Help for the whole
// pass.
namespace {
constexpr int kEditMenuIndex = 1;
// "About(&A)" (JP: version information), the last entry of the original Help
// popup and the only command id the resource templates place there.  It
// identifies the Help popup without relying on that popup's position.
constexpr UINT kHelpAboutCommand = 201;
// Id the previous build used for the entry it appended to the Edit popup.  It
// is not used to recognise our menu: 32100 also names the first skinning
// adapter entry, which lives in the View popup.
constexpr unsigned kLegacyEditBakeCommand = 32100;
// Top-level position of the popup carrying `id`, or -1.
int TopLevelPositionOf(HMENU menu,unsigned id) {
    const int count=GetMenuItemCount(menu);
    for(int i=0;i<count;++i) {
        MENUITEMINFOW item{sizeof item};item.fMask=MIIM_ID;
        if(GetMenuItemInfoW(menu,UINT(i),TRUE,&item)&&item.wID==id)return i;
    }
    return -1;
}
// Top-level position of the Help popup, or -1 when this bar has none.
int HelpMenuPosition(HMENU menu) {
    const int count=GetMenuItemCount(menu);
    for(int i=0;i<count;++i) {
        MENUITEMINFOW item{sizeof item};item.fMask=MIIM_ID|MIIM_SUBMENU;
        if(!GetMenuItemInfoW(menu,UINT(i),TRUE,&item)||!item.hSubMenu)continue;
        if(GetMenuState(item.hSubMenu,kHelpAboutCommand,MF_BYCOMMAND)!=UINT(-1))return i;
    }
    return -1;
}
void PutCommand(HMENU popup,unsigned id,const wchar_t* text,UINT state,int position=-1) {
    MENUITEMINFOW info{sizeof info};info.fMask=MIIM_ID|MIIM_STRING|MIIM_STATE;
    info.wID=id;info.dwTypeData=const_cast<wchar_t*>(text);info.fState=state;
    MENUITEMINFOW old{sizeof old};old.fMask=MIIM_ID;
    if(GetMenuItemInfoW(popup,id,FALSE,&old))SetMenuItemInfoW(popup,id,FALSE,&info);
    else InsertMenuItemW(popup,position<0?GetMenuItemCount(popup):position,TRUE,&info);
}
bool HasBonePool(const mdl::ModelRecord& model) {
    return model.boneTable&&model.boneKeys&&model.boneCount>0;
}
bool HasKeyframeTargets(const mdl::ModelRecord& model) {
    if(!HasBonePool(model))return false;
    for(unsigned bone=0;bone<model.boneCount;++bone)
        if(model.boneTable[bone].type!=mdl::BoneType::InertTip)return true;
    return false;
}
bool HasPhysicsTargets(const mdl::ModelRecord& model) {
    if(!HasBonePool(model)||!model.rigidTable)return false;
    for(unsigned i=0;i<model.rigidCount;++i){
        const auto& body=model.rigidTable[i];
        if(!body.staticFlag&&body.body&&body.boneIndex>=0&&unsigned(body.boneIndex)<model.boneCount&&
           model.boneTable[body.boneIndex].hasRigidBody)return true;
    }
    return false;
}
bool SceneTracksAvailable(MMDApp& app) {
    if(app.CameraKeys()||app.LightKeys()||app.ShadowKeys()||app.GravityKeys())return true;
    for(int slot=0;slot<255;++slot)if(app.AccessorySlot(slot))return true;
    return false;
}
bool CanOpenBake(MMDApp* app,BakeKind kind) {
    if(!app||UiLocked()||app->PlaybackActive()||app->RecordingWindow()||alembic_export::IsExporting())return false;
    const bool physics=kind==BakeKind::Physics;
    if(physics&&(!app->Physics()||!app->Physics()->world))return false;
    if(channel_bake::CurrentScope(*app)==channel_bake::Scope::Scene){
        if(!physics)return SceneTracksAvailable(*app); // Scene tracks need no model.
        for(int slot=0;slot<kModelSlotCount;++slot)
            if(auto* model=app->ModelSlot(slot);model&&HasPhysicsTargets(*mdl::Mdl(model)))return true;
        return false;
    }
    // As before, don't change live model keys while its mesh cache owns the
    // displayed result. Scene-scope behavior and mesh bake stay independent.
    const auto* selected=app->SelectedModel();
    if(!selected||mesh_bake_cache::Has(selected))return false;
    return physics?HasPhysicsTargets(*mdl::Mdl(selected)):HasKeyframeTargets(*mdl::Mdl(selected));
}
// One gate for the "Bake mesh..." entry and for a directly dispatched command.
bool CanBakeMesh(MMDApp* app) {
    return app && !UiLocked() && alembic_export::CanExport(app);
}
void ShowBakeDialog(MMDApp* app,BakeKind kind) {
    if(!CanOpenBake(app,kind))return;
    Context c;c.app=app;c.kind=kind;c.request.settings=BulletDefaults();
    c.request.physics=IsPhysics(c);
    c.request.preview=c.request.physics&&previewEnabled;
    c.request.channelScope=channel_bake::CurrentScope(*app);
    c.sceneKeyframes=!IsModel(c)&&!c.request.physics;
    c.request.firstFrame=std::max(0,app->CurrentFrame());
    c.request.lastFrame=std::max(c.request.firstFrame,channel_bake::SceneLastFrame(*app));
    std::wstring message;
    if(channel_bake::Enumerate(*app,c.request.channelScope,c.groups,message)!=channel_bake::Error::None)
        c.groups.clear();
    channel_bake::DefaultSelection(*app,c.request.channelScope,c.groups,c.request.channels);
    c.request.keyframes=IsModel(c)&&!c.request.physics;
    c.request.step=1;
    Dialog(app->MainWindow(),c,MainHeight(kind));
}
}
bool CanBake(MMDApp* app){return CanOpenBake(app,BakeKind::Keyframes);}
void RefreshMenu(MMDApp* app){
    if(!app||!app->MainWindow())return;HMENU menu=GetMenu(app->MainWindow());if(!menu)return;
    // The legacy entry is removed inside the Edit popup only, so an adapter
    // entry carrying the same id in another popup is left untouched.
    if(HMENU edit=GetSubMenu(menu,kEditMenuIndex))DeleteMenu(edit,kLegacyEditBakeCommand,MF_BYCOMMAND);
    Context c;c.app=app;const wchar_t* title=Text(c,L"烘焙",L"Baking",L"ベイク");
    MENUITEMINFOW entry{sizeof entry};entry.fMask=MIIM_SUBMENU;
    HMENU baking=GetMenuItemInfoW(menu,kBakingMenuId,FALSE,&entry)?entry.hSubMenu:nullptr;
    const int help=HelpMenuPosition(menu);
    const int current=TopLevelPositionOf(menu,kBakingMenuId);
    // Already directly in front of Help, or already the last popup of a bar
    // without a Help popup: nothing to move, so a repeated refresh cannot
    // append a second copy.
    const bool placed=current>=0&&(help>=0?current+1==help:current==GetMenuItemCount(menu)-1);
    if(!placed) {
        MENUITEMINFOW item{sizeof item};item.fMask=MIIM_ID|MIIM_SUBMENU|MIIM_STRING;
        item.wID=kBakingMenuId;item.dwTypeData=const_cast<wchar_t*>(title);
        if(!baking) {
            baking=CreatePopupMenu();if(!baking)return;
        } else {
            // RemoveMenu detaches the popup without destroying its submenu.  The
            // insertion point is looked up after the removal because Help shifts
            // left when the popup came from Help's own left.
            RemoveMenu(menu,UINT(current),MF_BYPOSITION);
        }
        item.hSubMenu=baking;
        const int refreshed=HelpMenuPosition(menu);
        const int target=refreshed>=0?refreshed:GetMenuItemCount(menu);
        if(!InsertMenuItemW(menu,UINT(target),TRUE,&item)&&
           !InsertMenuItemW(menu,UINT(GetMenuItemCount(menu)),TRUE,&item)){DestroyMenu(baking);return;}
        DrawMenuBar(app->MainWindow());
    }
    MENUITEMINFOW caption{sizeof caption};caption.fMask=MIIM_STRING;caption.dwTypeData=const_cast<wchar_t*>(title);
    SetMenuItemInfoW(menu,kBakingMenuId,FALSE,&caption);
    PutCommand(baking,kBakeMenuCommand,Text(c,L"烧录关键帧…",L"Bake keyframes…",L"キー焼き込み…"),
        CanBake(app)?MFS_ENABLED:MFS_DISABLED);
    PutCommand(baking,kBakePhysicsMenuCommand,Text(c,L"烧录物理…",L"Bake physics…",L"物理焼き込み…"),
        CanOpenBake(app,BakeKind::Physics)?MFS_ENABLED:MFS_DISABLED,1);
    // Mesh baking still owns optional ABC export; don't recreate the old peer.
    DeleteMenu(baking,kExportMenuCommand,MF_BYCOMMAND);
    // Mesh baking and optional ABC export share this dialog.
    PutCommand(baking,kBakeMeshMenuCommand,Text(c,L"烘焙网格…",L"Bake mesh…",L"メッシュ焼き込み…"),
        CanBakeMesh(app)?MFS_ENABLED:MFS_DISABLED);
    // Clearing only means something while a mesh is cached, and it must not run
    // while the timeline plays or an AVI/ABC export drives the same frames.
    // CanExport carries the export-in-progress flag that playback and the
    // recording window alone do not cover.
    PutCommand(baking,kClearMeshBakeMenuCommand,Text(c,L"清除网格烧录…",L"Clear mesh bake…",L"ベイク解除…"),
        !UiLocked() && !mesh_bake_cache::Empty() && alembic_export::CanExport(app)?MFS_ENABLED:MFS_DISABLED);
}
void ShowDialog(MMDApp* app){ShowBakeDialog(app,BakeKind::Keyframes);}
// The scope dialog stays reachable with an empty cache so the disabled scopes
// remain visible, but an export in flight must refuse it outright.
void ClearMeshBakeDialog(MMDApp* app){
    if(!app||!app->MainWindow()||UiLocked()||!alembic_export::CanExport(app))return;
    Context c;c.app=app;c.clear=true;Dialog(app->MainWindow(),c,142);
}
bool HandleCommand(MMDApp* app,unsigned command){
    if(command==kBakeMenuCommand){ShowDialog(app);return true;}
    if(command==kBakePhysicsMenuCommand){ShowBakeDialog(app,BakeKind::Physics);return true;}
    if(command==kExportMenuCommand){alembic_export::ShowDialog(app);return true;}
    // The mesh bake dialog belongs to alembic_export; it is refused here with
    // the same gate the menu entry uses, so a stale menu or a shortcut can never
    // start it while the scene is busy.
    if(command==kBakeMeshMenuCommand){
        if(CanBakeMesh(app))alembic_export::ShowBakeDialog(app);
        return true;
    }
    if(command==kClearMeshBakeMenuCommand){ClearMeshBakeDialog(app);return true;}
    return false;
}
}
