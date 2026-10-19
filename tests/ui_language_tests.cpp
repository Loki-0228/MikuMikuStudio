// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// ===========================================================================
// User-acceptance regressions for the UI language of model names (defect 3)
// and for the Japanese captions of the bone-operation panel (defect 4).
// ===========================================================================
// Field report (defect 3): with the Chinese UI the bone list showed English
// bone names while the facial (morph) list showed the model's own Japanese
// names; the mix drifted with the order in which models and languages were
// loaded.
// Root cause: name selection tested the same UI-language byte (0 = Japanese,
// 1 = English, 2 = Chinese) two different ways - `EnglishUI() == 1` in the
// morph combos, the panel tree and the palette, but `EnglishUI() != 0` in the
// bone-combo paths (RefillBoneRegisterCombo, RepopulateBoneCombo, the select
// dialog, the physics dialog).  The original's byte is two-valued, so `!= 0`
// meant "English UI"; the port's third value 2 = Chinese is *not* English, so
// every `!= 0` bone list flipped to the English name fields under the Chinese
// UI while the morph lists stayed on the model's own names.
//
// Field report (defect 4): bone-operation panel buttons had no (or English)
// captions under the Japanese UI.  Controls 494 ("select all") and 501
// ("select unregistered") have an empty Japanese caption in the generated
// creation table (src/window/ui_controls.inc) and previously fell back to the
// English labels, and 562 ("nothing"/off) carried the byte-shifted mis-read
// "q_j0W0" of its UTF-16 caption.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/bone_combo.hpp"
#include "../src/window/ui_controls.inc"  // generated creation table (caption sweep)

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <clocale>

namespace mikumikustudio {
// LocalizeUI's caption lookup and localized-id list (src/window/localize_ui.cpp);
// not exported through a header because only the UI pass and these tests use
// them.
std::wstring UiControlCaption(int id, unsigned char language);
int UiLocalizedControlCount();
int UiLocalizedControlId(int index);
void DrawTimelineLabel(MMDApp*, HDC, const wchar_t*, int, int, COLORREF);
HWND CreateUiPanelControl(const ui::ControlSpec&, HWND, HINSTANCE, int);
void RefreshModelSelectorNames(MMDApp*);
}  // namespace mikumikustudio

using namespace mikumikustudio;

static void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

namespace {

const char* LanguageName(unsigned char language) {
    return language == kUiJapanese ? "Japanese"
         : language == kUiEnglish  ? "English"
                                   : "Chinese";
}

std::wstring ItemTextW(HWND combo, int index) {
    wchar_t buffer[256]{};
    SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(buffer));
    return buffer;
}

HWND MakeCombo(HWND parent, int id, bool wide) {
    return wide ? CreateWindowExW(0, L"COMBOBOX", L"",
                                  WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 200, 200,
                                  parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr)
                : CreateWindowExA(0, "COMBOBOX", "",
                                  WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 200, 200,
                                  parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
}

void MainPanelComboRegression() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
        0, 0, 320, 240, nullptr, nullptr, instance, nullptr);
    Check(parent != nullptr, "create hidden main-panel owner");
    const wchar_t* labels[] = {L"模型_洛天依_😀.pmx", L"左足ＩＫ_髪", L"眨眼_中文_完整名称"};
    for (const auto& spec : ui::kControls) {
        const bool combo = spec.wcls ? std::wcscmp(spec.wcls, L"COMBOBOX") == 0
            : spec.acls && std::strcmp(spec.acls, "COMBOBOX") == 0;
        if (!combo) continue;
        HWND control = CreateUiPanelControl(spec, parent, instance, 250);
        Check(control != nullptr, "create actual main-panel combo");
        for (const auto* label : labels) {
            const auto row = SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            if (row < 0 || ItemTextW(control, static_cast<int>(row)) != label) {
                std::fprintf(stderr, "FAIL: main-panel combo %d loses Unicode name (ACP=%u)\n", spec.id, GetACP());
                std::exit(1);
            }
        }
        Check(IsWindowUnicode(control), "main-panel name combo is Unicode");
        DestroyWindow(control);
    }
    DestroyWindow(parent);
    std::puts("PASS production main-panel combos preserve Unicode names");
}

void ModelSelectorSelectionRegression() {
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    auto model = std::make_unique<mdl::ModelRecord>();
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
        0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "create hidden selector owner");
    HWND main = MakeCombo(parent, 436, true);
    HWND ground = MakeCombo(parent, 474, true);
    HWND normal = MakeCombo(parent, 449, true);
    app->Hwnd() = parent;
    app->ModelSlot(7) = reinterpret_cast<unsigned char*>(model.get());
    app->SetSelectedModelSlot(7);
    app->CameraMode() = 0;
    app->CameraParentModel() = 7;
    model->comboSelIndex = 1;
    model->pmxTextBuffers[0] = const_cast<wchar_t*>(L"模型_中文_髪_😀");
    const auto previousPreference = gModelInfoLanguagePreference;
    SetModelInfoLanguagePreference(kModelInfoLanguageAuto);
    for (unsigned char language : {kUiChinese, kUiJapanese, kUiEnglish}) {
        app->EnglishUI() = language;
        for (HWND combo : {main, ground, normal}) SendMessageW(combo, CB_SETCURSEL, -1, 0);
        RefreshModelSelectorNames(app.get());
        Check(SendMessageW(main, CB_GETCURSEL, 0, 0) == 1,
              "initial model selector recovers the current model without a click");
        Check(SendMessageW(normal, CB_GETCURSEL, 0, 0) == 1,
              "initial camera parent selector recovers the bound model");
        Check(SendMessageW(ground, CB_GETCURSEL, 0, 0) == 0,
              "unbound accessory selector shows ground instead of a blank caption");
        wchar_t caption[128]{};
        GetWindowTextW(main, caption, _countof(caption));
        Check(std::wcscmp(caption, model->pmxTextBuffers[0]) == 0,
              "collapsed model caption retains Unicode before any click");
        SendMessageW(main, CB_SETCURSEL, 0, 0);
        RefreshModelSelectorNames(app.get());
        Check(SendMessageW(main, CB_GETCURSEL, 0, 0) == 0,
              "an existing valid selector choice survives refresh");
    }
    SetModelInfoLanguagePreference(previousPreference);
    app->ModelSlot(7) = nullptr;
    DestroyWindow(parent);
    std::puts("PASS initial model selectors: active model, parent fallback, closed Unicode caption, preserved selection");
}

// ---------------------------------------------------------------------------
// Defect 3: one language for every list.
//
// Use actual Japanese and full PMX Unicode names on non-Japanese Windows.
// ---------------------------------------------------------------------------
void NameLanguageRegression() {
    // The rule itself: Chinese is not English.
    Check(!UiUsesEnglishNames(kUiJapanese), "Japanese UI uses the model's own names");
    Check(UiUsesEnglishNames(kUiEnglish), "English UI uses the English names");
    Check(!UiUsesEnglishNames(kUiChinese),
          "Chinese UI is not English (regression: `!= 0` sent bone lists to nameEn)");

    HWND parent = CreateWindowExW(0, L"STATIC", L"ui language test", WS_OVERLAPPED,
                                  0, 0, 320, 240, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "create hidden control owner");

    HWND ikCombo = MakeCombo(parent, panel::kIkChainCombo, true);              // 443
    HWND boneCombo = MakeCombo(parent, panel::kBoneRegisterCombo, true);       // 450
    HWND morphCombos[4] = {
        MakeCombo(parent, panel::kMorphCombo0, true),                           // 504
        MakeCombo(parent, panel::kMorphCombo1, true),                           // 509
        MakeCombo(parent, panel::kMorphCombo2, true),                           // 514
        MakeCombo(parent, panel::kMorphCombo3, true),                           // 519
    };
    Check(ikCombo && boneCombo && morphCombos[0] && morphCombos[1] &&
              morphCombos[2] && morphCombos[3],
          "create the language test combos");

    std::array<mdl::BoneRecord, 2> bones{};
    std::strcpy(bones[0].name, "\x83\x5A\x83\x93\x83\x5E\x81\x5B");
    std::strcpy(bones[0].nameEn, "bone-en");
    bones[1].jpText = const_cast<wchar_t*>(L"左足ＩＫ_中文_長い骨の名前");
    bones[1].enText = const_cast<wchar_t*>(L"ik-en");
    std::strcpy(bones[1].nameEn, "ik-en");
    bones[1].type = mdl::BoneType::Ik;

    std::array<mdl::MorphRecord, 4> morphs{};
    const wchar_t* jpMorph[4] = {L"morph-jp0", L"morph-jp1", L"morph-jp2", L"morph-jp3"};
    const wchar_t* enMorph[4] = {L"morph-en0", L"morph-en1", L"morph-en2", L"morph-en3"};
    const mdl::MorphPanel panelOf[4] = {
        mdl::MorphPanel::eyebrow, mdl::MorphPanel::eye,
        mdl::MorphPanel::mouth, mdl::MorphPanel::other};
    for (int lane = 0; lane < 4; ++lane) {
        morphs[lane].jpText = const_cast<wchar_t*>(jpMorph[lane]);
        morphs[lane].enText = const_cast<wchar_t*>(enMorph[lane]);
        morphs[lane].panel = panelOf[lane];
    }

    // The IK chain names its own bone (combo 443) - bone 0 - while the bone
    // register combo (450) lists every selectable bone, so both lists start
    // with the same name and can be compared directly.
    mdl::IkChain chain{};
    chain.boneIndex = 0;

    auto model = std::make_unique<mdl::ModelRecord>();
    auto app = std::make_unique<MMDApp>();
    MMDApp* previousBlock = g_Block;
    g_Block = app.get();

    app->Hwnd() = parent;
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
    app->SetSelectedModelSlot(0);
    model->hwnd = parent;
    model->boneTable = bones.data();
    model->boneCount = static_cast<std::uint32_t>(bones.size());
    model->morphs = morphs.data();
    model->morphCount = static_cast<std::uint32_t>(morphs.size());
    model->ikChains = &chain;
    model->ikChainCount = 1;
    model->displayRootBone = 0;
    for (int lane = 0; lane < 4; ++lane)
        model->selectedMorphs[lane] = lane;

    const unsigned char languages[] = {kUiJapanese, kUiEnglish, kUiChinese};
    for (unsigned char language : languages) {
        app->EnglishUI() = language;
        const bool englishNames = UiUsesEnglishNames(language);
        const std::wstring expectedBone = englishNames ? L"bone-en" : L"センター";
        const std::wstring expectedMorph = englishNames ? L"morph-en1" : L"morph-jp1";

        PostLoadInit(reinterpret_cast<unsigned char*>(model.get()));
        RefillBoneRegisterCombo(app.get(), 0);

        // IK chain combo 443 and the model/camera follow-bone list (450).
        Check(ItemTextW(ikCombo, 0) == expectedBone,
              "IK bone combo follows the UI language");
        Check(ItemTextW(boneCombo, 0) == expectedBone,
              "follow-bone list follows the UI language (RefillBoneRegisterCombo)");
        Check(ItemTextW(boneCombo, 0) == ItemTextW(ikCombo, 0),
              "the two bone lists must agree");

        // The facial (morph) list is the reference: same language or not.
        const std::wstring morphName = ItemTextW(morphCombos[1], 0);
        Check(morphName == expectedMorph, "facial list follows the UI language");
        const bool bonesEnglish = ItemTextW(boneCombo, 0) == L"bone-en";
        const bool morphsEnglish = morphName == L"morph-en1";
        Check(bonesEnglish == morphsEnglish,
              "bone list and facial list must use one language");
        Check(ItemTextW(boneCombo, 1) == (englishNames ? L"ik-en" : L"左足ＩＫ_中文_長い骨の名前"),
              "PMX names retain Unicode and their complete length");
        SelectBoneCombo(boneCombo, 1);
        Check(SelectedComboBone(boneCombo, model.get()) == 1, "selection uses bone identity");
        std::printf("PASS %s UI: Unicode bones and morphs\n", LanguageName(language));
    }

    // The reported case, from a language switch instead of a fresh load: a
    // stale per-model language byte must not move the bone list either.
    app->EnglishUI() = kUiEnglish;
    PostLoadInit(reinterpret_cast<unsigned char*>(model.get()));
    RefillBoneRegisterCombo(app.get(), 0);
    app->EnglishUI() = kUiChinese;
    PostLoadInit(reinterpret_cast<unsigned char*>(model.get()));
    RefillBoneRegisterCombo(app.get(), 0);
    Check(ItemTextW(boneCombo, 0) == L"センター" &&
              ItemTextW(morphCombos[1], 0) == L"morph-jp1",
          "EN -> CHS switch keeps bone and facial lists in one language");
    std::puts("PASS English -> Chinese switch keeps one name language");

    bones[0].jpText = bones[1].jpText;
    FillBoneCombo(boneCombo, model.get(), false);
    SelectBoneCombo(boneCombo, 1);
    Check(SelectedComboBone(boneCombo, model.get()) == 1,
          "duplicate display names select the requested bone index");
    bones[0].type = mdl::BoneType::InertTip;
    FillBoneCombo(boneCombo, model.get(), false);
    SelectBoneCombo(boneCombo, 1);
    Check(SendMessageW(boneCombo, CB_GETCURSEL, 0, 0) == 0 &&
          SelectedComboBone(boneCombo, model.get()) == 1,
          "filtered bone lists retain actual bone indices");

    g_Block = previousBlock;
    DestroyWindow(parent);
}

// ---------------------------------------------------------------------------
// Defect 4: no bone-operation panel caption may be empty in any language, and
// the three captions the generated table lost keep their originals.
//
// The windowed equivalent (creating all 168 controls and calling LocalizeUI)
// cannot run headless: LocalizeUI ends in PostLanguageSweep, which walks the
// loaded model's display groups and the panel back-store, neither of which
// exists without a device and an open model.  UiControlCaption() is that pass's
// caption lookup factored out, so asserting on it pins the same strings.
// ---------------------------------------------------------------------------
void CaptionRegression() {
    // 494 全て選択 / 501 未登録選 (bone-operation panel) and 562 なし
    // (self-shadow mode "off") - recovered originals, see localize_ui.cpp.
    Check(UiControlCaption(494, kUiJapanese) == std::wstring(L"\u5168\u3066\u9078\u629e"),
          "494 Japanese caption is 全て選択 (not the English fallback)");
    Check(UiControlCaption(501, kUiJapanese) == std::wstring(L"\u672a\u767b\u9332\u9078"),
          "501 Japanese caption is 未登録選 (not the English fallback)");
    Check(UiControlCaption(562, kUiJapanese) == std::wstring(L"\u306a\u3057"),
          "562 Japanese caption is なし (not the byte-shifted q_j0W0)");
    Check(UiControlCaption(494, kUiEnglish) == std::wstring(L"Select all") &&
              UiControlCaption(501, kUiEnglish) == std::wstring(L"Unregisted") &&
              UiControlCaption(562, kUiEnglish) == std::wstring(L"Off"),
          "the English captions start with uppercase letters");
    Check(UiControlCaption(494, kUiChinese) == std::wstring(L"\u5168\u9009") &&
              UiControlCaption(501, kUiChinese) == std::wstring(L"\u672a\u767b\u8bb0"),
          "the Chinese captions are unchanged");
    std::puts("PASS bone-operation panel captions 494/501 and shadow toggle 562");

    // Every control of the bone-operation panel (option band 4 of
    // HandleWindowSize: the select/rotate/move group, the keyframe buttons and
    // the two column toggles) carries a caption in all three languages.
    const int bonePanel[] = {
        panel::kBoneSelectRadio, 491, panel::kBoneMoveRadio, panel::kBoneRotateRadio,
        494, 495, 496, panel::kBonePasteButton, panel::kBoneReversePasteButton,
        panel::kPhysicsCheckbox, panel::kRegisterFrameButton, 501, 502, 503};
    for (int id : bonePanel) {
        for (unsigned char language : {kUiJapanese, kUiEnglish, kUiChinese}) {
            if (UiControlCaption(id, language).empty()) {
                std::fprintf(stderr, "FAIL: bone panel control %d has no %s caption\n",
                             id, LanguageName(language));
                std::exit(1);
            }
        }
    }
    std::puts("PASS every bone-operation panel control has a caption in JP/EN/CHS");

    // No control LocalizeUI rewrites may end up blank in any language: the JP
    // pass used to write L"" over a control whose caption was missing from both
    // the creation table and the English table.
    const int localized = UiLocalizedControlCount();
    Check(localized > 0, "the localized control table is populated");
    for (int index = 0; index < localized; ++index) {
        const int id = UiLocalizedControlId(index);
        Check(id > 0, "localized control id");
        for (unsigned char language : {kUiJapanese, kUiEnglish, kUiChinese}) {
            if (UiControlCaption(id, language).empty()) {
                std::fprintf(stderr, "FAIL: localized control %d has no %s caption\n",
                             id, LanguageName(language));
                std::exit(1);
            }
        }
    }
    std::puts("PASS no localized control loses its caption");

    // Sweep the generated creation table: any BUTTON that carries an English
    // caption must carry a Japanese one too (icon-only toggles carry neither).
    int captionedButtons = 0;
    for (const ui::ControlSpec& control : ui::kControls) {
        if (control.wcls == nullptr || std::wcscmp(control.wcls, L"BUTTON") != 0)
            continue;
        if (UiControlCaption(control.id, kUiEnglish).empty())
            continue;
        ++captionedButtons;
        if (UiControlCaption(control.id, kUiJapanese).empty()) {
            std::fprintf(stderr, "FAIL: button %d has an English caption but no Japanese one\n",
                         control.id);
            std::exit(1);
        }
    }
    Check(captionedButtons > 0, "the creation table advertises captioned buttons");
    std::printf("PASS %d captioned buttons all keep a Japanese caption\n",
                captionedButtons);
}

}  // namespace

static void CheckImportedTimelineNames(mdl::ModelRecord& model,
                                        const wchar_t* const* names) {
    // Exercise the actual import -> timeline sweep -> Unicode GDI path. The
    // independent reference bitmap uses the original fixture names, not the
    // lossy FrameGroup mirrors that caused this regression on Chinese Windows.
    constexpr int width = 100, height = 140;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* actual = nullptr; void* expected = nullptr;
    HBITMAP actualBitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &actual, nullptr, 0);
    HBITMAP expectedBitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &expected, nullptr, 0);
    HBITMAP blank = CreateBitmap(width, height, 1, 32, nullptr);
    HDC actualDc = CreateCompatibleDC(nullptr), expectedDc = CreateCompatibleDC(nullptr);
    HDC blankDc = CreateCompatibleDC(nullptr);
    Check(actualBitmap && expectedBitmap && blank && actualDc && expectedDc && blankDc,
          "create timeline name regression bitmaps");
    auto oldActual = SelectObject(actualDc, actualBitmap);
    auto oldExpected = SelectObject(expectedDc, expectedBitmap);
    auto oldBlank = SelectObject(blankDc, blank);
    PatBlt(blankDc, 0, 0, width, height, WHITENESS);
    SelectObject(blankDc, oldBlank); DeleteDC(blankDc);
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>();
    renderer->screenWidth = width; renderer->screenHeight = height;
    app->Renderer() = renderer.get(); app->PanelDC() = actualDc;
    app->state.bmpPanelSpare = blank;
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(&model);
    auto* groups = mdl::DisplayGroups(app->ModelSlot(0));
    for (int i = 0; i < model.groupCount; ++i) groups[i].flags = 1;
    const auto compareRow = [&](int row, int x, const wchar_t* name, bool group = false) {
        const int language = app->EnglishUI();
        Check(row >= 0 && row < 8, "fixture name has a visible timeline row");
        PatBlt(expectedDc, 0, 0, width, height, WHITENESS);
        if (group)
            DrawTimelineLabel(app.get(), expectedDc, language == 1 ? L"-" : L"－",
                              1, 17 + row * 14, RGB(0, 0, 0));
        DrawTimelineLabel(app.get(), expectedDc, name, x, 17 + row * 14, RGB(0, 0, 0));
        GdiFlush();
        for (int y = 16 + row * 14; y < 30 + row * 14; ++y)
            for (int px = 12; px < 91; ++px) {
                if ((static_cast<unsigned*>(actual)[y * width + px] & 0xffffff) !=
                    (static_cast<unsigned*>(expected)[y * width + px] & 0xffffff))
                    std::fprintf(stderr, "timeline mismatch: language=%d row=%d pixel=%d,%d actual=%06x expected=%06x\n",
                        language, row, px, y,
                        static_cast<unsigned*>(actual)[y * width + px] & 0xffffff,
                        static_cast<unsigned*>(expected)[y * width + px] & 0xffffff);
                Check((static_cast<unsigned*>(actual)[y * width + px] & 0xffffff) ==
                      (static_cast<unsigned*>(expected)[y * width + px] & 0xffffff),
                      "timeline renders the expected Unicode label in every UI language");
            }
    };
    for (int language : {1, 2, 0, 1}) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        PostLanguageSweep(app.get());
        compareRow(0, 12, names[0]);
        compareRow(static_cast<int>(model.boneKeyIndices[1]), 15,
                   language == 1 ? L"left arm" : names[1]);
        compareRow(static_cast<int>(model.boneKeyIndices[2]), 15, names[2]);
        compareRow(static_cast<int>(model.morphKeyIndices[0]), 15, names[3]);
        int groupRow = -1;
        for (int row = 1; row < 8; ++row)
            if (model.boneListRowType[row] == 3) groupRow = row;
        compareRow(groupRow, 12, names[4], true);
    }
    // Built-in tracks follow the UI language, unlike imported model names.
    // Exercise switching both ways, including returning to Chinese.
    const wchar_t* headers[][4] = {
        {L"カメラ", L"照　明", L"セルフ影", L"重　力"},
        {L"camera", L"light", L"s shadow", L"gravity"},
        {L"相机", L"照明", L"自阴影", L"重力"},
    };
    app->state.optflag[0] = 1;
    for (int language : {2, 0, 1, 2}) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        PostLanguageSweep(app.get());
        for (int row = 0; row < 4; ++row)
            compareRow(row, 12, headers[language][row]);
    }
    SelectObject(actualDc, oldActual); SelectObject(expectedDc, oldExpected);
    DeleteDC(actualDc); DeleteDC(expectedDc);
    DeleteObject(actualBitmap); DeleteObject(expectedBitmap); DeleteObject(blank);
    app->Renderer() = nullptr; app->ModelSlot(0) = nullptr; app->PanelDC() = nullptr;
}

static void MissingEnglishPmxRegression(bool chineseNames = false) {
    using namespace mikumikustudio;
    wchar_t directory[MAX_PATH]{}; GetTempPathW(MAX_PATH, directory);
    const auto path = std::wstring(directory) + L"MDS-bone-names-" +
        std::to_wstring(GetCurrentProcessId()) + L".pmx";
    int fd = -1;
    Check(_wsopen_s(&fd, path.c_str(), _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY | _O_TEMPORARY,
                   _SH_DENYRW, _S_IREAD | _S_IWRITE) == 0, "create temporary PMX fixture");
    const auto put = [&](auto value) { Check(_write(fd, &value, sizeof value) == sizeof value, "write fixture field"); };
    const auto text = [&](const wchar_t* value) {
        const auto bytes = static_cast<std::uint32_t>(std::wcslen(value) * sizeof(wchar_t));
        put(bytes); Check(_write(fd, value, bytes) == bytes, "write Unicode fixture name");
    };
    // LoadPMX starts just after the three-byte PMX magic consumed by its caller.
    put(' '); put(2.0f); put(std::uint8_t(8));
    put(std::uint8_t(0)); put(std::uint8_t(0));
    for (int i = 0; i < 6; ++i) put(std::uint8_t(1));
    const wchar_t* modelName = chineseNames ? L"模型_洛天依_长名称_😀" : L"名前テスト";
    text(modelName); text(L"name test"); text(L"fixture"); text(L"fixture");
    for (int i = 0; i < 4; ++i) put(std::uint32_t(0)); // vertices, indices, textures, materials
    const wchar_t* native[] = {L"センター_中文_長い骨の名前_😀", L"左腕", L"右腕", L"まばたき", L"腕"};
    if (chineseNames) {
        native[0] = L"操作中心_中文_很长的骨骼名称_😀";
        native[1] = L"左臂_完整名称";
        native[2] = L"右臂_完整名称";
        native[3] = L"眨眼_中文表情_完整名称_😀";
        native[4] = L"骨骼分组_完整名称_超过旧式五十字节字段_😀";
    }
    put(std::uint32_t(3));
    for (int i = 0; i < 3; ++i) {
        text(native[i]); text(i == 1 ? L"left arm" : L"");
        put(float(i)); put(0.f); put(0.f);
        put(std::int8_t(-1)); put(std::int32_t(0)); put(std::uint16_t(0x1B));
        put(std::int8_t(-1)); // visible, rotating bone; no tail
    }
    put(std::uint32_t(1)); text(native[3]); text(L"");
    put(std::uint8_t(2)); put(std::uint8_t(0)); put(std::uint32_t(0)); // empty group morph
    put(std::uint32_t(3));
    text(L"Root"); text(L"Root"); put(std::uint8_t(1)); put(std::uint32_t(1));
    put(std::uint8_t(0)); put(std::int8_t(0));
    text(L"表情"); text(L""); put(std::uint8_t(1)); put(std::uint32_t(1));
    put(std::uint8_t(1)); put(std::int8_t(0));
    text(native[4]); text(L""); put(std::uint8_t(0)); put(std::uint32_t(2));
    put(std::uint8_t(0)); put(std::int8_t(1)); put(std::uint8_t(0)); put(std::int8_t(2));
    put(std::uint32_t(0)); put(std::uint32_t(0)); // rigid bodies, joints
    Check(_lseek(fd, 0, SEEK_SET) == 0, "rewind PMX fixture");
    auto model = std::make_unique<mdl::ModelRecord>();
    auto* raw = reinterpret_cast<unsigned char*>(model.get());
    ModelInitDefaults(raw);
    PathResolutionWorkspace paths{};
    Check(LoadPMX(raw, nullptr, 0, 1, paths, fd), "load actual PMX with missing English names");
    for (int i = 0; i < 3; ++i) {
        const auto& bone = model->boneTable[i];
        Check(text_encoding::BoneName(bone, true) == (i == 1 ? L"left arm" : native[i]),
              "PMX English name falls back per bone without Null placeholders");
        Check(text_encoding::BoneName(bone, false) == native[i], "native bone names remain intact");
        Check(bone.jpText != bone.enText, "fallback buffers have independent ownership");
    }
    Check(text_encoding::ModelName(*model, false) == modelName, "PMX Unicode model name survives import");
    Check(text_encoding::MorphName(model->morphs[0], true) == native[3], "optional English morph also falls back");
    Check(std::strcmp(mdl::DisplayGroups(raw)[3].nameEn, mdl::DisplayGroups(raw)[3].name) == 0,
          "display groups do not introduce English Null placeholders");
    if (chineseNames) {
        HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(parent != nullptr, "create hidden name-list owner");
        HWND registration = MakeCombo(parent, 434, true);
        HWND morphCombo = MakeCombo(parent, 509, true);
        HWND boneCombo = MakeCombo(parent, 450, true);
        Check(registration && morphCombo && boneCombo, "create hidden name lists");
        auto app = std::make_unique<MMDApp>();
        auto* previousApp = g_Block;
        g_Block = app.get();
        model->hwnd = parent;
        app->Hwnd() = parent;
        app->ModelSlot(0) = raw;
        for (unsigned char language : {kUiChinese, kUiEnglish, kUiJapanese, kUiChinese}) {
            app->EnglishUI() = language;
            const bool english = UiUsesEnglishNames(language);
            PostLoadInit(raw);
            FillBoneCombo(boneCombo, model.get(), english);
            Check(ItemTextW(registration, 1) == native[0], "registration root keeps full Unicode name");
            Check(ItemTextW(registration, 6) == native[3], "registration morph keeps full Unicode name");
            Check(ItemTextW(registration, 8) == (english ? L"left arm" : native[1]),
                  "registration bone keeps full Unicode name");
            Check(ItemTextW(registration, 9) == native[2], "registration bone English fallback keeps Unicode");
            Check(SendMessageW(registration, CB_GETCOUNT, 0, 0) == 10,
                  "registration has no uninitialized trailing bone rows");
            Check(ItemTextW(morphCombo, 0) == native[3], "morph selector keeps Unicode");
            Check(ItemTextW(boneCombo, 2) == native[2], "bone selector keeps Unicode");
            Check(text_encoding::DisplayGroupName(mdl::DisplayGroups(raw)[3], english) == native[4],
                  "display group keeps full Unicode name and English fallback");
        }
        g_Block = previousApp;
        model->hwnd = nullptr;
        DestroyWindow(parent);
    }
    CheckImportedTimelineNames(*model, native);
    if (chineseNames) {
        auto& group = mdl::DisplayGroups(raw)[3];
        std::strcpy(group.nameEn, "edited group");
        Check(text_encoding::DisplayGroupName(group, true) == L"edited group",
              "English group edits override the imported Unicode name");
        Check(text_encoding::DisplayGroupName(group, false) == native[4],
              "English edits leave the native group name intact");
    }
    ModelDispose(raw);
    std::puts("PASS actual PMX import: missing English names use original Unicode names");
}

void DisplayGroupLifetimeRegression() {
    auto model = std::make_unique<mdl::ModelRecord>();
    auto* raw = reinterpret_cast<unsigned char*>(model.get());
    mdl::DisplayGroup group{};
    std::strcpy(group.name, "legacy");
    text_encoding::SetDisplayGroupName(*model, group, L"骨骼分组_😀", L"");
    Check(text_encoding::DisplayGroupName(group, true) == L"骨骼分组_😀",
          "missing group translation falls back to native Unicode");
    ModelInitDefaults(raw);
    Check(text_encoding::DisplayGroupName(group, false) == L"legacy",
          "model initialization forgets previous Unicode group names");
    text_encoding::SetDisplayGroupName(*model, group, L"新的分组", L"new group");
    ModelDispose(raw);
    Check(text_encoding::DisplayGroupName(group, false) == L"legacy",
          "model disposal forgets Unicode group names before address reuse");
}

void LegacyNameFallbackRegression() {
    const bool previousTranslation = gModelNameAutoTranslation;
    gModelNameAutoTranslation = false;
    mdl::MorphRecord morph{};
    mdl::BoneRecord bone{};
    mdl::DisplayGroup group{};
    // Fixed-width PMD/legacy fields can end halfway through a CP932 character.
    // Keep all bytes, including the missing terminator, to exercise the bound.
    std::memset(morph.name, 'm', sizeof(morph.name));
    morph.name[sizeof(morph.name) - 1] = char(0x82);
    std::strcpy(morph.nameEn, "blink");
    std::memset(group.name, 'g', sizeof(group.name));
    group.name[sizeof(group.name) - 1] = char(0x82);
    std::strcpy(group.nameEn, "custom frame");
    const auto originalMorph = morph;
    const auto originalGroup = group;
    Check(text_encoding::MorphLabel(morph, false) == L"blink",
          "undecodable native morph falls back to the valid English name");
    Check(text_encoding::DisplayGroupLabel(group, false) == L"custom frame",
          "undecodable native display group falls back to the valid English name");
    gModelNameAutoTranslation = true;
    Check(text_encoding::MorphLabel(morph, false) == L"まばたき",
          "fallback morph name still participates in display-only translation");
    gModelNameAutoTranslation = false;
    Check(std::memcmp(&morph, &originalMorph, sizeof(morph)) == 0 &&
          std::memcmp(&group, &originalGroup, sizeof(group)) == 0,
          "display fallback never rewrites model name bytes");
    morph.nameEn[0] = group.nameEn[0] = 0;
    Check(text_encoding::MorphLabel(morph, false) == std::wstring(19, L'm') + L"…",
          "truncated morph without an alternate keeps the readable prefix");
    Check(text_encoding::DisplayGroupLabel(group, false) == std::wstring(49, L'g') + L"…",
          "truncated display group without an alternate keeps the readable prefix");
    std::strcpy(morph.name, "\xEF\xBB\xBF\xE7\x9C\xA8\xE7\x9C\xBC");
    std::strcpy(group.name, morph.name);
    Check(text_encoding::MorphLabel(morph, false) == L"眨眼" &&
          text_encoding::DisplayGroupLabel(group, false) == L"眨眼",
          "explicit UTF-8 legacy names use their marker under every system code page");
    std::strcpy(bone.name, "center");
    bone.nameEn[0] = char(0x82);
    Check(text_encoding::BoneLabel(bone, true) == L"center",
          "bone name fallback uses the same decoding rules");
    gModelNameAutoTranslation = previousTranslation;
    std::puts("PASS legacy name decoding: invalid preferred field, truncated character, UTF-8 marker, unchanged identity");
}

void ModelNameTranslationRegression() {
    const auto previousPreference = gModelInfoLanguagePreference;
    const bool previousTranslation = gModelNameAutoTranslation;
    mdl::BoneRecord bone{};
    mdl::MorphRecord morph{};
    bone.jpText = const_cast<wchar_t*>(L"センター");
    bone.enText = const_cast<wchar_t*>(L"custom center");
    morph.jpText = const_cast<wchar_t*>(L"まばたき");
    morph.enText = const_cast<wchar_t*>(L"custom blink");
    morph.panel = mdl::MorphPanel::eye;
    auto model = std::make_unique<mdl::ModelRecord>();
    auto app = std::make_unique<MMDApp>();
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
        0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "create hidden translated-list owner");
    HWND bones = MakeCombo(parent, 450, true);
    HWND morphs = MakeCombo(parent, 509, true);
    Check(bones && morphs, "create translated name lists");
    model->hwnd = parent;
    model->boneTable = &bone; model->boneCount = 1;
    model->morphs = &morph; model->morphCount = 1;
    auto* previousApp = g_Block;
    g_Block = app.get();
    for (unsigned char ui : {kUiJapanese, kUiEnglish, kUiChinese}) {
        for (unsigned char preference : {kModelInfoLanguageAuto, kModelInfoLanguageJapanese, kModelInfoLanguageEnglish}) {
            app->EnglishUI() = ui;
            SetModelInfoLanguagePreference(preference);
            const bool english = UiUsesEnglishNames(ui);
            for (bool enabled : {false, true, false}) {
                SetModelNameAutoTranslation(enabled);
                const wchar_t* expectedBone = enabled ? (english ? L"center" : L"センター")
                    : (english ? L"custom center" : L"センター");
                const wchar_t* expectedMorph = enabled ? (english ? L"blink" : L"まばたき")
                    : (english ? L"custom blink" : L"まばたき");
                FillBoneCombo(bones, model.get(), english);
                PostLoadInit(reinterpret_cast<unsigned char*>(model.get()));
                Check(ItemTextW(bones, 0) == expectedBone, "bone list follows translation and default language");
                Check(ItemTextW(morphs, 0) == expectedMorph, "morph list follows translation and default language");
                Check(text_encoding::BoneName(bone, true) == L"custom center" &&
                      text_encoding::MorphName(morph, true) == L"custom blink",
                      "display translation never changes raw names used by files and motion matching");
            }
        }
    }
    SetModelNameAutoTranslation(true);
    bone.jpText = const_cast<wchar_t*>(L"ARM_L"); bone.enText = nullptr;
    Check(text_encoding::BoneLabel(bone, false) == L"左腕", "English bone names translate back to Japanese");
    morph.jpText = const_cast<wchar_t*>(L"wink L"); morph.enText = nullptr;
    Check(text_encoding::MorphLabel(morph, false) == L"ウィンク右", "morph left/right follows the supplied dictionary");
    bone.jpText = const_cast<wchar_t*>(L"左手捩");
    Check(text_encoding::BoneLabel(bone, true) == L"wristTwist_L", "bone dictionary uses its supplied English spelling");
    morph.jpText = const_cast<wchar_t*>(L"上");
    Check(text_encoding::MorphLabel(morph, true) == L"Up", "duplicate dictionary entries use the first definition");
    morph.jpText = const_cast<wchar_t*>(L"∧");
    Check(text_encoding::MorphLabel(morph, false) == L"∧", "native names take precedence over ambiguous aliases");
    bone.jpText = const_cast<wchar_t*>(L"センター_custom_😀");
    morph.jpText = const_cast<wchar_t*>(L"自定义表情_😀");
    Check(text_encoding::BoneLabel(bone, true) == bone.jpText &&
          text_encoding::MorphLabel(morph, true) == morph.jpText, "unmapped Unicode names remain unchanged");
    mdl::DisplayGroup group{};
    text_encoding::SetDisplayGroupName(*model, group, L"センター", L"center");
    Check(text_encoding::DisplayGroupLabel(group, true) == L"center" &&
          text_encoding::DisplayGroupLabel(group, false) == L"センター", "known display-group names follow the target language");
    text_encoding::ForgetDisplayGroupNames(*model);
    g_Block = previousApp;
    SetModelInfoLanguagePreference(previousPreference);
    SetModelNameAutoTranslation(previousTranslation);
    DestroyWindow(parent);
    std::puts("PASS dictionary translation: JP/EN/Chinese auto fallback, overrides, toggle, raw names and unknown names");
}

void ModelLanguagePreferenceRegression() {
    wchar_t previousDirectory[MAX_PATH]{}, tempDirectory[MAX_PATH]{};
    Check(GetCurrentDirectoryW(MAX_PATH, previousDirectory) > 0, "read preference-test working directory");
    Check(GetTempPathW(MAX_PATH, tempDirectory) > 0, "find preference-test temporary directory");
    const auto directory = std::wstring(tempDirectory) + L"MMS-language-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto dataDirectory = directory + L"\\Data";
    Check(CreateDirectoryW(directory.c_str(), nullptr) && CreateDirectoryW(dataDirectory.c_str(), nullptr),
          "create isolated preference directory");
    Check(SetCurrentDirectoryW(directory.c_str()), "enter isolated preference directory");
    const auto previousPreference = gModelInfoLanguagePreference;
    const bool previousTranslation = gModelNameAutoTranslation;
    for (unsigned char preference : {kModelInfoLanguageAuto, kModelInfoLanguageJapanese, kModelInfoLanguageEnglish}) {
        for (bool enabled : {false, true}) {
            SetModelInfoLanguagePreference(preference);
            SetModelNameAutoTranslation(enabled);
            SaveModelInfoLanguagePreference();
            SetModelInfoLanguagePreference(kModelInfoLanguageAuto);
            SetModelNameAutoTranslation(!enabled);
            LoadModelInfoLanguagePreference();
            Check(gModelInfoLanguagePreference == preference && gModelNameAutoTranslation == enabled,
                  "language and translation setting survive save/reload");
        }
    }
    FILE* file = nullptr;
    Check(fopen_s(&file, "Data\\model_info_language.ini", "wt") == 0, "write old preference format");
    std::fputs("2\n", file); std::fclose(file);
    LoadModelInfoLanguagePreference();
    Check(gModelInfoLanguagePreference == kModelInfoLanguageEnglish && !gModelNameAutoTranslation,
          "old one-value preferences retain language and default translation off");
    Check(DeleteFileW(L"Data\\model_info_language.ini"), "remove temporary preference file");
    LoadModelInfoLanguagePreference();
    Check(gModelInfoLanguagePreference == kModelInfoLanguageAuto && !gModelNameAutoTranslation,
          "missing preference file restores Auto with translation off");
    SetModelInfoLanguagePreference(previousPreference);
    SetModelNameAutoTranslation(previousTranslation);
    Check(SetCurrentDirectoryW(previousDirectory), "restore preference-test working directory");
    Check(RemoveDirectoryW(dataDirectory.c_str()) && RemoveDirectoryW(directory.c_str()), "remove temporary preference directories");
}

void TimelineLabelRasterRegression() {
    constexpr int width = 120, height = 112;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    Check(bitmap && dc && pixels, "create timeline font raster surface");
    auto previous = SelectObject(dc, bitmap);
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    const wchar_t* labels[] = {L"操作中心", L"表示・IK・外親", L"上半身（上）",
                               L"Left shoulder & a very long bone name",
                               L"カメラ", L"まばたき"};
    const int labelCount = static_cast<int>(std::size(labels));
    for (int language = 0; language < 3; ++language) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        for (int row = 0; row < labelCount; ++row) {
            GdiFlush();
            std::memset(pixels, 0xff, width * height * 4);
            const int top = 16 + 14 * row;
            SetTextAlign(dc, TA_BASELINE | TA_RIGHT);
            DrawTimelineLabel(app.get(), dc, labels[row], 12, top + 1, RGB(0, 0, 0));
            GdiFlush();
            Check(GetTextAlign(dc) == (TA_BASELINE | TA_RIGHT), "label drawing restores DC text alignment");
            int ink = 0;
            int firstInkY = height, lastInkY = -1;
            auto* data = static_cast<unsigned*>(pixels);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    if ((data[y * width + x] & 0xffffff) == 0xffffff) continue;
                    ++ink;
                    if (y < firstInkY) firstInkY = y;
                    if (y > lastInkY) lastInkY = y;
                    Check(x >= 12 && x < 91 && y >= top && y < top + 14,
                          "Unicode label stays in its track row and never overlaps the zero-frame column");
                }
            }
            Check(ink > 12, "timeline label actually renders Unicode glyphs");
            if (language == 2)
                Check(lastInkY - firstInkY + 1 >= 10,
                      "Chinese UI keeps Chinese, Japanese and Latin glyphs readable within the row");
        }
    }
    // Optional local preview of the actual GDI result, including row boundaries.
    char output[MAX_PATH]{};
    if (GetEnvironmentVariableA("MIKUMIKUSTUDIO_TIMELINE_FONT_PREVIEW", output, MAX_PATH)) {
        GdiFlush();
        std::memset(pixels, 0xff, width * height * 4);
        app->EnglishUI() = 2;
        for (int row = 0; row < labelCount; ++row) {
            const int top = 16 + row * 14;
            MoveToEx(dc, 0, top, nullptr);
            LineTo(dc, width, top);
            DrawTimelineLabel(app.get(), dc, labels[row], 12, top + 1, RGB(0, 0, 0));
        }
        GdiFlush();
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
        header.bfSize = header.bfOffBits + width * height * 4;
        FILE* file = nullptr;
        Check(fopen_s(&file, output, "wb") == 0, "open optional timeline preview");
        std::fwrite(&header, sizeof(header), 1, file);
        std::fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file);
        std::fwrite(pixels, width * height * 4, 1, file);
        std::fclose(file);
    }
    SelectObject(dc, previous);
    DeleteDC(dc);
    DeleteObject(bitmap);
    std::puts("PASS timeline Unicode labels: readable glyph size, row alignment and clipping in all UI languages");
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--name-lookup-benchmark") == 0) {
        mdl::MorphRecord morph{};
        mdl::DisplayGroup group{};
        std::strcpy(morph.name, "\x82\xDC\x82\xCE\x82\xBD\x82\xAB");
        std::strcpy(morph.nameEn, "blink");
        std::strcpy(group.name, "\x98\x72");
        std::strcpy(group.nameEn, "arms");
        gModelNameAutoTranslation = false;
        std::size_t checksum = 0;
        const auto run = [&] {
            checksum += text_encoding::MorphLabel(morph, false).size();
            checksum += text_encoding::DisplayGroupLabel(group, false).size();
            checksum += text_encoding::MorphLabel(morph, true).size();
            checksum += text_encoding::DisplayGroupLabel(group, true).size();
        };
        for (int i = 0; i < 1000; ++i) run();
        checksum = 0;
        LARGE_INTEGER start{}, end{}, frequency{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        for (int i = 0; i < 100000; ++i) run();
        QueryPerformanceCounter(&end);
        Check(checksum == 1400000, "benchmark resolves the same names in both builds");
        std::printf("{\"acp\":%u,\"lookups\":400000,\"checksum\":%zu,\"milliseconds\":%.6f}\n",
            GetACP(), checksum, (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--model-selectors") == 0) {
        std::printf("Model selector regression: ACP=%u\n", GetACP());
        ModelSelectorSelectionRegression();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--name-gaps") == 0) {
        Check(_wsetlocale(LC_ALL, L"Japanese_Japan.932") != nullptr, "set Japanese CRT locale");
        std::printf("Name gap regression: ACP=%u\n", GetACP());
        LegacyNameFallbackRegression();
        ModelSelectorSelectionRegression();
        MissingEnglishPmxRegression(true);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--panel-combos") == 0) {
        MainPanelComboRegression();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--japanese-names") == 0) {
        Check(_wsetlocale(LC_ALL, L"Japanese_Japan.932") != nullptr, "set Japanese CRT locale");
        std::printf("Japanese name regression: ACP=%u, CRT=%s\n", GetACP(), std::setlocale(LC_ALL, nullptr));
        MissingEnglishPmxRegression(true);
        return 0;
    }
    for (unsigned short language : {0x0804, 0x0404, 0x0c04, 0x1004, 0x1404})
        Check(ResolveStartupUiLanguage(-1, language) == kUiChinese,
              "Chinese regions default to Chinese UI");
    Check(ResolveStartupUiLanguage(-1, 0x0411) == kUiJapanese, "Japanese default");
    for (unsigned short language : {0x0409, 0x0809, 0x040c, 0x0000})
        Check(ResolveStartupUiLanguage(-1, language) == kUiEnglish,
              "English and unsupported languages default to English UI");
    for (int saved : {0, 1, 2})
        for (unsigned short language : {0x0804, 0x0411, 0x0409})
            Check(ResolveStartupUiLanguage(saved, language) == saved,
                  "Saved language overrides Windows language, including Chinese");
    Check(ResolveStartupUiLanguage(3, 0x0804) == kUiChinese,
          "Invalid saved language falls back to Windows");
    NameLanguageRegression();
    MainPanelComboRegression();
    ModelSelectorSelectionRegression();
    MissingEnglishPmxRegression();
    Check(_wsetlocale(LC_ALL, L"Japanese_Japan.932") != nullptr, "set Japanese CRT locale");
    MissingEnglishPmxRegression(true);
    DisplayGroupLifetimeRegression();
    LegacyNameFallbackRegression();
    ModelNameTranslationRegression();
    ModelLanguagePreferenceRegression();
    CaptionRegression();
    TimelineLabelRasterRegression();
    std::puts("UI language regressions passed");
    return 0;
}
