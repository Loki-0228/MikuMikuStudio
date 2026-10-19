// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "btBulletDynamicsCommon.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>

namespace mikumikustudio {
void LocalizeEffectMenu(HMENU menu, unsigned char language);
}

extern "C" {
int ExpGetPmdOrder(int);
int ExpGetAcsOrder(int);
int ExpGetCurrentObject();
std::uint32_t ExpGetPmdID(int);
std::uint32_t ExpGetAcsID(int);
char* ExpGetPmdFilename(int);
char* ExpGetAcsFilename(int);
}

namespace {
bool effectEnabled = true;
bool englishMenu = true;

HMENU MakeEffectPopup(bool english) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 40005, L"Effect Mapping");
    AppendMenuW(menu, MF_STRING | MF_CHECKED, english ? 40020 : 40006, L"Enable Effect");
    AppendMenuW(menu, MF_STRING | MF_CHECKED, 40001, L"Auto Reload");
    AppendMenuW(menu, MF_STRING | MF_CHECKED, 40022, L"Auto Save Mapping File");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 40004, L"Reload All");
    AppendMenuW(menu, MF_STRING, 40002, L"Log Window");
    AppendMenuW(menu, MF_STRING, 40003, L"About MMEffect");
    AppendMenuW(menu, MF_STRING, 49999, L"Third-party command");
    return menu;
}

LRESULT CALLBACK FakeEffectWindow(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_COMMAND) {
        HMENU root = GetMenu(hwnd);
        HMENU popup = GetSubMenu(root, 0);
        if (LOWORD(wp) == 40006) {
            effectEnabled = (GetMenuState(popup, 40006, MF_BYCOMMAND) & MF_CHECKED) == 0;
            CheckMenuItem(popup, 40006, effectEnabled ? MF_CHECKED : MF_UNCHECKED);
            return 0;
        }
        if (LOWORD(wp) == 260) {
            RemoveMenu(root, 0, MF_BYPOSITION);
            DestroyMenu(popup);
            englishMenu = !englishMenu;
            AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(MakeEffectPopup(englishMenu)), L"MMEffect");
            return 0;
        }
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

std::wstring MenuCaption(HMENU menu, UINT id) {
    wchar_t text[256]{};
    GetMenuStringW(menu, id, text, 256, MF_BYCOMMAND);
    return text;
}

const wchar_t* const materialNames[][2] = {
    {L"髪_材質_日本語", nullptr}, {nullptr, L"English only"},
    {L"左袖", L"left sleeve"}, {L"", L""}, {nullptr, nullptr},
    {L"中文_😀", L""}, {L"", L"subset0"}};
int materialNameQueries = 0;
const wchar_t* __cdecl QueryMaterialName(std::uintptr_t object, int index, int english) {
    Check(object == 0x1234, "material lookup preserves opaque object identity");
    ++materialNameQueries;
    return materialNames[index][english ? 1 : 0];
}

void CheckMaterialNameFallback() {
    using namespace mikumikustudio;
    for (int index = 0; index < _countof(materialNames); ++index) {
        for (int english : {0, 1}) {
            materialNameQueries = 0;
            const auto* preferred = materialNames[index][english];
            const auto* other = materialNames[index][1 - english];
            const bool hasPreferred = preferred && *preferred;
            const auto* expected = hasPreferred ? preferred : other && *other ? other : preferred;
            Check(EffectMaterialNameWithFallback(QueryMaterialName, 0x1234,
                      index, english) == expected,
                  "MME preserves preferred Unicode names and falls back in either direction");
            Check(materialNameQueries == (hasPreferred ? 1 : 2),
                  "MME only queries the alternate language when the preferred field is empty");
        }
    }
    Check(EffectMaterialNameWithFallback(nullptr, 0, 0, 0) == nullptr,
          "missing name provider remains harmless");
    Check(!InstallEffectMaterialNameCompatibility() &&
          GetModuleHandleW(L"MMEffect.dll") == nullptr && GetModuleHandleW(L"MMHack.dll") == nullptr,
          "name fallback does not load optional effect DLLs");
}

void CheckMenuTranslation(HMENU menu) {
    using mikumikustudio::LocalizeEffectMenu;
    const int count = GetMenuItemCount(menu);
    const UINT enabled = GetMenuState(menu, 40006, MF_BYCOMMAND);
    const UINT disabled = GetMenuState(menu, 40004, MF_BYCOMMAND);
    for (int repeat = 0; repeat < 2; ++repeat) {
        LocalizeEffectMenu(menu, 2);
        for (const auto& item : {
                 std::pair<UINT, const wchar_t*>{40005, L"效果分配(&M)"},
                 {40006, L"启用效果(&E)\tCtrl+Shift+E"},
                 {40001, L"自动重载(&O)"},
                 {40022, L"自动保存效果分配文件(&S)"},
                 {40004, L"全部重载(&R)"},
                 {40002, L"日志窗口(&L)"},
                 {40003, L"关于 MMEffect(&A)"}})
            Check(MenuCaption(menu, item.first) == item.second, "MME command has its Chinese caption and access key");
        Check(GetMenuState(menu, 40006, MF_BYCOMMAND) == enabled &&
              GetMenuState(menu, 40004, MF_BYCOMMAND) == disabled,
              "localization preserves checked and disabled states");
        Check(GetMenuItemCount(menu) == count &&
              (GetMenuState(menu, 4, MF_BYPOSITION) & MF_SEPARATOR) != 0 &&
              MenuCaption(menu, 49999) == L"Third-party command",
              "localization preserves separators and unknown commands");
        LocalizeEffectMenu(menu, 0);
        Check(MenuCaption(menu, 40005) == L"エフェクト割当(&M)", "Japanese restores the native MME caption");
        LocalizeEffectMenu(menu, 1);
        Check(MenuCaption(menu, 40006) == L"Enable Effect(&E)\tCtrl+Shift+E",
              "English restores the native caption and keyboard shortcut");
    }
    LocalizeEffectMenu(nullptr, 2);
    HMENU empty = CreatePopupMenu();
    LocalizeEffectMenu(empty, 2);
    Check(GetMenuItemCount(empty) == 0, "missing MME commands are not inserted");
    DestroyMenu(empty);
    LocalizeEffectMenu(empty, 2);  // a destroyed handle must also be harmless
}
}

int main() {
    using namespace mikumikustudio;
    CheckMaterialNameFallback();
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof(app->state));
    g_Block = app.get();
    app->WindowLayoutReady() = 1;
    app->state.fpsOverlayFrameCount = 17;
    {
        ScopedSceneMutation load(*app);
        Check(app->SceneMutationInProgress(), "Loading suspends scene access");
        app->WindowLayoutReady() = 1; // A nested WM_SIZE must not reopen rendering.
        FrameDriver(app.get()); // Renderer/scene are deliberately uninitialized.
        {
            ScopedSceneMutation reset(*app);
            Check(app->SceneMutationInProgress(), "Nested abort/reset remains guarded");
        }
        FrameDriver(app.get());
        Check(app->SceneMutationInProgress() && app->state.fpsOverlayFrameCount == 17,
              "Partial scenes are not rendered after nested reset or resize");
    }
    Check(!app->SceneMutationInProgress() && app->WindowLayoutReady() == 1,
          "Scene access and previous layout gate restored on scope exit");
    // Exercise native command routing and popup recreation without loading a
    // third-party DLL: the fake follows MME's resource IDs and switch handler.
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EffectMenuRegression";
    wc.lpfnWndProc = FakeEffectWindow;
    Check(RegisterClassW(&wc) != 0, "Register menu test window");
    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
    Check(window != nullptr, "Create menu test window");
    HMENU menu = CreateMenu();
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(MakeEffectPopup(true)), L"MMEffect");
    SetMenu(window, menu);
    app->state.hwnd = window;
    Check(GetModuleHandleW(L"MMEffect.dll") == nullptr, "unit tests run without MME loaded");
    app->EnglishUI() = 2;
    RefreshEffectMenuLanguage(nullptr);
    RefreshEffectMenuLanguage(app.get());
    Check(GetModuleHandleW(L"MMEffect.dll") == nullptr &&
          MenuCaption(menu, 40005) == L"Effect Mapping",
          "without the DLL, localization does not load MME or alter matching menu IDs");
    InstallEffectMenuCompatibility(app.get());
    InstallEffectMenuCompatibility(app.get()); // Installing twice must not chain twice.
    Check(GetMenuState(menu, 40020, MF_BYCOMMAND) == UINT(-1) &&
          (GetMenuState(menu, 40006, MF_BYCOMMAND) & MF_CHECKED) != 0,
          "English enable item uses the native switch command");
    CheckMenuTranslation(GetSubMenu(menu, 0));
    SendMessageW(window, WM_COMMAND, 40006, 0);
    Check(!effectEnabled, "Menu command disables the engine");
    for (int i = 0; i < 3; ++i) {
        SendMessageW(window, WM_COMMAND, 260, 0);
        Check(GetMenuState(menu, 40006, MF_BYCOMMAND) == MF_UNCHECKED && !effectEnabled,
              "Language recreation preserves disabled state and command ID");
    }
    SendMessageW(window, WM_COMMAND, 40006, 0);
    Check(effectEnabled, "Menu command re-enables after language changes");
    SendMessageW(window, WM_COMMAND, 260, 0);
    Check((GetMenuState(menu, 40006, MF_BYCOMMAND) & MF_CHECKED) != 0 && effectEnabled,
          "Language recreation preserves enabled state");
    auto model = std::make_unique<mdl::ModelRecord>();
    auto replacement = std::make_unique<mdl::ModelRecord>();
    auto accessory = std::make_unique<mdl::AccessoryRecord>();
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get();
    // Explicit Chinese fallback, independent of the machine's active locale.
    renderer->localeTable[0] = _create_locale(LC_ALL, ".936");
    Check(renderer->localeTable[0] != nullptr, "Chinese locale available");
    app->ModelSlot(3) = reinterpret_cast<unsigned char*>(model.get());
    app->AccessorySlot(7) = accessory.get();
    model->comboSelIndex = 1;
    app->state.accessoryRenderSplitOrder = 1;
    app->state.activeRenderObject = model.get();

    BeginEffectObjectRegistration();
    Check(ExpGetCurrentObject() == 0, "Unregistered models must not reach MME draw hook");
    Check(ExpGetPmdOrder(0) == 2 && ExpGetCurrentObject() == 2,
          "Registered model uses combined order");
    for (int i = 0; i < 1000; ++i)
        Check(ExpGetCurrentObject() == 2, "Repeated current-object cache hit");
    model->comboSelIndex = 4;
    Check(ExpGetCurrentObject() == 5, "Order changes invalidate current-object hit");
    model->comboSelIndex = 1;
    ++app->state.renderPassCount;
    Check(ExpGetCurrentObject() == 2, "Repeat pass refreshes current-object cache");
    InvalidateEffectCurrentObjectCache();
    Check(ExpGetCurrentObject() == 2, "Scene-generation invalidation preserves registered order");
    Check(ExpGetPmdID(0) == static_cast<std::uint32_t>(
              reinterpret_cast<std::uintptr_t>(model.get())), "Model ID ABI");
    Check(ExpGetAcsID(0) == static_cast<std::uint32_t>(
              reinterpret_cast<std::uintptr_t>(accessory.get())), "Accessory ID ABI");
    Check(ExpGetPmdID(1) == 0 && ExpGetAcsID(1) == 0, "Missing IDs are zero");

    app->state.activeRenderObject = accessory.get();
    Check(ExpGetCurrentObject() == 0, "Model registration must not register accessory");
    Check(ExpGetAcsOrder(0) == -1 && ExpGetCurrentObject() == -1,
          "Pre-model accessory order");
    app->state.accessoryRenderSplitOrder = 0;
    Check(ExpGetAcsOrder(0) == 2 && ExpGetCurrentObject() == 2,
          "Post-model accessory order");

    // Simulate deletion followed by reuse of the same occupied ordinal.
    app->ModelSlot(3) = reinterpret_cast<unsigned char*>(replacement.get());
    replacement->comboSelIndex = 1;
    BeginEffectObjectRegistration();
    Check(ExpGetCurrentObject() == 0, "Accessory registration expires each frame");
    app->state.activeRenderObject = replacement.get();
    Check(ExpGetCurrentObject() == 0, "Replacement must not inherit old registration");
    Check(ExpGetPmdOrder(0) == 1 && ExpGetCurrentObject() == 1,
          "Replacement registers normally");

    wcscpy_s(replacement->path, L"C:\\models\\星穹铁道—遐蝶.pmx");
    wchar_t decoded[256]{};
    Check(MultiByteToWideChar(936, 0, ExpGetPmdFilename(0), -1, decoded, 256) > 0 &&
              wcscmp(decoded, replacement->path) == 0,
          "Chinese model filename survives locale fallback");
    wcscpy_s(accessory->sourcePath, L"C:\\effects\\镰刀.x");
    Check(MultiByteToWideChar(936, 0, ExpGetAcsFilename(0), -1, decoded, 256) > 0 &&
              wcscmp(decoded, accessory->sourcePath) == 0,
          "Chinese accessory filename survives locale fallback");
    wcscpy_s(replacement->path, L"C:\\models\\ミク.pmx");
    Check(MultiByteToWideChar(932, 0, ExpGetPmdFilename(0), -1, decoded, 256) > 0 &&
              wcscmp(decoded, replacement->path) == 0, "CP932 path remains compatible");
    Check(ExpGetPmdFilename(1) == nullptr && ExpGetAcsFilename(1) == nullptr,
          "Missing filenames remain null");

    // Language changes must touch the named UI flag, not x86 offset 12740
    // (which is inside boneListRowType on x64), and use ABI-correct names.
    app->AccessorySlot(7) = nullptr;
    app->state.optflag[0] = 1;
    HWND combo = CreateWindowA("COMBOBOX", "", WS_CHILD | CBS_DROPDOWNLIST,
        0, 0, 200, 100, window, reinterpret_cast<HMENU>(436), wc.hInstance, nullptr);
    Check(combo != nullptr, "Create model selector");
    strcpy_s(replacement->name, "JP model");
    strcpy_s(replacement->nameEn, "EN model");
    wcscpy_s(replacement->path, L"C:\\模型\\洛天依.pmx");
    std::memset(replacement->boneListRowType, 0x5A, sizeof(replacement->boneListRowType));
    for (int language : {1, 2, 0}) {
        app->state.englishUI = static_cast<unsigned char>(language);
        LocalizeUI(app.get());
        Check(replacement->physicsFlags == (language == 1), "Model UI language flag");
        for (auto type : replacement->boneListRowType)
            Check(type == 0x5A, "Language change preserves model row metadata");
        wchar_t name[64]{};
        SendMessageW(combo, CB_GETLBTEXT, 1, reinterpret_cast<LPARAM>(name));
        Check(wcscmp(name, language == 1 ? L"EN model" : L"JP model") == 0,
              "Model selector displays the model metadata in the selected language");
        Check(std::strcmp(replacement->name, "JP model") == 0 &&
              std::strcmp(replacement->nameEn, "EN model") == 0 &&
              std::wcscmp(replacement->path, L"C:\\模型\\洛天依.pmx") == 0,
              "display language leaves model names and source identity untouched");
    }
    DestroyWindow(window);
    app->state.hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    _free_locale(renderer->localeTable[0]);
    app->ModelSlot(3) = nullptr;
    app->AccessorySlot(7) = nullptr;
    app->Renderer() = nullptr;

    // A ground body remains after scene models have been disposed. The old
    // x86 collision-array offsets crashed here in a normal x64 WM_CLOSE.
    auto* physics = static_cast<PhysicsScene*>(std::calloc(1, sizeof(PhysicsScene)));
    app->Physics() = physics;
    physics->collisionConfig = new btDefaultCollisionConfiguration();
    physics->dispatcher = new btCollisionDispatcher(physics->collisionConfig);
    physics->broadphase = new bt32BitAxisSweep3(btVector3(-100, -100, -100),
                                             btVector3(100, 100, 100));
    physics->solver = new btSequentialImpulseConstraintSolver();
    physics->world = new btDiscreteDynamicsWorld(physics->dispatcher,
        physics->broadphase, physics->solver, physics->collisionConfig);
    auto* shape = new btBoxShape(btVector3(1, 1, 1));
    auto* motion = new btDefaultMotionState(btTransform::getIdentity());
    physics->groundBody = new btRigidBody(0, motion, shape);
    physics->world->addRigidBody(physics->groundBody);
    physics->world->addConstraint(new btPoint2PointConstraint(
        *physics->groundBody, btVector3(0, 0, 0)));
    ShutdownCleanup(app.get());
    Check(app->Physics() == nullptr, "Physics world teardown completes");
    g_Block = nullptr;
    std::puts("PASS effect menu, language, scene mutation, IDs, order lifecycle, filenames, and teardown");
}
