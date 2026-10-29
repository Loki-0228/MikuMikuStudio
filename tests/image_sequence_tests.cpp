// Real dialog, Direct3D readback, image encoders and recording lifecycle.
// Run in a disposable directory. --hidden-production allows GPU testing on
// the input desktop with visibility/activation blocked for all test windows.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/antialias_motion.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_layout.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "../src/window/image_sequence_dialog.hpp"
#include <MinHook.h>
#include <ShObjIdl.h>
#include <wrl/client.h>
#include <wincodec.h>
#include "mikumikustudio/d3dx_dyn.hpp"
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <memory>

extern "C" float ExpGetFrameTime();
namespace mikumikustudio {
void RegisterCameraState(MMDApp*, int);
void RegisterSelfShadowState(MMDApp*, int);
}

using namespace mikumikustudio;
namespace seq = mikumikustudio::image_sequence;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0, warnings = 0;
bool expectWarning = false;
bool dialogOnly = false;
fs::path output;
void Check(bool value, const char* reason) {
    ++checks;
    if (!value) { std::printf("FAIL %s\n", reason); std::exit(1); }
}
int WINAPI MessageW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    if (expectWarning) { ++warnings; return IDOK; }
    std::fwprintf(stderr, L"FAIL unexpected message: %ls\n", text); std::exit(2);
}
int WINAPI MessageA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("FAIL unexpected message: %s\n", text); std::exit(2);
}
decltype(&ShowWindow) originalShowWindow;
BOOL WINAPI Hidden(HWND window, int) { return originalShowWindow(window, SW_HIDE); }
LRESULT CALLBACK HideCreated(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) reinterpret_cast<CBT_CREATEWND*>(lp)->lpcs->style &= ~WS_VISIBLE;
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
std::wstring Read(HWND dialog, int id) {
    wchar_t text[512]{}; GetDlgItemTextW(dialog, id, text, 512); return text;
}
void Capture(HWND dialog, const fs::path& path) {
    if (dialogOnly) {
        originalShowWindow(dialog, SW_SHOWNOACTIVATE);
        UpdateWindow(dialog);
    }
    RECT rect{}; GetWindowRect(dialog, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC dc = GetWindowDC(dialog), memory = CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    auto old = SelectObject(memory, bitmap);
    SendMessageW(dialog, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + width * height * 4;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    stream.write(static_cast<char*>(pixels), width * height * 4);
    Check(bool(stream), "save dialog screenshot");
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(dialog, dc);
}
seq::DialogContext* dialogContext = nullptr;
bool cancelDialog = false;
INT_PTR CALLBACK Probe(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message != WM_INITDIALOG) return seq::DialogProc(dialog, message, wp, lp);
    seq::DialogProc(dialog, message, wp, lp);
    auto& c = *dialogContext;
    Check(Read(dialog, panel::kAviWidthEdit) == L"1920" && Read(dialog, panel::kAviHeightEdit) == L"1080", "dialog dimensions initialized once");
    Check(SendDlgItemMessageW(dialog, seq::FormatCombo, CB_GETCOUNT, 0, 0) == 7, "seven single-image formats");
    wchar_t selected[32]{};
    const auto selection = SendDlgItemMessageW(dialog, seq::FormatCombo, CB_GETCURSEL, 0, 0);
    SendDlgItemMessageW(dialog, seq::FormatCombo, CB_GETLBTEXT, selection, reinterpret_cast<LPARAM>(selected));
    Check(wcscmp(selected, L"PNG") == 0, "PNG is the displayed default format");
    Check(Read(dialog, seq::Example).find(L"frame_0001.png") != std::wstring::npos, "500-frame filename uses four digits");
    SetDlgItemInt(dialog, panel::kAviEndFrameEdit, 499, FALSE);
    Check(Read(dialog, seq::Example).find(L"frame_001.png") != std::wstring::npos, "preview shrinks below leading-five threshold");
    SetDlgItemInt(dialog, panel::kAviEndFrameEdit, 500, FALSE);
    SetDlgItemTextW(dialog, seq::Prefix, L"镜头A_");
    Check(Read(dialog, seq::Example).find(L"镜头A_0001.png") != std::wstring::npos, "Unicode prefix preview");
    Capture(dialog, output / (L"dialog-" + std::to_wstring(c.app->EnglishUI()) + L".bmp"));
    if (cancelDialog) {
        seq::DialogProc(dialog, WM_COMMAND, IDCANCEL, 0);
        return TRUE;
    }
    SetDlgItemInt(dialog, panel::kAviWidthEdit, 640, FALSE);
    Check(Read(dialog, panel::kAviHeightEdit) == L"360", "size editing preserves video aspect ratio");
    SetDlgItemTextW(dialog, panel::kAviFpsEdit, L"nan");
    expectWarning = true; seq::DialogProc(dialog, WM_COMMAND, IDOK, 0); expectWarning = false;
    Check(!seq::Active(c.app), "invalid FPS does not arm export");
    SetDlgItemTextW(dialog, panel::kAviFpsEdit, L"29.97");
    CheckDlgButton(dialog, panel::kAviCloseAfterExportCheckbox, BST_CHECKED);
    seq::DialogProc(dialog, WM_COMMAND, IDOK, 0);
    return TRUE;
}
void DialogTests(MMDApp* app) {
    for (int language = 0; language < 3; ++language) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        for (bool cancel : {false, true}) {
            seq::DialogContext c{app}; c.options.directory = output.wstring();
            c.options.width = 1920; c.options.height = 1080; c.options.end = 500;
            dialogContext = &c; cancelDialog = cancel;
            const auto result = UiDialogBoxParamA(GetModuleHandleW(nullptr), MAKEINTRESOURCEA(seq::kDialog),
                app->MainWindow(), Probe, reinterpret_cast<LPARAM>(&c));
            Check(result == (cancel ? IDCANCEL : IDOK), "real dialog completes through production callback");
            Check(seq::Active(app) == !cancel, "cancel does not prepare export");
            if (!cancel) Check(c.options.width == 640 && c.options.height == 360 &&
                c.options.prefix == L"镜头A_" && c.options.fps == 29.97f && c.options.closeAfterExport,
                "dialog commits all displayed settings");
            seq::Abort(app);
        }
        seq::RefreshMenu(app); seq::RefreshMenu(app);
        HMENU file = GetSubMenu(GetMenu(app->MainWindow()), 0);
        unsigned count = 0;
        for (int i = 0; i < GetMenuItemCount(file); ++i) count += GetMenuItemID(file, i) == seq::kMenuCommand;
        Check(count == 1, "menu survives language refresh without duplication");
        wchar_t menuText[128]{};
        GetMenuStringW(file, seq::kMenuCommand, menuText, 128, MF_BYCOMMAND);
        if (language == 2) Check(std::wstring(menuText) == L"导出图片序列(&I)…", "Chinese sequence export naming is consistent");
    }
    Check(warnings == 3, "all three languages reject invalid FPS");
}
void NamesAndValidation(MMDApp* app) {
    seq::Options o; o.directory = output.wstring(); o.width = 64; o.height = 48;
    const std::pair<int, const wchar_t*> cases[] = {
        {0,L"frame_1.png"}, {4,L"frame_1.png"}, {5,L"frame_01.png"},
        {49,L"frame_01.png"}, {50,L"frame_001.png"},
        {499,L"frame_001.png"}, {500,L"frame_0001.png"},
        {1100,L"frame_0001.png"}, {4999,L"frame_0001.png"},
        {5000,L"frame_00001.png"}, {10000,L"frame_00001.png"},
    };
    for (const auto& item : cases) {
        o.end = item.first;
        Check(fs::path(seq::FramePath(o, 1)).filename() == item.second, "leading-digit rounding boundary");
    }
    o.end = 499; o.fps = 240;
    Check(fs::path(seq::FramePath(o, 1)).filename() == L"frame_0001.png", "high FPS has enough numbering digits");
    std::wstring error;
    for (const auto* prefix : {L"../bad", L"bad\\name", L"bad:", L"bad*", L"trailing.", L"trailing "}) {
        o.prefix = prefix; Check(!seq::Validate(app, o, error) && !error.empty(), "invalid filename rejected");
    }
    o.prefix = L""; Check(seq::Validate(app, o, error), "empty prefix permitted");
    o.fps = 0; Check(!seq::Validate(app, o, error), "zero FPS rejected");
    o.fps = 30; o.start = 600; Check(!seq::Validate(app, o, error), "reversed range rejected");
}
void Start(MMDApp* app, const seq::Options& o) {
    std::wstring error;
    if (!seq::Prepare(app, o, error)) std::fwprintf(stderr, L"%ls\n", error.c_str());
    Check(seq::Active(app), "prepare real export");
    app->AviOutputWidth() = o.width; app->AviOutputHeight() = o.height;
    app->AviRecordStartFrame() = o.start; app->AviRecordEndFrame() = o.end;
    app->AviRecordFps() = o.fps; app->AviIncludeWave() = 0; app->AviStereoOutput() = 0;
    app->AviCloseAfterExport() = false;
    StartAviRecordWindow(app);
    Check(app->RecordingWindow() && app->state.recordPlaybackStartPending, "start through video recording entry");
}
void RunExport(MMDApp* app, seq::Options o, bool cancel = false) {
    const int oldW = app->RenderWidth(), oldH = app->RenderHeight();
    const int oldScreenW = app->Renderer()->screenWidth, oldScreenH = app->Renderer()->screenHeight;
    Start(app, o);
    unsigned steps = 0;
    while (seq::Active(app) && steps++ < 150) {
        app->state.messageSeen = 1; FrameDriver(app);
        if (cancel && steps == 5) FinishAviRecord(app);
    }
    Check(!seq::Active(app) && !app->RecordingWindow() && !app->FrameStepPlayback(), "export releases recording state");
    Check(app->RenderWidth() == oldW && app->RenderHeight() == oldH, "render resolution restored");
    Check(app->Renderer()->screenWidth == oldScreenW && app->Renderer()->screenHeight == oldScreenH, "device preview resolution restored");
    std::uint64_t files = 0;
    for (const auto& file : fs::directory_iterator(o.directory)) {
        if (file.path().filename().wstring().find(o.prefix) == 0 && file.path().extension() == seq::kFormats[o.format].extension) ++files;
    }
    Check(cancel ? files > 0 && files < seq::FrameCount(o) : files == seq::FrameCount(o), "inclusive range and output FPS produce correct image count");
    for (std::uint64_t number = 1; number <= files; ++number) Check(fs::file_size(seq::FramePath(o, number)) > 0, "complete consecutive images");
    std::printf("PASS format=%ls fps=%.3f range=%d..%d images=%llu cancel=%d\n",
        seq::kFormats[o.format].name, o.fps, o.start, o.end, static_cast<unsigned long long>(files), cancel);
}
unsigned folderPickers = 0, settingsDialogs = 0;
bool cancelFolder = false, cancelSettings = false;
std::wstring menuPrefix;
HRESULT STDMETHODCALLTYPE ChooseFolder(IFileOpenDialog* picker, HWND owner) {
    ++folderPickers;
    Check(owner == g_Block->MainWindow(), "menu folder picker belongs to the main window");
    FILEOPENDIALOGOPTIONS options{};
    Check(SUCCEEDED(picker->GetOptions(&options)) && (options & FOS_PICKFOLDERS), "menu requests a folder picker");
    return cancelFolder ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK;
}
HRESULT STDMETHODCALLTYPE ChosenFolder(IFileOpenDialog*, IShellItem** item) {
    return SHCreateItemFromParsingName(output.c_str(), nullptr, IID_PPV_ARGS(item));
}
INT_PTR CALLBACK MenuSettingsProbe(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message != WM_INITDIALOG) return seq::DialogProc(dialog, message, wp, lp);
    seq::DialogProc(dialog, message, wp, lp);
    Check(Read(dialog, panel::kAviEndFrameEdit) == std::to_wstring(seq::SceneEndFrame(g_Block)),
        "menu initializes the automatically detected scene range");
    if (cancelSettings) { seq::DialogProc(dialog, WM_COMMAND, IDCANCEL, 0); return TRUE; }
    SetDlgItemInt(dialog, panel::kAviWidthEdit, 64, FALSE);
    SetDlgItemInt(dialog, panel::kAviHeightEdit, 48, FALSE);
    SetDlgItemInt(dialog, panel::kAviStartFrameEdit, 0, FALSE);
    SetDlgItemInt(dialog, panel::kAviEndFrameEdit, 1, FALSE);
    SetDlgItemTextW(dialog, panel::kAviFpsEdit, L"30");
    SetDlgItemTextW(dialog, seq::Prefix, menuPrefix.c_str());
    CheckDlgButton(dialog, panel::kAviCloseAfterExportCheckbox, BST_UNCHECKED);
    seq::DialogProc(dialog, WM_COMMAND, IDOK, 0);
    return TRUE;
}
decltype(&UiDialogBoxParamA) originalMenuDialog = nullptr;
INT_PTR MenuDialog(HINSTANCE instance, LPCSTR name, HWND owner, DLGPROC proc, LPARAM parameter) {
    if (proc == seq::DialogProc) { ++settingsDialogs; proc = MenuSettingsProbe; }
    return originalMenuDialog(instance, name, owner, proc, parameter);
}
void MenuEntryTests(MMDApp* app) {
    wchar_t modelPath[32768]{};
    const auto length = GetEnvironmentVariableW(L"MMS_MENU_SEQUENCE_MODEL", modelPath, std::size(modelPath));
    if (length) {
        Check(length < std::size(modelPath) && fs::is_regular_file(modelPath), "menu export model fixture exists");
        LoadModelFile(app, modelPath, false);
        Check(app->SelectedModel() != nullptr, "load menu export model fixture");
    }
    // Exercise the real WM_COMMAND entry and shell setup; only replace the
    // user's folder selection and dialog input so the host stays hidden.
    Microsoft::WRL::ComPtr<IFileOpenDialog> picker;
    Check(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&picker))), "create shell picker for menu regression");
    auto** table = *reinterpret_cast<void***>(picker.Get());
    void* show = table[3]; void* result = table[20];
    Check(MH_CreateHook(show, &ChooseFolder, nullptr) == MH_OK &&
        MH_CreateHook(result, &ChosenFolder, nullptr) == MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&UiDialogBoxParamA), &MenuDialog,
            reinterpret_cast<void**>(&originalMenuDialog)) == MH_OK, "hook user input for menu export");
    MH_EnableHook(show); MH_EnableHook(result); MH_EnableHook(reinterpret_cast<void*>(&UiDialogBoxParamA));
    const auto camera = app->CameraMode();
    for (int mode : {0, 1}) {
        if (!mode && !app->SelectedModel()) { std::puts("SKIP model-mode menu export: set MMS_MENU_SEQUENCE_MODEL"); continue; }
        app->CameraMode() = mode;
        menuPrefix = L"menu_camera_" + std::to_wstring(mode) + L"_";
        folderPickers = settingsDialogs = 0;
        SendMessageW(app->MainWindow(), WM_COMMAND, seq::kMenuCommand, 0);
        std::printf("MENU camera=%d folder_pickers=%u settings=%u recording=%d\n",
            mode, folderPickers, settingsDialogs, seq::Active(app));
        Check(folderPickers == 1 && settingsDialogs == 1 && seq::Active(app) && app->RecordingWindow(),
            "image sequence menu starts export in model and camera/accessory modes");
        unsigned steps = 0;
        while (seq::Active(app) && steps++ < 150) { app->state.messageSeen = 1; FrameDriver(app); }
        Check(!seq::Active(app) && !app->RecordingWindow(), "menu export finishes recording");
        for (int frame : {1, 2})
            Check(fs::file_size(output / (menuPrefix + std::to_wstring(frame) + L".png")) > 0,
                "menu export writes consecutive PNG images");
        for (bool folder : {true, false}) {
            folderPickers = settingsDialogs = 0; cancelFolder = folder; cancelSettings = !folder;
            SendMessageW(app->MainWindow(), WM_COMMAND, seq::kMenuCommand, 0);
            Check(folderPickers == 1 && settingsDialogs == unsigned(!folder) &&
                !seq::Active(app) && !app->RecordingWindow(), "canceling either menu dialog leaves export inactive");
        }
        cancelFolder = cancelSettings = false;
        folderPickers = settingsDialogs = 0;
        const auto playback = app->PlaybackActive(); app->PlaybackActive() = 1;
        SendMessageW(app->MainWindow(), WM_COMMAND, seq::kMenuCommand, 0);
        app->PlaybackActive() = playback;
        Check(folderPickers == 0 && settingsDialogs == 0, "playback still blocks menu export");
    }
    app->CameraMode() = camera;
    for (void* hook : {show, result, reinterpret_cast<void*>(&UiDialogBoxParamA)}) {
        MH_DisableHook(hook); MH_RemoveHook(hook);
    }
    if (length) ResetAppState(app);
}
fs::path stillPath;
bool cancelStill = false;
unsigned stillPickers = 0;
BOOL WINAPI StillSaveDialog(LPOPENFILENAMEW parameters) {
    ++stillPickers;
    const wchar_t* expected = g_Block->EnglishUI() == 2 ? L"导出图片" :
        g_Block->EnglishUI() == 1 ? L"render to picture file" : L"画像ファイル出力";
    Check(std::wcscmp(parameters->lpstrTitle, expected) == 0, "still-image save dialog title follows the UI language");
    if (cancelStill) return FALSE;
    wcscpy_s(parameters->lpstrFile, parameters->nMaxFile, stillPath.c_str());
    return TRUE;
}
void StillImageLanguageTests(MMDApp* app) {
    Check(MH_CreateHook(reinterpret_cast<void*>(&GetSaveFileNameW), &StillSaveDialog, nullptr) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(&GetSaveFileNameW)) == MH_OK, "control still-image save selection");
    const int language = app->EnglishUI(), frame = app->CurrentFrame();
    const int width = app->RenderWidth(), height = app->RenderHeight();
    app->RenderWidth() = 64; app->RenderHeight() = 48;
    const wchar_t* formats[] = {L"画像出力中（%d フレーム）", L"Exporting image (frame %d)", L"正在导出图片（第 %d 帧）"};
    for (int mode : {0, 1, 2}) {
        app->EnglishUI() = mode;
        for (int current : {0, 137}) {
            app->CurrentFrame() = current; stillPickers = 0;
            stillPath = output / (L"单张_" + std::to_wstring(mode) + L"_" + std::to_wstring(current) + L".png");
            SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
            const HWND window = app->RecordingWindow();
            Check(stillPickers == 1 && window && IsWindowUnicode(window), "still-image menu creates a Unicode export window");
            wchar_t expected[96]{}, actual[128]{};
            swprintf_s(expected, formats[mode], current);
            GetWindowTextW(window, actual, std::size(actual));
            Check(std::wcscmp(actual, expected) == 0, "still-image window preserves localized title and current frame");
            // WM_SETTEXT/WM_GETTEXT must also stay Unicode after creation,
            // including characters outside the system ANSI code page.
            const wchar_t* unicode = L"图片・画像・image \U0001F3AC";
            SetWindowTextW(window, unicode); GetWindowTextW(window, actual, std::size(actual));
            Check(std::wcscmp(actual, unicode) == 0, "export window caption updates preserve Unicode");
            SetWindowTextW(window, expected);
            for (unsigned step = 0; app->RecordingWindow() && step < 12; ++step) {
                app->state.messageSeen = 1; FrameDriver(app);
            }
            Check(!app->RecordingWindow() && fs::is_regular_file(stillPath) && fs::file_size(stillPath) > 0,
                "localized still-image export completes and writes a PNG");
            std::printf("PASS still-image language=%d frame=%d Unicode caption and PNG output\n", mode, current);
        }
        cancelStill = true; stillPickers = 0;
        SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
        Check(stillPickers == 1 && !app->RecordingWindow(), "canceling still-image selection creates no export window");
        cancelStill = false;
    }
    app->EnglishUI() = language; app->CurrentFrame() = frame;
    app->RenderWidth() = width; app->RenderHeight() = height;
    MH_DisableHook(reinterpret_cast<void*>(&GetSaveFileNameW)); MH_RemoveHook(reinterpret_cast<void*>(&GetSaveFileNameW));
}
std::vector<unsigned char> ImagePixels(const fs::path& path,UINT* width=nullptr,UINT* height=nullptr) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),"create independent image decoder");
    Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)) &&
        SUCCEEDED(decoder->GetFrame(0,&frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)),"decode exported image");
    Check(SUCCEEDED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),"decode straight-alpha BGRA pixels");
    UINT w=0,h=0;Check(SUCCEEDED(converter->GetSize(&w,&h)),"read encoded dimensions");
    std::vector<unsigned char> pixels(std::size_t(w)*h*4);
    Check(SUCCEEDED(converter->CopyPixels(nullptr,w*4,static_cast<UINT>(pixels.size()),pixels.data())),"read encoded pixels");
    if(width)*width=w;if(height)*height=h;return pixels;
}
void TransparentImages(MMDApp* app) {
    Check(seq::FormatFromPath(L"C:/directory.png/picture.JPG")==1 && seq::FormatFromPath(L"picture.JPEG")==1 &&
        seq::FormatFromPath(L"picture.PNG")==2 && seq::FormatFromPath(L"picture.png.bad")==-1,"image format uses only the final extension, including uppercase and JPEG");
    for(int language : {0,1,2}) {
        UINT width=0,height=0;
        const auto pixels=ImagePixels(output/(L"单张_"+std::to_wstring(language)+L"_0.png"),&width,&height);
        Check(width==64 && height==48,"single-image export retains the requested dimensions");
        unsigned clear=0;
        for(std::size_t i=0;i<pixels.size();i+=4)if(pixels[i+3]==0) {
            ++clear;Check(pixels[i]==0 && pixels[i+1]==0 && pixels[i+2]==0,"transparent pixels contain no hidden background colour");
        }
        Check(clear>64,"single-image PNG exports contain a transparent background");
    }
    app->GroundGridEnabled()=false;app->GroundShadowEnabled()=false;
    seq::Options o;o.directory=output.wstring();o.width=64;o.height=48;o.end=0;
    for(auto algorithm : {antialias::Algorithm::MmdOriginal,antialias::Algorithm::Fxaa,antialias::Algorithm::Taa,antialias::Algorithm::Smaa}) {
        std::wstring error;Check(antialias::Select(app,algorithm,error),"select AA for alpha export");
        o.prefix=L"alpha-aa-"+std::to_wstring(unsigned(algorithm))+L"_";
        RunExport(app,o);
        const auto pixels=ImagePixels(seq::FramePath(o,1));
        for(std::size_t i=3;i<pixels.size();i+=4)Check(pixels[i]==0,"post-process AA preserves empty-background alpha");
    }
    std::wstring error;Check(antialias::Select(app,antialias::Algorithm::MmdOriginal,error),"restore native AA");
    o.format=0;o.prefix=L"opaque-bmp_";RunExport(app,o);
    const auto opaque=ImagePixels(seq::FramePath(o,1));
    for(std::size_t i=3;i<opaque.size();i+=4)Check(opaque[i]==255,"BMP automatically retains an opaque scene background");
    ResetAppState(app);
    // Exercise half coverage independently of the scene/shaders and decode the
    // actual PNG to catch premultiplied colour being written as straight alpha.
    Microsoft::WRL::ComPtr<IDirect3DSurface9> surface;
    auto* device=app->Renderer()->device;
    Check(SUCCEEDED(device->CreateRenderTarget(2,2,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&surface,nullptr)),"create semi-transparent image fixture");
    Check(SUCCEEDED(device->ColorFill(surface.Get(),nullptr,0x80402010)),"fill premultiplied sample");
    const auto path=output/L"half-alpha.png";
    Check(SUCCEEDED(seq::SaveSurface(path.c_str(),2,surface.Get(),true)),"write half-alpha PNG");
    const auto half=ImagePixels(path);
    Check(half[0]==32 && half[1]==64 && half[2]==128 && half[3]==128,"PNG stores straight alpha without dark fringes");
}
#include "antialias_image_sequence_checks.hpp"
#include "image_sequence_continuity_checks.hpp"
#include "image_visibility_checks.hpp"
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Check(argc == 2 || argc == 3, "isolated artifact directory required"); output = fs::absolute(argv[1]);
    fs::create_directories(output);
    wchar_t desktop[256]{}; DWORD bytes = 0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, &bytes) &&
        (_wcsicmp(desktop, L"Default") != 0 || (argc == 3 &&
            (wcscmp(argv[2], L"--hidden-production") == 0 || wcscmp(argv[2],L"--still-only")==0))),
        "private desktop or explicit hidden GPU test required");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Check(MH_Initialize() == MH_OK, "initialize message guards");
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW), reinterpret_cast<void*>(&MessageW), nullptr);
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxA), reinterpret_cast<void*>(&MessageA), nullptr);
    Check(MH_CreateHook(reinterpret_cast<void*>(&ShowWindow), reinterpret_cast<void*>(&Hidden), reinterpret_cast<void**>(&originalShowWindow)) == MH_OK, "install visibility guard");
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable visibility guard");
    auto hide = SetWindowsHookExW(WH_CBT, HideCreated, nullptr, GetCurrentThreadId());
    Check(hide != nullptr, "install activation guard");
    auto* app = new MMDApp; g_Block = app; app->state = MMDAppState{}; app->InitDefaults(); app->EnglishUI() = 2;
    dialogOnly = argc == 3 && wcscmp(argv[2], L"--dialogs-only") == 0;
    if (dialogOnly) {
        app->MainWindow() = CreateWindowW(L"STATIC", L"Image sequence dialog fixture", WS_POPUP,
            0, 0, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(app->MainWindow() != nullptr, "create hidden dialog owner");
        SetMenu(app->MainWindow(), LoadMenuW(GetModuleHandleW(nullptr), L"SAMPLE02E"));
        DialogTests(app);
        DestroyWindow(app->MainWindow()); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
        std::printf("PASS image-sequence visible private-dialog checks=%u\n", checks);
        return 0;
    }
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize production renderer");
    Check(!IsWindowVisible(app->MainWindow()), "production host remains hidden");
    if (const auto* model = _wgetenv(L"MMS_IMAGE_VISIBILITY_MODEL")) {
        LoadModelFile(app, model, false);
        CheckImageVisibility(app, output);
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide); return 0;
    }
    if(argc==3 && wcscmp(argv[2],L"--still-only")==0) {
        StillImageLanguageTests(app);
        const auto pixels=ImagePixels(output/L"单张_0_0.png");
        unsigned clear=0;for(std::size_t i=3;i<pixels.size();i+=4)clear+=pixels[i]==0;
        Check(clear>10,"single-image PNG background becomes transparent automatically");
        ShutdownCleanup(app);g_Block=nullptr;delete app;UnhookWindowsHookEx(hide);
        std::printf("PASS automatic still-image checks=%u\n",checks);return 0;
    }
    NamesAndValidation(app); SceneRangeTests(app); DialogTests(app); app->EnglishUI() = 2;
    MenuEntryTests(app);
    CameraAndContinuityTests(app);
    StillImageLanguageTests(app);
    TransparentImages(app);
    seq::Options o; o.directory = output.wstring(); o.width = 64; o.height = 48; o.end = 1;
    for (int format = 0; format < std::size(seq::kFormats); ++format) {
        o.format = format; o.prefix = L"中文_" + std::to_wstring(format) + L"_";
        RunExport(app, o);
        std::wstring error;
        const auto originalSize = fs::file_size(seq::FramePath(o, 1));
        Check(!seq::Prepare(app, o, error) && !error.empty() && !seq::Active(app), "existing image blocks export");
        Check(fs::file_size(seq::FramePath(o, 1)) == originalSize, "existing image unchanged");
    }
    o.format = 2; o.start = 10; o.end = 39;
    for (float fps : {24.f, 30.f, 60.f, 29.97f}) {
        o.fps = fps; o.prefix = L"fps_" + std::to_wstring(fps) + L"_"; RunExport(app, o);
    }
    o.fps = 30; o.prefix = L"cancel_"; RunExport(app, o, true);
    o.prefix = L"collision_"; std::wstring error;
    Check(seq::Prepare(app, o, error), "prepare before late collision");
    { std::ofstream sentinel(fs::path(seq::FramePath(o, 1))); sentinel << "keep"; }
    Check(!seq::WriteFrame(app, app->Renderer()->backbufferSurface, error) && !error.empty(), "late collision stops output");
    Check(fs::file_size(seq::FramePath(o, 1)) == 4, "late collision cannot overwrite image");
    seq::Abort(app);
    o.prefix = L"incomplete_"; Check(seq::Prepare(app, o, error), "prepare incomplete sequence");
    Check(!seq::Finish(app, true, error) && !error.empty() && !seq::Active(app), "incomplete sequence cannot report success");
    CheckTemporalImageSequence(app);
    for (const auto& entry : fs::directory_iterator(output)) Check(entry.path().extension() != L".tmp", "no temporary frames remain");
    ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
    std::printf("PASS image-sequence checks=%u\n", checks);
}
