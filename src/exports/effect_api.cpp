#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/effect_display.hpp"
#include "mikumikustudio/perf_timing.hpp"
// ===========================================================================
// VA 0x004C3590..0x004C3A00 - MMD effect/accessory plugin API (37 exports)
// ===========================================================================
// The original exports (RVA 0x00141328, ordinals 1..37) let MME-style
// effect DLLs query scene state.  Every export is a thin __cdecl thunk that
// loads `Block` (0x54593C) into ecx and forwards to a __thiscall query in
// the 0x42A0D0..0x42AE10 model-query family (verified on the live
// disassembly of 0x4C3590..0x4C3A10).
//
// GetPmdNum (0x42A110) is the canonical full port in src/unported/stubs.cpp;
// the former file-local CountModels twin was removed and its callers
// (ExpGetPmdNum + the two ordering helpers) now forward to GetPmdNum.
//
// Model-object layout used below (model = slot array app+0x780 [100]).
// Every model field goes through the mdl::ModelRecord members or the
// typed BoneRecord/MorphRecord/ModelMaterialRecord views, so one field
// name compiles to the correct offset on both ABIs.  x86 offset ->
// x64 twin (each verified on the x64 export twins in MikuMikuDance.exe):
//   materialCount   +0x001C -> +0x0038  (0x4A48D0)
//   materials       +0x0020 -> +0x0040  2292 B records, first 68 B = 17
//                                       floats (0x4A48E0; the 0x42AC50
//                                       accessory twin uses 68 B records)
//   path            +0x24BC -> +0x2548  wchar_t[256] (resolved model path)
//   boneTable       +0x26BC -> +0x2748  BoneRecord 604/624 B, name at +0
//   morphs          +0x26C4 -> +0x2758  MorphRecord 136/192 B, name at +0,
//                                       morph value (float) +0x30/+0x38
//   comboSelIndex   +0x2D7C -> +0x3108  combo/draw order byte (+1 based)
//   morphCount      +0x2D80 -> +0x310C  (dword)
//   boneCount       +0x2D84 -> +0x3110  (dword)
//   loadComplete    +0x2D8D -> +0x3119  display flag byte
//   toonShared      +0x37C4 -> +0x3B6C  current material index (render
//                                       state; see ExpGetCurrentMaterial)
//   +0x31C0  current-FPS float (0x41E950/0x41E980)
//   +0x33D8  enhance-model name block, 10 x char[100]
// Bone record sub-fields: matInit 4x4 at +0x34/+0x3C, position (x,y,z)
// floats at +0x134/+0x138/+0x13C (x64 +0x13C..+0x144).
//
// Accessory-object offsets (acc = slot array app+0x9DD70 [255]):
//   +0x0004  material records base pointer, 68 B (17 float) stride
//   +0x0210  display flag byte
//   +0x0214/+0x0218/+0x021C  X / Y / Z translation
//   +0x0220/+0x0224/+0x0228  Rx / Ry / Rz (radians)
//   +0x022C  size (Si; the export multiplies by dbl 10.0 @ 0x52C170)
//   +0x0230  parent model slot (-1 = none), +0x0234 parent bone index
//   +0x029C  wchar_t path[256]
//   +0x049D  order byte
//   +0x04A0  transparency (Tr)
//   +0x04A4  material count (dword), +0x04A8 current material index
//
// .rdata constants: dbl 0x52B8E0 = 100.0 (percent), dbl 0x52C170 = 10.0
// (Si scaling), dbl 0x52BA68 = 30.0 (frame rate), flt 0x52B9F0 =
// 4294967296.0 (negative-frame fixup).
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>

#include <cmath>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <string>

#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/pmm_paths.hpp"
#include "mikumikustudio/text_encoding.hpp"

// VA 0x0042A110 - model-slot count; real body in src/unported/stubs.cpp.
namespace mikumikustudio {
int GetPmdNum(MMDApp* app);
}  // namespace mikumikustudio

namespace {

using mikumikustudio::MMDApp;

// x64 reference RVA 0xA6FA0: try CP932, then the renderer's explicit
// locales. The process-wide Japanese locale cannot encode Chinese paths.
void EffectFilename(MMDApp* app, char* out, const wchar_t* path) {
    // Native MME receives this path through MMHack and probes the same-name
    // EMD with ANSI file APIs. CP932 bytes for shared Chinese/Japanese glyphs
    // identify a different file on a Chinese Windows installation.
    // This plugin boundary is shared by both builds; file-format encoding
    // remains governed separately by the original-profile save policy.
    if (GetModuleHandleW(L"MMEffect.dll")) {
        std::string nativePath;
        if (mikumikustudio::text_encoding::EncodeExact(path, CP_ACP, nativePath) && nativePath.size() < 256) {
            strcpy_s(out, 256, nativePath.c_str());
            return;
        }
    }
    char encoded[3 * 256]{};
    BOOL usedDefault = FALSE;
    if (WideCharToMultiByte(932, 0, path, -1, encoded, sizeof(encoded),
                            nullptr, &usedDefault) != 0 && !usedDefault) {
        strncpy_s(out, 256, encoded, _TRUNCATE);
        return;
    }
    if (auto* renderer = app->Renderer()) {
        for (auto locale : renderer->localeTable) {
            if (locale != nullptr &&
                _wcstombs_s_l(nullptr, out, 256, path, _TRUNCATE, locale) == 0)
                return;
        }
    }
    out[0] = '\0';
}

// ---- slot resolution (shared shape of the 0x42A1xx family) ----------------
// Occupied-slot counter: index counts only non-null slots, matching the
// original `cmp (%esi); je skip / inc edi; cmp edi, arg` loops.

unsigned char* ModelByIndex(MMDApp* app, int index) {  // 0x780/x64 0xBE8
    int occupied = -1;
    for (int i = 0; i < mikumikustudio::kModelSlotCount; ++i) {
        unsigned char* slot = app->ModelSlot(i);
        if (slot != nullptr && ++occupied == index)
            return mikumikustudio::native_mesh::RenderModel(slot);
    }
    return nullptr;
}

mikumikustudio::mdl::AccessoryRecord* AcsByIndex(MMDApp* app, int index) {
    int occupied = -1;
    for (int i = 0; i < 0xFF; ++i) {
        mikumikustudio::mdl::AccessoryRecord* slot = app->AccessorySlot(i);
        if (slot != nullptr && ++occupied == index)
            return mikumikustudio::native_mesh::RenderAccessory(slot);
    }
    return nullptr;
}

// VA 0x0042A5F0 - count occupied accessory slots (51 x 5 unrolled).
int CountAcs(MMDApp* app) {
    int count = 0;
    for (int i = 0; i < 0xFF; ++i) {
        if (app->AccessorySlot(i) != nullptr)
            ++count;
    }
    return count;
}

// Model/accessory fields are reached through the typed records
// (mdl::Mdl / mdl::Bones / mdl::Morphs / mdl::Materials), never through
// numeric offsets - the layouts differ between the x86 and x64 ABIs.

// Objects the effect engine has registered.  MMHack rebuilds its order->state
// map every frame from ExpGetPmdOrder/ExpGetAcsOrder, but only asks for the
// order of an object whose pointer it already tracks, so an object it has not
// registered is missing from that map.  ExpGetCurrentObject supplies the map
// key of the object being drawn, and MMHack's draw hook dereferences the null
// state record it inserts for an unknown key (MMHack+0x28D8, an access
// violation at address 0 that cannot be intercepted from the host side).
// Report the key only for objects the engine has asked about this frame.
// Otherwise return 0, which MMHack reads as "no current object", continuing
// without per-object effect state.
std::uint8_t g_modelOrderSeen[mikumikustudio::kModelSlotCount] = {0};
std::uint8_t g_accessoryOrderSeen[0xFF] = {0};

// Memoize repeated MME queries only within the owning render thread's current
// registration/pass. Keep source identity (MMV/native proxies resolve below).
std::uint64_t g_currentObjectGeneration = 1;
DWORD g_effectRenderThread = 0;
struct CurrentObjectCache {
    MMDApp* app = nullptr;
    const void* object = nullptr;
    std::uint64_t generation = 0;
    int repeat = 0, split = 0, result = 0;
    int slot = -1, order = -1;
    bool accessory = false;
};
thread_local CurrentObjectCache g_currentObjectCache;


constexpr UINT kEffectEnable = 40006;
constexpr UINT kEffectEnableEnglish = 40020;
constexpr UINT_PTR kEffectMenuSubclass = 0x4D4D45;

HMENU EffectMenu(HWND window) {
    if (!IsWindow(window))
        return nullptr;
    HMENU menu = GetMenu(window);
    if (!IsMenu(menu))
        return nullptr;
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        HMENU popup = GetSubMenu(menu, i);
        if (!IsMenu(popup))
            continue;
        if (GetMenuState(popup, 40005, MF_BYCOMMAND) != UINT(-1) &&
            GetMenuState(popup, 40001, MF_BYCOMMAND) != UINT(-1) &&
            (GetMenuState(popup, kEffectEnable, MF_BYCOMMAND) != UINT(-1) ||
             GetMenuState(popup, kEffectEnableEnglish, MF_BYCOMMAND) != UINT(-1)))
            return popup;
    }
    return nullptr;
}

HHOOK effectEditorHook = nullptr;
MMDApp* effectEditorApp = nullptr;

void TranslateEffectEditor(HWND window) {
    if (!effectEditorApp) return;
    // Match the plugin window and its public command set, never host controls
    // or arbitrary application windows with a coincidentally similar caption.
    const HMODULE module = GetModuleHandleW(L"MMEffect.dll");
    if (!module || reinterpret_cast<HMODULE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)) != module)
        return;
    HMENU menu = GetMenu(window);
    if (!menu || GetMenuState(menu, 40028, MF_BYCOMMAND) == UINT(-1) ||
        GetMenuState(menu, 40007, MF_BYCOMMAND) == UINT(-1)) return;
    mikumikustudio::InstallEffectListDisplay(window, effectEditorApp);
    if (effectEditorApp->EnglishUI() != 2) return;
    SetWindowTextW(window, L"效果文件分配");
    const auto setMenuText = [](HMENU target, UINT id, bool byPosition, const wchar_t* text) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_STRING;
        item.dwTypeData = const_cast<wchar_t*>(text);
        SetMenuItemInfoW(target, id, byPosition, &item);
    };
    const wchar_t* categories[] = {L"文件(&F)", L"编辑(&E)", L"工具(&T)"};
    for (UINT i = 0; i < 3 && i < static_cast<UINT>(GetMenuItemCount(menu)); ++i)
        setMenuText(menu, i, true, categories[i]);
    struct Label { UINT id; const wchar_t* text; };
    static constexpr Label labels[] = {
        {40009, L"打开(&O)"}, {40010, L"保存(&S)"},
        {40016, L"按模型打开(&M)"}, {40017, L"按模型保存(&N)"},
        {40011, L"关闭(&C)"}, {40007, L"设置效果文件(&F)"},
        {40012, L"移除效果(&N)"}, {40013, L"隐藏/显示(&H)"},
        {40008, L"恢复默认(&R)"}, {40006, L"展开材质子集(&E)"},
        {40014, L"反汇编效果(&D)"}, {40028, L"跳过设备验证(&S)"},
    };
    for (const auto& label : labels) setMenuText(menu, label.id, false, label.text);
    EnumChildWindows(window, [](HWND child, LPARAM) -> BOOL {
        struct Text { const wchar_t* en; const wchar_t* zh; };
        static constexpr Text labels[] = {
            {L"Main Render Target", L"主渲染目标"}, {L"Selected Items", L"所选项目"},
            {L"Set Effect", L"设置效果"}, {L"Remove Effect", L"移除效果"},
            {L"Hide/Show", L"隐藏/显示"}, {L"OK", L"确定"},
            {L"Cancel", L"取消"}, {L"Apply", L"应用"},
        };
        wchar_t cls[64]{}, text[256]{};
        GetClassNameW(child, cls, 64);
        if (_wcsicmp(cls, L"Static") == 0 || _wcsicmp(cls, L"Button") == 0) {
            GetWindowTextW(child, text, 256);
            for (const auto& label : labels)
                if (std::wcscmp(text, label.en) == 0) SetWindowTextW(child, label.zh);
        } else if (_wcsicmp(cls, WC_LISTVIEWW) == 0) {
            for (int i = 0; i < 2; ++i) {
                LVCOLUMNW column{}; column.mask = LVCF_TEXT;
                column.pszText = text; column.cchTextMax = 256;
                if (SendMessageW(child, LVM_GETCOLUMNW, i, reinterpret_cast<LPARAM>(&column)) &&
                    (std::wcscmp(text, L"Object") == 0 || std::wcscmp(text, L"Effect File") == 0)) {
                    column.pszText = const_cast<wchar_t*>(i == 0 ? L"对象" : L"效果文件");
                    SendMessageW(child, LVM_SETCOLUMNW, i, reinterpret_cast<LPARAM>(&column));
                }
            }
        } else if (_wcsicmp(cls, WC_TABCONTROLW) == 0) {
            TCITEMW tab{}; tab.mask = TCIF_TEXT; tab.pszText = text; tab.cchTextMax = 256;
            if (SendMessageW(child, TCM_GETITEMW, 0, reinterpret_cast<LPARAM>(&tab)) &&
                std::wcscmp(text, L"Main") == 0) {
                tab.pszText = const_cast<wchar_t*>(L"主目标");
                SendMessageW(child, TCM_SETITEMW, 0, reinterpret_cast<LPARAM>(&tab));
            }
        }
        return TRUE;
    }, 0);
}

LRESULT CALLBACK EffectEditorMessages(int code, WPARAM wp, LPARAM lp) {
    static thread_local bool translating = false;
    if (code >= 0 && !translating) {
        const auto* message = reinterpret_cast<const CWPRETSTRUCT*>(lp);
        if (message->message == WM_SHOWWINDOW || message->message == WM_INITDIALOG ||
            message->message == WM_INITMENUPOPUP || message->message == WM_NOTIFY) {
            translating = true;
            TranslateEffectEditor(message->hwnd);
            translating = false;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void RepairEffectMenu(HMENU menu) {
    // The shipped MME English resource (107) uses 40020, but its WndProc
    // and Ctrl+Shift+E hook both dispatch 40006, as does Japanese resource
    // 101. Fix the live menu only; the DLL and its effect engine stay intact.
    if (menu != nullptr &&
        GetMenuState(menu, kEffectEnable, MF_BYCOMMAND) == UINT(-1) &&
        GetMenuState(menu, kEffectEnableEnglish, MF_BYCOMMAND) != UINT(-1)) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_ID;
        item.wID = kEffectEnable;
        SetMenuItemInfoW(menu, kEffectEnableEnglish, FALSE, &item);
    }
}

LRESULT CALLBACK EffectMenuProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                               UINT_PTR id, DWORD_PTR reference) {
    // MME recreates its popup before forwarding the language command to
    // the host, restoring the resource's default check mark even when the
    // engine is disabled. Save the actual check before that recreation.
    const bool languageChange = message == WM_COMMAND && LOWORD(wp) == 260;
    UINT enabled = UINT(-1);
    if (languageChange)
        enabled = GetMenuState(EffectMenu(hwnd), kEffectEnable, MF_BYCOMMAND);
    if (message == WM_NCDESTROY) {
        if (effectEditorApp == reinterpret_cast<MMDApp*>(reference)) {
            if (effectEditorHook) UnhookWindowsHookEx(effectEditorHook);
            effectEditorHook = nullptr;
            effectEditorApp = nullptr;
        }
        RemoveWindowSubclass(hwnd, EffectMenuProc, id);
    }
    const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
    if (languageChange) {
        HMENU menu = EffectMenu(hwnd);
        RepairEffectMenu(menu);
        if (menu != nullptr && enabled != UINT(-1))
            CheckMenuItem(menu, kEffectEnable, MF_BYCOMMAND | (enabled & MF_CHECKED));
        mikumikustudio::RefreshEffectMenuLanguage(
            reinterpret_cast<mikumikustudio::MMDApp*>(reference));
        DrawMenuBar(hwnd);
    } else if (message == WM_INITMENUPOPUP) {
        // Apply after MME handles the message: it may have rebuilt its popup.
        mikumikustudio::RefreshEffectMenuLanguage(
            reinterpret_cast<mikumikustudio::MMDApp*>(reference));
    }
    if (languageChange || (message == WM_COMMAND && (LOWORD(wp) == 310 || LOWORD(wp) == 311))) {
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
            TranslateEffectEditor(window);
            if (reinterpret_cast<HMODULE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)) ==
                GetModuleHandleW(L"MMEffect.dll"))
                RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return TRUE;
        }, 0);
    }
    return result;
}

// ---- D3DX wrappers with local fallbacks (d3dx9_32.dll may be absent) ------
void Identity(mikumikustudio::d3dx::D3DXMATRIXF* m) {
    std::memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.0f;
}

void MulFallback(mikumikustudio::d3dx::D3DXMATRIXF* o, const mikumikustudio::d3dx::D3DXMATRIXF* a,
                 const mikumikustudio::d3dx::D3DXMATRIXF* b) {  // o = a * b (row vectors)
    mikumikustudio::d3dx::D3DXMATRIXF t;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a->m[i][k] * b->m[k][j];
            t.m[i][j] = s;
        }
    *o = t;
}

struct D3dx {
    mikumikustudio::d3dx::Api& api;
    bool ok;
    D3dx() : api(mikumikustudio::d3dx::Get()), ok(api.Load()) {}
    void Translation(mikumikustudio::d3dx::D3DXMATRIXF* m, float x, float y, float z) {
        if (ok && api.translation) {
            api.translation(m, x, y, z);
        } else {  // documented fallback, same formula
            Identity(m);
            m->m[3][0] = x;
            m->m[3][1] = y;
            m->m[3][2] = z;
        }
    }
    void Multiply(mikumikustudio::d3dx::D3DXMATRIXF* o, const mikumikustudio::d3dx::D3DXMATRIXF* a,
                  const mikumikustudio::d3dx::D3DXMATRIXF* b) {
        if (ok && api.multiply)
            api.multiply(o, a, b);
        else
            MulFallback(o, a, b);
    }
    void Rot(float angle, int axis, mikumikustudio::d3dx::D3DXMATRIXF* m) {
        if (ok && (axis == 0 ? api.rotX : axis == 1 ? api.rotY : api.rotZ)) {
            (axis == 0 ? api.rotX : axis == 1 ? api.rotY : api.rotZ)(m, angle);
            return;
        }
        Identity(m);
        const float c = cosf(angle), s = sinf(angle);
        if (axis == 0) {          // X
            m->m[1][1] = c; m->m[1][2] = s; m->m[2][1] = -s; m->m[2][2] = c;
        } else if (axis == 1) {   // Y
            m->m[0][0] = c; m->m[0][2] = -s; m->m[2][0] = s; m->m[2][2] = c;
        } else {                  // Z
            m->m[0][0] = c; m->m[0][1] = s; m->m[1][0] = -s; m->m[1][1] = c;
        }
    }
    void Scaling(mikumikustudio::d3dx::D3DXMATRIXF* m, float x, float y, float z) {
        if (ok && api.scaling) {
            api.scaling(m, x, y, z);
        } else {
            Identity(m);
            m->m[0][0] = x;
            m->m[1][1] = y;
            m->m[2][2] = z;
        }
    }
};

// ===========================================================================
// VA 0x004C5D00 - accessory local-to-world matrix (callee of 0x42A720)
// ===========================================================================
// dst = Scaling(Si*10) * Rz * Rx * Ry * T(local xyz) * T(parent xyz) * bone
// (each step is D3DXMatrixMultiply(dst, dst, step); the original builds the
// result directly into the caller's buffer, zero-filled first).
float* AccWorldMatrix(const mikumikustudio::mdl::AccessoryRecord& acc, float* dst16,
                      float px, float py, float pz,
                      const float* boneMat16) {
    auto* dst = reinterpret_cast<mikumikustudio::d3dx::D3DXMATRIXF*>(dst16);
    auto* bone = reinterpret_cast<const mikumikustudio::d3dx::D3DXMATRIXF*>(boneMat16);
    mikumikustudio::d3dx::D3DXMATRIXF step;
    D3dx d3;

    const float si10 = static_cast<float>(acc.scale * 10.0);
    d3.Scaling(dst, si10, si10, si10);            // dbl 0x52C170 = 10.0
    d3.Rot(acc.rotation[2], 2, &step);            // Rz
    d3.Multiply(dst, dst, &step);
    d3.Rot(acc.rotation[0], 0, &step);            // Rx
    d3.Multiply(dst, dst, &step);
    d3.Rot(acc.rotation[1], 1, &step);            // Ry
    d3.Multiply(dst, dst, &step);
    d3.Translation(&step, acc.position[0], acc.position[1], acc.position[2]);
    d3.Multiply(dst, dst, &step);
    d3.Translation(&step, px, py, pz);
    d3.Multiply(dst, dst, &step);
    d3.Multiply(dst, dst, bone);
    return dst16;
}

}  // namespace

namespace mikumikustudio {

// Only the seven commands on the MME main popup are translated. Command IDs,
// checked/disabled states, separator items and third-party additions survive.
// The native labels match MMEffect's Japanese/English menu resources 101/107.
// Kept separate from module discovery so tests can use real Win32 menus without
// loading the third-party rendering hooks.
void LocalizeEffectMenu(HMENU menu, unsigned char language) {
    if (!IsMenu(menu) || language > 2)
        return;
    struct Label {
        UINT id;
        const wchar_t* text[3];  // Japanese, English, Chinese
    };
    static constexpr Label labels[] = {
        {40005, {L"エフェクト割当(&M)", L"Effect Mapping(&M)", L"效果分配(&M)"}},
        {40006, {L"エフェクト使用(&E)\tCtrl+Shift+E", L"Enable Effect(&E)\tCtrl+Shift+E",
                 L"启用效果(&E)\tCtrl+Shift+E"}},
        {40001, {L"自動更新(&O)", L"Auto Reload(&O)", L"自动重载(&O)"}},
        {40022, {L"割当ファイル自動保存(&S)", L"Auto Save Mapping File(&S)",
                 L"自动保存效果分配文件(&S)"}},
        {40004, {L"全て更新(&R)", L"Reload All(&R)", L"全部重载(&R)"}},
        {40002, {L"ログウィンドウ(&L)", L"Log Window(&L)", L"日志窗口(&L)"}},
        {40003, {L"MMEffectのバージョン情報(&A)", L"About MMEffect(&A)", L"关于 MMEffect(&A)"}},
    };
    for (const auto& label : labels) {
        UINT id = label.id;
        if (id == kEffectEnable && GetMenuState(menu, id, MF_BYCOMMAND) == UINT(-1))
            id = kEffectEnableEnglish;
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_STRING;
        item.dwTypeData = const_cast<wchar_t*>(label.text[language]);
        // A missing item in another MME version is simply left alone.
        SetMenuItemInfoW(menu, id, FALSE, &item);
    }
}

void RefreshEffectMenuLanguage(MMDApp* app) {
    // Never load MME for localization, dereference DLL internals, or retain a
    // menu handle across language changes. A DLL without a menu is also a no-op.
    if (app == nullptr || GetModuleHandleW(L"MMEffect.dll") == nullptr)
        return;
    HMENU menu = EffectMenu(app->MainWindow());
    if (menu != nullptr)
        LocalizeEffectMenu(menu, app->EnglishUI());
}

// MME reads these [System] settings from MMEffect.ini during initialization.
// Seed only absent settings before the D3D proxy initializes MME. Its own
// menu command remains responsible for subsequent user changes and persistence.
void PrepareEffectDefaults() {
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, 32768);
    if (count == 0 || count >= 32768) return;
    std::wstring directory(path, count);
    directory.resize(directory.find_last_of(L"\\/") + 1);
    if (GetFileAttributesW((directory + L"MMEffect.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    const std::wstring ini = directory + L"MMEffect.ini";
    for (const auto* key : {L"SkipValidation", L"EMMAutoSave"}) {
        wchar_t value[32]{};
        GetPrivateProfileStringW(L"System", key, L"__missing__", value, 32, ini.c_str());
        if (std::wcscmp(value, L"__missing__") == 0)
            WritePrivateProfileStringW(L"System", key, L"true", ini.c_str());
    }
}

void InstallEffectMenuCompatibility(MMDApp* app) {
    if (app == nullptr || !IsWindow(app->MainWindow()))
        return;
    HWND hwnd = app->MainWindow();
    DWORD_PTR reference = 0;
    if (GetWindowSubclass(hwnd, EffectMenuProc, kEffectMenuSubclass, &reference))
        return;
    HMENU menu = EffectMenu(hwnd);
    if (menu != nullptr) {
        RepairEffectMenu(menu);
        SetWindowSubclass(hwnd, EffectMenuProc, kEffectMenuSubclass,
                          reinterpret_cast<DWORD_PTR>(app));
        RefreshEffectMenuLanguage(app);
        if (GetModuleHandleW(L"MMEffect.dll") && !effectEditorHook) {
            effectEditorApp = app;
            effectEditorHook = SetWindowsHookExW(WH_CALLWNDPROCRET,
                EffectEditorMessages, nullptr, GetCurrentThreadId());
        }
    }
}

void InvalidateEffectCurrentObjectCache() {
    ++g_currentObjectGeneration;
}

void BeginEffectObjectRegistration() {
    InvalidateEffectCurrentObjectCache();
    g_effectRenderThread = GetCurrentThreadId();
    // Ordinals can be reused after deletion, Skip, or a new project load.
    // Only orders reported during this BeginScene are safe draw-hook keys.
    std::memset(g_modelOrderSeen, 0, sizeof(g_modelOrderSeen));
    std::memset(g_accessoryOrderSeen, 0, sizeof(g_accessoryOrderSeen));
}

void NotifyEffectFileOpen(const wchar_t* path) {
    if (path == nullptr || GetModuleHandleW(L"MMHack.dll") == nullptr)
        return;
    InstallEffectPathCompatibility();
    PrepareEffectProjectPaths(path);
    const auto notification = EffectProjectNotificationPath(path);
    HANDLE file = CreateFileW(notification.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
}

void NotifyEffectFileSave(const wchar_t* path) {
    if (path == nullptr || *path == L'\0' || GetModuleHandleW(L"MMHack.dll") == nullptr)
        return;
    if (!pmm_paths::IsProject(path)) return;
    InstallEffectPathCompatibility();
    const auto notification = EffectProjectNotificationPath(path);
    // MMHack watches the host's CreateFileW import for GENERIC_WRITE PMM
    // opens and queues their paths for MME. UCRT's _wsopen_s bypasses that
    // import. Preserve the completed PMM and let MME honor its live setting
    // when it consumes the notification; do not reload effect assignments.
    HANDLE file = CreateFileW(notification.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
}

}  // namespace mikumikustudio

extern "C" {

// --- model (PMD) queries ---------------------------------------------------
// 0x4C35A0 -> 0x42A110
__declspec(dllexport) int ExpGetPmdNum() {
    MMD_PERF_SCOPE(EffectApi);
    return mikumikustudio::GetPmdNum(mikumikustudio::g_Block);
}

// 0x4C35B0 -> 0x42A160: wide model path -> SJIS into app+0xA02B7.
__declspec(dllexport) char* ExpGetPmdFilename(int index) {
    MMD_PERF_SCOPE(EffectApi);
    MMDApp* app = mikumikustudio::g_Block;
    unsigned char* model = ModelByIndex(app, index);
    if (model == nullptr)
        return nullptr;                      // original: xor eax, eax
    char* out = reinterpret_cast<char*>(app->state.sjisOut);
    EffectFilename(app, out, mikumikustudio::mdl::Mdl(model)->path);
    return out;
}

// 0x4C35D0 -> 0x441000: order byte + min(AcsNum, PreAcsNum).
__declspec(dllexport) int ExpGetPmdOrder(int index) {
    MMD_PERF_SCOPE(EffectApi);
    MMDApp* app = mikumikustudio::g_Block;
    int base = CountAcs(app);
    const int pre = app->state.accessoryRenderSplitOrder;
    if (pre < base)
        base = pre;
    int occupied = -1;
    for (int i = 0; i < mikumikustudio::kModelSlotCount; ++i) {
        unsigned char* slot = app->ModelSlot(i);
        if (slot != nullptr && ++occupied == index) {
            if (index >= 0 && index < mikumikustudio::kModelSlotCount)
                { g_modelOrderSeen[index] = 1; ++g_currentObjectGeneration; }
            return mikumikustudio::mdl::Mdl(slot)->comboSelIndex + base;
        }
    }
    return 0;
}

// 0x4C35F0 -> 0x42A1C0 -> 0x4A48D0: dword model+0x1C.
__declspec(dllexport) int ExpGetPmdMatNum(int index) {
    MMD_PERF_SCOPE(EffectApi);
    unsigned char* model = ModelByIndex(mikumikustudio::g_Block, index);
    if (model == nullptr)
        return 0;
    return static_cast<int>(mikumikustudio::mdl::Mdl(model)->materialCount);
}

// 0x4C3610 -> 0x42A210 -> 0x4A48E0: copies 17 material floats into `out`.
// Out-of-range indices are zero-filled (0x4A48EE memset path). The upper bound was a closed
// interval, so mat == count still passed the test and copied 68 bytes starting
// one record past the end of the material table; the accessory twin
// ExpGetAcsMaterial (0x42AC50) uses the strict `<`, which is the bound applied
// here.
// Defect CB-06.
__declspec(dllexport) float* ExpGetPmdMaterial(float* out, int index,
                                               int mat) {
    MMD_PERF_SCOPE(EffectApi);
    std::memset(out, 0, 0x44);
    unsigned char* model = ModelByIndex(mikumikustudio::g_Block, index);
    if (model == nullptr)
        return out;
    const std::uint32_t count =
        mikumikustudio::mdl::Mdl(model)->materialCount;
    if (static_cast<std::uint32_t>(mat) < count) {
        const mikumikustudio::mdl::ModelMaterialRecord& rec =
            mikumikustudio::mdl::Materials(model)[
                static_cast<std::size_t>(mat)];
        std::memcpy(out, &rec, 0x44);
    }
    return out;
}

// 0x4C3650 -> 0x42A290: dword model+0x2D84.
__declspec(dllexport) int ExpGetPmdBoneNum(int index) {
    MMD_PERF_SCOPE(EffectApi);
    unsigned char* model = ModelByIndex(mikumikustudio::g_Block, index);
    if (model == nullptr)
        return 0;
    return static_cast<int>(mikumikustudio::mdl::Mdl(model)->boneCount);
}

// 0x4C3670 -> 0x42A2E0: SJIS bone name = bone record base (name at +0).
__declspec(dllexport) char* ExpGetPmdBoneName(int index, int bone) {
    MMD_PERF_SCOPE(EffectApi);
    unsigned char* model = ModelByIndex(mikumikustudio::g_Block, index);
    if (model == nullptr)
        return nullptr;
    // Deliberate hardening: the original checks only the model index -
    // the 255-slot scan returns NULL on a miss - then scales the bone index by
    // the 0x270 record stride and adds the bone table base (movsxd rax, edx;
    // imul rax, rax, 0x270; add rax, [rcx+0x2748] at 0x1400DB710) without
    // comparing it against boneCount, so an out-of-range index reads past the
    // bone table. The port reports the bone as not found.
    // Defect CB-11.
    const std::uint32_t boneCount = mikumikustudio::mdl::Mdl(model)->boneCount;
    if (bone < 0 || static_cast<std::uint32_t>(bone) >= boneCount)
        return nullptr;
    return mikumikustudio::mdl::Bones(model)[bone].name;
}

// 0x4C3690 -> 0x42A330: out = T(bone position) * bone world matrix;
// identity on a failed slot lookup.
__declspec(dllexport) float* ExpGetPmdBoneWorldMat(float* out, int index,
                                                   int bone) {
    MMD_PERF_SCOPE(EffectApi);
    mikumikustudio::d3dx::D3DXMATRIXF m;
    Identity(&m);
    unsigned char* model = ModelByIndex(mikumikustudio::g_Block, index);
    // Deliberate hardening: the original computes the bone address from the raw
    // index using the same unchecked 0x270 multiply (0x1400A745B inside
    // 0x1400A7370) and dereferences it without validating the index against
    // boneCount. This port returns the identity matrix for an out-of-range bone,
    // the same result it returns when the model slot is missing.
    // Defect CB-11.
    if (model != nullptr && bone >= 0 &&
        static_cast<std::uint32_t>(bone) <
            mikumikustudio::mdl::Mdl(model)->boneCount) {
        mikumikustudio::mdl::BoneRecord& boneRec =
            mikumikustudio::mdl::Bones(model)[bone];
        mikumikustudio::d3dx::D3DXMATRIXF t;
        D3dx d3;
        d3.Translation(&t, boneRec.position[0], boneRec.position[1],
                       boneRec.position[2]);
        d3.Multiply(&m, &m, &t);
        d3.Multiply(&m, &m,
                    reinterpret_cast<const mikumikustudio::d3dx::D3DXMATRIXF*>(boneRec.matInit));
    }
    std::memcpy(out, &m, 0x40);
    return out;
}

// 0x4C36D0 -> 0x42A460: dword model+0x2D80.
__declspec(dllexport) int ExpGetPmdMorphNum(int index) {
    MMD_PERF_SCOPE(EffectApi);
    unsigned char* model = ModelByIndex(mikumikustudio::g_Block, index);
    if (model == nullptr)
        return 0;
    return static_cast<int>(mikumikustudio::mdl::Mdl(model)->morphCount);
}

// 0x4C36F0 -> 0x42A4B0: SJIS morph name = morph record base (name at +0).
__declspec(dllexport) char* ExpGetPmdMorphName(int index, int morph) {
    MMD_PERF_SCOPE(EffectApi);
    unsigned char* model = ModelByIndex(mikumikustudio::g_Block, index);
    if (model == nullptr)
        return nullptr;
    // Deliberate hardening: the original only validates the model index, then
    // multiplies the morph index by the 0xC0 record stride (ExpGetPmdMorphName,
    // ordinal 30) without comparing it against morphCount, so an out-of-range
    // index reads past the morph table. The port reports the morph as not
    // found.
    // Defect CB-11.
    const std::uint32_t morphCount =
        mikumikustudio::mdl::Mdl(model)->morphCount;
    if (morph < 0 || static_cast<std::uint32_t>(morph) >= morphCount)
        return nullptr;
    return mikumikustudio::mdl::Morphs(model)[morph].name;
}

// 0x4C3710 -> 0x42A500: morph value (float) at record +0x30.
__declspec(dllexport) float ExpGetPmdMorphValue(int index, int morph) {
    MMD_PERF_SCOPE(EffectApi);
    unsigned char* model = ModelByIndex(mikumikustudio::g_Block, index);
    if (model == nullptr)
        return 0.0f;
    // Deliberate hardening: the original bounds only the model index and then
    // scales the morph index by the 0xC0 record stride (ExpGetPmdMorphValue,
    // ordinal 32) without comparing it against morphCount, so an out-of-range
    // index reads outside the morph table. The port reports the value as zero,
    // the result it also returns when the model slot is missing.
    // Defect CB-11.
    const std::uint32_t morphCount =
        mikumikustudio::mdl::Mdl(model)->morphCount;
    if (morph < 0 || static_cast<std::uint32_t>(morph) >= morphCount)
        return 0.0f;
    return mikumikustudio::mdl::Morphs(model)[morph].value;
}

// 0x4C3730 -> 0x42A560: display byte model+0x2D8D.
__declspec(dllexport) int ExpGetPmdDisp(int index) {
    MMD_PERF_SCOPE(EffectApi);
    unsigned char* model = ModelByIndex(mikumikustudio::g_Block, index);
    if (model == nullptr)
        return 0;
    return static_cast<unsigned char>(mikumikustudio::mdl::Mdl(model)->loadComplete);
}

// x64 reference RVA 0xDB91C returns EAX, not RAX: IDs are the low 32 bits
// of the object address. MME recovers the native pointer from resource slots.
__declspec(dllexport) std::uint32_t ExpGetPmdID(int index) {
    MMD_PERF_SCOPE(EffectApi);
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(
        ModelByIndex(mikumikustudio::g_Block, index)));
}

// --- accessory (ACS/stage) queries -----------------------------------------
// 0x4C3770 -> 0x42A5F0.
__declspec(dllexport) int ExpGetAcsNum() {
    MMD_PERF_SCOPE(EffectApi);
    return CountAcs(mikumikustudio::g_Block);
}

// 0x4C3780 -> 0x42A640.
__declspec(dllexport) int ExpGetPreAcsNum() {
    MMD_PERF_SCOPE(EffectApi);
    return mikumikustudio::g_Block->state.accessoryRenderSplitOrder;
}

// 0x4C3790 -> 0x42A650: wide path (acc+0x29C) -> SJIS into app+0xA02B7.
__declspec(dllexport) char* ExpGetAcsFilename(int index) {
    MMD_PERF_SCOPE(EffectApi);
    MMDApp* app = mikumikustudio::g_Block;
    mikumikustudio::mdl::AccessoryRecord* acc = AcsByIndex(app, index);
    if (acc == nullptr)
        return nullptr;
    char* out = reinterpret_cast<char*>(app->state.sjisOut);
    EffectFilename(app, out, acc->sourcePath);
    return out;
}

// 0x4C37B0 -> 0x42A6B0: -(order+1) for accessories ordered before the
// stage block, order+1+PmdNum for those after it.
__declspec(dllexport) int ExpGetAcsOrder(int index) {
    MMD_PERF_SCOPE(EffectApi);
    MMDApp* app = mikumikustudio::g_Block;
    mikumikustudio::mdl::AccessoryRecord* acc = AcsByIndex(app, index);
    if (acc == nullptr)
        return 0;
    if (index >= 0 && index < 0xFF)
        { g_accessoryOrderSeen[index] = 1; ++g_currentObjectGeneration; }
    int order = acc->order + 1;
    if (order < app->state.accessoryRenderSplitOrder + 1)
        return -order;
    return order + mikumikustudio::GetPmdNum(app);
}

// 0x4C37D0 -> 0x42A720 + 0x4C5D00: out zero-filled; a parented accessory
// uses the parent bone position/matrix, otherwise T(0,0,0) * identity.
__declspec(dllexport) float* ExpGetAcsWorldMat(float* out, int index) {
    MMD_PERF_SCOPE(EffectApi);
    MMDApp* app = mikumikustudio::g_Block;
    std::memset(out, 0, 0x40);
    mikumikustudio::mdl::AccessoryRecord* acc = AcsByIndex(app, index);
    if (acc != nullptr) {
        const std::int32_t parentSlot = acc->parentModel;
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        mikumikustudio::d3dx::D3DXMATRIXF bone;
        // Deliberate hardening: the original only tests this field for
        // `parentModel == -1` (cmp dword ptr [r10+240h], -1 at 0x1400A19F3), then
        // indexes the model slot array with the raw value, so an out-of-range or
        // empty parent slot is treated as a live model. This port validates the slot
        // against kModelSlotCount, so an unusable parent falls back to the
        // unparented identity matrix instead of being dereferenced.
        // Defect CB-02.
        unsigned char* model = nullptr;
        if (parentSlot >= 0 && parentSlot < mikumikustudio::kModelSlotCount)
            model = app->ModelSlot(parentSlot);
        // Deliberate hardening: the original code loads parentBone and scales it by
        // the 0x270 bone stride before dereferencing it (0x1400A1B10,
        // 0x1400A1B1F, 0x1400A1B2D), without comparing it against the parent
        // model's boneCount or applying even the max(..., 0) lower clamp, so an
        // out-of-range index reads past the end of the bone table. The port bounds
        // the index and falls back to the unparented identity path.
        // Defect RB-06.
        if (model != nullptr && acc->parentBone >= 0 &&
            static_cast<std::uint32_t>(acc->parentBone) <
                mikumikustudio::mdl::Mdl(model)->boneCount) {
            mikumikustudio::mdl::BoneRecord& boneRec =
                mikumikustudio::mdl::Bones(model)[acc->parentBone];
            px = boneRec.position[0];
            py = boneRec.position[1];
            pz = boneRec.position[2];
            std::memcpy(&bone, boneRec.matInit, sizeof(bone));
        } else {
            Identity(&bone);
        }
        AccWorldMatrix(*acc, out, px, py, pz, &bone.m[0][0]);
    }
    return out;
}

// 0x4C3800 -> 0x42A8F0 (float acc+0x214; 0 when the index never matches).
__declspec(dllexport) float ExpGetAcsX(int index) {
    MMD_PERF_SCOPE(EffectApi);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    return acc != nullptr ? acc->position[0] : 0.0f;
}
// 0x4C3820 -> 0x42A940 (acc+0x218).
__declspec(dllexport) float ExpGetAcsY(int index) {
    MMD_PERF_SCOPE(EffectApi);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    return acc != nullptr ? acc->position[1] : 0.0f;
}
// 0x4C3840 -> 0x42A990 (acc+0x21C).
__declspec(dllexport) float ExpGetAcsZ(int index) {
    MMD_PERF_SCOPE(EffectApi);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    return acc != nullptr ? acc->position[2] : 0.0f;
}
// 0x4C3860 -> 0x42A9E0 (acc+0x220).
__declspec(dllexport) float ExpGetAcsRx(int index) {
    MMD_PERF_SCOPE(EffectApi);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    return acc != nullptr ? acc->rotation[0] : 0.0f;
}
// 0x4C3880 -> 0x42AA30 (acc+0x224).
__declspec(dllexport) float ExpGetAcsRy(int index) {
    MMD_PERF_SCOPE(EffectApi);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    return acc != nullptr ? acc->rotation[1] : 0.0f;
}
// 0x4C38A0 -> 0x42AA80 (acc+0x228).
__declspec(dllexport) float ExpGetAcsRz(int index) {
    MMD_PERF_SCOPE(EffectApi);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    return acc != nullptr ? acc->rotation[2] : 0.0f;
}
// 0x4C38C0 -> 0x42AAD0 (acc+0x22C scaled by dbl 10.0 @ 0x52C170).
__declspec(dllexport) float ExpGetAcsSi(int index) {
    MMD_PERF_SCOPE(EffectApi);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    return acc != nullptr
               ? static_cast<float>(acc->scale * 10.0)
               : 0.0f;
}
// 0x4C38E0 -> 0x42AB20 (acc+0x4A0).
__declspec(dllexport) float ExpGetAcsTr(int index) {
    MMD_PERF_SCOPE(EffectApi);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    return acc != nullptr ? acc->opacity : 0.0f;
}

// 0x4C3900 -> 0x42AB70: display byte acc+0x210.
__declspec(dllexport) int ExpGetAcsDisp(int index) {
    MMD_PERF_SCOPE(EffectApi);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    if (acc == nullptr)
        return 0;
    return acc->visible;
}

// x64 reference RVA 0xDBACC uses the same 32-bit ID ABI as models.
__declspec(dllexport) std::uint32_t ExpGetAcsID(int index) {
    MMD_PERF_SCOPE(EffectApi);
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(
        AcsByIndex(mikumikustudio::g_Block, index)));
}

// 0x4C3940 -> 0x42AC00: dword acc+0x4A4.
__declspec(dllexport) int ExpGetAcsMatNum(int index) {
    MMD_PERF_SCOPE(EffectApi);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    if (acc == nullptr)
        return 0;
    return static_cast<int>(acc->materialCount);
}

// 0x4C3960 -> 0x42AC50 -> 0x4C5EE0: copies 17 material floats into `out`
// (68-byte records at acc+4); out-of-range values are zero-filled (strict `<`, the bound
// the model twin applies as well).
// Defect CB-06.
__declspec(dllexport) float* ExpGetAcsMaterial(float* out, int index,
                                               int mat) {
    MMD_PERF_SCOPE(EffectApi);
    std::memset(out, 0, 0x44);
    const auto* acc = AcsByIndex(mikumikustudio::g_Block, index);
    if (acc == nullptr)
        return out;
    if (acc->materials != nullptr && mat >= 0 &&
        mat < static_cast<std::int32_t>(acc->materialCount)) {
        const auto& material = static_cast<const D3DMATERIAL9*>(acc->materials)[mat];
        std::memcpy(out, &material, sizeof(material));
    }
    return out;
}

// --- render state ----------------------------------------------------------
// 0x4C39A0 -> 0x42ACD0: encode app+0xA0268 (object being rendered) as the
// combined model+accessory order index (same encoding as ExpGetAcsOrder /
// ExpGetPmdOrder); 0 when null, unmatched, or not registered by the effect
// engine yet (see g_modelOrderSeen).
__declspec(dllexport) int ExpGetCurrentObject() {
    MMD_PERF_SCOPE(EffectApi);
    MMDApp* app = mikumikustudio::g_Block;
    const void* cur = mikumikustudio::native_mesh::SourceObject(app->state.activeRenderObject);
    if (cur == nullptr)
        return 0;
    auto& cache = g_currentObjectCache;
    const bool canCache = GetCurrentThreadId() == g_effectRenderThread && !app->SceneMutationInProgress();
    if (canCache && cache.app == app && cache.object == cur &&
        cache.generation == g_currentObjectGeneration &&
        cache.repeat == app->state.renderPassCount &&
        cache.split == app->state.accessoryRenderSplitOrder && cache.slot >= 0) {
        if (cache.accessory) {
            const auto* slot = app->AccessorySlot(cache.slot);
            if (slot == cur && slot->order == cache.order) return cache.result;
        } else {
            const auto* slot = app->ModelSlot(cache.slot);
            if (slot == cur && mikumikustudio::mdl::Mdl(slot)->comboSelIndex == cache.order)
                return cache.result;
        }
    }
    int foundSlot = -1, foundOrder = -1;
    bool foundAccessory = false;
    const auto resolve = [&]() -> int {
        // accessory slots first
        int ordinal = 0;
        for (int i = 0; i < 0xFF; ++i) {
            mikumikustudio::mdl::AccessoryRecord* slot = app->AccessorySlot(i);
            if (slot == nullptr)
                continue;
            if (slot == cur) {
                foundSlot = i; foundOrder = slot->order; foundAccessory = true;
                if (g_accessoryOrderSeen[ordinal] == 0)
                    return 0;  // not in MMHack's order map yet
                int order = slot->order + 1;
                if (order >= app->state.accessoryRenderSplitOrder + 1)
                    return order + mikumikustudio::GetPmdNum(app);
                return -order;
            }
            ++ordinal;
        }
        // then model slots, offset by min(AcsNum, PreAcsNum)
        int base = CountAcs(app);
        const int pre = app->state.accessoryRenderSplitOrder;
        if (pre < base)
            base = pre;
        ordinal = 0;
        for (int i = 0; i < mikumikustudio::kModelSlotCount; ++i) {
            unsigned char* slot = app->ModelSlot(i);
            if (slot == nullptr)
                continue;
            if (slot == cur) {
                foundSlot = i; foundOrder = mikumikustudio::mdl::Mdl(slot)->comboSelIndex;
                if (g_modelOrderSeen[ordinal] == 0)
                    return 0;  // not in MMHack's order map yet
                return mikumikustudio::mdl::Mdl(slot)->comboSelIndex + base;
            }
            ++ordinal;
        }
        return 0;
    };
    const int result = resolve();
    if (canCache && foundSlot >= 0)
        cache = {app, cur, g_currentObjectGeneration, app->state.renderPassCount,
            app->state.accessoryRenderSplitOrder, result, foundSlot, foundOrder, foundAccessory};
    else
        cache.app = nullptr;
    return result;
}


// 0x4C39B0 -> 0x42AD70: current material index of the object being
// rendered (accessory currentMaterial / model toonShared, the render-state
// material cursor at +0x37C4/+0x3B6C); -1 when null or unmatched.
__declspec(dllexport) int ExpGetCurrentMaterial() {
    MMD_PERF_SCOPE(EffectApi);
    MMDApp* app = mikumikustudio::g_Block;
    unsigned char* cur = static_cast<unsigned char*>(app->state.activeRenderObject);
    if (cur == nullptr)
        return -1;
    for (int i = 0; i < 0xFF; ++i) {
        mikumikustudio::mdl::AccessoryRecord* slot = mikumikustudio::native_mesh::RenderAccessory(app->AccessorySlot(i));
        if (slot == reinterpret_cast<mikumikustudio::mdl::AccessoryRecord*>(cur))
            return slot->currentMaterial;
    }
    for (int i = 0; i < mikumikustudio::kModelSlotCount; ++i) {
        unsigned char* slot = mikumikustudio::native_mesh::RenderModel(app->ModelSlot(i));
        if (slot == cur)
            return static_cast<std::int32_t>(
                mikumikustudio::mdl::Mdl(cur)->toonShared);
    }
    return -1;
}

// 0x4C39C0 -> 0x42ADE0.
__declspec(dllexport) int ExpGetCurrentTechnic() {
    MMD_PERF_SCOPE(EffectApi);
    return mikumikustudio::g_Block->state.activeRenderPass;
}

// 0x4C39D0 -> 0x42ADF0.
__declspec(dllexport) void ExpSetRenderRepeatCount(int count) {
    MMD_PERF_SCOPE(EffectApi);
    mikumikustudio::g_Block->state.renderPassCount = count;
}

// 0x4C39F0 -> 0x42AE00.
__declspec(dllexport) int ExpGetRenderRepeatCount() {
    MMD_PERF_SCOPE(EffectApi);
    return mikumikustudio::g_Block->state.renderPassCount;
}

// 0x4C3A00 -> 0x42AE10: English UI flag byte (app+0xA0B4C).
__declspec(dllexport) int ExpGetEnglishMode() {
    MMD_PERF_SCOPE(EffectApi);
    return mikumikustudio::g_Block->state.englishUI;
}

// 0x4C3590 -> 0x42A0D0: physics cursor (0x9E64C) while playing, else the
// current frame (0x980; negative values re-read as unsigned via the 2^32
// add) divided by 30.
__declspec(dllexport) float ExpGetFrameTime() {
    MMD_PERF_SCOPE(EffectApi);
    MMDApp* app = mikumikustudio::g_Block;
    if (app->state.playbackActive != 0)
        return app->PlaybackCursorSeconds();
    const std::int32_t frame = app->state.currentFrame;
    // fild / fadds flt_52B9F0 (2^32) when negative / fdivl dbl_52BA68 (30.0)
    return static_cast<float>(static_cast<double>(
                                  static_cast<std::uint32_t>(frame)) /
                              30.0);
}

}  // extern "C"
