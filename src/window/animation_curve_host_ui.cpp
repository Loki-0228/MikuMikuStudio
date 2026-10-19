#include "mikumikustudio/frame_limits.hpp"
#include "curve_editor_selection.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/ui_translation.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/animation_curve_motion.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <cstdlib>
#include "mikumikustudio/global_key_layout.hpp"

namespace mikumikustudio { void RefreshAfterFrameApply(MMDApp*); }

namespace mikumikustudio::curve_host {
namespace {
const wchar_t* Text(unsigned lang,const wchar_t*ja,const wchar_t*en,const wchar_t*zh){return lang==0?ja:lang==1?en:zh;}
std::wstring Title(unsigned l,Kind k){
    switch(k){case Kind::Camera:return Text(l,L"カメラ",L"Camera",L"摄像机");
    case Kind::Light:return Text(l,L"照明",L"Light",L"灯光");case Kind::Shadow:return Text(l,L"セルフ影",L"Self shadow",L"自阴影");
    case Kind::Gravity:return Text(l,L"重力",L"Gravity",L"重力");default:return Text(l,L"アクセサリ",L"Accessory",L"附件");}
}
std::wstring Title(MMDApp&app,Kind k){return Title(app.EnglishUI(),k);}
RECT Rect(HWND owner,int id){RECT r{};GetWindowRect(GetDlgItem(owner,id),&r);MapWindowPoints(nullptr,owner,reinterpret_cast<POINT*>(&r),2);return r;}
void Move(HWND owner,int id,int x,int y,int w,int h){MoveWindow(GetDlgItem(owner,id),x,y,w,h,TRUE);}
void Refresh(MMDApp&app){
    if(app.Renderer())RefreshAfterFrameApply(&app);
    else {
        for(auto kind:{Kind::Camera,Kind::Light,Kind::Shadow,Kind::Gravity})Apply(&app,kind,app.CurrentFrame());
        for(int slot=0;slot<255;++slot)if(app.AccessorySlot(slot))Apply(&app,Kind::Accessory,app.CurrentFrame(),slot);
        if(app.SelectedModel()){
            morph_curve::Evaluate(app.SelectedModel(),app.CurrentFrame());
            bone_scale::Evaluate(app.SelectedModel(),app.CurrentFrame());
            bone_curve::Evaluate(app.SelectedModel(),app.CurrentFrame());
        }
    }
    const auto text=std::to_wstring(app.CurrentFrame());
    SetDlgItemTextW(app.MainWindow(),panel::kCurrentFrameEdit,text.c_str());
    InvalidateRect(app.MainWindow(),nullptr,FALSE);
}
// The frame-apply chain needs the renderer and the model's display and bone
// tables, so a caller without a live main window (the background regression)
// must not run it.
void RefreshMain(MMDApp&app){if(IsWindow(app.MainWindow()))Refresh(app);}
// Use the same playback command as the main window, including audio/physics.
void BindTransport(curve_editor::Session& session,MMDApp* app){
    session.theme.enabled=true;
    session.theme.text=RGB(app->UiTextRed(),app->UiTextGreen(),app->UiTextBlue());
    for(int i=0;i<35;++i)session.theme.colors[i]=app->ThemeColorAt(i);
    session.currentFrame=[app]{return app->PlaybackActive()?double(app->PlaybackCursorSeconds())*30.0:double(app->CurrentFrame());};
    session.isPlaying=[app]{return IsWindow(app->MainWindow())&&app->PlaybackActive()!=0;};
    session.setPlaying=[app](bool playing){
        if(!IsWindow(app->MainWindow())||playing==(app->PlaybackActive()!=0))return;
        const int frame=std::max(0,int(std::lround(double(app->PlaybackCursorSeconds())*30.0)));
        const auto startsAtCurrent=app->PlaybackStartsAtCurrentFrame();
        if(playing)app->PlaybackStartsAtCurrentFrame()=1;
        SendMessageW(app->MainWindow(),WM_COMMAND,MAKEWPARAM(panel::kPlayButton,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(app->MainWindow(),panel::kPlayButton)));
        app->PlaybackStartsAtCurrentFrame()=startsAtCurrent;
        if(!playing){app->CurrentFrame()=frame;RefreshMain(*app);}
    };
    const auto seek=session.seek;
    const auto stop=session.setPlaying;
    session.seek=[app,seek,stop](int frame){
        if(app->PlaybackActive())stop(false);
        if(seek)seek(std::clamp(frame,0,mikumikustudio::frame_limits::maximum));
    };
}
std::uint8_t& SelectionFlag(mdl::BoneKey& key){return key.allocated;}
std::uint8_t& SelectionFlag(mdl::MorphKey& key){return key.allocated;}
std::uint8_t& SelectionFlag(mdl::DisplayKey& key){return key.allocated;}
template<class Key> std::uint8_t& SelectionFlag(Key& key){return key.selected;}
template<class Key>
std::vector<int> NativeSelection(Key* keys,std::size_t capacity,std::size_t root,const std::vector<int>* selected){
    std::vector<int> frames;std::vector<std::size_t> chain;
    if(!keys||root>=capacity)return frames;
    std::size_t slot=root;
    for(std::size_t visited=0;visited<capacity;++visited){
        if(slot>=capacity||(!chain.empty()&&keys[slot].frame<=keys[chain.back()].frame))return {};
        chain.push_back(slot);
        if(!keys[slot].next)break;
        slot=keys[slot].next;
        if(visited+1==capacity)return {};
    }
    if(selected){
        curve_editor::FrameSelectionCursor wanted(*selected);
        for(auto index:chain)SelectionFlag(keys[index])=wanted.Contains(int(keys[index].frame));
    }
    for(auto index:chain){auto& key=keys[index];
        if(SelectionFlag(key))frames.push_back(int(key.frame));
    }
    return frames;
}
template<class Callback>
void BindSelection(curve_editor::Source& source,MMDApp* app,Callback callback){
    source.readKeySelection=[callback](const std::string& channel){return callback(channel,nullptr);};
    source.writeKeySelection=[app,callback](const std::string& channel,const std::vector<int>& frames){
        if(app->PlaybackActive())return;
        if(callback(channel,nullptr)==frames)return;
        callback(channel,&frames);
        // This updates selected keys, not a native timeline drag. Arming
        // TimelineSelectionChanged here makes viewport mouse moves re-seek
        // the model and discard manually edited bone rotation/translation.
        if(app->Renderer()){SelectionReeval(app);PanelPaint(app);}
        InvalidateRect(app->MainWindow(),nullptr,FALSE);
    };
}
std::wstring Wide(const std::string&text){
    int n=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);std::wstring out(n,0);
    if(n)MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),out.data(),n);return out;
}
bool Motion(MMDApp&app,Kind kind,int slot,HWND owner,bool save,const std::vector<curve_editor::Channel>*supplied,std::wstring&error){
    std::vector<curve_editor::Channel> channels;
    if(supplied)channels=*supplied;else if(!Read(app,kind,slot,channels,error))return false;
    wchar_t path[32768]{};OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=owner;
    dialog.lpstrFilter=L"VMD (*.vmd)\0*.vmd\0\0";dialog.lpstrFile=path;dialog.nMaxFile=32768;dialog.lpstrDefExt=L"vmd";
    std::wstring title=Title(app,kind)+L" · "+Text(app.EnglishUI(),save?L"VMD 保存":L"VMD 読込",save?L"Export VMD":L"Read VMD",save?L"导出 VMD":L"读取 VMD");
    if(kind==Kind::Accessory&&app.AccessorySlot(slot))title+=L" · "+text_encoding::Display(app.AccessorySlot(slot)->name);
    dialog.lpstrTitle=title.c_str();dialog.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if(!(save?GetUiSaveFileNameW(&dialog):GetUiOpenFileNameW(&dialog)))return false;
    curve_motion::Motion motion;motion.kind=static_cast<curve_motion::Kind>(kind);std::string detail;
    if(save){
        motion=curve_motion::CaptureChannels(motion.kind,channels);
        if(!curve_motion::Save(path,motion,detail)){error=Wide(detail);return false;}return true;
    }
    if(!curve_motion::Load(path,motion.kind,motion,detail)){error=Wide(detail);return false;}
    // Read into the explicit source, never infer the recipient from VMD names.
    // Matching channels replace this source's tracks; other channels survive.
    bool matched=false;
    for(const auto&c:channels)if(std::any_of(motion.tracks.begin(),motion.tracks.end(),[&](const auto&t){return t.id==c.id;}))matched=true;
    if(!curve_motion::RestoreChannels(motion,channels,detail)){error=Wide(detail);return false;}
    for(auto&c:channels){
        if(c.track.keys.empty())continue;
        if(c.track.keys.front().frame>0){
            auto base=c.track.keys.front();base.frame=0;c.track.keys.insert(c.track.keys.begin(),base);
            if(!c.nativeControls.empty())c.nativeControls.insert(c.nativeControls.begin(),{});
            if(!c.controlGeometry.empty())c.controlGeometry.insert(c.controlGeometry.begin(),{});
            if(!c.nativeOneFrameHolds.empty())c.nativeOneFrameHolds.insert(c.nativeOneFrameHolds.begin(),true);
        }
    }
    if(!matched){error=Text(app.EnglishUI(),L"一致するトラックがありません。",L"No matching tracks.",L"没有可匹配的轨道。");return false;}
    if(!Write(app,kind,slot,channels,error))return false;Refresh(app);return true;
}
// ---------------------------------------------------------------------------
// Expression (morph) sources.
//
// Every expression of the model's four user panels becomes one channel, and
// Channel::group is the panel minus one, so the editor's same-group,
// four-group and free comparison modes work on the data as it already is.  A
// panel-0 morph is a system morph: the timeline hides it and the editor must
// not offer it either, so it stays out of the list altogether.
// ---------------------------------------------------------------------------
constexpr int kMorphComboIds[kMorphLaneCount] = {
    panel::kMorphCombo0,panel::kMorphCombo1,panel::kMorphCombo2,panel::kMorphCombo3};
// The four selectors' left-hand "previous" arrows, at 507/512/517/522.  The
// expression band places each selector 16 px right of its own arrow (618
// against 602), and nothing else moves those arrows.
constexpr int kMorphPrevArrowIds[kMorphLaneCount] = {507,512,517,522};
constexpr int kMorphNextArrowIds[kMorphLaneCount] = {508,513,518,523};
constexpr int kMorphSliderIds[kMorphLaneCount] = {
    panel::kMorphSlider0,panel::kMorphSlider1,panel::kMorphSlider2,panel::kMorphSlider3};
constexpr int kMorphComboArrowOffset = 16;
constexpr int kMorphLauncherWidth = 22;
constexpr int kMorphLauncherHeight = 22;
// The selector keeps the rest of its original 92 px.
constexpr int kMorphComboNarrowWidth = 92 - kMorphLauncherWidth;
// The height the option band itself passes for all four selectors.  It is
// repeated here instead of reading it back, because a drop-down list's own
// rectangle is not the geometry the band asked for: only the band's number
// leaves the drop-down exactly as it was.
constexpr int kMorphComboHeight = 500;

// The collapsed selector shares space with arrows and the curve launcher;
// its popup can use the full width needed by the model's expression names.
void FitMorphDropDown(HWND combo){
    if(!combo)return;
    HDC dc=GetDC(combo);
    if(!dc)return;
    const auto font=reinterpret_cast<HFONT>(SendMessageW(combo,WM_GETFONT,0,0));
    HGDIOBJ previous=font?SelectObject(dc,font):nullptr;
    int textWidth=0;
    const int count=static_cast<int>(SendMessageW(combo,CB_GETCOUNT,0,0));
    for(int row=0;row<count;++row){
        const LRESULT length=SendMessageW(combo,CB_GETLBTEXTLEN,row,0);
        if(length==CB_ERR||length<0)continue;
        std::vector<wchar_t> text(static_cast<std::size_t>(length)+1);
        if(SendMessageW(combo,CB_GETLBTEXT,row,reinterpret_cast<LPARAM>(text.data()))==CB_ERR)continue;
        SIZE size{};
        if(GetTextExtentPoint32W(dc,text.data(),static_cast<int>(length),&size))
            textWidth=std::max(textWidth,static_cast<int>(size.cx));
    }
    if(previous&&previous!=HGDI_ERROR)SelectObject(dc,previous);
    ReleaseDC(combo,dc);
    RECT rect{};GetWindowRect(combo,&rect);
    int width=std::max({240,static_cast<int>(rect.right-rect.left),
                        textWidth+GetSystemMetrics(SM_CXVSCROLL)+24});
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if(GetMonitorInfoW(MonitorFromWindow(combo,MONITOR_DEFAULTTONEAREST),&monitor))
        width=std::min(width,static_cast<int>(monitor.rcWork.right-monitor.rcWork.left)-16);
    SendMessageW(combo,CB_SETDROPPEDWIDTH,width,0);
}
// One key of one INI beside the executable.  Only the values 1..3 are stored;
// anything else reads back as the same-group default.
constexpr wchar_t kSettingsSection[] = L"CurveEditor";
constexpr wchar_t kSettingsKey[] = L"MorphComparison";

// The morph table digest, with the same coverage the backend's own fingerprint
// uses: the count, every twenty-byte name, the panel and the type.  A model
// reloaded in place keeps the pointers and the count, so only this catches it.
std::uint64_t MorphTableDigest(const mdl::ModelRecord& record){
    std::uint64_t hash=1469598103934665603ull;
    const auto feed=[&hash](const void* bytes,std::size_t size){
        const auto* data=static_cast<const std::uint8_t*>(bytes);
        for(std::size_t index=0;index<size;++index){hash^=data[index];hash*=1099511628211ull;}
    };
    feed(&record.morphCount,sizeof(record.morphCount));
    const mdl::MorphRecord* morphs=record.morphs;
    if(morphs==nullptr)return hash;
    for(std::uint32_t index=0;index<record.morphCount;++index){
        feed(morphs[index].name,sizeof(morphs[index].name));
        feed(&morphs[index].panel,sizeof(morphs[index].panel));
        feed(&morphs[index].type,sizeof(morphs[index].type));
    }
    return hash;
}

bool ModelPresent(const MMDApp& app,const unsigned char* model){
    if(model==nullptr)return false;
    for(int slot=0;slot<kModelSlotCount;++slot)if(app.ModelSlot(slot)==model)return true;
    return false;
}

// Live and every captured field still equal.  The slot lookup runs first, so a
// caller that gets true may dereference the model safely; a recycled address
// passes the lookup and then fails on the table, bone or path comparison.
bool IdentityMatches(const MMDApp& app,const MorphModelIdentity& identity){
    if(identity.model==nullptr||!ModelPresent(app,identity.model))return false;
    const mdl::ModelRecord& record=*mdl::Mdl(identity.model);
    const bool samePool=identity.keyPoolIdentity
        ?identity.keyPoolIdentity==mdl::MorphKeyPoolIdentity(record.morphKeys)
        :identity.keys==record.morphKeys;
    if(identity.morphs!=record.morphs||!samePool||
       identity.morphCount!=record.morphCount)return false;
    if(identity.bones!=record.boneTable||identity.boneCount!=record.boneCount)return false;
    if(std::memcmp(identity.path,record.path,sizeof(record.path))!=0)return false;
    return identity.digest==MorphTableDigest(record);
}

// What a source reports when the object it captured is no longer the one it may
// touch.  A model at a recycled address is a different case from a deleted one,
// and a model that is merely no longer selected is a third; all are
// three-language.
std::wstring ModelGoneMessage(MMDApp& app,const MorphModelIdentity& identity){
    if(!ModelPresent(app,identity.model))
        return Text(app.EnglishUI(),L"モデルが読み込まれていません。",L"The model is no longer loaded.",L"模型已被移除。");
    if(!IdentityMatches(app,identity))
        return Text(app.EnglishUI(),L"モデルが読み込み直されました。",L"The model has been reloaded or replaced.",L"模型已被重新加载或替换。");
    return Text(app.EnglishUI(),L"選択中のモデルが変わりました。",L"The selected model has changed.",L"当前选中的模型已改变。");
}

std::wstring MorphTitle(unsigned language,const MorphModelIdentity& identity){
    const std::wstring tail=Text(language,L"表情カーブ",L"Expression curves",L"表情曲线");
    if(identity.model==nullptr||identity.morphs==nullptr)return tail;
    return text_encoding::ModelLabel(*mdl::Mdl(identity.model),UiUsesEnglishNames(static_cast<unsigned char>(language)))+L" · "+tail;
}

std::wstring MorphCaption(unsigned language,int lane){
    return std::wstring(morph_curve::PanelLabel(static_cast<std::uint8_t>(lane+1),language))+L" · "+
        Text(language,L"表情カーブを開く",L"Open expression curve",L"打开表情曲线");
}

// The four launchers are the four expression panels: lane i is morph_curve's
// group i, and the model keeps the four selectors in selectedMorphs[0..3].
static_assert(kMorphLaneCount==4,"the four expression lanes are the four morph panels");

// A lane is usable only when that panel's selector currently names a morph of
// that very panel.  "The panel holds some morph" is not enough: the launcher
// opens the editor on the selector's current morph, so a stale selection or one
// that belongs to another panel has nothing to open and must not be clickable.
unsigned MorphLaneMask(MMDApp& app){
    unsigned mask=0;
    for(int lane=0;lane<kMorphLaneCount;++lane)
        if(!MorphLaneInitial(app,lane).empty())mask|=1u<<lane;
    return mask;
}

bool SameCurveKey(const animation_curve::Key& left,const animation_curve::Key& right){
    return left.frame==right.frame&&left.value==right.value&&left.inSlope==right.inSlope&&
        left.outSlope==right.outSlope&&left.inWeight==right.inWeight&&left.outWeight==right.outWeight&&
        left.interpolation==right.interpolation&&left.tangent==right.tangent;
}

bool SameCurve(const animation_curve::Track& left,const animation_curve::Track& right){
    if(left.discrete!=right.discrete||left.keys.size()!=right.keys.size())return false;
    for(std::size_t index=0;index<left.keys.size();++index)
        if(!SameCurveKey(left.keys[index],right.keys[index]))return false;
    return true;
}

std::wstring MorphSettingsPath(){
    wchar_t executable[32768]{};
    const DWORD count=GetModuleFileNameW(nullptr,executable,32768);
    if(count==0||count>=32768)return {};
    std::wstring path(executable,count);
    const std::size_t slash=path.find_last_of(L"\\/");
    if(slash==std::wstring::npos)return {};
    path.resize(slash+1);
    path+=L"Data\\curve_editor.ini";
    return path;
}

struct MorphSourceState{MorphModelIdentity identity;std::vector<curve_editor::Channel> baseline;};

void ShowMorphControls(MMDApp* app){
    HWND hwnd=app->MainWindow();
    if(hwnd==nullptr)return;
    // The expression band is a model-mode band: it collapses with the camera
    // panels, and its own expand toggle is optflag[5] (the same flag the
    // 504..528 visibility group in PostModelReload2 uses).
    const bool visible=app->state.optflag[0]==0&&app->state.optflag[5]!=0;
    const unsigned mask=visible?MorphLaneMask(*app):0u;
    for(int lane=0;lane<kMorphLaneCount;++lane){
        HWND button=GetDlgItem(hwnd,kMorphCurveLane[lane]);
        if(button==nullptr)continue;
        if(!visible){ShowWindow(button,SW_HIDE);continue;}
        ShowWindow(button,SW_SHOWNA);
        EnableWindow(button,(mask&(1u<<lane))!=0);
    }
}
LRESULT CALLBACK Parent(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data){
    auto*app=reinterpret_cast<MMDApp*>(data);
    if(msg==WM_MOUSEWHEEL){
        // Buttons forward wheel input to their parent. Keep the selector's
        // native wheel behavior over the curve launcher that shares its row.
        // Modified wheel messages are ignored by combos and sent back here.
        const POINT point{static_cast<short>(LOWORD(lp)),static_cast<short>(HIWORD(lp))};
        for(int lane=0;lane<kMorphLaneCount;++lane){
            HWND button=GetDlgItem(hwnd,kMorphCurveLane[lane]);RECT rect{};
            if(!button||(GetWindowLongPtrW(button,GWL_STYLE)&WS_VISIBLE)==0||
               !GetWindowRect(button,&rect)||!PtInRect(&rect,point))continue;
            HWND combo=GetDlgItem(hwnd,kMorphComboIds[lane]);
            if(IsWindowEnabled(hwnd)&&IsWindowEnabled(button)&&IsWindowEnabled(combo)&&
               app->state.optflag[0]==0&&app->state.optflag[5]!=0&&
               (LOWORD(wp)&(MK_CONTROL|MK_SHIFT))==0&&
               static_cast<short>(HIWORD(wp))!=0&&SendMessageW(combo,CB_GETCOUNT,0,0)>0)
                SendMessageW(combo,WM_MOUSEWHEEL,wp,lp);
            return 0;
        }
    }
    if(msg==WM_COMMAND&&HIWORD(wp)==CBN_DROPDOWN){
        for(int lane=0;lane<kMorphLaneCount;++lane)
            if(LOWORD(wp)==kMorphComboIds[lane])FitMorphDropDown(reinterpret_cast<HWND>(lp));
    }
    if(msg==WM_COMMAND&&LOWORD(wp)==curve_editor::kAccessoryCurve){Open(app);return 0;}
    if(msg==WM_COMMAND&&LOWORD(wp)==curve_editor::kInterpolationCurve){OpenInterpolation(app);return 0;}
    if(msg==WM_COMMAND){for(int lane=0;lane<kMorphLaneCount;++lane)
        if(LOWORD(wp)==kMorphCurveLane[lane]){OpenMorph(app,lane);return 0;}}
    if(msg==WM_COMMAND&&LOWORD(wp)==curve_editor::kAccessoryMotionMenu){
        std::wstring error;const int slot=app->SelectedAccessorySlot();
        if(app->AccessorySlot(slot))Motion(*app,Kind::Accessory,slot,hwnd,false,nullptr,error);
        else error=Text(app->EnglishUI(),L"アクセサリを選択してください。",L"Select an accessory first.",L"请先选择附件。");
        if(!error.empty())MessageBoxW(hwnd,error.c_str(),L"VMD",MB_OK|MB_ICONWARNING);return 0;
    }
    if(msg==WM_INITMENUPOPUP){Localize(app);EnableMenuItem(GetMenu(hwnd),curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND|(app->AccessorySlot(app->SelectedAccessorySlot())?MF_ENABLED:MF_GRAYED));}
    if(msg==WM_DRAWITEM){auto*d=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if(d){bool launcher=d->CtlID==curve_editor::kAccessoryCurve||d->CtlID==curve_editor::kInterpolationCurve;
            for(int lane=0;lane<kMorphLaneCount&&!launcher;++lane)launcher=d->CtlID==kMorphCurveLane[lane];
            if(launcher){curve_editor::DrawLauncher(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right-d->rcItem.left,d->rcItem.bottom-d->rcItem.top,
                (d->itemState&ODS_SELECTED)!=0,(d->itemState&ODS_DISABLED)!=0,(d->itemState&ODS_FOCUS)!=0);return TRUE;}
        }
    }
    if(msg==WM_NCDESTROY){Reset(app);RemoveWindowSubclass(hwnd,Parent,curve_editor::kAccessoryCurve);}
    return DefSubclassProc(hwnd,msg,wp,lp);
}
}
static void OpenScene(MMDApp*app,bool visible,const std::string&cameraChannel){
    if(!app)return;
    curve_editor::Session session;session.id="mms.scene";session.language=app->EnglishUI();
    session.currentFrame=[app]{return double(app->CurrentFrame());};
    session.seek=[app](int f){app->CurrentFrame()=f;Refresh(*app);};
    auto append=[&](Kind kind,int slot){
        auto*identity=kind==Kind::Accessory?app->AccessorySlot(slot):nullptr;
        curve_editor::Source source;source.id=std::to_string(int(kind))+"/"+std::to_string(slot);source.label=Title(*app,kind);
        source.linkAdjacentControls=[app]{return IsDlgButtonChecked(app->MainWindow(),panel::kPhysicsFrameCheckbox)==BST_CHECKED;};
        if(identity)source.label+=L" · "+text_encoding::Display(identity->name);
        auto alive=[app,slot,kind,identity]{return IsWindow(app->MainWindow())&&(kind!=Kind::Accessory||app->AccessorySlot(slot)==identity);};
        BindSelection(source,app,[app,kind,slot,alive](const std::string&,const std::vector<int>* frames)->std::vector<int>{
            if(!alive())return {};
            switch(kind){
            case Kind::Camera:return NativeSelection(app->CameraKeys(), mdl::KeyPoolCapacity(app->CameraKeys()),0,frames);
            case Kind::Light:return NativeSelection(app->LightKeys(), mdl::KeyPoolCapacity(app->LightKeys()),0,frames);
            case Kind::Shadow:return NativeSelection(app->ShadowKeys(), mdl::KeyPoolCapacity(app->ShadowKeys()),0,frames);
            case Kind::Gravity:return NativeSelection(app->GravityKeys(), mdl::KeyPoolCapacity(app->GravityKeys()),0,frames);
            default:return NativeSelection(app->AccessoryKeys(slot), mdl::KeyPoolCapacity(app->AccessoryKeys(slot)),0,frames);
            }
        });
        source.localizedLabel=[app,kind,slot,alive](unsigned language){
            auto label=Title(language,kind);
            if(kind==Kind::Accessory&&alive())label+=L" · "+text_encoding::Display(app->AccessorySlot(slot)->name);
            return label;
        };
        source.read=[app,kind,slot,alive](auto&channels,auto&error){
            if(!alive()){error=L"The source has been removed.";return false;}
            if(!Read(*app,kind,slot,channels,error))return false;
            std::wstring object=Title(*app,kind);
            if(kind==Kind::Accessory)object+=L" · "+text_encoding::Display(app->AccessorySlot(slot)->name);
            for(auto&channel:channels){
                const std::string category=channel.id=="x"||channel.id=="y"||channel.id=="z"?"position":
                    channel.id=="rx"||channel.id=="ry"||channel.id=="rz"?"rotation":channel.id=="scale"?"scale":"parameters";
                const auto label=category=="position"?(kind==Kind::Light||kind==Kind::Gravity?Text(app->EnglishUI(),L"方向",L"Direction",L"方向"):Text(app->EnglishUI(),L"移動",L"Position",L"位移")):
                    category=="rotation"?Text(app->EnglishUI(),L"回転",L"Rotation",L"旋转"):
                    category=="scale"?Text(app->EnglishUI(),L"拡大",L"Scale",L"缩放"):Text(app->EnglishUI(),L"パラメータ",L"Parameters",L"参数");
                channel.collections={{"object",object},{category,label}};
            }
            return true;};
        source.apply=[app,kind,slot,alive](const auto&channels,auto&error){
            if(!alive()){error=L"The source has been removed.";return false;}
            if(!IsWindowEnabled(app->MainWindow())){error=Text(app->EnglishUI(),L"処理の終了を待ってください。",L"Wait for the active operation to finish.",L"请等待当前操作完成。");return false;}
            if(!Write(*app,kind,slot,channels,error))return false;Refresh(*app);return true;};
        if(kind==Kind::Camera){
            source.initialMainChannel=cameraChannel;
            source.insertPreservingShape=InsertCameraKey;
        }
        source.readMotion=[app,kind,slot,alive](void*owner,std::wstring&error){return alive()&&Motion(*app,kind,slot,static_cast<HWND>(owner),false,nullptr,error);};
        source.writeMotion=[app,kind,slot,alive](void*owner,const auto&channels,std::wstring&error){return alive()&&Motion(*app,kind,slot,static_cast<HWND>(owner),true,&channels,error);};
        if(kind==Kind::Accessory&&slot==app->SelectedAccessorySlot())session.initialSource=session.sources.size();
        session.sources.push_back(std::move(source));
    };
    for(auto k:{Kind::Camera,Kind::Light,Kind::Shadow,Kind::Gravity})append(k,0);
    for(int i=0;i<255;++i)if(app->AccessorySlot(i))append(Kind::Accessory,i);
    if(!cameraChannel.empty())session.initialSource=0;
    BindTransport(session,app);curve_editor::Open(app->Hwnd(),std::move(session),visible);
}
void Open(MMDApp*app,bool visible){OpenScene(app,visible,{});}
bool ReadMorphChannels(MMDApp&app,std::vector<curve_editor::Channel>&out,std::wstring&error){
    out.clear();
    const unsigned language=app.EnglishUI();
    if(app.SelectedModel()==nullptr){
        error=Text(language,L"モデルが読み込まれていません。",L"No model is loaded.",L"没有加载模型。");
        return false;
    }
    const auto infos=morph_curve::Enumerate(app,language);
    std::vector<int> indices;
    for(const auto&info:infos)if(morph_curve::PanelGroup(info.panel)>=0)indices.push_back(info.index);
    std::vector<animation_curve::Track> tracks;
    if(!morph_curve::ReadMany(app,indices,tracks,error))return false;
    std::size_t next=0;
    for(const auto&info:infos){
        const int group=morph_curve::PanelGroup(info.panel);
        // A system morph (panel 0) belongs to no comparison group.  The four
        // timeline panels hide it and the editor must not offer it either, so
        // it never becomes a channel.
        if(group<0)continue;
        auto& track=tracks[next++];
        curve_editor::Channel channel;
        channel.id=info.id;
        channel.label=info.label;
        channel.base=track.keys.empty()?0.0:track.keys.front().value;
        channel.minimum=0.0;channel.maximum=1.0;
        channel.selected=true;
        channel.group=group;
        channel.track=std::move(track);
        out.push_back(std::move(channel));
    }
    if(out.empty()){
        error=Text(language,L"編集できる表情がありません。",L"This model has no expression to edit.",L"该模型没有可编辑的表情。");
        return false;
    }
    return true;
}
std::vector<animation_curve::NamedTrack> ChangedMorphTracks(
    const std::vector<curve_editor::Channel>&next,
    const std::vector<curve_editor::Channel>&baseline){
    std::vector<animation_curve::NamedTrack> changed;
    for(std::size_t index=0;index<next.size();++index){
        if(index<baseline.size()&&baseline[index].id==next[index].id&&SameCurve(baseline[index].track,next[index].track))continue;
        changed.push_back({next[index].id,next[index].track});
    }
    return changed;
}
MorphModelIdentity MorphIdentityOf(const MMDApp&app,unsigned char*model){
    MorphModelIdentity identity;
    // An unknown model yields the all-zero identity, which matches nothing.
    if(model==nullptr||!ModelPresent(app,model))return identity;
    const mdl::ModelRecord& record=*mdl::Mdl(model);
    identity.model=model;
    identity.morphs=record.morphs;
    identity.keys=record.morphKeys;
    identity.keyPoolIdentity=mdl::MorphKeyPoolIdentity(record.morphKeys);
    identity.morphCount=record.morphCount;
    identity.bones=record.boneTable;
    identity.boneCount=record.boneCount;
    std::memcpy(identity.path,record.path,sizeof(record.path));
    identity.digest=MorphTableDigest(record);
    return identity;
}
bool MorphIdentityCurrent(const MMDApp&app,const MorphModelIdentity&identity){
    if(!IdentityMatches(app,identity))return false;
    // The write path targets the selected model, so the captured one has to be
    // it.  A live but unselected capture is refused rather than redirected.
    return app.SelectedModel()==identity.model;
}
std::string MorphLaneInitial(MMDApp&app,int lane){
    if(lane<0||lane>=kMorphLaneCount)return {};
    unsigned char* model=app.SelectedModel();
    if(model==nullptr)return {};
    const mdl::ModelRecord& record=*mdl::Mdl(model);
    const mdl::MorphRecord* morphs=record.morphs;
    if(morphs==nullptr)return {};
    const std::int32_t selected=record.selectedMorphs[lane];
    if(selected<0||static_cast<std::uint32_t>(selected)>=record.morphCount)return {};
    // Lane i is panel group i, so a selection that belongs to another panel is
    // not this lane's current morph.
    if(morph_curve::PanelGroup(static_cast<std::uint8_t>(morphs[selected].panel))!=lane)return {};
    return morph_curve::StableId(selected,morphs[selected].name);
}
int ReadMorphComparisonMode(){
    const std::wstring path=MorphSettingsPath();
    if(path.empty())return curve_editor::kCompareSameGroup;
    const int stored=static_cast<int>(GetPrivateProfileIntW(kSettingsSection,kSettingsKey,-1,path.c_str()));
    return stored>=curve_editor::kCompareSameGroup&&stored<=curve_editor::kCompareFree
        ?stored:curve_editor::kCompareSameGroup;
}
bool WriteMorphComparisonMode(unsigned language,int mode,std::wstring&error){
    if(mode<curve_editor::kCompareSameGroup||mode>curve_editor::kCompareFree){
        error=Text(language,L"比較モードが不正です。",L"The comparison mode is not valid.",L"比较模式不合法。");
        return false;
    }
    const std::wstring path=MorphSettingsPath();
    if(path.empty()){
        error=Text(language,L"設定ファイルの場所を特定できません。",L"The settings file cannot be located.",L"无法确定设置文件的位置。");
        return false;
    }
    const std::wstring text=std::to_wstring(mode);
    if(!WritePrivateProfileStringW(kSettingsSection,kSettingsKey,text.c_str(),path.c_str())){
        error=Text(language,L"設定を書き込めませんでした。",L"The setting could not be written.",L"无法写入设置。");
        return false;
    }
    // Flush the profile cache so a later read in this process sees the file.
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());
    return true;
}
bool WriteMorphChannels(MMDApp&app,const MorphModelIdentity&identity,
                        const std::vector<curve_editor::Channel>&next,
                        const std::vector<curve_editor::Channel>&baseline,std::wstring&error){
    if(!MorphIdentityCurrent(app,identity)){
        error=ModelGoneMessage(app,identity);
        return false;
    }
    const auto changed=ChangedMorphTracks(next,baseline);
    // Nothing to write: returning here is what keeps a plain look at the window
    // from registering every native track as an authored curve.
    if(changed.empty())return true;
    // The identity check above proves the selected model is the captured one,
    // which is exactly the model the backend's MMDApp overload writes to.
    if(!morph_curve::Write(app,changed,error))return false;
    RefreshMain(app);
    return true;
}
void OpenMorph(MMDApp*app,int lane,bool visible){
    if(app==nullptr||lane<0||lane>=kMorphLaneCount)return;
    unsigned char*model=app->SelectedModel();
    if(model==nullptr)return;
    const mdl::ModelRecord*record=mdl::Mdl(model);
    if(record==nullptr||record->morphs==nullptr||record->morphCount==0)return;
    // Capture the whole identity once.  Every callback below re-checks it
    // before it dereferences the model, so a deleted model whose address was
    // handed to a different one is refused instead of read or written.
    const MorphModelIdentity identity=MorphIdentityOf(*app,model);
    if(identity.model==nullptr)return;
    // Open on the morph the addressed panel selector currently names.  When it
    // names none of that panel there is nothing to open, and the editor must
    // not be left to fall back to the first channel of the list.
    const std::string initial=MorphLaneInitial(*app,lane);
    if(initial.empty())return;

    curve_editor::Session session;session.id="mms.morph";session.language=app->EnglishUI();
    session.currentFrame=[app]{return double(app->CurrentFrame());};
    session.seek=[app,identity](int f){
        if(!MorphIdentityCurrent(*app,identity))return;
        app->CurrentFrame()=f;RefreshMain(*app);};

    auto state=std::make_shared<MorphSourceState>();
    state->identity=identity;

    curve_editor::Source source;
    source.id="morph";
    BindSelection(source,app,[app,identity](const std::string& channel,const std::vector<int>* frames)->std::vector<int>{
        if(!MorphIdentityCurrent(*app,identity)||channel.empty()||channel[0]!='m')return {};
        char* end=nullptr;const long morph=std::strtol(channel.c_str()+1,&end,10);
        if(!end||*end!=':'||morph<0||std::uint32_t(morph)>=mdl::Mdl(identity.model)->morphCount)return {};
        return NativeSelection(mdl::MorphKeys(identity.model),mdl::MorphKeyCapacity(mdl::MorphKeys(identity.model)),std::size_t(morph),frames);
    });
    source.linkAdjacentControls=[app]{return IsDlgButtonChecked(app->MainWindow(),panel::kPhysicsFrameCheckbox)==BST_CHECKED;};
    source.label=MorphTitle(app->EnglishUI(),identity);
    source.localizedLabel=[app,identity](unsigned language){
        return IdentityMatches(*app,identity)?MorphTitle(language,identity)
                                            :std::wstring(Text(language,L"表情",L"Expressions",L"表情"));};
    source.read=[app,state](std::vector<curve_editor::Channel>&channels,std::wstring&error){
        if(!MorphIdentityCurrent(*app,state->identity)){
            error=ModelGoneMessage(*app,state->identity);
            return false;}
        if(!ReadMorphChannels(*app,channels,error))return false;
        static const wchar_t*const zh[]={L"眉",L"眼",L"口",L"其他"};
        static const wchar_t*const en[]={L"Eyebrows",L"Eyes",L"Mouth",L"Other"};
        static const wchar_t*const ja[]={L"眉",L"目",L"口",L"その他"};
        for(auto&channel:channels){const int group=std::clamp(channel.group,0,3);
            const auto label=app->EnglishUI()==0?ja[group]:app->EnglishUI()==1?en[group]:zh[group];
            channel.collections={{"object",text_encoding::ModelLabel(*mdl::Mdl(state->identity.model),UiUsesEnglishNames(app->EnglishUI()))},{"expression"+std::to_string(group),label}};
        }
        // The baseline is what the editor was handed, so the apply below can
        // tell an edit from an untouched curve.
        state->baseline=channels;
        return true;};
    source.apply=[app,state](const std::vector<curve_editor::Channel>&channels,std::wstring&error){
        if(!MorphIdentityCurrent(*app,state->identity)){
            error=ModelGoneMessage(*app,state->identity);
            return false;}
        if(!IsWindowEnabled(app->MainWindow())){error=Text(app->EnglishUI(),L"処理の終了を待ってください。",L"Wait for the active operation to finish.",L"请等待当前操作完成。");return false;}
        if(!WriteMorphChannels(*app,state->identity,channels,state->baseline,error))return false;
        state->baseline=channels;
        return true;};
    source.comparisonEnabled=true;
    source.initialMainChannel=initial;
    source.readComparisonMode=[]{return ReadMorphComparisonMode();};
    source.writeComparisonMode=[app](int mode,std::wstring&error){return WriteMorphComparisonMode(app->EnglishUI(),mode,error);};
    session.sources.push_back(std::move(source));
    session.initialSource=0;
    BindTransport(session,app);curve_editor::Open(app->Hwnd(),std::move(session),visible);
}
namespace {
struct BoneGraphKey {
    std::uint32_t frame=0,index=0;
    std::array<double,4> rotation{};
    bone_scale::Matrix scale;
    std::array<double,9> values{};
    std::array<bone_scale::Curve,5> controls{};
};
using BoneGraph=std::vector<BoneGraphKey>;
std::array<double,4> GraphQuaternion(const float* q){
    double norm=0;for(int c=0;c<4;++c)norm+=double(q[c])*q[c];
    if(!(norm>1e-20))return {0,0,0,1};
    const double length=std::sqrt(norm);
    return {q[0]/length,q[1]/length,q[2]/length,q[3]/length};
}
std::array<double,3> GraphAngles(const std::array<double,4>&q){
    const double x=q[0],y=q[1],z=q[2],w=q[3],degrees=180.0/3.14159265358979323846;
    // Same row-vector Y-X-Z decomposition as MMS's bone readout.
    const double m21=2*(y*z-x*w),m20=2*(x*z+y*w),m22=1-2*(x*x+y*y);
    const double m01=2*(x*y+z*w),m11=1-2*(x*x+z*z);
    const double rx=std::asin(std::clamp(-m21,-1.0,1.0));
    if(std::abs(std::cos(rx))<1e-7){
        // At gimbal lock choose zero roll and retain the equivalent yaw.
        const double m02=2*(x*z-y*w),m00=1-2*(y*y+z*z);
        return {rx*degrees,std::atan2(-m02,m00)*degrees,0};
    }
    return {rx*degrees,std::atan2(m20,m22)*degrees,std::atan2(m01,m11)*degrees};
}
double GraphScale(const bone_scale::Matrix&m,int axis){
    double square=0;for(int c=0;c<3;++c)square+=double(m.v[axis*3+c])*m.v[axis*3+c];
    return std::sqrt(square);
}
double NearAngle(double angle,double reference){return angle+360.0*std::round((reference-angle)/360.0);}
int GraphLane(int channel){return channel<3?channel:channel<6?3:4;}
double GraphValue(const BoneGraph&keys,const curve_editor::Channel&channel,int axis,double frame,bool smooth=false){
    if(keys.empty())return 0;
    if(frame<=keys.front().frame)return keys.front().values[axis];
    if(frame>=keys.back().frame)return keys.back().values[axis];
    const auto at=std::upper_bound(keys.begin(),keys.end(),frame,
        [](double value,const BoneGraphKey&key){return value<key.frame;});
    const std::size_t end=static_cast<std::size_t>(at-keys.begin());
    const auto&a=keys[end-1];const auto&b=keys[end];
    const double raw=(frame-a.frame)/double(b.frame-a.frame);
    std::array<double,4> controls{};
    if(!curve_editor::ReadChannelControls(channel,end,controls))
        for(int c=0;c<4;++c)controls[c]=b.controls[GraphLane(axis)][c];
    bone_scale::Curve native{};for(int c=0;c<4;++c)native[c]=static_cast<std::uint8_t>(controls[c]);
    const double eased=smooth?animation_curve::EvaluateMmdTiming(controls,raw).value:bone_scale::CurveTime(native,static_cast<float>(raw));
    if(axis<3)return a.values[axis]+(b.values[axis]-a.values[axis])*eased;
    if(axis>=6)return GraphScale(bone_scale::Interpolate(a.scale,b.scale,static_cast<float>(eased)),axis-6);
    auto qb=b.rotation;double dot=0;for(int c=0;c<4;++c)dot+=a.rotation[c]*qb[c];
    if(dot<0){dot=-dot;for(double&value:qb)value=-value;}
    double wa=1-eased,wb=eased;
    if(dot<0.9995){const double angle=std::acos(std::clamp(dot,0.0,1.0));
        wa=std::sin((1-eased)*angle)/std::sin(angle);wb=std::sin(eased*angle)/std::sin(angle);}
    std::array<double,4> q{};double norm=0;
    for(int c=0;c<4;++c){q[c]=a.rotation[c]*wa+qb[c]*wb;norm+=q[c]*q[c];}
    if(norm>1e-20)for(double&value:q)value/=std::sqrt(norm);
    const double angle=GraphAngles(q)[axis-3];
    return NearAngle(angle,a.values[axis]+(b.values[axis]-a.values[axis])*raw);
}
}
static curve_editor::Source BoneSource(MMDApp*app,const MorphModelIdentity&identity,int bone,bool includeDisplay,bool freeCurves){
    const auto*record=mdl::Mdl(identity.model);
    const auto alive=[app,identity,bone]{return MorphIdentityCurrent(*app,identity)&&bone>=0&&std::uint32_t(bone)<mdl::Mdl(identity.model)->boneCount;};
    static const char*const ids[]={"x","y","z","rx","ry","rz","sx","sy","sz"};
    static const wchar_t*const labelsZh[]={L"位移 X",L"位移 Y",L"位移 Z",L"旋转 X",L"旋转 Y",L"旋转 Z",L"缩放 X",L"缩放 Y",L"缩放 Z"};
    static const wchar_t*const labelsEn[]={L"Position X",L"Position Y",L"Position Z",L"Rotation X",L"Rotation Y",L"Rotation Z",L"Scale X",L"Scale Y",L"Scale Z"};
    static const wchar_t*const labelsJa[]={L"移動 X",L"移動 Y",L"移動 Z",L"回転 X",L"回転 Y",L"回転 Z",L"拡大 X",L"拡大 Y",L"拡大 Z"};
    auto graph=std::make_shared<BoneGraph>();
    auto discrete=std::make_shared<std::vector<curve_editor::Channel>>();
    curve_editor::Source source;source.id="bone/"+std::to_string(bone);
    source.linkAdjacentControls=[app]{return IsDlgButtonChecked(app->MainWindow(),panel::kPhysicsFrameCheckbox)==BST_CHECKED;};
    source.label=text_encoding::ModelLabel(*record,UiUsesEnglishNames(app->EnglishUI()))+L" · "+text_encoding::BoneLabel(record->boneTable[bone],UiUsesEnglishNames(app->EnglishUI()));
    source.read=[app,identity,bone,alive,graph,discrete,includeDisplay,freeCurves](std::vector<curve_editor::Channel>&channels,std::wstring&error){
        if(!alive()){error=Text(app->EnglishUI(),L"選択中のモデルまたはボーンが変わりました。",L"The selected model or bone has changed.",L"当前选中的模型或骨骼已改变。");return false;}
        const auto&modelRecord=*mdl::Mdl(identity.model);const auto*keys=mdl::BoneKeys(identity.model);
        const auto capacity=mdl::BoneKeyCapacity(modelRecord);BoneGraph fresh;
        if(!keys||std::size_t(bone)>=capacity){error=L"骨骼关键帧不可用。";return false;}
        std::uint32_t index=bone;bool ended=false;
        for(std::size_t visited=0;visited<capacity;++visited){
            if(index>=capacity||(visited&&keys[index].frame<=fresh.back().frame)){error=L"骨骼关键帧链表无效。";return false;}
            const auto&native=keys[index];BoneGraphKey key;key.index=index;key.frame=native.frame;
            key.rotation=GraphQuaternion(native.rotation);key.scale=bone_scale::Key(identity.model,index);
            auto angles=GraphAngles(key.rotation);
            for(int axis=0;axis<3;++axis){
                key.values[axis]=native.position[axis];
                key.values[3+axis]=fresh.empty()?angles[axis]:NearAngle(angles[axis],fresh.back().values[3+axis]);
                key.values[6+axis]=GraphScale(key.scale,axis);
                key.controls[axis]={native.interpolation[axis],native.interpolation[4+axis],native.interpolation[8+axis],native.interpolation[12+axis]};
            }
            key.controls[3]={native.interpolation[3],native.interpolation[7],native.interpolation[11],native.interpolation[15]};
            key.controls[4]=bone_scale::KeyCurve(identity.model,index);
            for(double value:key.values)if(!std::isfinite(value)){error=L"骨骼关键帧包含无效数值。";return false;}
            fresh.push_back(key);if(native.next==0){ended=true;break;}index=native.next;
        }
        if(!ended){error=L"骨骼关键帧链表未结束。";return false;}
        auto snapshot=std::make_shared<const BoneGraph>(fresh);channels.clear();
        for(int c=0;c<9;++c){
            curve_editor::Channel channel;channel.id=ids[c];
            channel.label=app->EnglishUI()==0?labelsJa[c]:app->EnglishUI()==1?labelsEn[c]:labelsZh[c];
            channel.minimum=-1e12;channel.maximum=1e12;channel.base=fresh.front().values[c];
            if(c>=3&&c<6)channel.unit=L"°";
            channel.interpolationGroup="bone/"+std::to_string(GraphLane(c));
            for(const auto&native:fresh){animation_curve::Key key;key.frame=native.frame;key.value=native.values[c];
                key.interpolation=animation_curve::Interpolation::Bezier;key.tangent=animation_curve::Tangent::Free;channel.track.keys.push_back(key);}
            channel.nativeControls.resize(fresh.size());channel.controlGeometry.resize(fresh.size());
            for(std::size_t end=1;end<fresh.size();++end){
                const auto&curve=fresh[end].controls[GraphLane(c)];auto&a=channel.track.keys[end-1];auto&b=channel.track.keys[end];
                const double x1=std::clamp(curve[0]/127.0,0.0002,0.9996),x2=std::clamp(curve[2]/127.0,x1+0.0002,0.9998);
                const double rate=(b.value-a.value)/double(b.frame-a.frame);
                a.outWeight=x1;b.inWeight=1-x2;a.outSlope=curve[1]/127.0/x1*rate;b.inSlope=(1-curve[3]/127.0)/(1-x2)*rate;
                for(int k=0;k<4;++k)channel.nativeControls[end][k]=curve[k];
                channel.controlGeometry[end]={a.outWeight,a.outSlope,b.inWeight,b.inSlope};
            }
            channel.sample=[snapshot,c](const curve_editor::Channel&current,double frame){
                const double value=GraphValue(*snapshot,current,c,frame);
                const double low=std::max(double(snapshot->front().frame),frame-0.001),high=std::min(double(snapshot->back().frame),frame+0.001);
                return animation_curve::Sample{value,high>low?(GraphValue(*snapshot,current,c,high,true)-GraphValue(*snapshot,current,c,low,true))/(high-low):0};
            };
            channels.push_back(std::move(channel));
        }
        bone_curve::Tracks authored;
        const bool hasAuthored=bone_curve::Read(identity.model,bone,authored);
        if(freeCurves||hasAuthored)for(int c=0;c<9;++c){
            auto& channel=channels[c];
            if(hasAuthored)channel.track=authored[c];
            channel.sample={};channel.nativeControls.clear();channel.controlGeometry.clear();channel.interpolationGroup.clear();
            if(c>=6){channel.minimum=0.001;channel.maximum=1000;}
        }
        // Visibility and IK are the model's separate discrete master track.
        const auto*display=mdl::DisplayKeys(identity.model);discrete->clear();
        if(display&&includeDisplay){
            if(modelRecord.ikChainCount<0){error=L"IK 通道数量无效。";return false;}
            const auto ikCount=static_cast<std::uint32_t>(modelRecord.ikChainCount);
            for(std::uint32_t c=0;c<=ikCount;++c){
                curve_editor::Channel channel;channel.id=c==0?"visible":"ik/"+std::to_string(c-1);
                channel.label=c==0?Text(app->EnglishUI(),L"表示",L"Visible",L"模型显示"):L"IK "+std::to_wstring(c);
                if(c>0&&modelRecord.ikChains){const int target=modelRecord.ikChains[c-1].boneIndex;
                    if(target>=0&&std::uint32_t(target)<modelRecord.boneCount)channel.label=L"IK · "+text_encoding::BoneLabel(modelRecord.boneTable[target],UiUsesEnglishNames(app->EnglishUI()));}
                channel.minimum=0;channel.maximum=1;channel.track.discrete=true;
                std::uint32_t slot=0;bool complete=false;
                for(std::size_t visited=0;visited<mdl::KeyPoolCapacity(display);++visited){
                    if(slot>=mdl::KeyPoolCapacity(display)||(visited&&std::int64_t(display[slot].frame)<=std::int64_t(channel.track.keys.back().frame))){error=L"IK 关键帧链表无效。";return false;}
                    const auto&key=display[slot];if(c>0&&!key.ikStates){error=L"IK 关键帧状态不可用。";return false;}
                    animation_curve::Key point;point.frame=key.frame;point.value=c==0?(key.visible?1:0):(key.ikStates[c-1]?1:0);point.interpolation=animation_curve::Interpolation::Step;
                    channel.track.keys.push_back(point);if(key.next==0){complete=true;break;}slot=key.next;
                }
                if(!complete){error=L"IK 关键帧链表未结束。";return false;}
                channel.base=channel.track.keys.front().value;discrete->push_back(channel);channels.push_back(std::move(channel));
            }
        }
        *graph=std::move(fresh);return true;
    };
    source.apply=[app,identity,bone,alive,graph,discrete,freeCurves](const std::vector<curve_editor::Channel>&channels,std::wstring&error){
        if(!alive()||!IsWindowEnabled(app->MainWindow())){error=L"当前模型或骨骼已改变，或仍在处理操作。";return false;}
        if(channels.size()!=9+discrete->size()){error=L"骨骼通道不匹配。";return false;}
        // Discrete keys are edited as 0/1 values, retaining their native frame positions.
        auto*display=mdl::DisplayKeys(identity.model);
        for(std::size_t c=0;c<discrete->size();++c){
            const auto&next=channels[9+c];const auto&baseline=(*discrete)[c];
            if(next.id!=baseline.id||!next.track.discrete||next.track.keys.size()!=baseline.track.keys.size()){error=L"IK 通道不匹配。";return false;}
            std::uint32_t index=0;
            for(std::size_t k=0;k<next.track.keys.size();++k){const auto&key=next.track.keys[k];
                if(!display||index>=mdl::KeyPoolCapacity(display)||key.frame!=display[index].frame||(key.value!=0&&key.value!=1)||(c>0&&!display[index].ikStates)){
                    error=L"IK 关键帧应保持原有帧号，状态只能为 0 或 1。";return false;}
                index=display[index].next;
                if((k+1<next.track.keys.size())==(index==0)){
                    error=L"IK 关键帧已改变，请等待同步。";return false;}
            }
        }
        if(freeCurves||bone_curve::Has(identity.model,bone)){
            bone_curve::Tracks tracks;
            for(int c=0;c<9;++c){if(channels[c].id!=ids[c]){error=L"骨骼通道不匹配。";return false;}tracks[c]=channels[c].track;}
            bone_curve::Tracks previous;
            const bool authored=bone_curve::Read(identity.model,bone,previous);
            bool same=authored;std::string reason;
            for(int c=0;c<9&&same;++c){std::vector<std::uint8_t> a,b;animation_curve::Encode({{"curve",tracks[c]}},a,reason);animation_curve::Encode({{"curve",previous[c]}},b,reason);same=a==b;}
            if(!same&&!bone_curve::Write(identity.model,bone,tracks,reason)){error=Wide(reason);return false;}
            for(std::size_t c=0;c<discrete->size();++c){std::uint32_t index=0;
                for(const auto& key:channels[9+c].track.keys){if(c==0)display[index].visible=std::uint8_t(key.value);else display[index].ikStates[c-1]=std::uint8_t(key.value);index=display[index].next;}
                (*discrete)[c]=channels[9+c];}
            app->LastRegisteredFrame()=std::max(app->LastRegisteredFrame(),static_cast<std::uint32_t>(mdl::Mdl(identity.model)->maxFrame));
            app->SceneModified()=1;Refresh(*app);SelectionReeval(app);return true;
        }
        auto*keys=mdl::BoneKeys(identity.model);const auto capacity=mdl::BoneKeyCapacity(*mdl::Mdl(identity.model));
        if(!keys){error=L"骨骼关键帧不可用。";return false;}
        std::vector<std::array<bone_scale::Curve,5>> changed(graph->size());
        std::uint32_t slot=bone;
        for(std::size_t end=0;end<graph->size();++end){
            const auto&expected=(*graph)[end];
            if(slot>=capacity||slot!=expected.index||keys[slot].frame!=expected.frame){error=L"骨骼关键帧已改变，请等待同步。";return false;}
            for(int c=0;c<9;++c){
                if(channels[c].id!=ids[c]||channels[c].track.keys.size()!=graph->size()||channels[c].track.keys[end].frame!=expected.frame||channels[c].track.keys[end].value!=expected.values[c]){
                    error=L"此入口只编辑插值控制柄，关键帧位置和骨骼数值应在 MMS 中修改。";return false;}
                if(end){std::array<double,4> controls{};if(!curve_editor::ReadChannelControls(channels[c],end,controls)){error=L"插值控制点无法读取。";return false;}
                    bone_scale::Curve native{};for(int k=0;k<4;++k){if(!std::isfinite(controls[k])||controls[k]<0||controls[k]>127){error=L"插值控制点超出范围。";return false;}native[k]=static_cast<std::uint8_t>(std::lround(controls[k]));}
                    const int lane=GraphLane(c);if(c==4||c==5||c==7||c==8){if(changed[end][lane]!=native){error=L"旋转或缩放各轴必须使用同一插值控制点。";return false;}}
                    else changed[end][lane]=native;
                }
            }
            if(end+1<graph->size()){if(keys[slot].next==0){error=L"骨骼关键帧已改变。";return false;}slot=keys[slot].next;}
            else if(keys[slot].next!=0){error=L"骨骼关键帧已改变。";return false;}
        }
        for(std::size_t end=1;end<graph->size();++end){auto&key=keys[(*graph)[end].index];
            for(int lane=0;lane<4;++lane)for(int k=0;k<4;++k)key.interpolation[lane+k*4]=changed[end][lane][k];
            bone_scale::SetKeyCurve(identity.model,(*graph)[end].index,changed[end][4]);
            (*graph)[end].controls=changed[end];
        }
        for(std::size_t c=0;c<discrete->size();++c){std::uint32_t index=0;
            for(const auto&key:channels[9+c].track.keys){if(c==0)display[index].visible=static_cast<std::uint8_t>(key.value);else display[index].ikStates[c-1]=static_cast<std::uint8_t>(key.value);index=display[index].next;}
            (*discrete)[c]=channels[9+c];}
        app->SceneModified()=1;Refresh(*app);return true;
    };
    return source;
}

static std::vector<curve_editor::Collection> BoneDisplayCollections(MMDApp*app,unsigned char*model){
    const auto&record=*mdl::Mdl(model);
    const auto*groups=mdl::DisplayGroups(model);
    const auto*rows=mdl::RigidGroups(model);
    std::vector<int> membership(record.boneCount,-1);
    // The native display table may list a bone more than once. Keep a single
    // editable track under its first display frame rather than duplicate keys.
    if(rows&&groups){
        for(std::uint32_t i=0;i<record.rigidBodyCount;++i){
            const auto&row=rows[i];
            if(mikumikustudio::mdl::FrameTargetIndex(row)<record.boneCount&&mikumikustudio::mdl::FrameGroupIndex(row)<mikumikustudio::mdl::DisplayGroupCount(record)&&
               membership[mikumikustudio::mdl::FrameTargetIndex(row)]<0)
                membership[mikumikustudio::mdl::FrameTargetIndex(row)]=mikumikustudio::mdl::FrameGroupIndex(row);
        }
    }
    if(groups&&mikumikustudio::mdl::DisplayGroupCount(record)>0&&record.displayRootBone<record.boneCount&&
       membership[record.displayRootBone]<0)
        membership[record.displayRootBone]=0;
    std::vector<curve_editor::Collection> result;
    result.reserve(record.boneCount);
    for(const int group:membership){
        if(group<0){
            result.push_back({"display-unassigned",Text(app->EnglishUI(),L"未分類",L"Unassigned",L"未分组")});
        }else{
            auto label=text_encoding::DisplayGroupLabel(groups[group],UiUsesEnglishNames(app->EnglishUI()));
            if(label.empty())label=Text(app->EnglishUI(),L"表示枠 ",L"Display frame ",L"显示枠 ")+std::to_wstring(group);
            result.push_back({"display"+std::to_string(group),std::move(label)});
        }
    }
    return result;
}

void OpenInterpolation(MMDApp*app,bool visible){
    if(!app)return;
    const int selected=int(SendMessageA(GetDlgItem(app->MainWindow(),panel::kInterpCurveCombo),CB_GETCURSEL,0,0));
    static const char*const mainIds[]={"x","y","z","rx","ry","rz","sx","sy","sz"};
    if(app->state.optflag[0]!=0){
        static const char*const camera[]={"x","y","z","rx","distance","fov"};
        OpenScene(app,visible,camera[selected>=0&&selected<6?selected:3]);return;
    }
    unsigned char*model=app->SelectedModel();const auto*record=model?mdl::Mdl(model):nullptr;
    if(!record||!record->boneTable||record->selectedBone<0||std::uint32_t(record->selectedBone)>=record->boneCount)return;
    const int initialBone=record->selectedBone;const auto identity=MorphIdentityOf(*app,model);
    curve_editor::Session session;session.id="mms.interpolation.bone";session.language=app->EnglishUI();
    session.currentFrame=[app]{return double(app->CurrentFrame());};
    session.seek=[app](int frame){app->CurrentFrame()=frame;Refresh(*app);SelectionReeval(app);};
    session.initialSource=bone_curve::Has(model,initialBone)?1:0;
    for(bool freeCurves : {false,true}) {
    struct Entry{curve_editor::Source source;std::vector<curve_editor::Channel> baseline;};
    auto entries=std::make_shared<std::vector<Entry>>();
    for(std::uint32_t bone=0;bone<record->boneCount;++bone)
        entries->push_back({BoneSource(app,identity,int(bone),int(bone)==initialBone,freeCurves),{}});
    curve_editor::Source source;source.id=freeCurves?"bones.free":"bones";
    source.label=text_encoding::ModelLabel(*record,UiUsesEnglishNames(app->EnglishUI()))+L" · "+
        Text(app->EnglishUI(),freeCurves?L"自由カーブ":L"標準補間",freeCurves?L"Free curves (Euler XYZ)":L"Native interpolation",freeCurves?L"自由曲线（XYZ 角度）":L"原生插值");
    source.initialMainChannel="bone"+std::to_string(initialBone)+"/"+mainIds[selected>=0&&selected<3?selected:selected==4?6:3];
    const auto displayCollections=BoneDisplayCollections(app,model);
    source.initialExpandedCollection="object/"+displayCollections[initialBone].id+"/bone"+std::to_string(initialBone);
    source.linkAdjacentControls=[app]{return IsDlgButtonChecked(app->MainWindow(),panel::kPhysicsFrameCheckbox)==BST_CHECKED;};
    source.read=[app,identity,entries,initialBone](std::vector<curve_editor::Channel>&channels,std::wstring&error){
        if(!MorphIdentityCurrent(*app,identity)){error=ModelGoneMessage(*app,identity);return false;}
        const auto&record=*mdl::Mdl(identity.model);channels.clear();
        const auto displayCollections=BoneDisplayCollections(app,identity.model);
        for(std::size_t bone=0;bone<entries->size();++bone){
            auto&entry=(*entries)[bone];std::vector<curve_editor::Channel> part;
            if(!entry.source.read(part,error))return false;
            const std::string prefix="bone"+std::to_string(bone);
            const std::wstring object=text_encoding::ModelLabel(record,UiUsesEnglishNames(app->EnglishUI()));
            const std::wstring name=text_encoding::BoneLabel(record.boneTable[bone],UiUsesEnglishNames(app->EnglishUI()));
            for(std::size_t c=0;c<part.size();++c){
                auto&channel=part[c];channel.id=prefix+"/"+channel.id;
                channel.selected=int(bone)==initialBone;
                if(!channel.interpolationGroup.empty())channel.interpolationGroup=prefix+"/"+channel.interpolationGroup;
                if(c<9){
                    const char*category=c<3?"position":c<6?"rotation":"scale";
                    const auto label=c<3?Text(app->EnglishUI(),L"移動",L"Position",L"位移"):
                        c<6?Text(app->EnglishUI(),L"回転",L"Rotation",L"旋转"):Text(app->EnglishUI(),L"拡大",L"Scale",L"缩放");
                    channel.collections={{"object",object},displayCollections[bone],{prefix,name},{category,label}};
                }else channel.collections={{"object",object},{"state",Text(app->EnglishUI(),L"状態",L"State",L"状态")}};
            }
            entry.baseline=part;channels.insert(channels.end(),part.begin(),part.end());
        }
        return true;
    };
    source.apply=[entries](const std::vector<curve_editor::Channel>&channels,std::wstring&error){
        std::size_t offset=0;int changed=-1;std::vector<curve_editor::Channel> target;
        const auto same=[](const curve_editor::Channel&a,const curve_editor::Channel&b){
            if(a.id!=b.id||a.nativeControls!=b.nativeControls||a.track.keys.size()!=b.track.keys.size())return false;
            for(std::size_t i=0;i<a.track.keys.size();++i){const auto&x=a.track.keys[i];const auto&y=b.track.keys[i];
                if(x.frame!=y.frame||x.value!=y.value||x.inWeight!=y.inWeight||x.outWeight!=y.outWeight||x.inSlope!=y.inSlope||x.outSlope!=y.outSlope||x.interpolation!=y.interpolation||x.tangent!=y.tangent)return false;}
            return true;
        };
        for(std::size_t bone=0;bone<entries->size();++bone){
            const auto&baseline=(*entries)[bone].baseline;
            if(offset+baseline.size()>channels.size()){error=L"骨骼集合通道不匹配。";return false;}
            bool different=false;
            for(std::size_t c=0;c<baseline.size();++c)different=different||!same(channels[offset+c],baseline[c]);
            if(different){
                if(changed>=0){error=L"一次操作只能编辑一根骨骼；跨骨骼集合用于对比查看。";return false;}
                changed=int(bone);target.assign(channels.begin()+offset,channels.begin()+offset+baseline.size());
            }
            offset+=baseline.size();
        }
        if(offset!=channels.size()){error=L"骨骼集合通道不匹配。";return false;}
        if(changed<0)return true;
        auto native=target;
        const std::string prefix="bone"+std::to_string(changed)+"/";
        for(auto&channel:native){
            if(channel.id.rfind(prefix,0)!=0){error=L"骨骼集合目标不匹配。";return false;}
            channel.id.erase(0,prefix.size());
        }
        auto&entry=(*entries)[changed];
        if(!entry.source.apply(native,error))return false;
        entry.baseline=std::move(target);return true;
    };

    BindSelection(source,app,[app,identity](const std::string& channel,const std::vector<int>* frames)->std::vector<int>{
        if(!MorphIdentityCurrent(*app,identity)||channel.rfind("bone",0)!=0)return {};
        char* end=nullptr;const long bone=std::strtol(channel.c_str()+4,&end,10);
        const auto& record=*mdl::Mdl(identity.model);
        if(!end||*end!='/'||bone<0||std::uint32_t(bone)>=record.boneCount)return {};
        const std::string lane=end+1;
        if(lane=="visible"||lane.rfind("ik/",0)==0)
            return NativeSelection(mdl::DisplayKeys(identity.model), mdl::KeyPoolCapacity(mdl::DisplayKeys(identity.model)),0,frames);
        return NativeSelection(mdl::BoneKeys(identity.model),mdl::BoneKeyCapacity(record),std::size_t(bone),frames);
    });
    session.sources.push_back(std::move(source));
    }
    BindTransport(session,app);curve_editor::Open(app->Hwnd(),std::move(session),visible);
}

void NotifyRegistered(MMDApp*app,Kind kind,int slot,int frame){
    if(!app)return;
    ReconcileRegisteredFrame(*app,kind,slot,frame);
    curve_editor::NotifySourceChanged(app->Hwnd(),std::to_string(int(kind))+"/"+std::to_string(slot));
}
void NotifyModelRegistered(MMDApp*app,bool morph){
    if(!app)return;
    unsigned char*model=app->SelectedModel();
    const mdl::ModelRecord*record=model?mdl::Mdl(model):nullptr;
    if(!record)return;
    if(morph)curve_editor::NotifySourceChanged(app->Hwnd(),"morph");
    else if(record->selectedBone>=0){
        curve_editor::NotifySourceChanged(app->Hwnd(),"bones");
        curve_editor::NotifySourceChanged(app->Hwnd(),"bones.free");
    }
}
void CreateControls(MMDApp*app,void*window){
    HWND hwnd=static_cast<HWND>(window);
    HWND button=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,0,0,22,22,hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(curve_editor::kAccessoryCurve)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(button,WM_SETFONT,SendMessageW(GetDlgItem(hwnd,472),WM_GETFONT,0,0),TRUE);
    HWND interpolation=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,0,0,22,22,hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(curve_editor::kInterpolationCurve)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(interpolation,WM_SETFONT,SendMessageW(GetDlgItem(hwnd,472),WM_GETFONT,0,0),TRUE);
    // MMS: no panel "read" button - `.x` motion travels in the camera / light
    // VMD (accessory_motion_io.hpp); the File menu entry still imports a curve
    // motion into the selected accessory.
    // The four expression launchers live at the left edge of the four expression
    // panel selectors; LayoutMorphControls places them.
    for(int lane=0;lane<kMorphLaneCount;++lane){
        HWND morph=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,0,0,
            kMorphLauncherWidth,kMorphLauncherHeight,hwnd,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kMorphCurveLane[lane])),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(morph,WM_SETFONT,SendMessageW(GetDlgItem(hwnd,472),WM_GETFONT,0,0),TRUE);
    }
    SetWindowSubclass(hwnd,Parent,curve_editor::kAccessoryCurve,reinterpret_cast<DWORD_PTR>(app));
    Localize(app);
}
void LayoutControls(MMDApp*app){
    if(!app)return;HWND hwnd=app->MainWindow();if(!GetDlgItem(hwnd,curve_editor::kAccessoryCurve))return;
    RECT anchor=Rect(hwnd,474),combo=Rect(hwnd,471);
    const int x=anchor.left;
    Move(hwnd,471,x,combo.top,99,150);
    SendMessageW(GetDlgItem(hwnd,471),CB_SETDROPPEDWIDTH,240,0);
    Move(hwnd,curve_editor::kAccessoryCurve,x+103,combo.top-1,22,22);
    RECT autoBox=Rect(hwnd,530);
    const RECT interpolationCombo=Rect(hwnd,panel::kInterpCurveCombo);
    constexpr int launcherSize=18;
    Move(hwnd,curve_editor::kInterpolationCurve,interpolationCombo.right-launcherSize,
         autoBox.top+(autoBox.bottom-autoBox.top-launcherSize)/2,launcherSize,launcherSize);
    // Equal field columns. Labels are right aligned separately, not embedded
    // into the field. The current native panel uses logical (virtualised) px.
    for(int row=0;row<3;++row)for(int col=0;col<(row==2?2:3);++col){
        int id=478+row*3+col;
        if(row==2)id=484+col;
        RECT old=Rect(hwnd,id);
        Move(hwnd,id,x+20+col*58,old.top,36,18);
        SendMessageW(GetDlgItem(hwnd,id),EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(4,4));
    }
    RECT last=Rect(hwnd,485);Move(hwnd,487,x+116,last.top-2,68,22);
    ShowControls(app,app->state.optflag[0]!=0&&app->state.optflag[3]!=0);
}
void LayoutMorphControls(MMDApp*app){
    if(app==nullptr)return;
    HWND hwnd=app->MainWindow();
    if(hwnd==nullptr)return;
    for(int lane=0;lane<kMorphLaneCount;++lane){
        HWND button=GetDlgItem(hwnd,kMorphCurveLane[lane]);
        HWND combo=GetDlgItem(hwnd,kMorphComboIds[lane]);
        HWND arrow=GetDlgItem(hwnd,kMorphPrevArrowIds[lane]);
        if(button==nullptr||combo==nullptr||arrow==nullptr)continue;
        const RECT arrowRect=Rect(hwnd,kMorphPrevArrowIds[lane]);
        const RECT comboRect=Rect(hwnd,kMorphComboIds[lane]);
        // The selector's original left edge is a fixed 16 px right of its own
        // previous arrow, which no other code moves.  Deriving the position from
        // that arrow rather than from the selector's current rect is what keeps
        // this idempotent: a second relayout cannot narrow the selector again.
        const int left=arrowRect.left+kMorphComboArrowOffset;
        // The right edge is unchanged, so the forward arrow beside it stays put.
        MoveWindow(combo,left+kMorphLauncherWidth,comboRect.top,kMorphComboNarrowWidth,
                   kMorphComboHeight,TRUE);
        // Native combo height follows its font/theme, not the requested popup
        // height. Align the entire row to its actual collapsed rectangle.
        const RECT aligned=Rect(hwnd,kMorphComboIds[lane]);
        const int rowHeight=static_cast<int>(aligned.bottom-aligned.top);
        MoveWindow(button,left,aligned.top,kMorphLauncherWidth,rowHeight,TRUE);
        MoveWindow(arrow,arrowRect.left,aligned.top,
                   arrowRect.right-arrowRect.left,rowHeight,TRUE);
        const RECT next=Rect(hwnd,kMorphNextArrowIds[lane]);
        MoveWindow(GetDlgItem(hwnd,kMorphNextArrowIds[lane]),next.left,aligned.top,
                   next.right-next.left,rowHeight,TRUE);
        // Font-dependent combo heights can extend into the legacy slider row.
        // Keep sibling windows disjoint; otherwise the slider paints across
        // the selector until the selector repaints on hover.
        HWND slider=GetDlgItem(hwnd,kMorphSliderIds[lane]);
        const RECT sliderRect=Rect(hwnd,kMorphSliderIds[lane]);
        const int sliderTop=static_cast<int>(aligned.bottom)+2;
        const int sliderHeight=std::max(1,std::min(25,
            static_cast<int>(comboRect.top)+48-sliderTop));
        MoveWindow(slider,sliderRect.left,sliderTop,
                   sliderRect.right-sliderRect.left,sliderHeight,TRUE);
        SendMessageW(combo,CB_SETDROPPEDWIDTH,240,0);
        for(HWND control:{arrow,button,combo,GetDlgItem(hwnd,kMorphNextArrowIds[lane]),slider})
            RedrawWindow(control,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME);
    }
    ShowMorphControls(app);
}
void Localize(MMDApp*app){
    if(!app)return;const unsigned lang=app->EnglishUI();HWND hwnd=app->MainWindow();
    SetWindowTextW(GetDlgItem(hwnd,curve_editor::kAccessoryCurve),Text(lang,L"カーブエディターを開く",L"Open curve editor",L"打开曲线编辑器"));
    SetWindowTextW(GetDlgItem(hwnd,curve_editor::kInterpolationCurve),Text(lang,L"補間カーブエディターを開く",L"Open interpolation curve editor",L"打开插值曲线编辑器"));
    for(int lane=0;lane<kMorphLaneCount;++lane)
        SetWindowTextW(GetDlgItem(hwnd,kMorphCurveLane[lane]),MorphCaption(lang,lane).c_str());
    SetWindowTextW(GetDlgItem(hwnd,472),Text(lang,L"追加",L"Load",L"载入"));
    SetWindowTextW(GetDlgItem(hwnd,487),Text(lang,L"登録",L"Key",L"登记"));
    HMENU menu=GetMenu(hwnd),file=menu?GetSubMenu(menu,0):nullptr;
    if(file){const wchar_t*caption=Text(lang,L"選択中のアクセサリのモーション読込（VMD）…",L"Read selected accessory motion (VMD)…",L"读取当前附件动作（VMD）…");
        if(GetMenuState(file,curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND)==UINT(-1))AppendMenuW(file,MF_STRING,curve_editor::kAccessoryMotionMenu,caption);
        else ModifyMenuW(file,curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND|MF_STRING,curve_editor::kAccessoryMotionMenu,caption);
    }
    curve_editor::SetLanguage(hwnd,lang);
}
void ShowControls(MMDApp*app,bool show){
    if(!app)return;
    ShowWindow(GetDlgItem(app->MainWindow(),curve_editor::kAccessoryCurve),show?SW_SHOWNA:SW_HIDE);
    bool targetAvailable=app->state.optflag[0]!=0;
    if(!targetAvailable){
        unsigned char*model=app->SelectedModel();
        const mdl::ModelRecord*record=model?mdl::Mdl(model):nullptr;
        targetAvailable=record!=nullptr&&record->boneTable!=nullptr&&record->selectedBone>=0&&
            static_cast<std::uint32_t>(record->selectedBone)<record->boneCount;
    }
    HWND interpolation=GetDlgItem(app->MainWindow(),curve_editor::kInterpolationCurve);
    ShowWindow(interpolation,SW_SHOWNA);
    EnableWindow(interpolation,targetAvailable?TRUE:FALSE);
    ShowMorphControls(app);
}
bool Command(MMDApp*app,unsigned id){
    if(id==curve_editor::kAccessoryCurve){Open(app);return true;}
    for(int lane=0;lane<kMorphLaneCount;++lane)
        if(id==kMorphCurveLane[lane]){OpenMorph(app,lane);return true;}
    return false;
}
void DrawLabels(MMDApp*app,void*dc){
    HWND hwnd=app->MainWindow();HDC hdc=static_cast<HDC>(dc);
    const wchar_t*labels[]={L"X",L"Y",L"Z",L"Rx",L"Ry",L"Rz",L"S",L"Tr"};
    auto old=SelectObject(hdc,reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(hwnd,478),WM_GETFONT,0,0)));
    int oldMode=SetBkMode(hdc,TRANSPARENT);
    auto oldColor=SetTextColor(hdc,RGB(app->UiTextRed(),app->UiTextGreen(),app->UiTextBlue()));
    for(int c=0;c<8;++c){RECT r=Rect(hwnd,478+c);r.right=r.left-4;r.left=r.right-17;DrawTextW(hdc,labels[c],-1,&r,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);}
    SetTextColor(hdc,oldColor);SetBkMode(hdc,oldMode);SelectObject(hdc,old);
}
}
