#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <cstring>
#include <cwchar>
#include <string>
#include "mikumikustudio/text_encoding.hpp"

namespace mikumikustudio {
namespace {
struct Translation { const char* en; const wchar_t* zh; };
constexpr Translation kLabels[] = {
    {"Load physics preset", L"加载物理预设"}, {"Save physics preset", L"保存物理预设"},
    {"load AVI file", L"加载视频"}, {"load picture data", L"加载背景图片"},
    {"Open background video", L"加载背景视频"}, {"Open audio", L"加载音频"},
    {"load pose data", L"加载姿势"}, {"save pose data", L"保存姿势"},
    {"load vsq data", L"加载 VSQ 数据"}, {"open WAVE File", L"加载音频"},
    {"open file data", L"打开文件"}, {"output video file", L"导出视频"},
    {"render to picture file", L"导出图片"}, {"save enhanced model", L"保存扩展模型"},
    {"Bone Name", L"骨骼名称"},
    {"Cancel to load this project", L"取消加载工程"},
    {"Close MMS after successful export", L"导出成功后关闭 MMS"},
    {"Division Name", L"显示分组名称"},
    {"Facial Name", L"表情名称"},
    {"Fit pmm model data to this model and contine", L"适配模型数据并继续"},
    {"If name is writen by Japanese Font,Rename it to English Font.", L"编辑英文名称（使用半角英数字）"},
    {"Model Info", L"模型信息"},
    {"Model Name", L"模型名称"},
    {"OP register", L"登记外亲"},
    {"Pixel", L"像素"},
    {"Skip this model and continue", L"跳过此模型并继续"},
    {"Specify the place of another model file", L"选择其他模型文件"},
    {"Specify the place of model file and load", L"选择模型文件并加载"},
    {"Spring constant", L"弹簧系数"},
    {"accessory", L"配件"},
    {"accessory described after models", L"在模型之后绘制配件"},
    {"apply center position bias", L"应用中心位置偏移"},
    {"at place", L"位置"},
    {"axis X", L"X 轴"},
    {"axis Y", L"Y 轴"},
    {"axis Z", L"Z 轴"},
    {"calculate order", L"计算顺序"},
    {"center position", L"中心位置"},
    {"degree", L"角度"},
    {"degree *", L"角度倍率"},
    {"dist *", L"距离倍率"},
    {"down", L"下移"},
    {"drawing order", L"绘制顺序"},
    {"frame from", L"起始帧"},
    {"frame number to shift", L"偏移帧数"},
    {"frame register", L"登记关键帧"},
    {"frame to", L"结束帧"},
    {"model calculate order", L"模型计算顺序"},
    {"model disply order", L"模型绘制顺序"},
    {"multiply of camera frame position-angle", L"相机帧位置与角度调整"},
    {"multiply of facial expression", L"表情权重调整"},
    {"name(press 'Enter')", L"名称（按 Enter 确认）"},
    {"numeric input(bone)", L"骨骼数值输入"},
    {"numeric input(camera)", L"相机数值输入"},
    {"outside parent setting", L"外部亲骨骼设置"},
    {"outside parent(OP) setting", L"外部亲骨骼设置"},
    {"over", L"叠加"},
    {"place X", L"位置 X"},
    {"pmm ver.2.0 load", L"加载 PMM 工程"},
    {"pos X", L"位置 X"},
    {"pos Y", L"位置 Y"},
    {"pos Z", L"位置 Z"},
    {"position", L"位置"},
    {"register blinking", L"登记眨眼"},
    {"rot    X", L"旋转 X"},
    {"screen size", L"画面尺寸"},
    {"subject bone", L"当前骨骼"},
    {"subject bone relative place(auto calculated)", L"骨骼相对位置（自动计算）"},
    {"target bone", L"目标骨骼"},
    {"target model", L"目标模型"},
    {"th", L"号"},
    {"thickness", L"粗细"},
    {"thickness of edge line", L"描边粗细"},
    {"up", L"上移"},
    {"v-angle *", L"视角倍率"},
    {"width", L"宽度"},
    {"open file", L"打开文件"},
    {"save file", L"保存文件"},
    {"save motion data", L"保存动作"},
    {"load motion data", L"加载动作"},
    {"load model", L"加载模型"},
    {"save", L"保存"},
    {"error", L"错误"},
    {"Cannot open save file", L"无法打开保存文件"},
    {"Cannot open the model file", L"无法打开模型文件"},
    {"Cannot open file", L"无法打开文件"},
    {"fine shadow mode", L"精细阴影模式"},
    {"This command is changed for \"Fshadow\" button.", L"请使用精细阴影按钮进行此操作。"},
    {"expand/shrink", L"帧范围缩放"}, {"frame", L"帧范围"},
    {"magnification", L"倍率"}, {"Bone", L"骨骼"},
    {"Facial", L"表情"}, {"Disp/IK/OP", L"显示/IK/外亲"},
    {"bone/frame manipulation", L"骨骼 / 帧操作"},
    {"frame manipulation", L"帧操作"},
    {"Interpolation curve", L"插值曲线"}, {"auto", L"自动"},
    {"model manipulation", L"模型操作"}, {"disp", L"显示"},
    {"ON", L"开"}, {"OFF", L"关"},
    {"camera", L"相机"}, {"camer", L"相机"},
    {"view angle ", L"视角"}, {"view angle", L"视角"},
    {"follow bone", L"跟随骨骼"}, {"bone manipulation", L"骨骼操作"},
    {"light manipulation", L"照明操作"}, {"facial manipulation", L"表情操作"},
    {"eyes", L"眼睛"}, {"brow", L"眉毛"}, {"mouth", L"嘴部"}, {"other", L"其他"},
    {"self_shadow manipulation", L"自阴影操作"},
    {"shadow precision", L"阴影精度"}, {"shadow range", L"阴影范围"}, {"detail", L"精细"}, {"far", L"远处"},
    {"accessory manipulation", L"配件操作"}, {"display", L"显示"}, {"shadow", L"阴影"},
    {"view", L"视图"}, {"model", L"模型"}, {"bone", L"骨骼"},
    {"play", L"播放"}, {"stop", L"停止"}, {"repeat", L"循环"},
    {"from flame", L"开始"}, {"stop flame", L"结束"}, {"vol", L"音量"},
    {"camera light accessary", L"相机 / 照明 / 配件"},
    {"camera/light/accessory", L"相机/照明/配件"},
    {"Playing", L"播放中"}, {"Recording(Esc:Stop)", L"录制中(Esc:停止)"},
    {"angle", L"角度"}, {"bone place", L"骨骼位置"},
    {"scale", L"缩放"},
    {"camera bone trace mode", L"相机追踪骨骼"},
    {"camera bone trace mode (release trace button)", L"相机追踪骨骼（点击追踪按钮取消）"},
    {"camera model trace mode", L"相机追踪模型"},
    {"light", L"照明"}, {"s shadow", L"自阴影"}, {"gravity", L"重力"},
    {"To model", L"到模型"}, {"To camera", L"到相机"}, {"btm", L"下面"},
    {"distance", L"距离"}, {"all", L"全部"}, {"rotation", L"旋转"},
    {"x axis move", L"X轴移动"}, {"y axis move", L"Y轴移动"}, {"z axis move", L"Z轴移动"},
    {"ground", L"地面"}, {"non", L"无"}, {"Go", L"跳转"},
    {"global", L"全局"}, {"local", L"局部"},
    // Coordinate-system captions.  The HUD atlas keeps them in the capitalized
    // English spelling the selector displays (viewport operation 21 cycles the
    // three states), so the Chinese wording needs its own entries for those
    // keys; the lowercase entries stay for the other controls that use them.
    {"Global", L"全局"}, {"Local", L"局部"}, {"View", L"视图"},
    {"Accessory", L"配件"},
    {"frame (", L"帧 ("}, {"1fps or less", L"低于1帧/秒"},
    {"ground shadow color", L"地面阴影亮度"}, {"brightness", L"亮度"},
    {"multiply of bone position-angle", L"骨骼帧位置与角度调整"},
    {"toon texture", L"Toon 纹理"}, {"AVI-out", L"视频导出"},
    {"AVI size", L"视频尺寸"}, {"frame rate", L"帧率"},
    {"fps", L"帧/秒"}, {"recording frame", L"导出帧范围"},
    {"output WAVE", L"同时导出音频"}, {"WAVE", L"音频"},
    {"3D-AVI", L"立体视频"}, {"NVIDIA 3D Vision", L"NVIDIA 3D Vision"},
    {"left half(for Discover)", L"仅输出左半画面（Discover）"},
    {"video compressor", L"视频编码器"}, {"video encoder", L"视频编码器"},
    {"setting", L"设置"},
    {"tex01", L"纹理 01"}, {"tex02", L"纹理 02"},
    {"tex03", L"纹理 03"}, {"tex04", L"纹理 04"},
    {"tex05", L"纹理 05"}, {"tex06", L"纹理 06"},
    {"tex07", L"纹理 07"}, {"tex08", L"纹理 08"},
    {"tex09", L"纹理 09"}, {"tex10", L"纹理 10"},
    {"physics model", L"物理模型编辑"},
    {"Rigid Bodies", L"刚体"}, {"Constraints", L"关节"},
    {"Shape", L"形状"}, {"Transform", L"位置与旋转"},
    {"Data", L"参数"}, {"add", L"添加"}, {"delete", L"删除"},
    {"copy", L"复制"}, {"paste", L"粘贴"}, {"name", L"名称"},
    {"attach to", L"关联骨骼"}, {"sphere", L"球体"},
    {"box", L"方盒"}, {"capsule", L"胶囊"},
    {"radius", L"半径"}, {"height", L"高度"}, {"depth", L"深度"},
    {"pos x", L"位置 X"}, {"rot  x", L"旋转 X"},
    {"static(to bone)", L"跟随骨骼"}, {"dynamic", L"物理模拟"},
    {"bone matching", L"与骨骼位置对齐"}, {"mass", L"质量"},
    {"group", L"碰撞组"}, {"pos damping", L"移动阻尼"},
    {"rot damping", L"旋转阻尼"}, {"restitution", L"弹力"},
    {"friction", L"摩擦力"}, {"body A", L"刚体 A"},
    {"body B", L"刚体 B"}, {"limitation", L"限制范围"},
    {"move x", L"移动 X"}, {"angle x", L"旋转 X"},
    {"cancel", L"取消"},
    {"gravity setting", L"重力设置"}, {"acceleration", L"加速度"},
    {"dir x", L"方向X"}, {"noize", L"风力"},
    {"register", L"登记"}, {"close", L"关闭"},
    {"change physics ON/OFF frame", L"切换物理开关帧"},
    {"Change all selected frame point(under physics)", L"将选中的物理骨骼关键帧"},
    {"to physics", L"物理设为"}, {"ON (X mark)", L"开启（X标记）"},
    {"OK", L"确定"}, {"cancel", L"取消"},
};
}

const wchar_t* ChineseUiText(const char* english) {
    if (!english) return nullptr;
    std::string key(english);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    for (const auto& entry : kLabels) {
        std::string candidate(entry.en);
        while (!candidate.empty() && (candidate.back() == ' ' || candidate.back() == '\t')) candidate.pop_back();
        if (_stricmp(key.c_str(), candidate.c_str()) == 0) return entry.zh;
    }
    return nullptr;
}

namespace {
void TranslateFixedWindowText(HWND window) {
    wchar_t wide[1024]{};
    GetWindowTextW(window, wide, 1024);
    std::string ascii;
    for (const wchar_t* ch = wide; *ch; ++ch) {
        if (*ch > 127) return; // Model names and already translated text remain intact.
        ascii.push_back(static_cast<char>(*ch));
    }
    if (const auto* translated = ChineseUiText(ascii.c_str())) SetWindowTextW(window, translated);
}
struct DialogInit { DLGPROC procedure; LPARAM parameter; };
constexpr wchar_t kDialogProcedure[] = L"MMS.OriginalLocalizedDialogProc";
INT_PTR CALLBACK LocalizedDialogProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    DLGPROC original = reinterpret_cast<DLGPROC>(GetPropW(dialog, kDialogProcedure));
    if (message == WM_INITDIALOG) {
        const auto* init = reinterpret_cast<const DialogInit*>(lp);
        original = init->procedure;
        lp = init->parameter;
        SetPropW(dialog, kDialogProcedure, reinterpret_cast<HANDLE>(original));
    }
    const INT_PTR result = original ? original(dialog, message, wp, lp) : FALSE;
    if (IsWindow(dialog) && (message == WM_INITDIALOG || (message == WM_SHOWWINDOW && wp)))
        LocalizeChineseDialog(g_Block, dialog);
    if (message == WM_NCDESTROY) RemovePropW(dialog, kDialogProcedure);
    return result;
}
}

void LocalizeChineseDialog(MMDApp* app, HWND dialog) {
    if (!app || app->EnglishUI() != kUiChinese || !IsWindow(dialog)) return;
    TranslateFixedWindowText(dialog);
    EnumChildWindows(dialog, [](HWND child, LPARAM context) -> BOOL {
        (void)context;
        wchar_t className[32]{};
        GetClassNameW(child, className, 32);
        if (_wcsicmp(className, L"Static") == 0 || _wcsicmp(className, L"Button") == 0)
            TranslateFixedWindowText(child);
        return TRUE;
    }, reinterpret_cast<LPARAM>(app));
}

HWND CreateUiDialogParamA(HINSTANCE instance, LPCSTR name, HWND owner, DLGPROC proc, LPARAM parameter) {
    DialogInit init{proc, parameter};
    return CreateDialogParamA(instance, name, owner, LocalizedDialogProc, reinterpret_cast<LPARAM>(&init));
}
HWND CreateUiDialogParamW(HINSTANCE instance, LPCWSTR name, HWND owner, DLGPROC proc, LPARAM parameter) {
    DialogInit init{proc, parameter};
    return CreateDialogParamW(instance, name, owner, LocalizedDialogProc, reinterpret_cast<LPARAM>(&init));
}
INT_PTR UiDialogBoxParamA(HINSTANCE instance, LPCSTR name, HWND owner, DLGPROC proc, LPARAM parameter) {
    DialogInit init{proc, parameter};
    return DialogBoxParamA(instance, name, owner, LocalizedDialogProc, reinterpret_cast<LPARAM>(&init));
}
INT_PTR UiDialogBoxParamW(HINSTANCE instance, LPCWSTR name, HWND owner, DLGPROC proc, LPARAM parameter) {
    DialogInit init{proc, parameter};
    return DialogBoxParamW(instance, name, owner, LocalizedDialogProc, reinterpret_cast<LPARAM>(&init));
}

int LegacyUiMessageBoxA(HWND owner, LPCSTR body, LPCSTR caption, UINT flags) {
    const auto decode = [](const char* text) {
        std::wstring result;
        if (!text) return result;
        if (g_Block && g_Block->EnglishUI() == kUiChinese)
            if (const auto* translated = ChineseUiText(text)) return std::wstring(translated);
        // ASCII and UTF-8 diagnostics are accepted first; original Japanese
        // literals are Shift-JIS, independently of the Windows ANSI code page.
        if (!text_encoding::Decode(text, CP_UTF8, result) &&
            !text_encoding::Decode(text, 932, result)) text_encoding::Decode(text, CP_ACP, result);
        return result;
    };
    const auto wideBody = decode(body), wideCaption = decode(caption);
    return MessageBoxW(owner, wideBody.c_str(), wideCaption.c_str(), flags);
}

const wchar_t* LocalizedUiTextW(const wchar_t* text) {
    if (!text || !g_Block || g_Block->EnglishUI() != kUiChinese) return text;
    std::string ascii;
    for (const wchar_t* ch = text; *ch; ++ch) {
        if (*ch > 127) return text;
        ascii.push_back(static_cast<char>(*ch));
    }
    const auto* translated = ChineseUiText(ascii.c_str());
    return translated ? translated : text;
}
BOOL GetUiOpenFileNameW(LPOPENFILENAMEW parameters) {
    const auto* title = parameters->lpstrTitle;
    parameters->lpstrTitle = LocalizedUiTextW(title);
    const BOOL result = GetOpenFileNameW(parameters);
    parameters->lpstrTitle = title;
    return result;
}
BOOL GetUiSaveFileNameW(LPOPENFILENAMEW parameters) {
    const auto* title = parameters->lpstrTitle;
    parameters->lpstrTitle = LocalizedUiTextW(title);
    const BOOL result = GetSaveFileNameW(parameters);
    parameters->lpstrTitle = title;
    return result;
}

const wchar_t* UiCaptionFaceName(unsigned char englishUI) {
    // The caption is the sheet's only serif run, but only the two Latin
    // wordings can share that face: the English and Japanese UIs both spell
    // the caption "Local" / "Global" / "View" / "Accessory", so both take the
    // same Times New Roman and paint identical captions. Giving the Japanese
    // UI MS PMincho instead made the same word change shape with the menu
    // language, and Mincho has no bold face, so GDI synthesised one.
    //
    // The Chinese UI shows the translated Han wording, and the serif Han faces
    // are the wrong choice here: SimSun's hairline strokes (with a synthesised
    // bold on top) read worse than the label face at the caption's 30-pixel
    // size, so the Chinese caption stays on Microsoft YaHei UI, the same face
    // the rest of the Chinese HUD uses.
    if (englishUI == kUiChinese)
        return L"Microsoft YaHei UI";
    return L"Times New Roman";
}

int DrawWideUiGlyph(const wchar_t* text, HDC hdc, int size, int x, int y,
                    unsigned char r, unsigned char g, unsigned char b, int bold,
                    const wchar_t* face) {
    LOGFONTW lf{};
    // Keep the same logical size as the English renderer.  Position tuning is
    // handled at the call boundary so Chinese glyphs do not become smaller.
    lf.lfHeight = -size;
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face != nullptr ? face : L"Microsoft YaHei UI");
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ previous = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(r, g, b));
    const int length = static_cast<int>(std::wcslen(text));
    TextOutW(hdc, x, y, text, length);
    SIZE extent{};
    GetTextExtentPoint32W(hdc, text, length, &extent);
    SelectObject(hdc, previous);
    DeleteObject(font);
    return extent.cx;
}

int DrawUiGlyph(MMDApp* app, const char* text, HDC hdc, int size, int x, int y,
                unsigned char r, unsigned char g, unsigned char b, int bold,
                UiGlyphFace face) {
    const bool caption = face == UiGlyphFace::Caption;
    const wchar_t* captionFace = caption ? UiCaptionFaceName(app->EnglishUI())
                                         : L"Microsoft YaHei UI";
    if (app->EnglishUI() == kUiChinese)
        if (const auto* zh = ChineseUiText(text)) {
            // The translated glyphs sit too low in the original text rows.
            // Preserve size and horizontal anchor. The FPS suffix has a
            // taller three-glyph run than the Latin source and needs a little
            // more lift to align visually with its numeric value.
            const int verticalLift = std::strcmp(text, "fps") == 0 ? 5 : 3;
            return DrawWideUiGlyph(zh, hdc, size, x, y - verticalLift, r, g,
                                   b, bold,
                                   captionFace);
        }
    if (caption) {
        // Caption keys are ASCII ("Local", "Global", "View", "Accessory"), but
        // every language's face is a Unicode face here, so widen the key and
        // paint it through the same renderer as the Chinese wording.
        wchar_t wide[64]{};
        int length = 0;
        for (; text[length] != '\0' && length < 63; ++length)
            wide[length] = static_cast<wchar_t>(
                static_cast<unsigned char>(text[length]));
        return DrawWideUiGlyph(wide, hdc, size, x, y, r, g, b, bold,
                               captionFace);
    }
    return DrawGlyph(app, text, hdc, size, x, y, r, g, b, bold);
}

void SetUiControlText(MMDApp* app, HWND control, const char* english) {
    if (app->EnglishUI() == kUiChinese)
        if (const auto* zh = ChineseUiText(english)) {
            SetWindowTextW(control, zh);
            return;
        }
    SetWindowTextA(control, english);
}

LRESULT AddUiComboText(MMDApp* app, HWND combo, const char* english) {
    if (app->EnglishUI() == kUiChinese)
        if (const auto* zh = ChineseUiText(english))
            return SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(zh));
    return SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(english));
}
}
