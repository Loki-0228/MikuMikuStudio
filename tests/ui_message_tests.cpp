// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include "mikudancestudio/ui_message.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include <vector>
#include <type_traits>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cmath>
#include <array>
#include <cstring>
#include <string>

using namespace mikudancestudio;
namespace mikudancestudio {
void DragInterpolationControlPoint(MMDApp*);
void SnapshotSelectedKeysForUndo(unsigned char*,int);
void UndoModelEdit(unsigned char*,std::int32_t*);
void RedoModelEdit(unsigned char*,std::int32_t*);
void CmdControl400(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void CmdControl450(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void CmdFileMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void CmdViewMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void LoadSceneFile();
}
namespace {
void Check(bool condition, const char* what) {
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(1); }
}
void NativeCurveLinkRegression(MMDApp* app, HWND owner) {
    auto model=std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneKey> bones(mdl::kBoneKeyCapacity);
    using CameraKey=std::remove_pointer_t<std::remove_reference_t<decltype(app->CameraKeys())>>;
    std::vector<CameraKey> cameras(mdl::kTimelineKeyCapacity);
    auto* raw=reinterpret_cast<unsigned char*>(model.get());
    *reinterpret_cast<HWND*>(raw)=owner;
    model->boneKeys=bones.data();app->ModelSlot(0)=raw;app->SetSelectedModelSlot(0);
    auto* oldCamera=app->CameraKeys();app->CameraKeys()=cameras.data();
    HWND combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,0,100,100,owner,reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kInterpCurveCombo)),nullptr,nullptr);
    HWND automatic=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|BS_AUTOCHECKBOX,0,0,20,20,owner,reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kPhysicsFrameCheckbox)),nullptr,nullptr);
    for(int i=0;i<7;++i)SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"curve"));
    HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateBitmap(128,128,1,32,nullptr);
    auto oldBitmap=SelectObject(dc,bitmap);auto oldDC=app->CurveDC();app->CurveDC()=dc;
    RECT client{};GetClientRect(owner,&client);
    for(bool camera : {false,true})for(bool enabled : {false,true})for(int mode : {1,2})for(int lane=0;lane<(camera?7:6);++lane){
        std::fill(bones.begin(),bones.end(),mdl::BoneKey{});std::fill(cameras.begin(),cameras.end(),CameraKey{});
        for(int i=0;i<4;++i){
            bones[i].frame=cameras[i].frame=i*10;bones[i].previous=cameras[i].previous=i?i-1:0;bones[i].next=cameras[i].next=i<3?i+1:0;
            for(int k=0;k<4;++k){for(int c=0;c<4;++c)bones[i].interpolation[k*4+c]=k<2?20:107;for(int c=0;c<6;++c)cameras[i].interpolation[k][c]=k<2?20:107;}
            bone_scale::SetKeyCurve(raw,i,{20,20,107,107});
        }
        bones[2].allocated=1;cameras[2].selected=1;
        app->CameraMode()=camera?1:0;app->state.pendingTimelineSelectionRow=mode;
        SendMessageW(combo,CB_SETCURSEL,lane,0);SendMessageW(automatic,BM_SETCHECK,enabled?BST_CHECKED:BST_UNCHECKED,0);
        int x=mode==1?40:80,y=mode==1?60:110;app->MouseX()=x+8;app->MouseY()=client.bottom-y-8;
        if(!camera&&enabled)SnapshotSelectedKeysForUndo(raw,20);
        DragInterpolationControlPoint(app);
        const int neighbor=mode==1?1:3,target=mode==1?2:0;
        for(int c=0;c<(camera?6:5);++c){
            const bool changed=lane==c||lane==(camera?6:5);
            std::array<int,4> actual{};
            for(int k=0;k<4;++k)actual[k]=camera?cameras[neighbor].interpolation[k][c]:c==4?bone_scale::KeyCurve(raw,neighbor)[k]:bones[neighbor].interpolation[k*4+c];
            Check(actual[target]==(enabled&&changed?127-x:target==0?20:107)&&actual[target+1]==(enabled&&changed?127-y:target==0?20:107),"native auto links only the requested channel and adjacent handle");
            Check(actual[2-target]==(target==0?107:20)&&actual[3-target]==(target==0?107:20),"native linkage preserves the neighbor's other handle");
        }
        Check(bones[0].frame==0&&bones[3].frame==30&&cameras[3].frame==30,"native linkage preserves keyframe positions");
        if(!camera&&enabled){
            const auto linked=bones[neighbor];const auto linkedScale=bone_scale::KeyCurve(raw,neighbor);
            std::int32_t frame=20;UndoModelEdit(raw,&frame);
            Check(bones[neighbor].interpolation[target*4]==(target==0?20:107)&&bone_scale::KeyCurve(raw,neighbor)==bone_scale::Curve{20,20,107,107},"native undo restores the adjacent bone and scale controls");
            RedoModelEdit(raw,&frame);
            Check(std::memcmp(bones[neighbor].interpolation,linked.interpolation,16)==0&&bone_scale::KeyCurve(raw,neighbor)==linkedScale,"native redo restores the complete linked edit");
        }
    }
    for(auto& ring:model->undoRings)for(auto& slot:ring.slots){std::free(slot.bonePose);std::free(slot.auxiliaryPose);}
    // The first incoming segment has no predecessor; the last has no successor.
    app->CameraMode()=1;SendMessageW(combo,CB_SETCURSEL,3,0);SendMessageW(automatic,BM_SETCHECK,BST_CHECKED,0);
    for(int mode : {1,2}){
        for(auto& key:cameras)key.selected=0;
        cameras[mode==1?1:3].selected=1;app->state.pendingTimelineSelectionRow=mode;
        const auto root=cameras[0],other=cameras[mode==1?2:1];
        DragInterpolationControlPoint(app);
        Check(std::memcmp(cameras[0].interpolation,root.interpolation,sizeof root.interpolation)==0&&std::memcmp(cameras[mode==1?2:1].interpolation,other.interpolation,sizeof other.interpolation)==0,"native boundary edit cannot link outside the track");
    }
    bone_scale::Forget(raw);
    app->ModelSlot(0)=nullptr;app->CameraKeys()=oldCamera;app->CurveDC()=oldDC;
    SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);DestroyWindow(combo);DestroyWindow(automatic);
    std::puts("PASS hidden native interpolation linkage: bone/camera channels, scale, all, checkbox off, both handles, boundaries and undo/redo");
}
LRESULT CALLBACK HideEdgeWindow(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR){
    if(message==WM_WINDOWPOSCHANGING){auto* position=reinterpret_cast<WINDOWPOS*>(lp);position->flags&=~SWP_SHOWWINDOW;position->flags|=SWP_NOACTIVATE;}
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,HideEdgeWindow,id);
    return DefSubclassProc(window,message,wp,lp);
}
LRESULT CALLBACK HideEdgeDialog(int code,WPARAM wp,LPARAM lp){
    if(code==HCBT_CREATEWND){auto* created=reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;if(!(created->style&WS_CHILD)){created->style&=~WS_VISIBLE;SetWindowSubclass(reinterpret_cast<HWND>(wp),HideEdgeWindow,1,0);}}
    if(code==HCBT_ACTIVATE)return 1;
    return CallNextHookEx(nullptr,code,wp,lp);
}
LONG CALLBACK EdgeCrash(EXCEPTION_POINTERS* exception){
    if(exception->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION){
        std::fprintf(stderr,"FAIL edge dialog access violation at %p\n",exception->ExceptionRecord->ExceptionAddress);ExitProcess(1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void EdgeThicknessRegression(MMDApp* app,HWND owner){
    auto* previous=g_Block;g_Block=app;app->HInstance()=GetModuleHandleW(nullptr);app->CameraMode()=0;
    auto model=std::make_unique<mdl::ModelRecord>();app->ModelSlot(0)=reinterpret_cast<unsigned char*>(model.get());app->SetSelectedModelSlot(0);
    INITCOMMONCONTROLSEX controls{sizeof controls,ICC_BAR_CLASSES};InitCommonControlsEx(&controls);
    const auto exceptionHandler=AddVectoredExceptionHandler(1,EdgeCrash);
    HHOOK hide=SetWindowsHookExW(WH_CBT,HideEdgeDialog,nullptr,GetCurrentThreadId());Check(hide!=nullptr,"hide actual edge dialog");
    for(unsigned char language : {0,1,2,0,1,2}){
        app->EnglishUI()=language;model->edgeScale=0.75f;
        CmdViewMenu(app,owner,253,0);
        HWND dialog=app->state.edgeThicknessDialog;Check(dialog!=nullptr&&!IsWindowVisible(dialog),"edge dialog opens and stays hidden");
        HWND edit=GetDlgItem(dialog,panel::kEdgeThicknessEdit),slider=GetDlgItem(dialog,panel::kEdgeThicknessSlider);
        Check(SendMessageW(slider,TBM_GETPOS,0,0)==75,"edge dialog loads model thickness");
        SetWindowTextA(edit,"1.25");SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);
        Check(std::fabs(model->edgeScale-1.25f)<0.001f&&SendMessageW(slider,TBM_GETPOS,0,0)==125,"typing edge thickness updates model and slider");
        SendMessageW(slider,TBM_SETPOS,TRUE,50);SendMessageW(dialog,WM_HSCROLL,TB_THUMBPOSITION,reinterpret_cast<LPARAM>(slider));
        char value[32]{};GetWindowTextA(edit,value,32);
        Check(std::fabs(model->edgeScale-0.5f)<0.001f&&std::fabs(std::atof(value)-0.5)<0.001,"edge slider updates model and edit");
        SendMessageW(dialog,WM_COMMAND,IDCANCEL,0);Check(app->state.edgeThicknessDialog==nullptr&&!IsWindow(dialog),"edge dialog closes without stale callback");
    }
    UnhookWindowsHookEx(hide);RemoveVectoredExceptionHandler(exceptionHandler);app->ModelSlot(0)=nullptr;g_Block=previous;
    std::puts("PASS hidden edge thickness dialog: typing, slider, close and reopen in three languages");
}
std::wstring Text(HWND window) {
    std::wstring text(GetWindowTextLengthW(window) + 1, L'\0');
    text.resize(GetWindowTextW(window, text.data(), static_cast<int>(text.size())));
    return text;
}
#include "accessory_message_regression.inc"

int physicsDialogsSeen = 0;
LRESULT CALLBACK InspectPhysicsDialog(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        HWND physics = reinterpret_cast<HWND>(wp);
        if (GetDlgItem(physics, 669)) {
            const bool chinese = g_Block->EnglishUI() == 2;
            Check(Text(physics) == (chinese ? L"切换物理开关帧" : L"change physics ON/OFF frame") &&
                  Text(GetDlgItem(physics, 1)) == (chinese ? L"确定" : L"OK") &&
                  Text(GetDlgItem(physics, 2)) == (chinese ? L"取消" : L"Cancel"),
                  "physics switch title and buttons follow current language");
            wchar_t text[128]{};
            SendDlgItemMessageW(physics, 669, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(text));
            Check(std::wstring(text) == (chinese ? L"开启（X标记）" : L"ON (X mark)"),
                  "physics mode combo follows current language");
            Check(SendDlgItemMessageW(physics, 669, CB_GETCURSEL, 0, 0) == 0,
                  "localization preserves default ON selection");
            ++physicsDialogsSeen;
            ShowWindow(physics, SW_HIDE);
            PostMessageW(physics, WM_COMMAND, IDCANCEL, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void PhysicsDialogTranslation(MMDApp* app, HWND owner) {
    auto* previous = g_Block;
    g_Block = app;
    app->HInstance() = GetModuleHandleW(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    for (unsigned char language : {1, 2, 1, 2}) {
        app->EnglishUI() = language;
        const bool chinese = language == 2;
        app->GravityNoise() = 10;
        app->GravityNoiseEnabled() = 1;
        CmdViewMenu(app, owner, 266, 0);
        HWND gravity = app->GravitySettingDialog();
        ShowWindow(gravity, SW_HIDE);
        Check(gravity != nullptr, "create actual gravity dialog resource");
        HWND windToggle = GetDlgItem(gravity, 731), windAmount = GetDlgItem(gravity, 713);
        RECT toggleRect{}, amountRect{}, overlap{};
        GetWindowRect(windToggle, &toggleRect);
        GetWindowRect(windAmount, &amountRect);
        Check(!IntersectRect(&overlap, &toggleRect, &amountRect),
              "gravity wind checkbox never paints over its numeric input");
        Check(Text(gravity) == (chinese ? L"重力设置" : L"gravity setting") &&
              Text(GetDlgItem(gravity, 1)) == (chinese ? L"登记" : L"Register") &&
              Text(GetDlgItem(gravity, 2)) == (chinese ? L"关闭" : L"Close") &&
              Text(windToggle) == (chinese ? L"风力" : L"Noize"),
              "gravity title, buttons and wind follow current language");
        bool sawAcceleration = false;
        for (HWND child = GetWindow(gravity, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            if (Text(child) == (chinese ? L"加速度" : L"acceleration")) sawAcceleration = true;
        Check(sawAcceleration, "gravity static text is translated too");
        const auto amount = Text(windAmount);
        for (int toggle = 0; toggle < 4; ++toggle) {
            SendMessageW(windToggle, BM_CLICK, 0, 0);
            RedrawWindow(gravity, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            Check((IsWindowEnabled(windAmount) != FALSE) == (app->GravityNoiseEnabled() != 0) &&
                  Text(windAmount) == amount,
                  "wind input keeps its number and follows the checkbox through repeated redraws");
        }
        DestroyWindow(gravity);
        app->GravitySettingDialog() = nullptr;
        HHOOK hook = SetWindowsHookExW(WH_CBT, InspectPhysicsDialog, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "install physics dialog inspection");
        const int before = physicsDialogsSeen;
        CmdViewMenu(app, owner, 275, 0);
        UnhookWindowsHookEx(hook);
        Check(physicsDialogsSeen == before + 1, "physics menu opens and cancels its actual dialog");
    }
    g_Block = previous;
    std::puts("PASS actual gravity and physics-frame dialog localization");
}


void ShadowBrightnessDialog(MMDApp* app, HWND owner) {
    auto* previous = g_Block; g_Block = app;
    app->InitDefaults();
    app->HInstance() = GetModuleHandleW(nullptr);
    app->state.hwnd = owner;
    Check(app->ProjectedShadowAmbientIntensity() == 1.f, "ground-shadow brightness defaults to one");
    std::array<unsigned char, 172> scratch;
    scratch.fill(0x5a);
    std::memcpy(app->CameraFrameScratch(), scratch.data(), scratch.size());
    for (unsigned char language : {1, 2}) {
        app->EnglishUI() = language;
        CmdFileMenu(app, owner, 248, 0);
        HWND dialog = app->GroundShadowColorDialog();
        Check(dialog != nullptr, "open actual ground-shadow dialog");
        Check(Text(dialog) == (language == 2 ? L"地面阴影亮度" : L"ground shadow color"),
              "ground-shadow title follows language");
        HWND edit = GetDlgItem(dialog, 626), slider = GetDlgItem(dialog, 625);
        Check(Text(edit) == L"1.00" && SendMessageW(slider, TBM_GETPOS, 0, 0) == 100,
              "ground-shadow dialog reads rendering material, not camera scratch");
        SetWindowTextW(edit, L"0.75");
        SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
        Check(std::fabs(app->ProjectedShadowMaterial().Ambient.r - .75f) < 1e-5f &&
              SendMessageW(slider, TBM_GETPOS, 0, 0) == 75,
              "typing brightness updates actual shadow material and slider");
        SendMessageW(slider, TBM_SETPOS, TRUE, 100);
        SendMessageW(dialog, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(slider));
        Check(Text(edit) == L"1.00" && app->ProjectedShadowAmbientIntensity() == 1.f,
              "slider updates brightness and visible numeric edit");
        Check(std::memcmp(app->CameraFrameScratch(), scratch.data(), scratch.size()) == 0,
              "ground-shadow edits never corrupt the camera clipboard");
        SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
        Check(app->GroundShadowColorDialog() == nullptr, "close modeless shadow dialog");
    }
    g_Block = previous;
    std::puts("PASS shadow brightness default, translation, typing and slider");
}

const wchar_t* expectedTitle;
const wchar_t* expectedBody;
const wchar_t* expectedDetail;
unsigned char language;
int response = IDCANCEL;
int seen = 0;
bool projectDialog = false;
bool mismatchDialog = false;
const wchar_t* expectedButtons[4]{};
LRESULT CALLBACK Inspect(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        HWND dialog = reinterpret_cast<HWND>(wp);
        wchar_t cls[32]{}; GetClassNameW(dialog, cls, 32);
        if (std::wstring(cls) == L"#32770") {

            ++seen;
            Check(Text(dialog) == expectedTitle, "localized title on actual dialog");
            const auto body = Text(GetDlgItem(dialog, projectDialog ? 686 : 0xFFFF));
            Check(body.find(expectedBody) != std::wstring::npos, "localized body on actual dialog");
            if (expectedDetail) Check(body.find(expectedDetail) != std::wstring::npos, "Unicode name preserved");
            if (HWND cancel = GetDlgItem(dialog, IDCANCEL))
                Check(Text(cancel) == (language == 0 ? L"キャンセル" : language == 1 ? L"Cancel" : L"取消"),
                      "cancel button follows app language, not OS language");
            if (projectDialog) {
                for (int i = 0; i < (mismatchDialog ? 4 : 3); ++i)
                    Check(Text(GetDlgItem(dialog, 687 + i)) == expectedButtons[i],
                          "project model action follows current UI language");
                Check((GetDlgItem(dialog, 690) != nullptr) == mismatchDialog,
                      "missing and mismatched models use the correct resources");
                RECT bodyRect{}, firstButton{};
                GetWindowRect(GetDlgItem(dialog, 686), &bodyRect);
                GetWindowRect(GetDlgItem(dialog, 687), &firstButton);
                HDC dc = GetDC(dialog);
                HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendDlgItemMessageW(dialog, 686, WM_GETFONT, 0, 0)));
                TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
                SelectObject(dc, oldFont); ReleaseDC(dialog, dc);
                Check(bodyRect.bottom <= firstButton.top && bodyRect.bottom - bodyRect.top >= 2 * metrics.tmHeight,
                      "prompt and Unicode model name fit above the action buttons");
            }
            ShowWindow(dialog, SW_HIDE);
            if (response == IDOK)
                PostMessageW(dialog, WM_CLOSE, 0, 0);
            else
                PostMessageW(dialog, WM_COMMAND, response, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}
int main(int argc,char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    HWND owner = CreateWindowExW(0, L"STATIC", L"dialog regression", WS_OVERLAPPED,
                                0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(owner != nullptr, "create test-owned window");
    app->state.hwnd = owner;
    if(argc>1&&std::strcmp(argv[1],"accessory-load")==0){const bool ok=AccessoryLoadMessageRegression(app.get(),owner);DestroyWindow(owner);return ok?0:1;}
    if(argc>1&&std::strcmp(argv[1],"auto-curve")==0){NativeCurveLinkRegression(app.get(),owner);DestroyWindow(owner);return 0;}
    if(argc>1&&std::strcmp(argv[1],"edge")==0){EdgeThicknessRegression(app.get(),owner);DestroyWindow(owner);return 0;}
    auto combo = [&](int id) {
        HWND window = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
            0, 0, 100, 100, owner, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(window, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"fixture"));
        SendMessageW(window, CB_SETCURSEL, 0, 0);
        return window;
    };
    Check(combo(436) && combo(471), "create selection fixtures");
    auto model = std::make_unique<mdl::ModelRecord>();
    wchar_t nativeName[] = L"中文モデル😀";
    wchar_t englishName[] = L"English model😀";
    model->pmxTextBuffers[0] = nativeName;
    model->pmxTextBuffers[1] = englishName;
    model->comboSelIndex = 0;
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
    auto accessory = std::make_unique<mdl::AccessoryRecord>();
    Check(text_encoding::EncodeDisplay(accessory->name, sizeof(accessory->name), nativeName), "accessory Unicode fixture");
    app->AccessorySlot(0) = accessory.get();
    HHOOK hook = SetWindowsHookExW(WH_CBT, Inspect, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "install modal inspection");
    const wchar_t* modelTitles[] = {L"モデル削除", L"Delete model", L"删除模型"};
    const wchar_t* accessoryTitles[] = {L"アクセサリ削除", L"Delete accessory", L"删除配件"};
    const wchar_t* deleteBodies[] = {L"元に戻せません", L"cannot be undone", L"无法撤销"};
    const wchar_t* newTitles[] = {L"新規作成", L"New scene", L"新建场景"};
    const wchar_t* newBodies[] = {L"破棄", L"Discard", L"放弃"};
    const wchar_t* eyeTitles[] = {L"目フレーム削除", L"Delete eye frames", L"删除眼部帧"};
    const wchar_t* projectTitles[] = {L"プロジェクト読み込み", L"Load project", L"加载项目"};
    const wchar_t* missingBodies[] = {L"モデルファイルを開けません", L"Cannot open the model file", L"找不到或无法打开模型文件"};
    const wchar_t* mismatchBodies[] = {L"モデルの構造が PMM と異なります", L"Model structure differs from the PMM", L"模型结构与 PMM 工程不一致"};
    const wchar_t* locate[] = {L"モデルファイルを指定して読み込む", L"Locate and load the model file", L"指定模型文件位置并加载"};
    const wchar_t* locateAnother[] = {L"別のモデルファイルを指定して読み込む", L"Locate another model file", L"指定其他模型文件"};
    const wchar_t* fit[] = {L"PMM のモデル情報を合わせて続行", L"Adapt PMM model data and continue", L"将 PMM 数据适配到此模型并继续"};
    const wchar_t* skip[] = {L"このモデルを飛ばして続行", L"Skip this model and continue", L"跳过此模型并继续加载"};
    const wchar_t* cancel[] = {L"プロジェクトの読み込みを中止", L"Cancel loading this project", L"取消加载整个项目"};
    // Reuse the same app and return to Japanese to catch cached language state.
    for (unsigned char selected : {0, 2, 1, 0}) {
        language = selected; app->EnglishUI() = selected;
        projectDialog = false; response = IDCANCEL;
        expectedTitle = modelTitles[selected]; expectedBody = deleteBodies[selected];
        expectedDetail = selected == 1 ? englishName : nativeName;
        int before = seen;
        CmdControl400(app.get(), owner, 437, 0);
        Check(seen == before + 1 && app->ModelSlot(0) == reinterpret_cast<unsigned char*>(model.get()),
              "model delete cancel preserves selected model");
        expectedTitle = accessoryTitles[selected]; expectedDetail = nativeName;
        before = seen;
        CmdControl450(app.get(), owner, 473, 0);
        Check(seen == before + 1 && app->AccessorySlot(0) == accessory.get(),
              "accessory delete cancel preserves selected accessory");
        expectedTitle = eyeTitles[selected]; expectedDetail = nullptr;
        before = seen;
        CmdFileMenu(app.get(), owner, 228, 0);
        Check(seen == before + 1, "eye frame deletion has its own localized title");
        expectedTitle = newTitles[selected]; expectedBody = newBodies[selected]; expectedDetail = nullptr;
        before = seen;
        CmdResetState(app.get());
        Check(seen == before + 1 && app->ModelSlot(0) == reinterpret_cast<unsigned char*>(model.get()),
              "new scene cancel preserves scene");

        projectDialog = true; expectedTitle = projectTitles[selected]; expectedDetail = nativeName;
        for (bool mismatch : {false, true}) {
            mismatchDialog = mismatch;
            expectedBody = mismatch ? mismatchBodies[selected] : missingBodies[selected];
            expectedButtons[0] = mismatch ? fit[selected] : locate[selected];
            expectedButtons[1] = mismatch ? locateAnother[selected] : skip[selected];
            expectedButtons[2] = mismatch ? skip[selected] : cancel[selected];
            expectedButtons[3] = cancel[selected];
            for (int action = 0; action < (mismatch ? 4 : 3); ++action) {
                response = 687 + action; before = seen;
                const auto result = ShowProjectModelDialog(app.get(), GetModuleHandleW(nullptr), owner,
                    mismatch ? ProjectModelIssue::StructureMismatch : ProjectModelIssue::MissingFile, nativeName);
                Check(result == (action == 0 ? 1 : action + 2) && seen == before + 1,
                      "PMM model dialog preserves locate/fit, skip, and cancel results");
            }
            response = IDCANCEL; before = seen;
            Check(ShowProjectModelDialog(app.get(), GetModuleHandleW(nullptr), owner,
                mismatch ? ProjectModelIssue::StructureMismatch : ProjectModelIssue::MissingFile, nativeName) == 2
                && seen == before + 1, "PMM model dialog Escape cancels loading");
        }
    }
    // A missing project used to display Shift-JIS bytes through the system
    // ANSI code page, producing the reported garbled error box.
    auto* previousApp = g_Block; g_Block = app.get();
    Check(GetFullPathNameW(L"__mds_missing_project_regression__\\中文.pmm", 256, app->EnvFileName(), nullptr) > 0,
          "resolve missing fixture on local test volume");
    projectDialog = false; expectedDetail = L"中文.pmm"; response = IDOK;
    const wchar_t* openBodies[] = {L"プロジェクトファイルを開けません", L"Cannot open the project file", L"无法打开工程文件"};
    for (unsigned char selected : {0, 1, 2}) {
        language = selected; app->EnglishUI() = selected;
        expectedTitle = projectTitles[selected]; expectedBody = openBodies[selected];
        const int before = seen;
        std::printf("missing project dialog language=%u\n", selected);
        LoadSceneFile();
        Check(seen == before + 1, "missing project reports Unicode error in current language");
    }
    g_Block = previousApp;
    UnhookWindowsHookEx(hook);
    PhysicsDialogTranslation(app.get(), owner);
    ShadowBrightnessDialog(app.get(), owner);
    app->ModelSlot(0) = nullptr; app->AccessorySlot(0) = nullptr;
    DestroyWindow(owner);
    std::puts("PASS actual model/accessory/new/PMM dialogs, language switching, Unicode names and action results");
}
