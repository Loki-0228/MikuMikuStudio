// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_input_focus.hpp"
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
    HWND wrapper = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"property input", WS_CHILD | WS_VISIBLE,
                                   0, 50, 200, 50, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND nested = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                  0, 0, 180, 40, wrapper, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND field = CreateWindowExW(0, L"EDIT", L"draft", WS_CHILD | WS_VISIBLE,
                                 0, 0, 100, 20, nested, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND insideButton = CreateWindowExW(0, L"BUTTON", L"input child", WS_CHILD | WS_VISIBLE,
                                        100, 0, 70, 20, nested, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(wrapper && nested && field && insideButton, "create native property input descendants");
    Check(SetPropW(wrapper, plugins::kPropertyInputWindowMarker, reinterpret_cast<HANDLE>(1)) != FALSE,
          "attach a presence-only native input marker");
    Check(plugins::OwnsPropertyInput(wrapper) && plugins::OwnsPropertyInput(nested) &&
              plugins::OwnsPropertyInput(field) && plugins::OwnsPropertyInput(insideButton),
          "native input ownership follows the wrapper and descendant HWNDs");
    Check(!plugins::OwnsPropertyInput(nullptr) && !plugins::OwnsPropertyInput(owner) && !plugins::OwnsPropertyInput(edit),
          "the marker does not claim the owner or an ordinary panel edit");
    pressB(insideButton); pressB(wrapper); pressB(field);
    Check(app->state.blackBackgroundEnabled == 0, "all native INPUT descendants keep the letter ladder inactive");
    Check(RemovePropW(wrapper, plugins::kPropertyInputWindowMarker) == reinterpret_cast<HANDLE>(1),
          "remove the native input marker without dereferencing its value");
    Check(!plugins::OwnsPropertyInput(field) && !plugins::OwnsPropertyInput(insideButton),
          "removing a marker releases descendant keyboard ownership");
    pressB(insideButton);
    Check(app->state.blackBackgroundEnabled == 1, "the original button shortcut resumes after marker removal");
    pressB(edit);
    Check(app->state.blackBackgroundEnabled == 1, "ordinary native EDIT retains its existing shortcut exclusion");
    DestroyWindow(wrapper);
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

    // PolledKeyState is a plain value: ineligible until the first poll, an
    // edge on the first eligible poll, no edge while eligibility is sustained,
    // and a fresh history whenever either owner identity changes.  The owner
    // tokens below are synthetic pointers; the state only compares them.
    {
        PolledKeyState polled;
        Check(!polled.Eligible() && !polled.Regained(),
              "Default PolledKeyState is ineligible");
        Check(!polled.HasForegroundOverride() && polled.ForegroundOverride() == nullptr,
              "Default PolledKeyState has no foreground override");

        int mainToken = 0;
        int floatingToken = 0;
        int rebuiltToken = 0;
        void* const mainId = &mainToken;
        void* const floatingId = &floatingToken;
        void* const rebuiltId = &rebuiltToken;

        Check(polled.Advance(mainId, nullptr, true),
              "First eligible poll regains eligibility");
        Check(polled.Eligible() && polled.Regained(),
              "First eligible poll is eligible with an edge");
        Check(!polled.Advance(mainId, nullptr, true),
              "Sustained eligibility must not report a regain");
        Check(polled.Eligible() && !polled.Regained(),
              "Sustained poll stays eligible without an edge");
        Check(!polled.Advance(mainId, nullptr, false),
              "Losing eligibility is not a regain");
        Check(!polled.Eligible() && !polled.Regained(),
              "Loss clears both the latch and the edge");
        Check(!polled.Advance(mainId, nullptr, false),
              "Staying ineligible is not a regain");
        Check(polled.Advance(mainId, nullptr, true),
              "The same owners regain after a loss");
        Check(polled.Regained(), "Recovery reports the regained edge");

        Check(polled.Advance(rebuiltId, nullptr, true),
              "Replacing the main owner restarts from ineligible");
        Check(!polled.Advance(rebuiltId, nullptr, true),
              "The rebuilt main owner cannot inherit the old edge");
        Check(polled.Advance(rebuiltId, floatingId, true),
              "Replacing the floating owner restarts from ineligible");
        Check(!polled.Advance(rebuiltId, floatingId, true),
              "The full replacement owner pair then sustains");
        Check(!polled.Advance(rebuiltId, floatingId, false),
              "Loss with both owners is not a regain");
        Check(polled.Advance(rebuiltId, floatingId, true),
              "Recovery with both owners regains");

        polled.SetForegroundOverride(nullptr);
        Check(polled.HasForegroundOverride() && polled.ForegroundOverride() == nullptr,
              "An explicit nullptr override is still enabled");
        polled.SetForegroundOverride(mainId);
        Check(polled.HasForegroundOverride() && polled.ForegroundOverride() == mainId,
              "A later override replaces the explicit nullptr");
        polled.ClearForegroundOverride();
        Check(!polled.HasForegroundOverride() && polled.ForegroundOverride() == nullptr,
              "Clearing the override disables it");
        Check(polled.Eligible() && polled.Regained(),
              "Clearing the override leaves the eligibility latch unchanged");

        PolledKeyState reconstructed;
        Check(!reconstructed.Eligible() && !reconstructed.Regained(),
              "A fresh reconstructed PolledKeyState starts ineligible");
        Check(reconstructed.Advance(mainId, floatingId, true),
              "A fresh reconstructed instance regains from scratch");
        PolledKeyState independent;
        Check(!independent.Eligible(),
              "A second instance starts ineligible while the first is eligible");
        independent.Advance(mainId, nullptr, false);
        Check(polled.Eligible() && !independent.Eligible(),
              "Instances keep independent eligibility latches");
        std::puts("PASS polled key state value semantics");
    }

    // The seven polled-key helpers.  The owner tokens below stand in for HWNDs
    // and reach only identity comparisons: Advance, the latch readers, and the
    // foreground helpers (with the test override enabled) make no Win32 call on
    // a synthetic handle, and no real foreground window is activated.
    {
        int mainToken = 0;
        int floatingToken = 0;
        int replacementToken = 0;
        int foreignToken = 0;
        void* const mainId = &mainToken;
        void* const floatingId = &floatingToken;
        void* const replacementId = &replacementToken;
        void* const foreignId = &foreignToken;

        app->Hwnd() = mainId;
        app->state.floatingWindow = reinterpret_cast<HWND>(floatingId);

        Check(PollKeyInputEligibleWithForeground(app.get(), mainId),
              "Foreground matching the main owner is eligible");
        Check(PollKeyInputEligibleWithForeground(app.get(), floatingId),
              "Foreground matching the floating owner is eligible");
        Check(!PollKeyInputEligibleWithForeground(app.get(), foreignId),
              "A foreign foreground is not eligible");
        Check(!PollKeyInputEligibleWithForeground(app.get(), nullptr),
              "A null foreground is not eligible");
        app->state.floatingWindow = nullptr;
        Check(!PollKeyInputEligibleWithForeground(app.get(), floatingId),
              "A floating identity cannot match when the app has none");
        Check(PollKeyInputEligibleWithForeground(app.get(), mainId),
              "The main owner is eligible without a floating window");
        app->state.floatingWindow = reinterpret_cast<HWND>(floatingId);

        Check(!PollKeyEligible(app.get()) && !PollKeyRegained(app.get()),
              "A freshly constructed app has no polled keyboard history");

        SetPollKeyForegroundForTests(app.get(), mainId);
        Check(app->PolledKeys().HasForegroundOverride(),
              "The test foreground override enables");
        Check(PollKeyInputEligible(app.get()),
              "An override matching the main owner is eligible");
        SetPollKeyForegroundForTests(app.get(), floatingId);
        Check(PollKeyInputEligible(app.get()),
              "An override matching the floating owner is eligible");
        SetPollKeyForegroundForTests(app.get(), foreignId);
        Check(!PollKeyInputEligible(app.get()),
              "A foreign override is not eligible");
        SetPollKeyForegroundForTests(app.get(), nullptr);
        Check(app->PolledKeys().HasForegroundOverride() &&
                  app->PolledKeys().ForegroundOverride() == nullptr,
              "An explicit nullptr override stays enabled");
        Check(!PollKeyInputEligible(app.get()),
              "An explicit nullptr override reports ineligible");
        ClearPollKeyForegroundForTests(app.get());
        Check(!app->PolledKeys().HasForegroundOverride() &&
                  app->PolledKeys().ForegroundOverride() == nullptr,
              "Clearing the test override disables it");

        AdvancePollKeyEligibility(app.get(), false);
        Check(!PollKeyEligible(app.get()) && !PollKeyRegained(app.get()),
              "The first ineligible poll is not a regain");
        AdvancePollKeyEligibility(app.get(), true);
        Check(PollKeyEligible(app.get()) && PollKeyRegained(app.get()),
              "The first eligible poll regains through the helper");
        AdvancePollKeyEligibility(app.get(), true);
        Check(PollKeyEligible(app.get()) && !PollKeyRegained(app.get()),
              "Sustained eligibility does not regain through the helper");
        AdvancePollKeyEligibility(app.get(), false);
        Check(!PollKeyEligible(app.get()) && !PollKeyRegained(app.get()),
              "Loss clears the helper latch");
        AdvancePollKeyEligibility(app.get(), true);
        Check(PollKeyRegained(app.get()), "Recovery regains through the helper");

        SetPollKeyForegroundForTests(app.get(), nullptr);
        ClearPollKeyForegroundForTests(app.get());
        Check(PollKeyEligible(app.get()) && PollKeyRegained(app.get()),
              "Clearing the override leaves the helper latch untouched");

        app->Hwnd() = replacementId;
        AdvancePollKeyEligibility(app.get(), true);
        Check(PollKeyRegained(app.get()),
              "Replacing the main owner regains through the helper");
        AdvancePollKeyEligibility(app.get(), true);
        Check(!PollKeyRegained(app.get()),
              "The replacement main owner pair then sustains");
        app->state.floatingWindow = reinterpret_cast<HWND>(foreignId);
        AdvancePollKeyEligibility(app.get(), true);
        Check(PollKeyRegained(app.get()),
              "Replacing the floating owner regains through the helper");
        AdvancePollKeyEligibility(app.get(), true);
        Check(!PollKeyRegained(app.get()),
              "The full replacement owner pair then sustains");

        Check(!PollKeyInputEligibleWithForeground(nullptr, mainId),
              "A null app is never eligible with a foreground");
        Check(!PollKeyInputEligible(nullptr), "A null app is never eligible");
        Check(!PollKeyEligible(nullptr) && !PollKeyRegained(nullptr),
              "A null app has no polled keyboard latch");
        AdvancePollKeyEligibility(nullptr, true);
        SetPollKeyForegroundForTests(nullptr, mainId);
        ClearPollKeyForegroundForTests(nullptr);
        std::puts("PASS polled key eligibility helpers");

        app->Hwnd() = nullptr;
        app->state.floatingWindow = nullptr;
        AdvancePollKeyEligibility(app.get(), false);
    }
    return 0;
}
