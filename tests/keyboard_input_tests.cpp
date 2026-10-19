// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace mikumikustudio {
void ConsumeLetterHotkeys(MMDApp*);
void DispatchLetterHotkeys(MMDApp*, HWND);
void CmdControl450(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void CmdControl500(MMDApp*, HWND, std::uint16_t, std::uint16_t);
}

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

int main() {
    using namespace mikumikustudio;
    unsigned char keys[256]{};
    LetterHotkeyState input;
    constexpr int aSlot = 5;
    input.Update(keys, true);
    for (int i = 0; i < 10000; ++i) {
        input.Update(keys, true);
        for (int slot = 0; slot < 18; ++slot)
            Check(!input.Pressed(slot), "Idle frames must never synthesize shortcuts");
    }
    std::puts("PASS idle frames");

    for (int slot = 0; slot < 18; ++slot) {
        std::memset(keys, 0, sizeof(keys));
        input.Update(keys, true);
        keys[kLetterVirtualKeys[slot]] = 0x80;
        input.Update(keys, true);
        Check(input.Pressed(slot), "Real letter press must fire once");
        input.Update(keys, true);
        Check(input.Value(slot) == 3, "Held letter must not repeat a press");
        keys[kLetterVirtualKeys[slot]] = 0;
        input.Update(keys, true);
        Check(input.Value(slot) == 2, "Letter release");
        keys[kLetterVirtualKeys[slot]] = 0x80;
        input.Update(keys, true);
        Check(input.Pressed(slot), "Repress immediately after release must work");
    }
    std::puts("PASS all 18 letters, hold, release, rapid repress");

    std::memset(keys, 0, sizeof(keys));
    input.Update(keys, true);
    for (int vk = 0x60; vk <= 0x7B; ++vk) keys[vk] = 0x80; // numpad and function keys
    input.Update(keys, true);
    for (int slot = 0; slot < 18; ++slot)
        Check(!input.Pressed(slot), "Lowercase ASCII must not poll numpad/function keys");
    std::puts("PASS virtual-key mapping");

    keys['A'] = 0x80;
    input.Update(keys, false); // background window or text input
    Check(!input.Pressed(aSlot), "Ineligible input must not dispatch A");
    input.Update(keys, true);
    Check(!input.Pressed(aSlot), "Reactivation while held must not manufacture A");
    keys['A'] = 0;
    input.Update(keys, true);
    keys['A'] = 0x80;
    input.Update(keys, true);
    Check(input.Pressed(aSlot), "Fresh A after reactivation must work");
    std::puts("PASS focus loss and held-key reactivation");

    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof(app->state));
    std::memset(keys, 0, sizeof(keys));
    app->LetterHotkeys().Update(keys, true);
    app->state.dialogFlags[aSlot] = 1;
    Check(!app->LetterHotkeys().Pressed(aSlot), "Dialog flag must not become keyboard input");
    Check(!LetterHotkeyInputAllowed(app.get()), "No app window must reject shortcuts");
    keys['A'] = 0x80;
    app->LetterHotkeys().Update(keys, true);
    ConsumeLetterHotkeys(app.get()); // no foreground window, even with an A edge
    std::puts("PASS dialog flags isolated and inactive consumer");

    // Focus rule: a cleared focus must stay eligible (the panel pump used to
    // SetFocus(GetDlgItem(main, 0)) == SetFocus(NULL), which silently disabled
    // every letter shortcut and the Delete gate); a text field must not.
    HWND owner = CreateWindowExW(0, L"STATIC", L"hotkey test", WS_OVERLAPPED,
                                 0, 0, 320, 240, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    Check(owner != nullptr, "create hotkey test window");
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE,
                                0, 0, 100, 20, owner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    Check(edit != nullptr, "create hotkey test edit");
    Check(FocusAllowsLetterHotkeys(owner, nullptr, nullptr),
          "Cleared focus must keep letter shortcuts alive");
    Check(FocusAllowsLetterHotkeys(owner, nullptr, owner),
          "Main-window focus accepts letter shortcuts");
    Check(!FocusAllowsLetterHotkeys(owner, nullptr, edit),
          "Text-field focus must keep letter shortcuts off");
    HWND button = CreateWindowExW(0, L"BUTTON", L"select", WS_CHILD | WS_VISIBLE,
                                  0, 25, 100, 20, owner, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    Check(button != nullptr, "create hotkey test button");
    app->Hwnd() = owner;
    auto pressB = [&](HWND focus) {
        std::memset(keys, 0, sizeof(keys));
        app->LetterHotkeys().Update(keys, true);
        keys['B'] = 0x80;
        app->LetterHotkeys().Update(keys, true);
        DispatchLetterHotkeys(app.get(), focus);
    };
    pressB(button);
    Check(app->state.blackBackgroundEnabled == 1, "consumer accepts panel-button focus");
    pressB(nullptr);
    Check(app->state.blackBackgroundEnabled == 0, "consumer accepts cleared focus");
    pressB(edit);
    Check(app->state.blackBackgroundEnabled == 0, "consumer rejects text-field focus");
    DestroyWindow(owner);
    app->Hwnd() = nullptr;
    std::puts("PASS focus rule for the letter ladder");

    for (int slot : {0, 254, 255}) {
        app->SelectedModelSlot() = static_cast<std::uint8_t>(slot);
        for (int id = 494; id <= 498; ++id)
            CmdControl450(app.get(), nullptr, static_cast<std::uint16_t>(id), 0);
        // 500 is re-dispatched from the frame loop on Enter and must also
        // tolerate a missing model (RegisterSelectedBoneKeys does, but the
        // maxFrame read after it does not).
        CmdControl500(app.get(), nullptr, 500, 0);
        CmdControl500(app.get(), nullptr, 501, 0);
    }
    std::puts("PASS bone commands with empty and invalid model slots");

    auto model = std::make_unique<mdl::ModelRecord>();
    app->SelectedModelSlot() = 0;
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
    model->boneCount = 1;
    CmdControl450(app.get(), nullptr, 494, 0);
    CmdControl500(app.get(), nullptr, 501, 0);
    Check(model->selectedBone == 0, "Incomplete model must not be modified");
    std::puts("PASS incomplete bone buffers");
    return 0;
}
