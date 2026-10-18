#include "mikudancestudio/ui_translation.hpp"
#include <set>
// MAINTENANCE: integration-manual; purpose and cleanup conditions in tests/README.md.
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/ui_language.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/sequence_scatter.hpp"
#include "mikudancestudio/morph_curve.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/effect_api.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/mmv4mms.hpp"
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>
#include <filesystem>
#include "mikudancestudio/autosave.hpp"
#include <fstream>
#include <wrl/client.h>

namespace mikudancestudio {
void LoadSceneFile();
void RegisterCameraState(MMDApp*, int);
void CmdControl450(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void DispatchLetterHotkeys(MMDApp*, HWND);
void PumpDeleteRebuild(MMDApp*, HWND, bool);
void StepFrame(MMDApp*, bool);
void ConsumeRightButtonDrag(MMDApp*);
void ConsumeMiddleButtonPan(MMDApp*);
void CmdViewMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void CmdFileMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void HandleWindowPaint(MMDApp*);
void RefreshAfterFrameApply(MMDApp*);
void ConsumeArrowKeyNavigation(MMDApp*);
bool SaveSceneCopy(MMDApp*, const wchar_t*);
bool ApplyBoneFrameScaleAdd(MMDApp*, HWND);
}
using namespace mikudancestudio;
namespace {
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(7); }
}

void CheckCoordinateReset(MMDApp* app, const std::filesystem::path& output) {
    const HWND main = app->MainWindow();
    const auto owner = [&]() { return app->FloatingWindow() ? app->FloatingWindow() : main; };
    const auto expect = [&](bool enabled) {
        Check((app->GroundGridEnabled() != 0) == enabled, "coordinate render flag matches the requested state");
        Check(((GetMenuState(GetMenu(main), 215, MF_BYCOMMAND) & MF_CHECKED) != 0) == enabled,
              "coordinate menu matches the render flag");
        Check((SendDlgItemMessageW(owner(), panel::kCoordAxisCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED) == enabled,
              "coordinate checkbox matches the render flag");
    };
    const auto set = [&](bool enabled) {
        if ((app->GroundGridEnabled() != 0) != enabled) CommandDispatch(nullptr, 215);
        expect(enabled);
    };
    const auto viewport = [&]() {
        app->state.messageSeen = 10;
        RenderFrameScene(app);
        auto* device = app->Renderer()->device;
        Microsoft::WRL::ComPtr<IDirect3DSurface9> source, resolved, cpu;
        Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &source)), "get coordinate viewport");
        D3DSURFACE_DESC desc{}; source->GetDesc(&desc);
        Check(SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
            D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)), "create coordinate resolve target");
        Check(SUCCEEDED(device->StretchRect(source.Get(), nullptr, resolved.Get(), nullptr, D3DTEXF_NONE)),
              "resolve coordinate viewport");
        Check(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &cpu, nullptr)), "create coordinate readback");
        Check(SUCCEEDED(device->GetRenderTargetData(resolved.Get(), cpu.Get())), "read coordinate viewport pixels");
        D3DLOCKED_RECT lock{};
        Check(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "lock coordinate viewport pixels");
        std::vector<std::uint32_t> pixels;
        // Exclude the control rows and FPS text: these pixels belong to the 3D viewport.
        for (UINT y = desc.Height / 3; y < desc.Height * 3 / 4; ++y) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(lock.pBits) + y * lock.Pitch);
            for (UINT x = desc.Width / 3; x < desc.Width * 2 / 3; ++x) pixels.push_back(row[x]);
        }
        cpu->UnlockRect(); return pixels;
    };
    expect(true);
    set(false);
    const auto hidden = viewport();
    ResetAppState(app);
    expect(true); // Regression: BM_CLICK used to toggle the just-enabled flag off.
    const auto visible = viewport();
    Check(visible != hidden, "new scene visibly restores ground coordinates in the 3D viewport");
    for (bool detached : {false, true}) {
        HWND floating = nullptr;
        if (detached) {
            floating = CreateWindowExW(0, L"MicWindow", L"Coordinate reset test", WS_POPUP,
                0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Check(floating != nullptr, "create hidden real floating-panel window");
            app->FloatingWindow() = floating;
            for (int id = 536; id <= 557; ++id) SetParent(GetDlgItem(main, id), floating);
        }
        for (int language : {0, 1, 2}) {
            app->EnglishUI() = language; LocalizeUI(app);
            for (bool previous : {false, true}) {
                set(previous);
                ResetAppState(app); expect(true);
                ResetAppState(app); expect(true);
                SendDlgItemMessageW(owner(), panel::kCoordAxisCheckbox, BM_CLICK, 0, 0); expect(false);
                SendDlgItemMessageW(owner(), panel::kCoordAxisCheckbox, BM_CLICK, 0, 0); expect(true);
            }
        }
        for (bool saved : {false, true}) {
            set(saved);
            const auto path = output / ((saved ? L"coordinates-on-" : L"coordinates-off-") +
                std::to_wstring(detached) + L".pmm");
            Check(SaveSceneCopy(app, path.c_str()), "save project coordinate visibility");
            set(!saved);
            wcscpy_s(app->EnvFileName(), 256, path.c_str()); LoadSceneFile(); expect(saved);
            ResetAppState(app); expect(true);
        }
        if (floating) {
            for (int id = 536; id <= 557; ++id) SetParent(GetDlgItem(floating, id), main);
            app->FloatingWindow() = nullptr; DestroyWindow(floating);
        }
    }
    std::puts("PASS coordinate new/open state, repeated reset, menu/checkbox, floating panel, three languages and viewport pixels");
}


#include "editor_feedback_checks.hpp"
#include "sequence_scatter_checks.hpp"
#include "accessory_feedback_checks.hpp"
#include "curve_transport_feedback_checks.hpp"

int multiplyAction = 0;
int multiplyDialogs = 0;
const wchar_t* invalidMultiplyValue = nullptr;
std::filesystem::path multiplyOutput;
void CaptureMultiplyDialog(HWND dialog) {
    if (multiplyOutput.empty()) return;
    RECT rect{}; GetWindowRect(dialog, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr); auto old = SelectObject(dc, bitmap);
    SendMessageW(dialog, WM_PRINT, reinterpret_cast<WPARAM>(dc),
        PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    GdiFlush();
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + width * height * 4;
    const auto path = multiplyOutput / (L"bone-multiply-" + std::to_wstring(g_Block->EnglishUI()) + L".bmp");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(static_cast<const char*>(pixels), width * height * 4);
    Check(bool(output), "capture real multiply window paint");
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}
LRESULT CALLBACK InspectMultiplyDialog(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        HWND dialog = reinterpret_cast<HWND>(wp);
        if (GetDlgItem(dialog, panel::kBoneMulPosXScaleEdit)) {
            if (GetPropW(dialog, L"MMS.Test.MultiplyInspected")) return 1;
            SetPropW(dialog, L"MMS.Test.MultiplyInspected", reinterpret_cast<HANDLE>(1));
            ++multiplyDialogs;
            if (g_Block->EnglishUI() == kUiChinese) {
                wchar_t text[128]{};
                GetWindowTextW(dialog, text, 128);
                Check(std::wcscmp(text, L"骨骼帧位置与角度调整") == 0,
                      "bone multiply dialog is localized");
                GetDlgItemTextW(dialog, IDOK, text, 128);
                Check(std::wcscmp(text, L"确定") == 0, "multiply confirmation is localized");
            }
            if (multiplyAction < 2)
                SetDlgItemTextW(dialog, panel::kBoneMulPosXOffsetEdit, L"2");
            else if (multiplyAction == 2)
                SetDlgItemTextW(dialog, panel::kBoneMulPosXOffsetEdit + 10, L"90");
            if (multiplyAction >= 3) {
                RECT client{}, angle{}, first{}, last{}, button{};
                GetClientRect(dialog, &client);
                const auto bounds = [&](int id, RECT& rect) {
                    HWND child = GetDlgItem(dialog, id);
                    Check(child != nullptr, "scale field exists in the original dialog");
                    GetWindowRect(child, &rect);
                    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
                    Check(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom,
                          "multiply control fits the client area");
                };
                bounds(panel::kBoneMulPosXOffsetEdit + 10, angle);
                bounds(panel::kBoneMulScaleXFactorEdit, first);
                bounds(panel::kBoneMulScaleXOffsetEdit + 4, last);
                bounds(IDOK, button);
                Check(first.top > angle.bottom && last.bottom < button.top,
                      "scale rows appear below angle and above confirmation");
                HWND previous = GetDlgItem(dialog, panel::kBoneMulPosXOffsetEdit + 10);
                for (int i = 0; i < 6; ++i) {
                    HWND field = GetDlgItem(dialog, panel::kBoneMulScaleXFactorEdit + i);
                    Check(GetNextDlgTabItem(dialog, previous, FALSE) == field,
                          "Tab follows angle, then scale X/Y/Z pairs");
                    wchar_t text[32]{}; GetWindowTextW(field, text, 32);
                    Check(std::wcscmp(text, i % 2 ? L"0.0" : L"1.0") == 0,
                          "new scale inputs default to identity");
                    previous = field;
                }
                Check(GetNextDlgTabItem(dialog, previous, FALSE) == GetDlgItem(dialog, IDOK),
                      "Tab reaches confirmation after scale");
                if (multiplyAction == 5) CaptureMultiplyDialog(dialog);
                if (multiplyAction == 3 || multiplyAction == 4) {
                    const wchar_t* values[] = {L"2", L"0.5", L"0.5", L"-0.25", L"3", L"1"};
                    for (int i = 0; i < 6; ++i)
                        SetDlgItemTextW(dialog, panel::kBoneMulScaleXFactorEdit + i, values[i]);
                    SetDlgItemTextW(dialog, panel::kBoneMulPosXOffsetEdit, L"2");
                }
                if (multiplyAction >= 10) {
                    SetDlgItemTextW(dialog, panel::kBoneMulPosXOffsetEdit, L"2");
                    SetDlgItemTextW(dialog, multiplyAction == 11 ? panel::kBoneMulScaleXOffsetEdit :
                        panel::kBoneMulScaleXFactorEdit, invalidMultiplyValue);
                    Check(!ApplyBoneFrameScaleAdd(g_Block, dialog), "invalid scale transaction is rejected");
                }
            }
            ShowWindow(dialog, SW_HIDE);
            PostMessageW(dialog, WM_COMMAND,
                multiplyAction == 0 || multiplyAction == 4 || multiplyAction >= 10 ? IDCANCEL : IDOK, 0);
            return 1; // Keep test dialogs from activating the user's desktop.
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
void CheckBoneMultiply(MMDApp* app, mdl::ModelRecord* model) {
    auto* bytes = reinterpret_cast<unsigned char*>(model);
    auto* keys = model->boneKeys;
    auto& bone = model->boneTable[0];
    app->CurrentFrame() = 0;
    bone.trans[0] = 3.f; bone.trans[1] = 4.f; bone.trans[2] = 5.f;
    std::fill(bone.rotQuat, bone.rotQuat + 4, 0.f); bone.rotQuat[3] = 1.f;
    std::fill(model->boneSelection, model->boneSelection + model->boneCount, 0);
    model->boneSelection[0] = 1; model->selectedBone = 0;
    RegisterSelectedBoneKeys(bytes, 0, 0);
    bone.trans[0] = 7.f;
    RegisterSelectedBoneKeys(bytes, 10, 0);
    int later = static_cast<int>(keys[0].next);
    while (later > 0 && keys[later].frame < 10) later = static_cast<int>(keys[later].next);
    Check(later > 0 && keys[later].frame == 10, "find registered key in the bone track");
    Check(later >= 0, "register another root key for multiply");
    for (std::size_t i = 0; i < mdl::kBoneKeyCapacity; ++i) keys[i].allocated = 0;
    keys[0].allocated = keys[later].allocated = 1;
    const auto untouched = keys[1];
    const auto camera = app->CameraKeys()[0];
    const auto view = [&]() { return std::array<float, 9>{app->ViewOffsetX(), app->ViewOffsetY(),
        app->CameraDistance(), app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(),
        app->CameraPitch(), app->CameraYaw(), app->CameraRoll()}; };
    const auto originalView = view();
    const float firstX = keys[0].position[0], laterX = keys[later].position[0];
    const auto invoke = [&](int action) {
        multiplyAction = action;
        const int seen = multiplyDialogs;
        HHOOK hook = SetWindowsHookExW(WH_CBT, InspectMultiplyDialog, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "install bone multiply dialog test hook");
        CmdViewMenu(app, app->MainWindow(), 251, 0);
        UnhookWindowsHookEx(hook);
        Check(multiplyDialogs == seen + 1, "open real bone multiply menu dialog");
    };
    invoke(0);
    Check(keys[0].position[0] == firstX && keys[later].position[0] == laterX,
          "cancel bone multiply preserves selected keys");
    invoke(1);
    Check(keys[0].position[0] == firstX + 2.f && keys[later].position[0] == laterX + 2.f,
          "bone multiply adds two X units to each selected key");
    Check(std::memcmp(&untouched, &keys[1], sizeof(untouched)) == 0,
          "bone multiply preserves unselected bones");
    Check(std::memcmp(&camera, &app->CameraKeys()[0], sizeof(camera)) == 0 && view() == originalView,
          "bone multiply leaves camera keys and viewport untouched");
    std::int32_t frame = app->CurrentFrame();
    UndoModelEdit(bytes, &frame);
    Check(keys[0].position[0] == firstX && keys[later].position[0] == laterX,
          "bone multiply supports undo");
    RedoModelEdit(bytes, &frame);
    Check(keys[0].position[0] == firstX + 2.f && keys[later].position[0] == laterX + 2.f,
          "bone multiply supports redo");
    keys[0].allocated = keys[later].allocated = 1;
    invoke(2);
    Check(std::fabs(std::fabs(keys[0].rotation[2]) - std::sqrt(.5f)) < 1e-4f &&
          keys[0].rotation[2] < 0.f,
          "bone angle add uses degrees and the MMD Z-axis direction");
    bone_scale::Matrix firstScale, laterScale, otherScale;
    laterScale.v[0] = 2.f; laterScale.v[4] = 3.f; laterScale.v[8] = 4.f;
    otherScale.v[0] = 1.25f;
    bone_scale::SetKey(bytes, 0, firstScale);
    bone_scale::SetKey(bytes, later, laterScale);
    bone_scale::SetKey(bytes, 1, otherScale);
    const bone_scale::Curve curve{10, 50, 90, 120};
    bone_scale::SetKeyCurve(bytes, later, curve);
    const auto savedFirst = keys[0], savedLater = keys[later];
    const auto undoBefore = model->undoState[0];
    invoke(5);
    invoke(4);
    Check(model->undoState[0] == undoBefore && bone_scale::Key(bytes, 0) == firstScale &&
          bone_scale::Key(bytes, later) == laterScale && keys[0].position[0] == savedFirst.position[0],
          "identity and cancel preserve scale, position and undo history");
    invoke(3);
    const auto resultFirst = bone_scale::Key(bytes, 0), resultLater = bone_scale::Key(bytes, later);
    Check(resultFirst.v[0] == 2.5f && resultFirst.v[4] == .25f && resultFirst.v[8] == 4.f &&
          resultLater.v[0] == 4.5f && resultLater.v[4] == 1.25f && resultLater.v[8] == 13.f,
          "scale multiply/add operates on each selected frame's own XYZ values");
    Check(bone_scale::Key(bytes, 1) == otherScale && bone_scale::KeyCurve(bytes, later) == curve,
          "unselected scale and selected interpolation curve are preserved");
    const auto refreshedPose = bone_scale::Pose(bytes, 0);
    Check(std::fabs(refreshedPose.v[0] - resultFirst.v[0]) < .01f &&
          std::fabs(refreshedPose.v[4] - resultFirst.v[4]) < .01f &&
          std::fabs(refreshedPose.v[8] - resultFirst.v[8]) < .01f,
          "current pose refreshes from the modified scale key");
    UndoModelEdit(bytes, &frame);
    Check(bone_scale::Key(bytes, 0) == firstScale && bone_scale::Key(bytes, later) == laterScale &&
          keys[0].position[0] == savedFirst.position[0], "one undo restores position and scale together");
    RedoModelEdit(bytes, &frame);
    Check(bone_scale::Key(bytes, 0) == resultFirst && bone_scale::Key(bytes, later) == resultLater &&
          keys[0].position[0] == savedFirst.position[0] + 2.f, "one redo restores the combined edit");
    keys[0].allocated = keys[later].allocated = 1;
    const auto validUndo = model->undoState[0];
    const auto validFirst = keys[0], validLater = keys[later];
    for (const wchar_t* value : {L"", L"abc", L"1x", L"nan", L"inf", L"1e50", L"0", L"-1", L"1e-8", L"1e5"}) {
        invalidMultiplyValue = value; invoke(10);
        Check(bone_scale::Key(bytes, 0) == resultFirst && bone_scale::Key(bytes, later) == resultLater &&
              std::memcmp(&keys[0], &validFirst, sizeof(validFirst)) == 0 &&
              std::memcmp(&keys[later], &validLater, sizeof(validLater)) == 0 && model->undoState[0] == validUndo,
              "invalid input leaves every selected channel and undo history unchanged");
    }
    // First selected key is valid; a later key fails. No earlier edit may leak.
    firstScale.v[0] = 2.f; laterScale.v[0] = .25f;
    bone_scale::SetKey(bytes, 0, firstScale); bone_scale::SetKey(bytes, later, laterScale);
    invalidMultiplyValue = L"-0.5"; invoke(11);
    Check(bone_scale::Key(bytes, 0) == firstScale && bone_scale::Key(bytes, later) == laterScale &&
          keys[0].position[0] == validFirst.position[0] && model->undoState[0] == validUndo,
          "a later invalid scale rejects the entire selection atomically");
    // Non-diagonal imported scales must retain their row directions.
    firstScale.v = {2, .5f, 0, .25f, 3, 0, 0, 0, 4};
    bone_scale::SetKey(bytes, 0, firstScale);
    invoke(3);
    const auto sheared = bone_scale::Key(bytes, 0);
    Check(std::fabs(sheared.v[1] / sheared.v[0] - .25f) < 1e-6f &&
          std::fabs(sheared.v[3] / sheared.v[4] - 1.f / 12.f) < 1e-6f,
          "scale adjustment preserves non-diagonal row directions");
    const int originalLanguage = app->EnglishUI();
    for (int language : {0, 1, 2}) {
        app->EnglishUI() = language; LocalizeUI(app); invoke(5);
    }
    app->EnglishUI() = originalLanguage; LocalizeUI(app);
    std::puts("PASS bone multiply XYZ scale, translation, rotation, cancel, validation, curves and undo/redo");
}

LRESULT CALLBACK KeepEdgeTestOffscreen(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_CREATEWND&&!(reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs->style&WS_CHILD)){auto* create=reinterpret_cast<CBT_CREATEWNDW*>(lp);create->lpcs->x=-32000;create->lpcs->y=-32000;}
    if(code==HCBT_ACTIVATE)return 1;return CallNextHookEx(nullptr,code,wp,lp);
}

INT_PTR CALLBACK ProbeUiDialog(HWND,UINT message,WPARAM,LPARAM parameter) {
    if(message==WM_INITDIALOG)Check(parameter==0x1234,"localized dialog preserves initialization parameter");
    return FALSE;
}
BOOL CALLBACK CollectDialogResource(HMODULE,LPCWSTR,LPWSTR name,LONG_PTR context) {
    if(IS_INTRESOURCE(name))reinterpret_cast<std::vector<int>*>(context)->push_back(static_cast<int>(reinterpret_cast<ULONG_PTR>(name)));
    return TRUE;
}
std::wstring WindowCaption(HWND control) {
    wchar_t text[1024]{};GetWindowTextW(control,text,1024);return text;
}
bool AsciiLabel(const std::wstring& wide,std::string& narrow) {
    narrow.clear();for(wchar_t ch:wide){if(ch>127)return false;narrow.push_back(static_cast<char>(ch));}return true;
}
void CheckChineseUi(MMDApp* app) {
    Check(app->EnglishUI()==kUiChinese,"PMM load preserves Chinese language");
    const auto comboCaption=[&](int id,int row){wchar_t text[256]{};SendMessageW(GetDlgItem(app->MainWindow(),id),CB_GETLBTEXT,row,reinterpret_cast<LPARAM>(text));return std::wstring(text);};
    Check(comboCaption(436,0)==L"相机/照明/配件","PMM model selector stays Chinese");
    Check(comboCaption(449,0)==L"无","PMM camera parent selector stays Chinese");
    Check(comboCaption(433,3)==L"旋转","PMM interpolation selector stays Chinese");
    Check(WindowCaption(GetDlgItem(app->MainWindow(),563))==L"模式1","selected shadow mode stores Chinese text");
    LOGFONTW font{};GetObjectW(app->state.hFontUI,sizeof(font),&font);
    Check(std::wcscmp(font.lfFaceName,L"Microsoft YaHei UI")==0,"Chinese controls use Unicode Chinese font");
    HDC fontDC=CreateCompatibleDC(nullptr);
    HGDIOBJ previous=SelectObject(fontDC,app->state.hFontUI);
    TEXTMETRICW metrics{};GetTextMetricsW(fontDC,&metrics);
    std::printf("Chinese control font: cell=%ld glyph=%ld leading=%ld\n",metrics.tmHeight,metrics.tmHeight-metrics.tmInternalLeading,metrics.tmInternalLeading);
    SelectObject(fontDC,previous);DeleteDC(fontDC);
    Check(metrics.tmHeight-metrics.tmInternalLeading>=12,"Chinese button glyphs retain intended 12px height");
    std::vector<int> resources;EnumResourceNamesW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(5),CollectDialogResource,reinterpret_cast<LONG_PTR>(&resources));
    int translated=0;
    for(int resource:resources){
        HWND raw=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(resource),app->MainWindow(),ProbeUiDialog,0x1234);
        Check(raw!=nullptr,"create raw resource dialog for audit");
        const auto title=WindowCaption(raw);std::string ascii;
        const wchar_t* expected=AsciiLabel(title,ascii)?ChineseUiText(ascii.c_str()):nullptr;
        LOGFONTW resourceFont{};
        HFONT rawFont=reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(raw,IDOK),WM_GETFONT,0,0));
        const bool hasResourceFont=GetObjectW(rawFont,sizeof(resourceFont),&resourceFont)!=0;
        DestroyWindow(raw);if(!expected)continue;
        HWND localized=CreateUiDialogParamA(GetModuleHandleW(nullptr),MAKEINTRESOURCEA(resource),app->MainWindow(),ProbeUiDialog,0x1234);
        Check(localized!=nullptr,"create translated dialog for audit");
        Check(WindowCaption(localized)==expected,"legacy dialog title translated after initialization");
        if(hasResourceFont) {
            LOGFONTW translatedFont{};
            HFONT font=reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(localized,IDOK),WM_GETFONT,0,0));
            Check(GetObjectW(font,sizeof(translatedFont),&translatedFont)!=0&&
                resourceFont.lfHeight==translatedFont.lfHeight,"translation preserves each dialog's original point size");
        }
        EnumChildWindows(localized,[](HWND child,LPARAM)->BOOL{
            wchar_t className[32]{};GetClassNameW(child,className,32);
            if(_wcsicmp(className,L"Static")&&_wcsicmp(className,L"Button"))return TRUE;
            std::string ascii;
            if(AsciiLabel(WindowCaption(child),ascii))if(const auto* expected=ChineseUiText(ascii.c_str()))
                Check(WindowCaption(child)==expected,"dialog fixed labels are translated");
            return TRUE;
        },0);
        std::printf("PASS Chinese dialog resource %d\n",resource);++translated;DestroyWindow(localized);
    }
    Check(translated>=20,"audit covers legacy English dialog resources");
    for(int language:{0,1,2}){app->EnglishUI()=language;LocalizeUI(app);Check(app->EnglishUI()==language,"language switch stays selected");}
    std::puts("PASS PMM Chinese selectors, Unicode font, shadow mode and dialog translations");
}

void CheckPanelControls(MMDApp* app) {
    SetWindowPos(app->MainWindow(),HWND_BOTTOM,-32000,-32000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    RedrawWindow(app->MainWindow(),nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    app->CameraMode()=0;app->SetSelectedModelSlot(0);app->EnglishUI()=1;
    HHOOK hook=SetWindowsHookExW(WH_CBT,KeepEdgeTestOffscreen,nullptr,GetCurrentThreadId());
    CmdViewMenu(app,app->MainWindow(),253,0);UnhookWindowsHookEx(hook);
    HWND edge=app->state.edgeThicknessDialog;
    Check(edge!=nullptr,"create hidden edge dialog");
    std::printf("edge-labels=%d/%d/%d\n",GetDlgItem(edge,1646)!=nullptr,GetDlgItem(edge,1647)!=nullptr,GetDlgItem(edge,1648)!=nullptr);
    HWND edgeSlider=GetDlgItem(edge,647);
    for(int index=0;index<3;++index){SendMessageW(edgeSlider,TBM_SETPOS,TRUE,index*100);RECT thumb{},label{};SendMessageW(edgeSlider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));POINT centre{(thumb.left+thumb.right)/2,0};MapWindowPoints(edgeSlider,edge,&centre,1);GetWindowRect(GetDlgItem(edge,1646+index),&label);MapWindowPoints(nullptr,edge,reinterpret_cast<POINT*>(&label),2);std::printf("edge %d thumb=%ld label=%ld\n",index,centre.x,(label.left+label.right)/2);Check(std::abs(centre.x-(label.left+label.right)/2)<=1,"edge tick label matches thumb centre");}
    DestroyWindow(edge);app->state.edgeThicknessDialog=nullptr;
    for(int id:{447,455,456,457,458,459,460,560,534}) {
        HWND control=GetDlgItem(app->MainWindow(),id); RECT rc{};GetClientRect(control,&rc);
        const int width=rc.right,height=rc.bottom; HDC dc=CreateCompatibleDC(nullptr);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
        info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* pixels=nullptr;
        HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(dc,bitmap);
        PatBlt(dc,0,0,width,height,BLACKNESS);CallWindowProcA(app->OriginalTrackbarProc(),control,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
        std::set<COLORREF> nativeColors;for(int y=0;y<height;++y)for(int x=0;x<width;++x)nativeColors.insert(GetPixel(dc,x,y));
        std::printf("native colors=%zu ",nativeColors.size());
        PatBlt(dc,0,0,width,height,BLACKNESS);SendMessageW(control,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
        RECT thumb{};SendMessageW(control,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
        std::set<COLORREF> colors;for(int y=0;y<height;++y)for(int x=0;x<width;++x)colors.insert(GetPixel(dc,x,y));
        std::printf("slider %d size=%dx%d colors=%zu thumb=(%ld,%ld,%ld,%ld)\n",id,width,height,colors.size(),thumb.left,thumb.top,thumb.right,thumb.bottom);
        Check(colors.size()>1,"native slider client includes track and thumb");
        char name[256]{};std::sprintf(name,"D:/MMD-Next/MMS/.local/artifacts/panel-controls/20261001/slider-%d.bmp",id);
        FILE* file=std::fopen(name,"wb");BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+width*height*4;
        if(file){std::fwrite(&header,sizeof(header),1,file);std::fwrite(&info.bmiHeader,sizeof(BITMAPINFOHEADER),1,file);std::fwrite(pixels,width*height*4,1,file);std::fclose(file);}
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
}
void CheckCameraBoneMotion(MMDApp* app) {
    Check((GetWindowLongPtrW(app->MainWindow(),GWL_STYLE)&WS_CLIPCHILDREN)!=0,"parent paint excludes native child controls");
    int slot=0;while(slot<kModelSlotCount&&!app->ModelSlot(slot))++slot;
    Check(slot<kModelSlotCount,"camera follow fixture loaded a model");
    auto*raw=app->ModelSlot(slot);auto&model=*mdl::Mdl(raw);
    const int bone=static_cast<int>(model.displayRootBone);
    Check(bone>=0&&bone<static_cast<int>(model.boneCount),"fixture root bone is valid");
    app->SetSelectedModelSlot(slot);app->PlaybackPhysicsMode()=0;
    model.selectedBone=bone;std::memset(model.boneSelection,0,model.boneCount);model.boneSelection[bone]=1;
    for(int frame:{0,30}){
        app->CurrentFrame()=frame;model.boneTable[bone].trans[0]=frame?10.f:0.f;
        RegisterSelectedBoneKeys(raw,frame,0);
    }
    app->CurrentFrame()=0;SeekModelFrame(raw,0,0);SetPhysicsMode(raw,0,app->ModelSlots(),0);
    const auto choose=[&](int id,int selection){
        HWND combo=GetDlgItem(app->MainWindow(),id);SendMessageW(combo,CB_SETCURSEL,selection,0);
        SendMessageW(app->MainWindow(),WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
    };
    choose(panel::kMainComboModel,0);choose(panel::kMainComboNormal,model.comboSelIndex);
    HWND combo=GetDlgItem(app->MainWindow(),panel::kBoneRegisterCombo);
    int row=0;for(;row<SendMessageW(combo,CB_GETCOUNT,0,0);++row)
        if(SendMessageW(combo,CB_GETITEMDATA,row,0)==bone)break;
    choose(panel::kBoneRegisterCombo,row);
    Check(app->CameraParentModel()==slot&&app->CameraParentBone()==bone,"UI binds the moving fixture bone");
    RegisterCameraState(app,0);RegisterCameraState(app,30);
    const auto cameraPoint=[&](){
        app->DeltaTime()=1.f/30.f;app->state.messageSeen=10;FrameDriver(app);
        SetupFrameWorldTransform(app); // Overlay draws replace device WORLD; rebuild the scene camera for sampling.
        d3dx::D3DXMATRIXF world{},view{},composed{};auto*device=app->Renderer()->device;
        device->GetTransform(D3DTS_WORLD,reinterpret_cast<D3DMATRIX*>(&world));
        device->GetTransform(D3DTS_VIEW,reinterpret_cast<D3DMATRIX*>(&view));d3dx::Get().multiply(&composed,&world,&view);
        const auto&b=model.boneTable[bone];float point[3]{};
        for(int axis=0;axis<3;++axis)point[axis]=b.position[0]*b.matInit[axis]+b.position[1]*b.matInit[4+axis]+b.position[2]*b.matInit[8+axis]+b.matInit[12+axis];
        std::printf("frame=%d parent=%d/%d bone-world=(%.3f,%.3f,%.3f)\n",app->CurrentFrame(),app->CameraParentModel(),app->CameraParentBone(),point[0],point[1],point[2]);
        return std::array<float,3>{point[0]*composed.m[0][0]+point[1]*composed.m[1][0]+point[2]*composed.m[2][0]+composed.m[3][0],
            point[0]*composed.m[0][1]+point[1]*composed.m[1][1]+point[2]*composed.m[2][1]+composed.m[3][1],
            point[0]*composed.m[0][2]+point[1]*composed.m[1][2]+point[2]*composed.m[2][2]+composed.m[3][2]};
    };
    const auto initial=cameraPoint();const float initialX=model.boneTable[bone].matInit[12];
    for(int frame=1;frame<=30;++frame){
        StepFrame(app,true);const auto point=cameraPoint();
        for(int axis=0;axis<3;++axis)Check(std::fabs(point[axis]-initial[axis])<.01f,"camera follows actual keyed bone through full FrameDriver");
    }
    Check(std::fabs(model.boneTable[bone].matInit[12]-initialX)>9.f,"fixture bone actually moved ten units");
    std::puts("PASS UI binding and 30 rendered frames of actual keyed bone motion");
    const int stoppedFrame=app->CurrentFrame();
    model.boneTable[bone].trans[0]+=5.f;SetPhysicsMode(raw,0,app->ModelSlots(),0);
    const auto stopped=cameraPoint();
    Check(app->CurrentFrame()==stoppedFrame&&std::fabs(model.boneTable[bone].matInit[12]-initialX)>14.f,"bone changes while timeline is stopped");
    for(int axis=0;axis<3;++axis)Check(std::fabs(stopped[axis]-initial[axis])<.01f,"stopped camera view follows live bone movement");
    std::puts("PASS stopped camera view follows live bone movement");
    wchar_t fixturePath[MAX_PATH]{};
    if(GetEnvironmentVariableW(L"MMS_CAMERA_FIXTURE_PATH",fixturePath,MAX_PATH)>0){
        app->CurrentFrame()=1;StepFrame(app,false);
        std::wcscpy(app->EnvFileName(),fixturePath);SaveSceneFile(app);
        Check(GetFileAttributesW(fixturePath)!=INVALID_FILE_ATTRIBUTES,"camera fixture PMM saved");
    }
}

void CheckViewTools(MMDApp* app) {
    const HWND window = app->MainWindow();
    int slot = 0;
    while (slot < kModelSlotCount && (!app->ModelSlot(slot) ||
           mdl::Mdl(app->ModelSlot(slot))->boneCount < 20)) ++slot;
    Check(slot < kModelSlotCount, "view tools have a character model");
    auto* model = mdl::Mdl(app->ModelSlot(slot));
    const auto choose = [&](int control, int item) {
        HWND combo = GetDlgItem(window, control);
        SendMessageW(combo, CB_SETCURSEL, item, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(control, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(combo));
    };
    const auto matrices = [&]() {
        SetupFrameWorldTransform(app);
        std::array<D3DMATRIX, 2> result{};
        app->Renderer()->device->GetTransform(D3DTS_WORLD, &result[0]);
        app->Renderer()->device->GetTransform(D3DTS_VIEW, &result[1]);
        return result;
    };
    choose(panel::kMainComboModel, 0);
    app->CameraParentModel() = -1;
    choose(panel::kMainComboNormal, model->comboSelIndex);
    Check(app->CameraMode() != 0 && app->CameraParentModel() == slot,
          "camera panel binds character through its actual combo");
    app->CameraPositionZ() = -30.f;
    auto before = matrices();
    app->ViewportInputActive() = 1;
    app->PreviousMouseX() = app->SidebarWidth() + 80; app->PreviousMouseY() = 100;
    app->MouseX() = app->PreviousMouseX() + 40; app->MouseY() = 120;
    app->MiddleMouseButtonState() = 3;
    const float x = app->CameraPositionX();
    ConsumeMiddleButtonPan(app);
    Check(app->CameraPositionX() != x, "attached camera middle drag changes relative X");
    auto after = matrices();
    Check(std::memcmp(before.data(), after.data(), sizeof(after)) != 0,
          "attached camera pan reaches rendered matrices");
    app->MiddleMouseButtonState() = 0;
    const float z = app->CameraPositionZ();
    HandleMouseWheel(-120);
    Check(app->CameraPositionZ() != z, "attached camera wheel changes relative distance");
    before = matrices();
    Check(std::memcmp(before.data(), after.data(), sizeof(after)) != 0,
          "attached camera wheel reaches rendered matrices");
    const float movedX = app->CameraPositionX(), movedZ = app->CameraPositionZ();
    for (int i = 0; i < 3; ++i) {
        app->DeltaTime() = 1.f / 30.f; app->state.messageSeen = 10;
        FrameDriver(app);
    }
    Check(app->CameraPositionX() == movedX && app->CameraPositionZ() == movedZ,
          "attached camera navigation survives subsequent frames");

    RegisterCameraState(app, app->CurrentFrame());
    Check(app->CameraKeys()[0].eye[2] == movedZ, "camera registration stores the actual Z field");
    app->CameraPositionZ() = 100.f;
    ReloadModels(app);
    Check(app->CameraPositionZ() == movedZ, "camera seek restores the actual Z field");
    app->ViewportInputActive() = 1;
    app->RightMouseButtonState() = app->CtrlModifierState() = 3;
    app->MouseY() = 120; app->PreviousMouseY() = 100;
    ConsumeRightButtonDrag(app);
    Check(app->CameraPositionZ() != movedZ, "attached camera Ctrl-right drag dollies relative Z");
    app->RightMouseButtonState() = app->CtrlModifierState() = 0;
    // A non-root parent may be rotated: binding must use its inverse basis.
    choose(panel::kBoneRegisterCombo, 1);
    const int parentBone = app->CameraParentBone();
    Check(parentBone >= 0 && parentBone < static_cast<int>(model->boneCount), "select bone parent");
    auto& attachedBone = model->boneTable[parentBone];
    const auto savedParent = attachedBone;
    d3dx::D3DXMATRIXF rotated{};
    d3dx::Get().rotY(&rotated, .7f);
    std::memcpy(attachedBone.matInit, &rotated, sizeof(rotated));
    app->CameraYaw() = app->CameraPitch() = app->CameraRoll() = 0.f;
    const auto attachedMatrices = matrices();
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            Check(std::fabs(attachedMatrices[0].m[row][column] - rotated.m[column][row]) < 1e-4f,
                  "attached camera uses the inverse of a rotated parent bone");
    attachedBone = savedParent;
    std::puts("PASS attached camera pan, dolly, register and seek");

    choose(panel::kMainComboModel, model->comboSelIndex);
    app->state.followCameraEnabled = 0;
    app->CameraParentModel() = -1;
    app->CameraReferenceMode() = CameraAttachmentReference::None;
    SendMessageW(GetDlgItem(window, panel::kCameraRefModelCheckbox), BM_SETCHECK, 0, 0);
    SendMessageW(GetDlgItem(window, panel::kCameraRefBoneCheckbox), BM_SETCHECK, 0, 0);
    app->CameraPitch() = app->CameraYaw() = app->CameraRoll() = 0.f;
    app->ViewOffsetX() = 3.f; app->ViewOffsetY() = 12.f; app->CameraDistance() = -70.f;
    // Deliberately separate the first bone from the named center bone.
    auto* root = &model->boneTable[0];
    auto savedRoot = *root;
    std::fill(root->matInit, root->matInit + 16, 0.f);
    root->matInit[0] = root->matInit[5] = root->matInit[10] = root->matInit[15] = 1.f;
    root->matInit[12] = 40.f; root->matInit[13] = 6.f; root->matInit[14] = -8.f;
    root->position[0] = root->position[1] = root->position[2] = 0.f;
    before = matrices();
    SendMessageW(GetDlgItem(window, panel::kCameraRefModelCheckbox), BM_CLICK, 0, 0);
    Check(app->CameraReferenceMode() == CameraAttachmentReference::ModelRoot,
          "model checkbox selects root reference");
    after = matrices();
    Check(std::fabs(after[0]._41 + 40.f) < 1e-4f &&
          std::fabs(after[0]._42 + 6.f) < 1e-4f && std::fabs(after[0]._43 - 8.f) < 1e-4f,
          "model orbit pivot is the first bone, including a distant operation center");
    for (int axis = 0; axis < 3; ++axis)
        Check(std::fabs(before[0].m[3][axis] + before[1].m[3][axis] -
                        after[0].m[3][axis] - after[1].m[3][axis]) < 1e-4f,
              "changing orbit reference preserves the current view");
    model->selectedBone = 1;
    SendMessageW(GetDlgItem(window, panel::kCameraRefBoneCheckbox), BM_CLICK, 0, 0);
    Check(app->CameraReferenceMode() == CameraAttachmentReference::SelectedBone,
          "bone checkbox selects selected-bone reference");
    after = matrices();
    auto& bone = model->boneTable[1];
    for (int axis = 0; axis < 3; ++axis) {
        float focus = bone.matInit[12 + axis];
        for (int j = 0; j < 3; ++j) focus += bone.matInit[j * 4 + axis] * bone.position[j];
        Check(std::fabs(after[0].m[3][axis] + focus) < 1e-4f,
              "bone orbit pivot uses the selected bone world position");
    }
    *root = savedRoot;
    SendMessageW(GetDlgItem(window, panel::kCameraRefBoneCheckbox), BM_CLICK, 0, 0);
    std::puts("PASS model and selected-bone orbit pivots");
    CheckBoneMultiply(app, model);

    // Optional render-target captures of the two operation-button rows. The
    // camera state paints the camera plate, the accessory state the ".x" mark;
    // comparing the two PNGs is how the mark is checked against the plate.
    // Nothing is written unless the directory is given.
    wchar_t previewDirectory[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"MIKUDANCESTUDIO_HUD_PREVIEW_DIR", previewDirectory,
                                MAX_PATH) > 0 && previewDirectory[0] != 0) {
        const auto captureRows = [&](const wchar_t* name) {
            app->state.messageSeen = 10;
            RenderFrameScene(app);
            IDirect3DSurface9* surface = nullptr;
            Check(SUCCEEDED(app->Renderer()->device->GetRenderTarget(0, &surface)),
                  "capture the operation-button rows");
            const auto path = std::wstring(previewDirectory) + L"/" + name + L".png";
            Check(SUCCEEDED(d3dx::Get().saveSurfaceToFileW(path.c_str(), 3, surface,
                                                           nullptr, nullptr)),
                  "save the operation-button rows");
            surface->Release();
        };
        app->state.optflag[0] = 1;          // camera/accessory panel
        app->state.playbackActive = 0;
        app->state.coordinateSystem = 0;
        captureRows(L"hud-camera-rows");
        if (CoordinateStateLimit(app) == 3) {
            app->state.coordinateSystem = 2;
            captureRows(L"hud-accessory-rows");
        } else {
            std::puts("no accessory in this scene: accessory row capture skipped");
        }
        app->state.coordinateSystem = 0;
        app->state.optflag[0] = 0;
    }
}

void CheckFreeViewPlayback(MMDApp* app, bool retainedCameraParent) {
    const HWND window = app->MainWindow();
    int slot = 0;
    while (slot < kModelSlotCount && (!app->ModelSlot(slot) ||
           mdl::Mdl(app->ModelSlot(slot))->boneCount < 20)) ++slot;
    Check(slot < kModelSlotCount, "playback scene has a model");
    const HWND modelCombo = GetDlgItem(window, panel::kMainComboModel);
    SendMessageW(modelCombo, CB_SETCURSEL, mdl::Mdl(app->ModelSlot(slot))->comboSelIndex, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(panel::kMainComboModel, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(modelCombo));
    Check(app->SelectedModelSlot() == slot && app->CameraMode() == 0,
          "selecting the character enters model editing");
    app->state.followCameraEnabled = 0;
    if (retainedCameraParent) {
        // A camera attachment can remain in a loaded project even though
        // model editing and free view are selected.
        app->CameraParentModel() = slot;
        app->CameraParentBone() = 0;
    }
    app->CameraPitch() = -.2f; app->CameraYaw() = .35f;
    app->ViewOffsetX() = 3.f; app->ViewOffsetY() = 12.f;
    app->CameraDistance() = -70.f;
    SetWindowTextW(GetDlgItem(window, panel::kPlayStartFrameEdit), L"0");
    SetWindowTextW(GetDlgItem(window, panel::kPlayStopFrameEdit), L"60");
    const auto view = [&]() {
        return std::array<float, 10>{app->ViewOffsetX(), app->ViewOffsetY(),
            app->CameraDistance(), app->CameraPitch(), app->CameraYaw(), app->CameraRoll(),
            app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(), app->CameraFov()};
    };
    const auto transforms = [&]() {
        SetupFrameWorldTransform(app);
        std::array<D3DMATRIX, 3> result{};
        int index = 0;
        for (auto kind : {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION})
            Check(SUCCEEDED(app->Renderer()->device->GetTransform(kind, &result[index++])),
                  "read actual free-view transforms");
        return result;
    };
    const auto capture = [&](const wchar_t* name) {
        wchar_t directory[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"MIKUDANCESTUDIO_PLAYBACK_VIEW_PREVIEW_DIR", directory, MAX_PATH)) return;
        app->state.messageSeen = 10;
        RenderFrameScene(app);
        IDirect3DSurface9* surface = nullptr;
        Check(SUCCEEDED(app->Renderer()->device->GetRenderTarget(0, &surface)), "capture free view surface");
        const auto path = std::wstring(directory) + L"/" + name + L".png";
        Check(SUCCEEDED(d3dx::Get().saveSurfaceToFileW(path.c_str(), 3, surface, nullptr, nullptr)), "save free view preview");
        surface->Release();
    };
    const auto pressP = [&]() {
        unsigned char keys[256]{};
        app->LetterHotkeys().Update(keys, true);
        keys['P'] = 0x80; app->LetterHotkeys().Update(keys, true);
        DispatchLetterHotkeys(app, GetDlgItem(window, panel::kPlayButton));
        keys['P'] = 0; app->LetterHotkeys().Update(keys, true);
    };
    auto expected = view();
    auto expectedTransforms = transforms();
    Check(view() == expected, "free-view rendering ignores a retained camera parent");
    capture(L"free-view-stopped");
    pressP();
    Check(app->PlaybackActive() != 0, "P starts playback in model view");
    Check(view() == expected, "starting playback preserves the free view");
    capture(L"free-view-playing");
    // A camera-follow refresh queued before returning to free view must be
    // discarded, even if playback starts before the next render.
    app->state.modelReloadPending = 1;
    for (int frame = 1; frame <= 4; ++frame) {
        app->TimeNowLow() = app->PlaybackClockAnchorLow() + frame * 33;
        app->DeltaTime() = .033f;
        FrameDriver(app);
        const auto actualTransforms = transforms();
        Check(std::memcmp(actualTransforms.data(), expectedTransforms.data(), sizeof(actualTransforms)) == 0,
              "playing with a selected model never anchors the view to its root bone");
        Check(app->state.modelReloadPending == 0, "obsolete camera refresh is consumed");
        Check(view() == expected, "playing frames preserve the free view");
    }
    app->ViewportInputActive() = 1;
    app->PreviousMouseX() = 100; app->PreviousMouseY() = 100;
    app->MouseX() = 112; app->MouseY() = 106;
    app->RightMouseButtonState() = 3;
    ConsumeRightButtonDrag(app);
    Check(view() != expected, "right drag rotates the free view while playing");
    expected = view();
    app->CtrlModifierState() = 3;
    ConsumeRightButtonDrag(app);
    Check(app->CameraDistance() != expected[2], "Ctrl-right drag dollies the free view while playing");
    app->CtrlModifierState() = 0;
    expected = view();
    app->RightMouseButtonState() = 0;
    app->MiddleMouseButtonState() = 3;
    ConsumeMiddleButtonPan(app);
    Check(view() != expected, "middle drag pans the free view while playing");
    expected = view();
    app->MiddleMouseButtonState() = 0;
    expectedTransforms = transforms();
    FrameDriver(app);
    const auto movedTransforms = transforms();
    Check(std::memcmp(movedTransforms.data(), expectedTransforms.data(), sizeof(movedTransforms)) == 0,
          "playback renders the adjusted free view");
    Check(view() == expected, "playback keeps the adjusted free view");
    pressP();
    Check(app->PlaybackActive() == 0, "P stops playback");
    Check(view() == expected, "stopping playback preserves the adjusted free view");
    const auto stoppedTransforms = transforms();
    Check(std::memcmp(stoppedTransforms.data(), expectedTransforms.data(), sizeof(stoppedTransforms)) == 0,
          "stopping playback keeps the actual view transforms");

    // An explicit camera parent applies in the camera panel and in tracked
    // model view. It is ignored only by the untracked free model view above.
    app->CameraParentModel() = slot; app->CameraParentBone() = 0;
    std::array<D3DMATRIX, 3> attachedTransforms{};
    for (bool cameraPanel : {true, false}) {
        app->CameraMode() = cameraPanel ? 1 : 0;
        app->state.followCameraEnabled = cameraPanel ? 0 : 1;
        ApplyCurrentCameraView(app);
        SetupFrameWorldTransform(app);
        float focus[3]{};
        Check(ResolveFrameFocus(app, static_cast<int>(app->CameraReferenceMode()), focus) ==
                  FrameFocusSource::AttachmentBone, "camera and tracked model views resolve the explicit parent");
        Check(app->ViewOffsetX() == app->CameraPositionX() &&
              app->ViewOffsetY() == app->CameraPositionY() &&
              app->CameraDistance() == app->CameraPositionZ(),
              "attached views use the camera's relative offset");
        const auto currentTransforms = transforms();
        if (cameraPanel) attachedTransforms = currentTransforms;
        else Check(std::memcmp(attachedTransforms.data(), currentTransforms.data(), sizeof(currentTransforms)) == 0,
                   "camera and tracked model panels render the same attached camera");
    }
    app->CameraMode() = app->state.followCameraEnabled = 0;
    app->CameraParentModel() = -1;
    std::printf("PASS selected-model playback: free view, orbit/pan/dolly, stop (retained parent=%d)\n",
                retainedCameraParent);
}
int confirmedDialogs = 0;
bool skipMissingModels=false;
int skippedModels=0;
BOOL CALLBACK PrintDialogText(HWND window, LPARAM) {
    wchar_t text[1024]{};
    GetWindowTextW(window, text, 1024);
    char utf8[4096]{};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (*utf8) std::fprintf(stderr, "  %s\n", utf8);
    return TRUE;
}
LRESULT CALLBACK RejectMenuProbeDialog(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        const HWND dialog = reinterpret_cast<HWND>(wp);
        wchar_t cls[32]{};
        GetClassNameW(dialog, cls, 32);
        if (std::wcscmp(cls, L"#32770") == 0) {
            std::fputs("FAIL: unexpected dialog during menu integration\n", stderr);
            PrintDialogText(dialog, 0);
            EnumChildWindows(dialog, PrintDialogText, 0);
            std::exit(8);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
LRESULT CALLBACK GuardCameraLoadProbe(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if ((create->style & WS_CHILD) == 0) {
            create->style &= ~WS_VISIBLE;
            create->x = create->y = -32000;
        }
    }
    if (code == HCBT_ACTIVATE) {
        RejectMenuProbeDialog(code, wp, lp);
        return 1;
    }
    if (code == HCBT_SETFOCUS) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}

void CheckPmmCameraLoad(MMDApp* app, const wchar_t* scene, const wchar_t* output,
                        bool loadedAtStartup = false) {
    if (!loadedAtStartup) {
        wcscpy_s(app->EnvFileName(), 256, scene);
        LoadSceneFile();
    }
    Check(app->WindowLayoutReady() != 0, "PMM load completes");
    const auto capture = [&](const char* label) {
        std::printf("%s frame=%d mode=%d follow=%d parent=%d/%d suppressed=%d "
                    "position=%.9g,%.9g,%.9g eye=%.9g,%.9g,%.9g "
                    "rotation=%.9g,%.9g,%.9g fov=%.9g\n", label,
            app->CurrentFrame(), app->CameraMode(), app->state.followCameraEnabled,
            app->CameraParentModel(), app->CameraParentBone(),
            app->CameraAttachmentTransformSuppressed(),
            app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(),
            app->ViewOffsetX(), app->ViewOffsetY(), app->CameraDistance(),
            app->CameraPitch(), app->CameraYaw(), app->CameraRoll(), app->CameraFov());
        SetupFrameWorldTransform(app);
        std::array<D3DMATRIX, 3> result{};
        int index = 0;
        for (auto kind : {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION})
            Check(SUCCEEDED(app->Renderer()->device->GetTransform(kind, &result[index++])),
                  "read loaded camera transforms");
        wchar_t directory[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"MMS_PMM_CAMERA_CAPTURE_DIR", directory, MAX_PATH)) {
            app->state.messageSeen = 1;
            RenderFrameScene(app);
            IDirect3DSurface9* surface = nullptr;
            Check(SUCCEEDED(app->Renderer()->device->GetRenderTarget(0, &surface)),
                  "read camera regression surface");
            const std::wstring name(label, label + std::strlen(label));
            const std::wstring path = std::wstring(directory) + L"/" + name + L".png";
            Check(SUCCEEDED(d3dx::Get().saveSurfaceToFileW(path.c_str(), 3, surface, nullptr, nullptr)),
                  "save camera regression surface");
            surface->Release();
        }
        return result;
    };
    const auto loaded = capture("loaded");
    Check(app->state.followCameraEnabled != 0, "fixture enables camera tracking");
    app->CameraAttachmentTransformSuppressed() = 0;
    // Evaluate only the camera part of a frame refresh: a full model seek
    // would also discard unregistered bone poses stored in the PMM.
    ReloadModels(app);
    const auto refreshed = capture("refreshed");
    const auto compare = [&](const auto& actual, const char* label) {
        float difference = 0;
        for (int matrix = 0; matrix < 3; ++matrix)
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    difference = (std::max)(difference, std::fabs(
                        loaded[matrix].m[row][column] - actual[matrix].m[row][column]));
        std::printf("%s: camera transform max difference=%.9g\n", label, difference);
        Check(difference < 1e-5f, label);
    };
    compare(refreshed, "opening tracking matches refreshing its current camera key");
    CmdFileMenu(app, app->MainWindow(), 247, 0);
    CmdFileMenu(app, app->MainWindow(), 247, 0);
    const auto tracked = capture("after restoring tracking");
    compare(tracked, "loading tracking restores the same view as enabling tracking");
    // The playback initializer clears the free-editor offsets independently
    // of the loader and tracking toggle. Keep the cursor at the loaded frame.
    app->PlaybackCursorSeconds() = double(app->CurrentFrame()) / 30.;
    UpdateBoneFrames(app);
    ReloadModels(app);
    compare(capture("after playback initialization"), "loading tracking matches playback initialization");
    // A parent bone can legitimately move when the first model/physics frame
    // evaluates saved edits. Compare the loader/toggle above at the same pose;
    // only an unattached camera must retain its world matrix across previews.
    if (app->CameraParentModel() < 0) {
        for (int frame = 0; frame < 3; ++frame) {
            app->DeltaTime() = 1.f / 30.f;
            app->state.messageSeen = 1;
            FrameDriver(app);
        }
        compare(capture("after preview frames"), "preview frames preserve the loaded camera");
    }
    Check(!IsWindowVisible(app->MainWindow()), "camera load regression stays hidden");
    std::puts("PASS PMM tracked camera is correct before the first frame refresh");

    if (!output || !*output) return;
    const auto cameraState = [&]() {
        return std::array<float, 15>{app->CameraPositionX(), app->CameraPositionY(),
            app->CameraPositionZ(), app->ViewOffsetX(), app->ViewOffsetY(),
            app->CameraDistance(), app->CameraPitch(), app->CameraYaw(), app->CameraRoll(),
            app->CameraFov(), float(app->CameraPerspective()), float(app->CameraParentModel()),
            float(app->CameraParentBone()), float(app->CameraAttachmentTransformSuppressed()),
            float(app->CurrentFrame())};
    };
    const auto roundtrip = [&](int number, const std::array<float, 15>& expected) {
        const std::wstring path = std::wstring(output) + L"/camera-" +
                                  std::to_wstring(number) + L".pmm";
        Check(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES,
              "roundtrip output path is fresh");
        Check(SaveSceneCopy(app, path.c_str()), "save isolated camera fixture");
        wcscpy_s(app->EnvFileName(), 256, path.c_str());
        LoadSceneFile();
        const auto actual = cameraState();
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (std::fabs(actual[i] - expected[i]) >= 1e-5f)
                std::printf("fixture=%d component=%zu expected=%.9g actual=%.9g\n",
                            number, i, expected[i], actual[i]);
            Check(std::fabs(actual[i] - expected[i]) < 1e-5f,
                  "PMM roundtrip restores the intended camera state");
        }
        Check(app->SceneModified() == 0, "camera restoration does not dirty the project");
        std::printf("PASS camera fixture %d\n", number);
    };
    const int modelSlot = app->SelectedModelSlot();
    Check(app->ModelSlot(modelSlot) != nullptr, "tracking fixture has a model");
    int number = 0;
    for (int parent : {-1, modelSlot}) {
        app->CameraMode() = 0;
        app->state.followCameraEnabled = 1;
        app->CameraParentModel() = parent;
        app->CameraParentBone() = 0;
        for (int frame : {0, 30}) {
            app->CameraPositionX() = 3.f + frame;
            app->CameraPositionY() = 12.f;
            app->CameraPositionZ() = -4.f;
            app->CameraDistance() = -45.f - frame;
            app->CameraPitch() = 0.2f;
            app->CameraYaw() = 0.3f;
            app->CameraRoll() = -0.1f;
            app->CameraFov() = float(30 + frame);
            RegisterCameraState(app, frame);
        }
        for (int frame : {0, 15}) {
            app->CurrentFrame() = frame;
            app->CameraAttachmentTransformSuppressed() = 0;
            ReloadModels(app);
            ApplyCurrentCameraView(app);
            const auto expected = cameraState();
            app->CameraDistance() = 123.f;
            app->CameraPositionX() = 456.f;
            app->CameraYaw() = 0.9f;
            app->CameraFov() = 70.f;
            app->CameraParentModel() = -1;
            app->CameraAttachmentTransformSuppressed() = 1;
            roundtrip(number++, expected);
        }
    }
    // Saved free views and unregistered camera edits must not be replaced by
    // the camera key just because they differ from it.
    for (int mode : {0, 1}) {
        for (int follow : {0, 1}) {
            if (mode == 0 && follow == 1) continue;
            app->CameraMode() = mode;
            app->state.followCameraEnabled = follow;
            app->ViewOffsetX() = 7.f;
            app->ViewOffsetY() = 11.f;
            app->CameraDistance() = -123.f;
            app->CameraPositionX() = 42.f;
            app->CameraYaw() = 0.7f;
            app->CameraFov() = 51.f;
            app->CameraAttachmentTransformSuppressed() = 1;
            roundtrip(number++, cameraState());
            Check(app->CameraMode() == mode && app->state.followCameraEnabled == follow,
                  "PMM preserves the active camera mode and tracking option");
        }
    }
    Check(!IsWindowVisible(app->MainWindow()), "camera roundtrips stay hidden");
    std::puts("PASS tracking parents/interpolation, free view and unregistered camera edits");
}
LRESULT CALLBACK ConfirmTestDialog(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        HWND dialog = reinterpret_cast<HWND>(wp);
        wchar_t cls[32]{}; GetClassNameW(dialog, cls, 32);
        if (std::wcscmp(cls, L"#32770") == 0 && skipMissingModels &&
            GetDlgItem(dialog,0x2B0) && !GetDlgItem(dialog,0x2B2)) {
            const wchar_t* titles[] = {L"プロジェクト読み込み", L"Load project", L"加载项目"};
            const wchar_t* skip[] = {L"このモデルを飛ばして続行", L"Skip this model and continue", L"跳过此模型并继续加载"};
            const wchar_t* body[] = {L"モデルファイルを開けません：", L"Cannot open the model file:", L"找不到或无法打开模型文件："};
            const auto language = g_Block->EnglishUI();
            Check(language <= kUiChinese, "test uses a supported UI language");
            wchar_t text[1024]{};
            GetWindowTextW(dialog, text, 1024);
            Check(std::wcscmp(text, titles[language]) == 0, "PMM missing model title follows live language");
            GetDlgItemTextW(dialog, 0x2AE, text, 1024);
            Check(std::wcsstr(text, body[language]) == text, "PMM missing model explanation follows live language");
            GetDlgItemTextW(dialog, 0x2B0, text, 1024);
            Check(std::wcscmp(text, skip[language]) == 0, "PMM missing model skip button follows live language");
            ++skippedModels;ShowWindow(dialog,SW_HIDE);
            PostMessageW(dialog,WM_COMMAND,0x2B0,0);
        } else if (std::wcscmp(cls, L"#32770") == 0 && GetDlgItem(dialog, IDOK)) {
            ++confirmedDialogs;
            PostMessageW(dialog, WM_COMMAND, IDOK, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
// Exercise production message/command paths against the loaded model. These
// edits exist only in this test process; the user's PMM is never saved.
void CheckEditorCommands(MMDApp* app) {
    const HWND window = app->MainWindow();
    auto pressLetter = [&](unsigned char key) {
        unsigned char keys[256]{};
        app->LetterHotkeys().Update(keys, true);
        keys[key] = 0x80;
        app->LetterHotkeys().Update(keys, true);
        DispatchLetterHotkeys(app, GetDlgItem(window, 494));
        keys[key] = 0;
        app->LetterHotkeys().Update(keys, true);
    };
    int slot = 0;
    while (slot < kModelSlotCount && (!app->ModelSlot(slot) ||
           mdl::Mdl(app->ModelSlot(slot))->boneCount < 2)) ++slot;
    Check(slot < kModelSlotCount, "scene has editable bones");
    app->SetSelectedModelSlot(slot); app->state.optflag[0] = 0;
    auto* raw = app->ModelSlot(slot); auto& model = *mdl::Mdl(raw);
    PostLoadInit(raw); PostLanguageSweep(app); PostLanguageSweep2(app);

    // A click without WM_MOUSEMOVE must use its own coordinates and select
    // the visible root row, even when the previously stored position is stale.
    app->MouseX() = 900; app->MouseY() = 900;
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 167));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(40, 167));
    Check(model.selectedBone == static_cast<int>(model.displayRootBone) &&
          model.boneSelection[model.displayRootBone], "name-column click selects the bone");

    CmdControl450(app, window, 494, 0);
    int target = -1;
    for (unsigned i = 1; i < model.boneCount; ++i) {
        if (!model.boneSelection[i] || model.boneTable[i].parent < 0) continue;
        target = static_cast<int>(i);
        model.boneTable[i].trans[1] = 5.f;
        model.boneTable[i].rotQuat[2] = .5f;
    }
    Check(target >= 0, "select-all includes parented bones");
    CmdControl450(app, window, 495, 0);
    for (unsigned i = 1; i < model.boneCount; ++i)
        if (model.boneSelection[i] && model.boneTable[i].parent >= 0)
            Check(model.boneTable[i].trans[1] == 0.f &&
                  model.boneTable[i].rotQuat[2] == 0.f && model.boneTable[i].rotQuat[3] == 1.f,
                  "initialize resets every selected parented bone");

    // Confirm the actual F-paste dialog and verify the target track, not just
    // its enable predicate. Use a name absent from the target model.
    model.selectedBone = target;
    operator delete(app->BoneClipboard());
    app->BoneClipboard() = static_cast<mdl::BoneClipboardRecord*>(operator new(sizeof(mdl::BoneClipboardRecord)));
    auto& copy = *app->BoneClipboard(); copy = {};
    std::strcpy(copy.name, "different source bone");
    copy.rotation[3] = 1.f; copy.rotation[2] = .25f;
    copy.frame = 0; app->ClipboardCounts().bones = 1;
    app->CurrentFrame() = 100;
    const int dialogsBefore = confirmedDialogs;
    HHOOK hook = SetWindowsHookExW(WH_CBT, ConfirmTestDialog, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "install test-owned dialog responder");
    app->state.ctrlModifierState = 3;
    pressLetter('F');
    app->state.ctrlModifierState = 0;
    UnhookWindowsHookEx(hook);
    Check(confirmedDialogs == dialogsBefore + 1, "one paste confirmation was handled");
    auto* keys = mdl::BoneKeys(raw);
    int key = target;
    while (keys[key].next && keys[key].frame < 100) key = static_cast<int>(keys[key].next);
    Check(keys[key].frame == 100 && keys[key].rotation[2] != 0.f, "OK pastes onto selected bone");
    pressLetter('I');
    Check(keys[key].frame == 101, "I shortcut inserts at a keyed frame from panel focus");
    pressLetter('K');
    Check(keys[key].frame == 100, "K shortcut removes the inserted empty frame from panel focus");
    std::memset(model.boneSelection, 0, model.boneCount); model.boneSelection[target] = 1;
    app->state.deleteKeyState = 1;
    PumpDeleteRebuild(app, GetDlgItem(window, 494), true);
    app->state.deleteKeyState = 0;
    Check(keys[key].frame == 0, "Del deletes the current selected key");

    app->state.optflag[0] = 1;
    app->state.cameraPerspective = 0;
    HWND slider = GetDlgItem(window, panel::kFovSlider);
    SendMessageW(slider, TBM_SETPOS, TRUE, 65);
    SendMessageW(window, WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(slider));
    Check(app->CameraFov() == 65.f, "FOV slider message reaches camera state");
    ApplyFrameCameraProjection(app);
    D3DMATRIX projection{};
    Check(SUCCEEDED(app->Renderer()->device->GetTransform(D3DTS_PROJECTION, &projection)) &&
          projection._22 > 1.5f && projection._22 < 1.6f, "FOV reaches live D3D projection");
    std::puts("editor command integration passed");
}

void CheckPhysicsFrameSelection(MMDApp* app) {
    int checked = 0;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        if (!app->ModelSlot(slot)) continue;
        auto& model = *mdl::Mdl(app->ModelSlot(slot));
        if (!model.boneCount) continue;
        app->SetSelectedModelSlot(slot);
        app->CameraMode() = 0;
        PostLanguageSweep(app);
        for (int repeat = 0; repeat < 3; ++repeat) {
            // Exercise the same native message route as the physical menu click.
            SendMessageW(app->MainWindow(), WM_COMMAND, 274, 0);
            for (unsigned bone = 0; bone < model.boneCount; ++bone)
                Check(bool(model.boneKeys[bone].allocated) ==
                    bool(model.boneTable[bone].hasRigidBody && !model.boneKeys[bone].physicsDisabled),
                    "physics menu selects only physics-enabled initial keys");
            Check(HeapValidate(GetProcessHeap(), 0, nullptr) != 0,
                  "physics selection preserves the scene heap");
        }
        ++checked;
    }
    Check(checked > 0, "physics selection was checked on actual scene models");
    std::printf("physics selection passed: %d models, three native menu commands each\n", checked);
}

void CheckNonzeroFrameBoneEditing(MMDApp* app) {
    int slot = -1, target = -1;
    for (int s = 0; s < kModelSlotCount && target < 0; ++s) {
        auto* raw = app->ModelSlot(s);
        if (!raw) continue;
        auto& model = *mdl::Mdl(raw);
        for (unsigned b = 0; b < model.boneCount; ++b) {
            const auto& bone = model.boneTable[b];
            const auto name = text_encoding::BoneName(bone, false);
            if ((bone.type == mdl::BoneType::Move || bone.type == mdl::BoneType::Ik) &&
                name.find(L"右足") != std::wstring::npos) {
                slot = s; target = static_cast<int>(b); break;
            }
        }
    }
    Check(target >= 0, "stage has a movable right-foot bone");
    app->SetSelectedModelSlot(slot);
    app->CameraMode() = 0;
    auto* raw = app->SelectedModel();
    auto& model = *mdl::Mdl(raw);
    auto& bone = model.boneTable[target];
    std::printf("bone movement: slot=%d bone=%d type=%d\n", slot, target, int(bone.type));
    std::memset(model.boneSelection, 0, model.boneCount);
    model.boneSelection[target] = 1;
    model.selectedBone = target;
    PostLanguageSweep(app);
    const HWND window = app->MainWindow();
    auto tick = [&] {
        app->DeltaTime() = 1.f / 30.f;
        app->state.messageSeen = 1;
        FrameDriver(app);
        Check(HeapValidate(GetProcessHeap(), 0, nullptr), "editing preserves the process heap");
    };
    auto worldPoint = [&] {
        std::array<float, 3> point{};
        for (int axis = 0; axis < 3; ++axis)
            point[axis] = bone.position[0] * bone.matInit[axis] +
                bone.position[1] * bone.matInit[4 + axis] +
                bone.position[2] * bone.matInit[8 + axis] + bone.matInit[12 + axis];
        return point;
    };
    auto checkMoved = [&](const std::array<float, 3>& before) {
        const auto after = worldPoint();
        const float distance = std::fabs(after[0] - before[0]) +
            std::fabs(after[1] - before[1]) + std::fabs(after[2] - before[2]);
        Check(distance > .00001f, "edited bone actually moves in the evaluated world pose");
    };
    // Include empty nonzero frames, a keyed frame, and a return to zero.
    for (int frame : {0, 5, 10, 0, 5}) {
        std::printf("bone movement: seek frame %d\n", frame);
        const HWND frameEdit = GetDlgItem(window, panel::kCurrentFrameEdit);
        SetWindowTextA(frameEdit, std::to_string(frame).c_str());
        CommitEditControl(app, frameEdit);
        Check(model.boneSelection[target] && model.selectedBone == target,
              "changing frame keeps the selected bone editable");
        StepFrame(app, true);
        StepFrame(app, false);
        Check(app->CurrentFrame() == frame && model.boneSelection[target],
              "stepping away and back preserves the active bone");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(106 + frame * 13, 150));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(106 + frame * 13, 150));
        Check(app->CurrentFrame() == frame && model.boneSelection[target],
              "timeline header seek preserves the active bone");
        tick();
        for (int axis = 0; axis < 3; ++axis) {
            const auto beforeWorld = worldPoint();
            const HWND edit = GetDlgItem(window, panel::kReadoutPosXEdit + axis);
            const float expected = bone.trans[axis] + .5f;
            SetWindowTextA(edit, std::to_string(expected).c_str());
            SendMessageW(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(window), 0);
            tick(); tick();
            Check(std::fabs(bone.trans[axis] - expected) < .0001f,
                  "coordinate edit survives full render and physics updates at the current frame");
            checkMoved(beforeWorld);
        }
        app->state.coordinateSystem = 1;
        app->ShiftModifierState() = app->CtrlModifierState() = 0;
        for (int mode : {7, 8, 9, 10, 11, 12}) {
            const auto beforeWorld = worldPoint();
            const float before[3] = {bone.trans[0], bone.trans[1], bone.trans[2]};
            app->PreviousMouseX() = app->ViewportRect().left + 80;
            app->PreviousMouseY() = app->ViewportRect().top + 100;
            app->InteractionDragMode() = static_cast<ViewportDragMode>(mode);
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON,
                         MAKELPARAM(app->PreviousMouseX() + 20, app->PreviousMouseY() - 20));
            BoneEditModes(app);
            app->InteractionDragMode() = ViewportDragMode::None;
            const float edited[3] = {bone.trans[0], bone.trans[1], bone.trans[2]};
            Check(std::fabs(edited[0] - before[0]) + std::fabs(edited[1] - before[1]) +
                  std::fabs(edited[2] - before[2]) > .0001f, "handle/axis drag changes the bone pose");
            tick(); tick();
            for (int axis = 0; axis < 3; ++axis)
                Check(std::fabs(bone.trans[axis] - edited[axis]) < .0001f,
                      "dragged pose survives full render and physics updates");
            checkMoved(beforeWorld);
        }
        RegisterSelectedBoneKeys(raw, frame, app->PlaybackPhysicsMode());
        tick();
        std::puts("bone movement: frame passed");
    }
    std::puts("nonzero-frame bone movement integration passed");
}

// Optional integration probe with/without the locally installed MME DLLs.
// No project or user model is needed, and no effect command opens a dialog.
void CheckEffectMenus(MMDApp* app) {
    for (int frame = 0; frame < 2; ++frame) {
        app->DeltaTime() = 1.f / 30.f;
        app->state.messageSeen = 10;
        FrameDriver(app);
    }
    const HWND window = app->MainWindow();
    const auto caption = [&](UINT id) {
        wchar_t text[256]{};
        GetMenuStringW(GetMenu(window), id, text, 256, MF_BYCOMMAND);
        return std::wstring(text);
    };
#ifdef REQUIRE_MME
    Check(!caption(40005).empty(), "loaded MME creates its effect menu");
    Check(InstallEffectMaterialNameCompatibility(), "real MME name import is patched before object registration");
    SendMessageW(window, WM_COMMAND, 40006, 0);
    const UINT enabled = GetMenuState(GetMenu(window), 40006, MF_BYCOMMAND) & MF_CHECKED;
#else
    Check(GetModuleHandleW(L"MMEffect.dll") == nullptr, "plain host starts without MME");
    Check(!InstallEffectMaterialNameCompatibility(), "plain host does not load MME for name fallback");
#endif
    for (int cycle = 0; cycle < 6; ++cycle) {
        SendMessageW(window, WM_COMMAND, 260, 0);
#ifdef REQUIRE_MME
        const wchar_t* labels[] = {L"エフェクト割当(&M)", L"Effect Mapping(&M)", L"效果分配(&M)"};
        Check(caption(40005) == labels[app->EnglishUI()], "real MME menu follows the live host language");
        Check((GetMenuState(GetMenu(window), 40006, MF_BYCOMMAND) & MF_CHECKED) == enabled,
              "real MME enable state survives translation and menu recreation");
        if (app->EnglishUI() == kUiChinese) {
            Check(caption(40006) == L"启用效果(&E)\tCtrl+Shift+E" &&
                  caption(40001) == L"自动重载(&O)" &&
                  caption(40022) == L"自动保存效果分配文件(&S)" &&
                  caption(40004) == L"全部重载(&R)" &&
                  caption(40002) == L"日志窗口(&L)" &&
                  caption(40003) == L"关于 MMEffect(&A)", "all real MME popup commands are translated");
        }
#else
        Check(GetModuleHandleW(L"MMEffect.dll") == nullptr && caption(40005).empty(),
              "language changes neither load MME nor add a phantom menu");
#endif
    }
    HMENU menu = GetMenu(window);
    SetMenu(window, nullptr);
    RefreshEffectMenuLanguage(app);
    InstallEffectMenuCompatibility(app);
    Check(GetMenu(window) == nullptr, "missing menu is a no-op even with the DLL loaded");
    SetMenu(window, menu);
    std::puts("MME menu language and optional-DLL integration passed");
}
}
#ifdef REQUIRE_MME
extern "C" std::uint32_t ExpGetPmdID(int);

static void CheckEffectMaterialNames(MMDApp* app) {
    Check(InstallEffectMaterialNameCompatibility(), "material name compatibility installed");
    // Resolve the test call by the import's NAME, independently of the
    // production installer's original-export-address lookup.
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(L"MMEffect.dll"));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    EffectMaterialNameQuery imported = nullptr;
    for (auto* entry = imports; entry->Name; ++entry) {
        if (_stricmp(reinterpret_cast<char*>(base + entry->Name), "MMHack.dll")) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char*>(name->Name), "GetMaterialName") == 0)
                imported = reinterpret_cast<EffectMaterialNameQuery>(slots[i].u1.Function);
        }
    }
    const auto original = reinterpret_cast<EffectMaterialNameQuery>(
        GetProcAddress(GetModuleHandleW(L"MMHack.dll"), "GetMaterialName"));
    Check(imported && original && imported != original, "MME actually calls the fallback import");
    int checked = 0, japaneseOnly = 0, englishOnly = 0, ordinal = 0;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        if (!app->ModelSlot(slot)) continue;
        const auto* model = mdl::Mdl(app->ModelSlot(slot));
        const std::uintptr_t id = ExpGetPmdID(ordinal++);
        for (unsigned material = 0; material < model->materialCount; ++material) {
            const auto* jp = original(id, material, 0);
            const auto* en = original(id, material, 1);
            if ((!jp || !*jp) && (!en || !*en)) continue;
            ++checked;
            if (jp && *jp && (!en || !*en)) ++japaneseOnly;
            if (en && *en && (!jp || !*jp)) ++englishOnly;
            for (int english : {0, 1}) {
                const wchar_t* expected = english ? en : jp;
                if (!expected || !*expected) expected = english ? jp : en;
                const wchar_t* actual = imported(id, material, english);
                Check(actual && std::wcscmp(actual, expected) == 0,
                      "actual MMHack Unicode names survive either language preference through MME's import");
            }
        }
    }
    Check(checked > 0 && japaneseOnly > 0, "integration fixture exercises missing English material names");
    std::printf("MME material names: %d checked, %d Japanese-only, %d English-only\n",
                checked, japaneseOnly, englishOnly);
}
#endif

// The reference's startup panel is the camera panel, so the first frame pivots
// on cameraPos and derives the eye from the view offsets; the ground origin must
// project `atan(10/45)` below the viewport centre. The reported defect was a
// startup view ten units too high, which one frame step repaired, so this checks
// the composition and then its invariance across a frame evaluation.
void CheckStartupView(MMDApp* app) {
    Check(app->CameraMode() != 0, "the reference starts in the camera panel");
    Check(app->CameraPositionX() == 0.0f && app->CameraPositionY() == 10.0f &&
              app->CameraPositionZ() == 0.0f,
          "startup camera position is (0, 10, 0)");
    Check(app->CameraDistance() == -45.0f, "startup camera distance is -45");
    Check(app->CameraPitch() == 0.0f && app->CameraYaw() == 0.0f &&
              app->CameraRoll() == 0.0f,
          "startup camera angles are zero");
    Check(app->CameraFov() == 30.0f, "startup view angle is 30 degrees");
    // The reference's initializer clears both offsets with a single 8-byte zero
    // store (x64 0x14000C5A1); the default height lives in cameraPosY, so an
    // offset that repeats it raises the eye 10 units above the pivot.
    Check(app->ViewOffsetX() == 0.0f && app->ViewOffsetY() == 0.0f,
          "startup view offsets are cleared");
    float focus[3]{};
    Check(ResolveFrameFocus(app, static_cast<int>(app->CameraReferenceMode()),
                            focus) == FrameFocusSource::CameraPosition &&
              focus[1] == 10.0f,
          "startup pivot is the camera position");
    Check(std::fabs(focus[1] + app->ViewOffsetY() - 10.0f) < 1e-4f,
          "startup ground origin projects atan(10/45) below the viewport centre");
    std::puts("PASS startup viewpoint matches the reference");

    // A frame step performs the same evaluation: ReloadModels applies the default
    // camera key. The view must remain where the startup frame placed it, and the
    // view offsets - which the port stores as the eye's lateral position -
    // must survive the evaluation.
    const auto transforms = [&]() {
        SetupFrameWorldTransform(app);
        std::array<D3DMATRIX, 2> result{};
        int index = 0;
        for (auto kind : {D3DTS_WORLD, D3DTS_VIEW})
            Check(SUCCEEDED(app->Renderer()->device->GetTransform(
                      kind, &result[index++])),
                  "read the world and view transforms");
        return result;
    };
    const auto before = transforms();
    ResetAppState(app);
    ReloadModels(app);
    const auto after = transforms();
    for (int matrix = 0; matrix < 2; ++matrix)
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                Check(std::fabs(before[matrix].m[row][column] -
                                after[matrix].m[row][column]) < 1e-5f,
                      "a frame step leaves the startup view unchanged");
    std::puts("PASS a frame step leaves the startup view unchanged");

    // The eye holds the viewpoint, and a PMM file stores the offsets, so a
    // camera-key evaluation must leave them untouched. Clearing them snapped the
    // view back on every frame step and every playback stop, which the field
    // report described as "stopping playback puts the view back where it was".
    app->ViewOffsetX() = 3.0f;
    app->ViewOffsetY() = 12.0f;
    ReloadModels(app);
    Check(app->ViewOffsetX() == 3.0f && app->ViewOffsetY() == 12.0f,
          "a camera-key evaluation keeps the view offsets");
    std::puts("PASS a camera-key evaluation keeps the view offsets");
    app->ViewOffsetX() = 0.0f;
    app->ViewOffsetY() = 0.0f;
    app->CameraDistance() = -45.0f;
}


// Run only in an isolated output directory: this creates fresh PMM/EMM fixtures.
static void CheckEffectAutoSave(MMDApp* app, const wchar_t* prefix, bool initiallyEnabled,
                                const wchar_t* fixture, bool checkStartup = false) {
    HHOOK guard = SetWindowsHookExW(WH_CBT, GuardCameraLoadProbe, nullptr, GetCurrentThreadId());
    Check(guard != nullptr, "install hidden EMM save dialog guard");
    const HWND window = app->MainWindow();
    const auto enabled = [&]() {
        const UINT state = GetMenuState(GetMenu(window), 40022, MF_BYCOMMAND);
        Check(state != UINT(-1), "real MME auto-save menu exists");
        return (state & MF_CHECKED) != 0;
    };
    const auto toggle = [&](bool value) {
        if (enabled() != value) SendMessageW(window, WM_COMMAND, 40022, 0);
        Check(enabled() == value, "MME auto-save command changes live state");
    };
    const auto frames = [&]() {
        for (int frame = 0; frame < 4; ++frame) {
            app->DeltaTime() = 1.f / 30.f;
            app->state.messageSeen = 10;
            FrameDriver(app);
        }
    };
    const std::wstring stem = prefix;
    const std::wstring pmm = stem + L".pmm", emm = stem + L".emm";
    Check(GetFileAttributesW(pmm.c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW(emm.c_str()) == INVALID_FILE_ATTRIBUTES,
          "fresh isolated save fixture paths");
    const auto save = [&](const std::wstring& path) {
        wcscpy_s(app->EnvFileName(), 256, path.c_str());
        SendMessageW(window, WM_COMMAND, 0xCF, 0);
        frames(); // MME consumes the host's saved-PMM queue while drawing.
        Check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
              "PMM save succeeds");
    };
    frames(); // MME creates its menu on the first rendered frames.
    if (checkStartup) {
        Check(enabled() == initiallyEnabled, "MME startup checkbox follows the default or saved user preference");
        std::puts("PASS startup EMM auto-save checkbox before any toggle");
    } else toggle(initiallyEnabled);
    if (fixture) {
        if (const auto* model = _wgetenv(L"MMS_EMM_FIXTURE_MODEL")) {
            LoadModelFile(app, model, false);
            frames(); // Register the real model before loading its MME mapping.
            NotifyEffectFileOpen(fixture); // The mapping PMM may be a notification-only stub.
        } else {
            wcscpy_s(app->EnvFileName(), 256, fixture);
            LoadSceneFile();
        }
        frames();
        Check(app->ModelSlot(0) != nullptr, "effect fixture loads a real model");
    }
    save(pmm);
    Check((GetFileAttributesW(emm.c_str()) != INVALID_FILE_ATTRIBUTES) == initiallyEnabled,
          "first save follows initial auto-save setting");
    std::puts("PASS first save follows initial auto-save setting");
    {
        autosave::Initialize(app);
        auto settings = autosave::Current();
        settings.enabled = true; settings.overwriteOldest = true; settings.maxBackups = 1;
        settings.directory = stem + L"-backups";
        std::wstring error;
        Check(autosave::Configure(app, settings, error), "configure isolated paired backup");
        Check(autosave::BackupNow(app) == autosave::Result::Saved, "create native MME backup");
        frames();
        auto backups = autosave::Backups(settings.directory);
        Check(backups.size() == 1, "one owned backup was created");
        auto sidecar = backups[0]; sidecar.replace_extension(L".emm");
        Check(std::filesystem::is_regular_file(sidecar) == initiallyEnabled,
              "automatic backup follows the live MME EMM save setting");
        const auto oldSidecar = sidecar;
        Sleep(2);
        Check(autosave::BackupNow(app) == autosave::Result::Saved, "rotate native MME backup");
        frames();
        backups = autosave::Backups(settings.directory);
        Check(backups.size() == 1 && !std::filesystem::exists(oldSidecar),
              "rotation removes the old owned EMM with its PMM");
        sidecar = backups[0]; sidecar.replace_extension(L".emm");
        Check(std::filesystem::is_regular_file(sidecar) == initiallyEnabled,
              "rotated backup keeps the new PMM/EMM pair");
        autosave::Shutdown();
    }
    ResetAppState(app);
    wcscpy_s(app->EnvFileName(), 256, pmm.c_str());
    LoadSceneFile();
    frames();
    if (!initiallyEnabled && fixture) {
        // With auto-save off the first PMM intentionally has no EMM. Reassign
        // the effects explicitly after reopening before testing the on toggle.
        NotifyEffectFileOpen(fixture);
        frames();
    }
    toggle(true);
    save(pmm);
    Check(GetFileAttributesW(emm.c_str()) != INVALID_FILE_ATTRIBUTES,
          "enabling auto-save after opening a project creates EMM");
    std::puts("PASS enabling auto-save after opening a project creates EMM");
    const auto appendMarker = [&]() {
        FILE* file = nullptr;
        Check(_wfopen_s(&file, emm.c_str(), L"ab") == 0, "open EMM marker fixture");
        std::fputs("\n;auto-save-regression-marker\n", file);
        std::fclose(file);
    };
    const auto readEmm = [&](const std::wstring& path) {
        FILE* file = nullptr;
        Check(_wfopen_s(&file, path.c_str(), L"rb") == 0, "read saved EMM");
        std::string contents;
        char bytes[1024];
        while (const auto count = std::fread(bytes, 1, sizeof(bytes), file))
            contents.append(bytes, count);
        std::fclose(file);
        Check(contents.find("[Info]") != std::string::npos,
              "saved EMM contains its format header");
        return contents;
    };
    const auto mapping = readEmm(emm);
    if (fixture)
        Check(mapping.find(".fx") != std::string::npos,
              "real model effect assignment is present in EMM");
    appendMarker();
    toggle(false);
    save(pmm);
    Check(readEmm(emm) == mapping + "\n;auto-save-regression-marker\n",
          "disabled auto-save leaves existing EMM unchanged");
    toggle(true);
    save(pmm);
    Check(readEmm(emm) == mapping, "re-enabled auto-save preserves effect assignments");
    std::puts("PASS off/on toggle updates existing EMM only while enabled");
    const std::wstring renamed = stem + L"_另存_日本語";
    save(renamed + L".pmm");
    Check(GetFileAttributesW((renamed + L".emm").c_str()) != INVALID_FILE_ATTRIBUTES,
          "save-as creates matching Unicode EMM");
    Check(readEmm(renamed + L".emm") == mapping,
          "save-as preserves effect assignments");
    std::puts("PASS save-as creates matching Unicode EMM and preserves assignments");
    if (checkStartup) toggle(initiallyEnabled);
    UnhookWindowsHookEx(guard);
}

namespace {
int numericAction=0;
int numericDialogs=0;
LRESULT CALLBACK NumericDialogHook(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_CREATEWND) {
        auto* create=reinterpret_cast<CBT_CREATEWNDW*>(lp);
        if(!(create->lpcs->style&WS_CHILD)) { create->lpcs->x=-32000;create->lpcs->y=-32000; }
    }
    if(code==HCBT_ACTIVATE) {
        HWND dialog=reinterpret_cast<HWND>(wp);
        if(GetDlgItem(dialog,637)&&GetDlgItem(dialog,642)) {
            ++numericDialogs;
            SetDlgItemTextW(dialog,637,L"1");
            if(numericAction==1) SetDlgItemTextW(dialog,642,L"90");
            if(numericAction==4) SetDlgItemTextW(dialog,637,L"NaN");
            if(numericAction==5) for(int id=637;id<=642;++id) SetDlgItemTextW(dialog,id,L"0");
            PostMessageW(dialog,WM_COMMAND,numericAction==2?IDCANCEL:IDOK,0);
            if(numericAction==4) PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
void CheckNumericInput(MMDApp* app) {
    app->state.optflag[0]=1;
    app->CameraPositionX()=0;app->CameraPositionY()=10;app->CameraPositionZ()=0;
    app->CameraPitch()=0;app->CameraYaw()=0;app->CameraRoll()=0;app->CameraDistance()=-45;
    HHOOK hook=SetWindowsHookExW(WH_CBT,NumericDialogHook,nullptr,GetCurrentThreadId());
    Check(hook!=nullptr,"install offscreen numeric dialog driver");
    numericAction=0;CmdViewMenu(app,app->MainWindow(),300,0);
    std::printf("camera numeric result: x=%g y=%g roll=%g distance=%g\n",app->CameraPositionX(),app->CameraPositionY(),app->CameraRoll(),app->CameraDistance());
    Check(numericDialogs>=1&&app->CameraPositionX()==1&&app->CameraPositionY()==10&&app->CameraDistance()==-45&&app->CameraRoll()==0,"camera X input writes back without dividing zero roll");
    numericAction=1;CmdViewMenu(app,app->MainWindow(),300,0);
    Check(std::fabs(app->CameraRoll()-1.5707963f)<1e-5f,"camera Z input converts degrees to radians");
    app->CameraPositionX()=4;numericAction=2;CmdViewMenu(app,app->MainWindow(),300,0);
    Check(app->CameraPositionX()==4,"camera cancel preserves position");
    numericAction=4;CmdViewMenu(app,app->MainWindow(),300,0);
    Check(app->CameraPositionX()==4,"invalid input cannot publish NaN");
    app->state.optflag[0]=0;
    auto* bytes=app->SelectedModel();Check(bytes!=nullptr,"numeric fixture has selected model");
    auto* model=mdl::Mdl(bytes);model->selectedBone=0;
    std::fill(model->boneSelection,model->boneSelection+model->boneCount,0);model->boneSelection[0]=1;
    auto& bone=mdl::Bones(bytes)[0];
    bone.trans[0]=0;bone.trans[1]=2;bone.trans[2]=3;
    std::fill(bone.rotQuat,bone.rotQuat+4,0.f);bone.rotQuat[3]=1;
    numericAction=1;CmdViewMenu(app,app->MainWindow(),300,0);
    Check(bone.trans[0]==1&&bone.trans[1]==2&&bone.trans[2]==3,"bone position dialog writes back all axes");
    for(float component:bone.rotQuat) Check(std::isfinite(component),"bone quaternion remains finite");
    d3dx::D3DXMATRIXF rotation{};d3dx::Get().matrixRotationQuaternion(&rotation,bone.rotQuat);
    Check(std::fabs(rotation.m[0][1]+1)<1e-5f,"bone Z input uses the same sign as toolbar");
    numericAction=0;CmdViewMenu(app,app->MainWindow(),300,0);
    d3dx::D3DXMATRIXF roundtrip{};d3dx::Get().matrixRotationQuaternion(&roundtrip,bone.rotQuat);
    for(int row=0;row<3;++row)for(int col=0;col<3;++col)Check(std::fabs(roundtrip.m[row][col]-rotation.m[row][col])<1e-4f,"reopening bone dialog preserves rotation");
    for(float pitch:{0.4f,1.57079632679f,-1.57079632679f}) {
        d3dx::D3DXMATRIXF temp{};
        d3dx::Get().rotZ(&rotation,0.6f);
        d3dx::Get().rotX(&temp,pitch);d3dx::Get().multiply(&rotation,&rotation,&temp);
        d3dx::Get().rotY(&temp,-0.3f);d3dx::Get().multiply(&rotation,&rotation,&temp);
        d3dx::Get().quatFromMatrix(bone.rotQuat,&rotation);
        CmdViewMenu(app,app->MainWindow(),300,0);
        d3dx::Get().matrixRotationQuaternion(&roundtrip,bone.rotQuat);
        for(int row=0;row<3;++row)for(int col=0;col<3;++col)Check(std::fabs(roundtrip.m[row][col]-rotation.m[row][col])<1e-3f,"combined and gimbal rotations survive numeric roundtrip");
    }
    UnhookWindowsHookEx(hook);
    std::puts("PASS real camera and bone numeric dialogs: position, rotation, cancel and roundtrip");
}
void CheckParentCoordinates(MMDApp* app) {
    app->state.optflag[0]=0;app->PlaybackPhysicsMode()=0;
    auto* bytes=app->SelectedModel();Check(bytes!=nullptr,"parent coordinate fixture model");
    auto& model=*mdl::Mdl(bytes);Check(model.boneCount>=2,"parent coordinate fixture bones");
    auto* bones=mdl::Bones(bytes);model.selectedBone=1;bones[1].parent=0;
    const float parent[]={2,4,6},child[]={5,8,10};
    for(int axis=0;axis<3;++axis){bones[0].position[axis]=parent[axis];bones[1].position[axis]=child[axis];bones[1].trans[axis]=parent[axis]-child[axis];}
    std::fill(bones[1].rotQuat,bones[1].rotQuat+4,0.f);bones[1].rotQuat[3]=1;
    PostViewRefresh(app);
    const auto read=[&](int id){char text[64]{};GetDlgItemTextA(app->MainWindow(),id,text,sizeof(text));return std::atof(text);};
    std::printf("coincident bone readout: %g %g %g\n",read(544),read(545),read(546));
    for(int axis=0;axis<3;++axis) Check(read(panel::kReadoutPosXEdit+axis)==0,"coincident child bone displays zero parent-local position");
    HWND xEdit=GetDlgItem(app->MainWindow(),panel::kReadoutPosXEdit);
    SetWindowTextW(xEdit,L"1");CommitEditControl(app,xEdit);
    Check(bones[1].trans[0]==-2,"parent-local toolbar input converts to animation offset");
    HHOOK hook=SetWindowsHookExW(WH_CBT,NumericDialogHook,nullptr,GetCurrentThreadId());
    numericAction=5;CmdViewMenu(app,app->MainWindow(),300,0);
    for(int axis=0;axis<3;++axis) Check(bones[1].trans[axis]==parent[axis]-child[axis],"zero numeric position aligns child with parent");
    for(int axis=0;axis<3;++axis) Check(read(panel::kReadoutRotXEdit+axis)==0,"aligned local rotation reads zero");
    float scale[3]{};Check(SelectedBoneScaleReadout(app,scale),"read local scale");
    for(float value:scale)Check(std::fabs(value-1)<1e-5f,"neutral local scale is one");
    app->state.optflag[0]=1;app->CameraParentModel()=app->SelectedModelSlot();app->CameraParentBone()=0;
    app->CameraPositionZ()=2;PostViewRefresh(app);
    Check(read(panel::kReadoutPosZEdit)==2,"bound camera Z is a parent-local coordinate");
    CmdViewMenu(app,app->MainWindow(),300,0);
    Check(app->CameraPositionX()==0&&app->CameraPositionY()==0&&app->CameraPositionZ()==0,"bound camera origin is zero");
    UnhookWindowsHookEx(hook);
    std::puts("PASS parent-local camera and bone position, rotation and neutral scale");
}
LRESULT CALLBACK ObserveFirstSliderPaint(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR context) {
    if(message==WM_PAINT&&!g_Block->SceneMutationInProgress()) ++*reinterpret_cast<int*>(context);
    return DefSubclassProc(window,message,wp,lp);
}
void CheckPmmFirstPaint(MMDApp* app,const wchar_t* scene) {
    SetWindowPos(app->MainWindow(),HWND_BOTTOM,-32000,-32000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    RedrawWindow(app->MainWindow(),nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    const int ids[]={447,455,456,457,458,459,460,560,534};
    int paints[9]{};
    for(int i=0;i<9;++i)SetWindowSubclass(GetDlgItem(app->MainWindow(),ids[i]),ObserveFirstSliderPaint,19,reinterpret_cast<DWORD_PTR>(&paints[i]));
    wcscpy_s(app->EnvFileName(),256,scene);LoadSceneFile();
    for(int i=0;i<9;++i){
        HWND control=GetDlgItem(app->MainWindow(),ids[i]);
        std::printf("PMM first paint slider %d: %d\n",ids[i],paints[i]);
        if(IsWindowVisible(control))Check(paints[i]>0,"visible slider paints after scene mutation before load returns without hover");
        RemoveWindowSubclass(control,ObserveFirstSliderPaint,19);
    }
    std::puts("PASS PMM slider first paints complete without mouse movement");
}
void CheckModalPaint(MMDApp* app) {
    HWND window=app->MainWindow();
    SetWindowPos(window,HWND_BOTTOM,-32000,-32000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    app->WindowLayoutReady()=1;
    app->FullscreenMode()=0;
    EnableWindow(window,FALSE);
    app->state.fpsOverlayFrameCount=100;
    app->state.fpsOverlayElapsedSeconds=0;
    app->DeltaTime()=0;
    d3dx::D3DXMATRIXF marker{};for(int i=0;i<4;++i)marker.m[i][i]=1;
    marker.m[3][0]=12345;
    auto* device=app->Renderer()->device;
    Check(SUCCEEDED(device->SetTransform(D3DTS_WORLD,reinterpret_cast<D3DMATRIX*>(&marker))),"set modal render sentinel");
    HandleWindowPaint(app);
    const auto frames=app->state.fpsOverlayFrameCount;
    d3dx::D3DXMATRIXF result{};
    Check(SUCCEEDED(device->GetTransform(D3DTS_WORLD,reinterpret_cast<D3DMATRIX*>(&result))),"read modal render sentinel");
    std::printf("disabled-owner paint world sentinel: %g\n",result.m[3][0]);
    std::printf("disabled-owner paint frame counter: %u\n",frames);
    EnableWindow(window,TRUE);
    Check(result.m[3][0]==marker.m[3][0],"modal owner repaint must not reenter scene rendering");
    Check(frames==100,"modal owner repaint must not enter frame evaluation");
    std::puts("PASS disabled modal owner paints its UI without scene/MMV rendering");
}
int pmmLoadWarnings=0;
LRESULT CALLBACK PmmErrorAuditHook(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_CREATEWND) {
        auto* create=reinterpret_cast<CBT_CREATEWNDW*>(lp);
        if(!(create->lpcs->style&WS_CHILD)){create->lpcs->x=-32000;create->lpcs->y=-32000;}
    }
    if(code==HCBT_ACTIVATE) {
        HWND dialog=reinterpret_cast<HWND>(wp);wchar_t type[32]{};GetClassNameW(dialog,type,32);
        if(std::wcscmp(type,L"#32770")==0) {
            if(GetPropW(dialog,L"MmsPmmAuditSeen")){PostMessageW(dialog,WM_CLOSE,0,0);return 1;}
            SetPropW(dialog,L"MmsPmmAuditSeen",reinterpret_cast<HANDLE>(1));
            ++pmmLoadWarnings;PrintDialogText(dialog,0);EnumChildWindows(dialog,PrintDialogText,0);
            if(GetDlgItem(dialog,IDOK)){PostMessageW(dialog,WM_COMMAND,IDOK,0);return 1;}
            std::fputs("audit: canceling secondary dialog\n",stderr);
            PostMessageW(dialog,WM_CLOSE,0,0);return 1;
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
void CheckRecordingFrames(MMDApp* app) {
    app->EnglishUI()=2;app->state.optflag[0]=1;
    app->PlaybackActive()=0;app->FrameStepPlayback()=0;
    app->AviRecordStartFrame()=0;app->AviRecordEndFrame()=600;app->AviRecordFps()=30;
    app->CurrentFrame()=600;app->state.recordedFrameCount=642;
    app->CameraParentModel()=-1;app->CameraParentBone()=-1;
    app->CameraPositionX()=0;RegisterCameraState(app,0);
    app->CameraPositionX()=60;RegisterCameraState(app,600);
    app->CameraPositionX()=0;app->CurrentFrame()=0;
    RefreshAfterFrameApply(app);
    app->CurrentFrame()=600;
    HWND recording=CreateWindowExW(0,L"STATIC",L"record-test",WS_POPUP,-32000,-32000,32,32,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(recording!=nullptr,"create hidden recording progress fixture");
    app->RecordingWindow()=recording;
    IDirect3DSurface9* backbuffer=nullptr;
    Check(SUCCEEDED(app->Renderer()->device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&backbuffer)),"get recording fixture backbuffer");
    D3DSURFACE_DESC description{};backbuffer->GetDesc(&description);backbuffer->Release();
    app->AviOutputWidth()=description.Width;app->AviOutputHeight()=description.Height;
    app->state.recordSavedFrame=600;
    app->state.recordPlaybackStartPending=1;
    app->PlaybackPhysicsMode()=0;
    float previous=-1;
    for(int i=0;i<5;++i) {
        app->DeltaTime()=1.f/30.f;
        FrameDriver(app);
        const auto title=WindowCaption(recording);
        std::printf("record sample %d: count=%d cursor=%g cameraX=%g\n",i,app->state.recordedFrameCount,app->PlaybackCursorSeconds(),app->CameraPositionX());
        Check(app->state.recordedFrameCount==i+1,"record counter starts at zero and advances once per sample");
        Check(std::fabs(app->PlaybackCursorSeconds()-float(i+1)/30)<1e-5f,"actual recording time advances from requested start");
        Check(app->CameraPositionX()>previous,"real camera keyframe sample changes on each recording frame");
        if(i==0)Check(title.find(L"第 0 帧")!=std::wstring::npos,"first recording title shows frame zero");
        previous=app->CameraPositionX();
    }
    app->RecordingWindow()=nullptr;app->PlaybackActive()=0;app->FrameStepPlayback()=0;
    DestroyWindow(recording);
    const auto count=app->state.recordedFrameCount;
    FrameDriver(app);
    Check(app->state.recordedFrameCount==count,"preview cannot advance recording counter");
    std::puts("PASS recording starts at frame zero and advances actual keyframe samples");
}

}
int wmain(int argc, wchar_t** argv) {
    using namespace mikudancestudio;
    if (argc < 2) return 2;
    const bool allstarMmv=argc>3&&std::wcscmp(argv[3],L"--allstar-mmv")==0;
    if(allstarMmv)SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool cameraStartup = argc > 3 && std::wcscmp(argv[3], L"--pmm-camera-startup") == 0;
    const bool cameraLoad = cameraStartup ||
        (argc > 3 && std::wcscmp(argv[3], L"--pmm-camera-load") == 0);
    HHOOK cameraLoadHook = cameraLoad ? SetWindowsHookExW(WH_CBT,
        GuardCameraLoadProbe, nullptr, GetCurrentThreadId()) : nullptr;
    if (cameraLoad) Check(cameraLoadHook != nullptr, "install hidden camera load guard");
    const bool menusOnly = std::wcscmp(argv[1], L"--menus-only") == 0;
    HHOOK menuHook = menusOnly ? SetWindowsHookExW(WH_CBT, RejectMenuProbeDialog,
        nullptr, GetCurrentThreadId()) : nullptr;
    if (menusOnly) {
        Check(menuHook != nullptr, "install menu integration dialog guard");
        std::puts("menu integration: initialize window and renderer");
    }
    wchar_t scenePath[256]{};
    wchar_t cameraOutput[256]{};
    if (cameraLoad && argc > 4) {
        const DWORD length = GetFullPathNameW(argv[4], 256, cameraOutput, nullptr);
        Check(length > 0 && length < 256, "resolve camera output before app initialization");
    }
    if (!menusOnly) {
        const DWORD length = GetFullPathNameW(argv[1], 256, scenePath, nullptr);
        Check(length > 0 && length < 256, "resolve scene path before app changes current directory");
    }
    if (argc > 4 && std::wcscmp(argv[3], L"--bone-multiply") == 0)
        multiplyOutput = std::filesystem::absolute(argv[4]);
    auto* app = new MMDApp();
    g_Block = app;
    app->state = MMDAppState{};
    app->InitDefaults();
    if (cameraStartup) wcscpy_s(app->EnvFileName(), 256, scenePath);
    if (!InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE)) return 3;
    ShowWindow(app->MainWindow(), SW_HIDE);
    if(argc>3&&std::wcscmp(argv[3],L"--curve-transport")==0){
        CheckCurveTransportFeedback(app,scenePath);
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }
    if (argc > 3 && std::wcscmp(argv[3], L"--accessory-feedback") == 0) {
        CheckAccessoryFeedback(app, scenePath, argc > 4 ? argv[4] : L"all");
        ShutdownCleanup(app); delete app; g_Block = nullptr; return 0;
    }
#if !MMD_ORIGINAL_PROFILE
    if (argc > 4 && std::wcscmp(argv[3], L"--sequence-scatter") == 0) {
        wcscpy_s(app->EnvFileName(), 256, scenePath); LoadSceneFile();
        CheckSequenceScatter(app, argv[4]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; return 0;
    }
#endif
    if (argc > 3 && std::wcscmp(argv[3], L"--repeat-delete") == 0) {
        wcscpy_s(app->EnvFileName(), 256, scenePath); LoadSceneFile();
        CheckRepeatedDelete(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; return 0;
    }
    if (argc > 5 && std::wcscmp(argv[3], L"--editor-feedback") == 0) {
        wcscpy_s(app->EnvFileName(), 256, scenePath); LoadSceneFile();
        if (std::wcscmp(argv[4], L"parent") == 0) CheckCameraBoneMotion(app);
        else CheckEditorFeedback(app, argv[4], argv[5]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; return 0;
    }
    if (argc > 3 && std::wcscmp(argv[3], L"--coordinate-reset") == 0) {
        CheckCoordinateReset(app, scenePath);
        ShutdownCleanup(app); delete app; g_Block = nullptr; return 0;
    }
    if (cameraLoad) {
#ifdef REQUIRE_MME
        Check(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"),
              "camera load test requires the real MME plugin");
        std::puts("MME loaded");
#endif
        CheckPmmCameraLoad(app, scenePath, cameraOutput, cameraStartup);
        ShutdownCleanup(app); delete app; g_Block = nullptr;
        UnhookWindowsHookEx(cameraLoadHook);
        return 0;
    }
    if(allstarMmv){
        Check(mmv4mms::Active(),"actual MMV4MMS plugin loaded");
        const auto started=std::chrono::steady_clock::now();
        wcscpy_s(app->EnvFileName(),256,scenePath);
        std::puts("load original AllStar PMM with MMV4MMS");
        LoadSceneFile();
        int models=0,empty=0;
        for(int slot=0;slot<kModelSlotCount;++slot)if(auto* model=app->ModelSlot(slot)){
            const auto& record=*mdl::Mdl(model);++models;
            if(record.vertexCount==0){
                ++empty;
                Check(record.boneCount>0&&record.boneTable,"bone-only model retains its bones");
                Check(!record.vertexBuffer,"bone-only model has no vertex buffer");
            }
        }
        std::printf("load_ms=%.3f models=%d bone_only=%d\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count(),models,empty);
        Check(models>1&&empty>0,"original scene retains regular and bone-only models");
        float world[16]{};world[0]=world[5]=world[10]=world[15]=1;
        for(int frame=0;frame<12;++frame){
            app->DeltaTime()=1.f/30;app->state.messageSeen=1;FrameDriver(app);
            // The plugin owns BeginScene/EndScene, as in RenderFrameScene.
            const int rendered=mmv4mms::Render(app,world);
            Check(rendered==1,"plugin completes frame including bone-only model");
            Check(HeapValidate(GetProcessHeap(),0,nullptr)!=FALSE,"scene heap remains valid");
        }
        Check(!IsWindowVisible(app->MainWindow()),"AllStar regression stays hidden");
        std::puts("PASS AllStar loads and renders 12 physical frames with the real MMV4MMS plugin");
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }
    if(argc>4&&std::wcscmp(argv[3],L"--pmm-template")==0){
        wcscpy_s(app->EnvFileName(),256,argv[4]);SaveSceneFile(app);
        Check(GetFileAttributesW(argv[4])!=INVALID_FILE_ATTRIBUTES,"save default recovery template");
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }
    if(argc>4&&std::wcscmp(argv[3],L"--pmm-atomic")==0){
        app->EnglishUI()=2;LocalizeUI(app);
        HANDLE locked=CreateFileW(argv[4],GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        Check(locked!=INVALID_HANDLE_VALUE,"create isolated locked save fixture");
        const char marker[]="original-project-marker";DWORD bytes=0;
        Check(WriteFile(locked,marker,sizeof(marker),&bytes,nullptr)!=FALSE,"write save fixture marker");
        HHOOK hook=SetWindowsHookExW(WH_CBT,PmmErrorAuditHook,nullptr,GetCurrentThreadId());
        wcscpy_s(app->EnvFileName(),256,argv[4]);SaveSceneFile(app);UnhookWindowsHookEx(hook);
        SetFilePointer(locked,0,nullptr,FILE_BEGIN);char read[sizeof(marker)]{};
        Check(ReadFile(locked,read,sizeof(read),&bytes,nullptr)!=FALSE&&std::memcmp(read,marker,sizeof(marker))==0,"failed atomic publication preserves existing file bytes");
        Check(pmmLoadWarnings==1&&app->SceneModified()!=0,"failed publication reports one error and keeps unsaved state");
        CloseHandle(locked);std::puts("PASS failed save preserves existing project");
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }
    if(argc>3&&std::wcscmp(argv[3],L"--pmm-errors")==0){
        app->EnglishUI()=argc>4?_wtoi(argv[4]):2;LocalizeUI(app);
        HHOOK hook=SetWindowsHookExW(WH_CBT,PmmErrorAuditHook,nullptr,GetCurrentThreadId());
        wcscpy_s(app->EnvFileName(),256,scenePath);LoadSceneFile();UnhookWindowsHookEx(hook);
        std::printf("PMM load warnings: %d\n",pmmLoadWarnings);
        for(int slot=0;slot<kModelSlotCount;++slot)if(auto* model=app->ModelSlot(slot)){
            const auto* record=mdl::Mdl(model);std::size_t keys=0;
            for(std::size_t i=record->boneCount;i<mdl::BoneKeyCapacity(*record);++i)if(record->boneKeys[i].frame!=0)++keys;
            std::printf("loaded model %d: bones=%u sparse keys=%zu\n",slot,record->boneCount,keys);
        }
        if(argc>5&&pmmLoadWarnings==0){wcscpy_s(app->EnvFileName(),256,argv[5]);SaveSceneFile(app);Check(app->SceneModified()==0,"save recovered project completes");}
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }

#ifdef REQUIRE_MME
    Check(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"),
          "MME test requires local MMEffect/MMHack and d3d9 proxy DLLs");
    std::puts("MME loaded");
#endif
    if (argc > 3 && (std::wcscmp(argv[3], L"--emm-save-off") == 0 ||
                     std::wcscmp(argv[3], L"--emm-save-on") == 0 ||
                     std::wcscmp(argv[3], L"--emm-default-on") == 0 ||
                     std::wcscmp(argv[3], L"--emm-default-off") == 0)) {
        const bool defaultOn = std::wcscmp(argv[3], L"--emm-default-on") == 0;
        const bool defaultOff = std::wcscmp(argv[3], L"--emm-default-off") == 0;
        CheckEffectAutoSave(app, scenePath, defaultOn || std::wcscmp(argv[3], L"--emm-save-on") == 0,
                            argc > 4 ? argv[4] : nullptr, defaultOn || defaultOff);
        ShutdownCleanup(app); delete app; g_Block = nullptr;
        return 0;
    }
    if (menusOnly) {
        CheckEffectMenus(app);
        UnhookWindowsHookEx(menuHook);
        ShutdownCleanup(app); delete app; g_Block = nullptr;
        return 0;
    }
    if (argc > 3 && std::wcscmp(argv[3], L"--startup-view") == 0) {
        CheckStartupView(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr;
        return 0;
    }
    if (argc > 3 && std::wcscmp(argv[3], L"--bone-multiply") == 0) {
        app->EnglishUI() = kUiChinese; LocalizeUI(app);
        LoadModelFile(app, scenePath, false);
        auto* model = mdl::Mdl(app->SelectedModel());
        Check(model && model->boneCount > 1, "load model for scale dialog integration");
        CheckBoneMultiply(app, model);
        if (!multiplyOutput.empty()) {
            const auto expected = bone_scale::Key(app->SelectedModel(), 0);
            const int later = static_cast<int>(model->boneKeys[0].next);
            const auto laterValue = bone_scale::Key(app->SelectedModel(), later);
            const auto curve = bone_scale::KeyCurve(app->SelectedModel(), later);
            const auto saved = multiplyOutput / L"bone-multiply-roundtrip.pmm";
            Check(SaveSceneCopy(app, saved.c_str()), "save adjusted scales through the real PMM writer");
            ResetAppState(app); wcscpy_s(app->EnvFileName(), 256, saved.c_str()); LoadSceneFile();
            model = mdl::Mdl(app->SelectedModel());
            Check(model != nullptr && bone_scale::Key(app->SelectedModel(), 0) == expected &&
                  bone_scale::Key(app->SelectedModel(), static_cast<int>(model->boneKeys[0].next)) == laterValue &&
                  bone_scale::KeyCurve(app->SelectedModel(), static_cast<int>(model->boneKeys[0].next)) == curve,
                  "real PMM save/reopen retains adjusted matrices and curves");
            std::puts("PASS adjusted bone scale PMM save/reopen");
        }
        ShutdownCleanup(app); delete app; g_Block = nullptr; return 0;
    }
    if(argc>3&&std::wcscmp(argv[3],L"--ui-language-audit")==0){
        app->EnglishUI()=kUiChinese;LocalizeUI(app);wcscpy_s(app->EnvFileName(),256,scenePath);LoadSceneFile();CheckChineseUi(app);
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }
    if(argc>3&&std::wcscmp(argv[3],L"--panel-controls")==0){ wcscpy_s(app->EnvFileName(),256,scenePath);LoadSceneFile();CheckPanelControls(app);ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;}
    if(argc>3&&std::wcscmp(argv[3],L"--modal-paint")==0){wcscpy_s(app->EnvFileName(),256,scenePath);LoadSceneFile();CheckModalPaint(app);ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;}
    if(argc>3&&std::wcscmp(argv[3],L"--record-frames")==0){wcscpy_s(app->EnvFileName(),256,scenePath);LoadSceneFile();CheckRecordingFrames(app);ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;}
    if(argc>3&&std::wcscmp(argv[3],L"--pmm-first-paint")==0){CheckPmmFirstPaint(app,scenePath);ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;}
    if(argc>3&&std::wcscmp(argv[3],L"--parent-coordinates")==0){wcscpy_s(app->EnvFileName(),256,scenePath);LoadSceneFile();CheckParentCoordinates(app);ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;}
    if(argc>3&&std::wcscmp(argv[3],L"--numeric-input")==0){wcscpy_s(app->EnvFileName(),256,scenePath);LoadSceneFile();CheckNumericInput(app);ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;}
    const int cycles = argc > 2 ? (std::max)(1, _wtoi(argv[2])) : 10;
    if(argc>3&&std::wcscmp(argv[3],L"--model-light")==0){
        LoadModelFile(app,scenePath,false);
        Check(app->SelectedModel()!=nullptr,"load model for lighting switch");
        app->CameraMode()=1;app->state.followCameraEnabled=0;app->CameraParentModel()=-1;
        const float direction[3]={.3f,-.7f,-.9f},color[3]={.2f,.4f,.8f};
        std::memcpy(app->LightDirection(),direction,sizeof direction);
        std::memcpy(app->LightColor(),color,sizeof color);app->ApplyTimelineLightState();
        RebuildModelModePanel(app);
        Check(std::memcmp(app->LightDirection(),direction,sizeof direction)==0,"model panel preserves edited light direction");
        Check(std::memcmp(&app->SceneLight().Ambient,color,sizeof color)==0,"model panel preserves edited brightness and colour");
        std::memcpy(app->LightKeys()[0].direction,direction,sizeof direction);
        std::memcpy(app->LightKeys()[0].color,color,sizeof color);
        const HWND follow=GetDlgItem(app->MainWindow(),panel::kFollowCameraCheckbox);
        for(bool menu:{false,true}){
            if(menu)SendMessageW(app->MainWindow(),WM_COMMAND,247,0);
            else{SendMessageW(follow,BM_SETCHECK,BST_CHECKED,0);SendMessageW(app->MainWindow(),WM_COMMAND,535,reinterpret_cast<LPARAM>(follow));}
            Check(app->state.followCameraEnabled!=0,"enable camera following through UI");
            SetupFrameWorldTransform(app);D3DLIGHT9 before{},after{};
            app->Renderer()->device->GetLight(0,&before);
            if(menu)SendMessageW(app->MainWindow(),WM_COMMAND,247,0);
            else{SendMessageW(follow,BM_SETCHECK,BST_UNCHECKED,0);SendMessageW(app->MainWindow(),WM_COMMAND,535,reinterpret_cast<LPARAM>(follow));}
            Check(app->state.followCameraEnabled==0,"disable camera following through UI");
            SetupFrameWorldTransform(app);app->Renderer()->device->GetLight(0,&after);
            Check(std::memcmp(&before.Direction,&after.Direction,sizeof before.Direction)==0,"disabling follow preserves device light direction");
            Check(std::memcmp(&before.Ambient,&after.Ambient,sizeof before.Ambient)==0,"disabling follow preserves device light colour");
        }
        Check(!IsWindowVisible(app->MainWindow()),"lighting switch test stays hidden");
        std::puts("PASS actual model panel, follow checkbox and follow menu preserve scene lighting");
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }
    if(argc>3&&std::wcscmp(argv[3],L"--camera-follow")==0){
        LoadModelFile(app,scenePath,false);CheckCameraBoneMotion(app);
        ShutdownCleanup(app);delete app;g_Block=nullptr;return 0;
    }
    skipMissingModels=argc>3&&std::wcscmp(argv[3],L"--skip-missing")==0;
    HHOOK loadHook=skipMissingModels?SetWindowsHookExW(WH_CBT,ConfirmTestDialog,nullptr,GetCurrentThreadId()):nullptr;
    int expectedModels = skipMissingModels&&argc>5?_wtoi(argv[4]):-1;
    int expectedAccessories = skipMissingModels&&argc>5?_wtoi(argv[5]):-1;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        app->EnglishUI() = cycle % 3;
        LocalizeUI(app);
        std::printf("cycle %d language %d: reset\n", cycle, int(app->EnglishUI()));
        ResetAppState(app);
        wcscpy_s(app->EnvFileName(), 256, scenePath);
        std::puts("load PMM");
        const auto started=std::chrono::steady_clock::now();
        const int skippedBefore=skippedModels;
        LoadSceneFile();
        std::printf("load_ms=%.3f skipped=%d\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count(),skippedModels-skippedBefore);
        int models = 0, accessories = 0;
        for (int slot = 0; slot < kModelSlotCount; ++slot)
            if (app->ModelSlot(slot)) ++models;
        for (int slot = 0; slot < 255; ++slot)
            if (app->AccessorySlot(slot)) ++accessories;
        std::printf("loaded %d models, %d accessories\n", models, accessories);
        if (models == 0 && accessories == 0) return 5;
        if (expectedModels < 0) { expectedModels = models; expectedAccessories = accessories; }
        if (models != expectedModels || accessories != expectedAccessories) return 6;
        std::puts("render 12 frames with physics");
        for (int frame = 0; frame < 12; ++frame) {
            app->DeltaTime() = 1.f / 30.f;
            app->state.messageSeen = 10;
            FrameDriver(app);
            if (!HeapValidate(GetProcessHeap(), 0, nullptr)) return 4;
        }
        if (argc > 3 && std::wcscmp(argv[3], L"--playback-view") == 0) {
            CheckFreeViewPlayback(app, false);
            CheckFreeViewPlayback(app, true);
        }
        else if (argc > 3 && std::wcscmp(argv[3], L"--view-tools") == 0)
            CheckViewTools(app);
        else if (argc > 3 && std::wcscmp(argv[3], L"--bone-movement") == 0)
            CheckNonzeroFrameBoneEditing(app);
        else if (argc > 3 && std::wcscmp(argv[3], L"--physics-selection") == 0)
            CheckPhysicsFrameSelection(app);
#ifdef REQUIRE_MME
        else if (argc > 3 && std::wcscmp(argv[3], L"--material-names") == 0)
            CheckEffectMaterialNames(app);
#endif
        else if (argc > 3&&!skipMissingModels) CheckEditorCommands(app);
        std::puts("cycle passed");
    }
    if(loadHook)UnhookWindowsHookEx(loadHook);
    ResetAppState(app);
    ShutdownCleanup(app); delete app; g_Block = nullptr;
    std::puts("PMM new/reopen and language-switch lifecycle passed");
}
