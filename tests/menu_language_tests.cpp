// MAINTENANCE: hardware-manual. Hidden window; no desktop capture.
//
// Menu localisation regression.  The menu under test is the one the program
// really ships: SAMPLE02E / SAMPLE02 out of res/MMD.rc, loaded with LoadMenuW.
// Nothing here builds a look-alike menu by hand, because a hand-built bar
// cannot drift and therefore cannot show the mis-translation this regression
// is about.  Every assertion reads the caption back by command id, which is
// exactly the identity the fix relies on.
//
// The test executable must link the menu resources:
//   add_executable(menu_language_tests EXCLUDE_FROM_ALL
//       tests/menu_language_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
//   target_link_libraries(menu_language_tests PRIVATE mikumikustudio_core winmm dsound)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include "menu_first_paint_checks.hpp"

using namespace mikumikustudio;


namespace {
int failures = 0;
void Check(bool good, const char* message) {
    if (!good) { std::printf("FAIL %s\n", message); ++failures; }
}

// The top-level popup that owns `command`, or -1.  Never a fixed index: the
// port inserts its own popups in front of Help.
int TopLevelWithCommand(HMENU bar, UINT command) {
    const int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW item{sizeof item};
        item.fMask = MIIM_SUBMENU;
        if (!GetMenuItemInfoW(bar, UINT(i), TRUE, &item) || !item.hSubMenu) continue;
        if (GetMenuState(item.hSubMenu, command, MF_BYCOMMAND) != UINT(-1)) return i;
    }
    return -1;
}

// Caption of `command` wherever it lives in `bar`, or empty.
std::wstring Caption(HMENU bar, UINT command) {
    const int top = TopLevelWithCommand(bar, command);
    if (top < 0) return std::wstring();
    wchar_t text[512]{};
    GetMenuStringW(GetSubMenu(bar, top), command, text, 512, MF_BYCOMMAND);
    return text;
}

std::wstring CaptionIn(HMENU popup, UINT command) {
    wchar_t text[512]{};
    GetMenuStringW(popup, command, text, 512, MF_BYCOMMAND);
    return text;
}

bool Same(const std::wstring& got, const wchar_t* want) { return got == want; }
}

int RunResource(const wchar_t* resourceName) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDPIAware();
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"menu-language-tests";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
                                  0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    Check(window != nullptr, "hidden window");

    // The shipped menu, not a hand-built stand-in.
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HMENU menu = LoadMenuW(instance, resourceName);
    Check(menu != nullptr,
          "the SAMPLE02/SAMPLE02E menu resource is linked into this binary");
    if (menu == nullptr) {
        std::printf("FAIL menu resource missing; link res/MMD.rc and the generated resources\n");
        return 1;
    }
    SetMenu(window, menu);

    auto ownedApp = std::make_unique<MMDApp>();
    MMDApp& app = *ownedApp;
    app.state = MMDAppState{};
    app.state.hwnd = window;
    Check(!app.AutoTransparentMaterialSortingEnabled(), "new app defaults to sorting off");

    const int view = TopLevelWithCommand(menu, 212);
    const int help = TopLevelWithCommand(menu, 201);
    const int file = TopLevelWithCommand(menu, 204);
    Check(view >= 0, "the View popup is found through its own command");
    Check(help >= 0, "the Help popup is found through its About command");
    Check(file >= 0, "the File popup is found through its own command");
    if (view < 0 || help < 0 || file < 0) return 1;
    HMENU viewPopup = GetSubMenu(menu, view);
    HMENU helpPopup = GetSubMenu(menu, help);
    HMENU filePopup = GetSubMenu(menu, file);

    struct Item { HMENU popup; UINT command; const wchar_t* zh; const wchar_t* en; const wchar_t* jp; };
    const Item items[] = {
        // View - the menu the bug report names
        {viewPopup, 212, L"画面尺寸(&O)", L"Screen size(&O)", L"出力サイズ(&O)"},
        {viewPopup, 280, L"独立窗口(&W)", L"Separate window(&W)", L"別窓(&W)"},
        {viewPopup, 247, L"相机与照明追踪(&C)", L"Camera & lighting tracking(&C)",
         L"ﾓﾃﾞﾙ編集時ｶﾒﾗ･照明追従(&C)"},
        {viewPopup, 211, L"显示信息(&D)", L"Information display(&D)", L"情報表示(&D)"},
        {viewPopup, 215, L"显示坐标轴(&G)", L"Display coordinate axis(&G)", L"座標軸表示(&G)"},
        {viewPopup, 221, L"显示地面阴影(&S)", L"Display ground shadow(&S)", L"地面影表示(&S)"},
        {viewPopup, 248, L"地面阴影亮度(&X)", L"Ground shadow color(&X)", L"地面影色設定(&X)"},
        {viewPopup, 254, L"透明地面阴影(&T)", L"Transparent ground shadow(&T)", L"地面影色透明化(&T)"},
        {viewPopup, 214, L"模型半透明(V键)", L"Character transparent mode(V_key)", L"半透明化(Vｷｰ)"},
        {viewPopup, 283, L"隐藏模型(&A)", L"Character Non-display mode(&A)", L"モデル非表示(&A)"},
        {viewPopup, 253, L"描边粗细(&E)", L"Thickness of edge line(&E)", L"エッジ太さ(&E)"},
        {viewPopup, 286, L"描边颜色(&B)", L"Edge line color(&B)", L"エッジ色(&B)"},
        {viewPopup, 277, L"抗锯齿(&H)", L"Anti-aliasing(&H)", L"アンチエイリアス(&H)"},
        {viewPopup, 279, L"自阴影(&P)", L"Self-shadow(&P)", L"セルフシャドウ表示(&P)"},
        {viewPopup, 287, L"线框(&R)", L"Wire frame(&R)", L"ワイヤーフレーム表示(&R)"},
        {viewPopup, 290, L"全屏(Alt+Enter)", L"Full screen(Alt+Enter)", L"フルスクリーン表示(Alt+Enter)"},
        {viewPopup, 298, L"多级纹理（各向异性过滤）(&M)", L"Mipmap(anisotropic)(&M)", L"ミップマップ(異方性フィルタ)(&M)"},
        {viewPopup, 234, L"不限帧率", L"Fps no limit", L"fps無制限"},
        {viewPopup, 235, L"限制为30帧/秒", L"Max fps restricted to 30fps", L"30fps制限"},
        {viewPopup, 236, L"限制为60帧/秒", L"Max fps restricted to 60fps", L"60fps制限"},
        {viewPopup, 299, L"节省内存模式", L"Save CPU power", L"省エネモード"},
        // File - holds one of the migrated items' old home
        {filePopup, 204, L"新建(&N)", L"New(&N)", L"新規(&N)"},
        {filePopup, 200, L"退出(&X)", L"Exit(&X)", L"終了(&X)"},
        // Help - holds the two migrated items
        {helpPopup, 260, L"日语(&J)", L"Chinese Mode(&C)", L"English Mode(&E)"},
        {helpPopup, 278, L"重新载入纹理(&R)", L"Restore texture(&R)", L"ﾃｸｽﾁｬ読直し(&R)"},
        {helpPopup, 201, L"关于(&A)", L"About(&A)", L"ﾊﾞｰｼﾞｮﾝ情報(&I)"},
        {helpPopup, 281, L"独立窗口置顶(&F)", L"Keep separate window on top(&F)", L"別窓最前列表示(&F)"},
        {helpPopup, 301, L"记住上次打开的目录(&E)", L"Set default folder to previous(&E)",
         L"ﾃﾞﾌｫﾙﾄﾌｫﾙﾀﾞを前回位置とする(&E)"},
    };
    const int itemCount = int(sizeof(items) / sizeof(items[0]));

    const auto sweep = [&](unsigned language, const wchar_t* name) {
        app.EnglishUI() = static_cast<unsigned char>(language);
        RefreshMenuLanguage(&app);
        for (int i = 0; i < itemCount; ++i) {
            const auto& item = items[i];
            const wchar_t* want = language == 2 ? item.zh : language == 1 ? item.en : item.jp;
            if (want == nullptr) continue;  // not asserted in this language
            const std::wstring got = CaptionIn(item.popup, item.command);
            if (!Same(got, want)) {
                std::printf("FAIL %ls command %u: got \"%ls\" want \"%ls\"\n",
                            name, item.command, got.c_str(), want);
                ++failures;
            }
        }
    };
    sweep(2, L"chinese");
    sweep(1, L"english");
    sweep(0, L"japanese");
    sweep(2, L"chinese-again");

    // Runtime injection must not disturb the translations: a decoy item in
    // View and a decoy popup in Help shift every position after them, and the
    // captions have to stay right because they are addressed by command.
    InsertMenuW(viewPopup, 0, MF_BYPOSITION | MF_STRING, 9901, L"decoy at the head of View");
    HMENU decoy = CreatePopupMenu();
    AppendMenuW(decoy, MF_STRING, 9902, L"decoy child");
    InsertMenuW(helpPopup, 0, MF_BYPOSITION | MF_POPUP, UINT_PTR(decoy), L"decoy at the head of Help");
    sweep(2, L"chinese-with-decoys");
    sweep(1, L"english-with-decoys");
    DeleteMenu(viewPopup, 9901, MF_BYCOMMAND);
    DeleteMenu(helpPopup, 0, MF_BYPOSITION);
    sweep(2, L"chinese-after-decoys-removed");

    // The open submenu must survive a language change: RefreshMenuLanguage
    // only writes captions, so the handles and the navigation stay put.
    app.EnglishUI() = 2;
    RefreshMenuLanguage(&app);
    const int stableView = TopLevelWithCommand(menu, 212);
    const int stableHelp = TopLevelWithCommand(menu, 201);
    HMENU viewBefore = GetSubMenu(menu, stableView);
    HMENU helpBefore = GetSubMenu(menu, stableHelp);
    for (int round = 0; round < 3; ++round) {
        app.EnglishUI() = static_cast<unsigned char>(round % 3);
        RefreshMenuLanguage(&app);
        Check(TopLevelWithCommand(menu, 212) == stableView, "the View popup keeps its identity");
        Check(TopLevelWithCommand(menu, 201) == stableHelp, "the Help popup keeps its identity");
        Check(GetSubMenu(menu, stableView) == viewBefore, "the View submenu handle survives");
        Check(GetSubMenu(menu, stableHelp) == helpBefore, "the Help submenu handle survives");
    }

    // Runtime refresh has installed the actual processor, bake and sort menus.
    Check((GetMenuState(viewPopup, 308, MF_BYCOMMAND) & MF_CHECKED) != 0, "default sort menu checks off");
    Check(!Caption(menu, 32002).empty(), "processor CPU entry exists");
    Check(!Caption(menu, 304).empty() && !Caption(menu, 308).empty(), "all sort endpoints exist");
    Check(GetMenuState(helpPopup, 32810, MF_BYCOMMAND) != UINT(-1), "owner-drawn corner selector exists");
    app.EnglishUI() = 2;
    RefreshMenuLanguage(&app);
    Check(CaptionIn(helpPopup, 259) == L"编辑英文名称(&E)", "enhance-model child remains translated");
    auto renderer = std::make_unique<D3DRenderer>();
    app.Renderer() = renderer.get();
    renderer->stereoEnabled = 1;
    RefreshMenuLanguage(&app);
    Check(CaptionIn(viewPopup, 290) == L"NVIDIA 3D Vision(Alt+Enter)", "stereo uses full-screen command");
    renderer->stereoEnabled = 0;
    app.Renderer() = nullptr;
    sweep(2, L"chinese-after-stereo");

    const wchar_t* languageCaptions[][3] = {
        {L"現在のモデルの既定言語：自動", L"現在のモデルの既定言語：日本語", L"現在のモデルの既定言語：英語"},
        {L"Current model default language: Auto", L"Current model default language: Japanese", L"Current model default language: English"},
        {L"当前模型默认语言：自动", L"当前模型默认语言：日语", L"当前模型默认语言：英语"}
    };
    const wchar_t* translationCaptions[] = {
        L"モデル名を自動翻訳", L"Model language auto-translation", L"模型语言自动翻译"};
    for (unsigned char language : {kUiJapanese, kUiEnglish, kUiChinese}) {
        for (unsigned char preference : {kModelInfoLanguageAuto, kModelInfoLanguageJapanese, kModelInfoLanguageEnglish}) {
            for (bool enabled : {false, true}) {
                app.EnglishUI() = language;
                SetModelInfoLanguagePreference(preference);
                SetModelNameAutoTranslation(enabled);
                RefreshMenuLanguage(&app);
                Check(CaptionIn(helpPopup, 310) == languageCaptions[language][preference],
                      "model-language caption includes the selected default");
                Check(CaptionIn(helpPopup, 311) == translationCaptions[language],
                      "auto-translation caption follows the UI language");
                const UINT state = GetMenuState(helpPopup, 311, MF_BYCOMMAND);
                Check(state != UINT(-1) && ((state & MF_CHECKED) != 0) == enabled,
                      "auto-translation check mark follows its setting");
                int position = -1;
                for (int i = 0; i < GetMenuItemCount(helpPopup); ++i)
                    if (GetMenuItemID(helpPopup, i) == 310) position = i;
                Check(position >= 0 && GetMenuItemID(helpPopup, position + 1) == 311,
                      "auto-translation is directly below the model language");
                Check((state & (MF_GRAYED | MF_DISABLED)) == 0,
                      "auto-translation remains available without a loaded model");
            }
        }
    }
    SetModelInfoLanguagePreference(kModelInfoLanguageAuto);
    SetModelNameAutoTranslation(false);

    // Model loading and panel transitions can leave the old parent-popup
    // state behind. Opening Help must use the current mode and selected model.
    auto model = std::make_unique<mdl::ModelRecord>();
    MMDApp* previousApp = g_Block;g_Block = &app;
    const auto extensionDisabled = [&]() {
        for(int i=0;i<GetMenuItemCount(helpPopup);++i){
            HMENU child=GetSubMenu(helpPopup,i);
            for(int j=0;child&&j<GetMenuItemCount(child);++j)if(GetMenuItemID(child,j)==259){
                const UINT state=GetMenuState(helpPopup,UINT(i),MF_BYPOSITION);
                return (state&(MF_GRAYED|MF_DISABLED))!=0;
            }
        }
        Check(false,"model extension popup exists");return true;
    };
    for(int language : {0,1,2}){
        app.EnglishUI()=static_cast<unsigned char>(language);RefreshMenuLanguage(&app);
        app.ModelSlot(0)=reinterpret_cast<unsigned char*>(model.get());app.SetSelectedModelSlot(0);app.CameraMode()=0;
        // A loaded read-only PMX or a legacy menu update can leave these
        // independent display preferences disabled until a language refresh.
        model->physicsMode = 2;
        for (unsigned char preference : {kModelInfoLanguageAuto, kModelInfoLanguageJapanese,
                                         kModelInfoLanguageEnglish}) {
            SetModelInfoLanguagePreference(preference);
            for (bool enabled : {false, true}) {
                SetModelNameAutoTranslation(enabled);
                CheckMenuItem(helpPopup, 311, MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
                EnableMenuItem(helpPopup, 310, MF_BYCOMMAND | MF_GRAYED);
                EnableMenuItem(helpPopup, 311, MF_BYCOMMAND | MF_GRAYED);
                MainWndProc(window, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(helpPopup), 0);
                Check((GetMenuState(helpPopup, 310, MF_BYCOMMAND) & (MF_GRAYED | MF_DISABLED)) == 0,
                      "opening Help restores the model-language preference");
                const auto state = GetMenuState(helpPopup, 311, MF_BYCOMMAND);
                Check((state & (MF_GRAYED | MF_DISABLED)) == 0,
                      "opening Help restores auto-translation for a read-only model");
                Check(((state & MF_CHECKED) != 0) == enabled && gModelNameAutoTranslation == enabled,
                      "restoring availability preserves the translation setting");
            }
        }
        SetModelExtensionMenuEnabled(&app,false);
        MainWndProc(window,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(helpPopup),0);
        Check(!extensionDisabled(),"opening Help enables model extension for the current model");
        app.CameraMode()=1;SetModelExtensionMenuEnabled(&app,true);
        MainWndProc(window,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(helpPopup),0);
        Check(extensionDisabled(),"camera mode disables model extension");
        app.CameraMode()=0;app.ModelSlot(0)=nullptr;SetModelExtensionMenuEnabled(&app,true);
        MainWndProc(window,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(helpPopup),0);
        Check(extensionDisabled(),"model extension is disabled without a selected model");
    }
    g_Block=previousApp;

    if (failures == 0)
        std::printf("PASS menu language: %d items x 3 languages, decoy injection, handle stability\n",
                    itemCount);
    SetMenu(window, nullptr);
    DestroyWindow(window);
    DestroyMenu(menu);
    return failures == 0 ? 0 : 1;
}

int main(int argc,char** argv) {
    if(argc==2 && !std::strcmp(argv[1],"--editor-first-paint"))return menu_first_paint::Run(true,false,false,true);
    if(argc==2 && !std::strcmp(argv[1],"--first-paint-late"))return menu_first_paint::Run(true,false);
    if(argc==2 && !std::strcmp(argv[1],"--first-paint-no-idle"))return menu_first_paint::Run(false,true);
    if(argc==2 && !std::strcmp(argv[1],"--first-paint-slow"))return menu_first_paint::Run(true,false,true);
    if(argc==2 && !std::strcmp(argv[1],"--first-paint"))return menu_first_paint::Run();
    std::printf("Menu regression: ACP=%u\n", GetACP());
    RunResource(L"SAMPLE02E");
    RunResource(L"SAMPLE02");
    return failures ? 1 : 0;
}
