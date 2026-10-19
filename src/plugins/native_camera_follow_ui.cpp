#include "native_camera_follow_ui.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ui_control_readout.hpp"
#include <CommCtrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
constexpr wchar_t kDraftOwner[] = L"MMS.NativeReadout.PendingEditOwner";
void CheckButton(HWND window, int id, bool checked) noexcept {
    const auto button = GetDlgItem(window, id);
    const WPARAM state = checked ? BST_CHECKED : BST_UNCHECKED;
    if (button && SendMessageW(button, BM_GETCHECK, 0, 0) != state) {
        SendMessageW(button, BM_SETCHECK, state, 0);
        InvalidateRect(button, nullptr, FALSE);
    }
}
int BoundedInteger(double value) noexcept {
    return static_cast<int>((std::clamp)(value,
        double((std::numeric_limits<int>::min)()), double((std::numeric_limits<int>::max)())));
}
void Text(HWND window, int id, const char* value) noexcept {
    const auto edit = GetDlgItem(window, id);
    if (!SendMessageW(edit, EM_GETMODIFY, 0, 0))
        ui::SetReadoutTextIfChangedA(edit, value);
}
void Slider(HWND window, int id, int value) noexcept {
    ui::SetTrackbarPositionIfChanged(GetDlgItem(window, id), value);
}
void FloatingGlobalControls(MMDApp& app, HWND floating) noexcept {
    // The current detached panel contains 536..557. Checking for each control
    // also supports a panel which exposes the other existing global controls.
    CheckButton(floating, panel::kPerspectiveCheckbox, app.CameraPerspective() == 0);
    char text[64]{};
    if (std::isfinite(app.CameraFov())) {
        std::snprintf(text, sizeof(text), "%.6g", double(app.CameraFov()));
        Text(floating, panel::kFovEdit, text);
        Slider(floating, panel::kFovSlider,
            static_cast<int>(std::lround((std::clamp)(app.CameraFov(), 1.f, 125.f))));
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
        const double color = double(app.LightColor()[axis]) * 256.0;
        const double direction = app.LightDirection()[axis];
        if (!std::isfinite(color) || !std::isfinite(direction)) continue;
        std::snprintf(text, sizeof(text), "%3d", BoundedInteger(color));
        Text(floating, 461 + int(axis), text);
        std::snprintf(text, sizeof(text), "%+.1f", direction);
        Text(floating, 464 + int(axis), text);
        Slider(floating, 455 + int(axis), BoundedInteger(color));
        const auto slider = GetDlgItem(floating, 458 + int(axis));
        if (slider) {
            const auto current = SendMessageW(slider, TBM_GETPOS, 0, 0);
            if (static_cast<float>(double(current) / 100.0) != app.LightDirection()[axis]) {
                const int nearest = BoundedInteger(std::round(direction * 100.0));
                Slider(floating, 458 + int(axis),
                    static_cast<float>(double(nearest) / 100.0) == app.LightDirection()[axis]
                        ? nearest : BoundedInteger(direction * 100.0));
            }
        }
    }
    const double shadow = 10000.0 - double(app.ShadowDistance()) * 100000.0;
    if (std::isfinite(shadow) && shadow >= double((std::numeric_limits<int>::min)()) + .5 &&
        shadow <= double((std::numeric_limits<int>::max)()) - .5) {
        // Match full-profile ShadowSliderPosition, including out-of-range
        // readout text. The native trackbar clamps only its displayed thumb.
        const int position = static_cast<int>(std::lround(shadow));
        std::snprintf(text, sizeof(text), "%d", position);
        Text(floating, panel::kSelfShadowRangeEdit, text);
        Slider(floating, panel::kSelfShadowRangeSlider, position);
    }
    CheckButton(floating, panel::kEditOffCheckbox, app.SelfShadowMode() == 0);
    CheckButton(floating, panel::kEditMode1Checkbox, app.SelfShadowMode() == 1);
    CheckButton(floating, panel::kEditMode2Checkbox,
        app.SelfShadowMode() != 0 && app.SelfShadowMode() != 1);
}
void GravityControls(MMDApp& app) noexcept {
    const auto dialog = app.GravitySettingDialog();
    if (!dialog || !IsWindow(dialog)) return;
    char text[64]{};
    const float values[] = { app.GravityMagnitude(), app.GravityX(), app.GravityY(), app.GravityZ() };
    for (unsigned field = 0; field < 4; ++field) if (std::isfinite(values[field])) {
        std::snprintf(text, sizeof(text), "%3.2f", double(values[field]));
        Text(dialog, panel::kGravityAccelEdit + int(field), text);
        if (field) Slider(dialog, panel::kGravityDirXSlider + int(field) - 1,
            BoundedInteger(double(values[field]) * 100.0));
    }
    std::snprintf(text, sizeof(text), "%d", app.GravityNoise());
    Text(dialog, panel::kGravityNoiseEdit, text);
    const bool enabled = app.GravityNoiseEnabled() != 0;
    CheckButton(dialog, panel::kGravityNoiseCheckbox, enabled);
    if (const auto noise = GetDlgItem(dialog, panel::kGravityNoiseEdit))
        if ((IsWindowEnabled(noise) != FALSE) != enabled) EnableWindow(noise, enabled);
}
}

struct ScopedFollowReadoutDrafts::Impl {
    inline static thread_local Impl* active = nullptr;
    MMDApp* app = nullptr;
    Impl* previous = nullptr;
    struct Draft {
        HWND edit = nullptr, parent = nullptr;
        int id = 0;
        std::wstring text;
        std::vector<wchar_t> comparison;
        DWORD first = 0, last = 0;
        HANDLE previousOwner = nullptr;
        bool tagged = false;
    };
    std::vector<Draft> drafts;
    std::exception_ptr error;

    static BOOL CALLBACK Collect(HWND edit, LPARAM context) noexcept {
        auto& self = *reinterpret_cast<Impl*>(context);
        try {
            wchar_t name[16]{};
            if (!GetClassNameW(edit, name, int(std::size(name))) || _wcsicmp(name, L"Edit") ||
                !SendMessageW(edit, EM_GETMODIFY, 0, 0)) return TRUE;
            // A child may be enumerated twice when host roots are nested.
            for (const auto& saved : self.drafts) if (saved.edit == edit) return TRUE;
            Draft draft;
            draft.edit = edit; draft.parent = GetParent(edit); draft.id = GetDlgCtrlID(edit);
            const int length = GetWindowTextLengthW(edit);
            draft.text.resize(std::size_t(length) + 1);
            const int copied = GetWindowTextW(edit, draft.text.data(), length + 1);
            draft.text.resize(std::size_t(copied));
            draft.comparison.resize(draft.text.size() + 1);
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&draft.first),
                reinterpret_cast<LPARAM>(&draft.last));
            self.drafts.push_back(std::move(draft));
            return TRUE;
        } catch (...) { self.error = std::current_exception(); return FALSE; }
    }
    explicit Impl(MMDApp& value) : app(&value) {
        bake::ScopedUiInputGate input;
        const HWND roots[] = {value.MainWindow(), value.FloatingWindow(), value.GravitySettingDialog()};
        for (const auto root : roots) {
            if (root && IsWindow(root)) EnumChildWindows(root, Collect, reinterpret_cast<LPARAM>(this));
            if (error) std::rethrow_exception(error);
        }
        // SetProp is intentionally after all C++ allocation. A destroyed edit
        // loses the marker, so a recycled HWND never receives an old draft.
        for (auto& draft : drafts) {
            if (!IsWindow(draft.edit) || GetParent(draft.edit) != draft.parent ||
                GetDlgCtrlID(draft.edit) != draft.id) continue;
            draft.previousOwner = GetPropW(draft.edit, kDraftOwner);
            if (!SetPropW(draft.edit, kDraftOwner, reinterpret_cast<HANDLE>(this))) {
                Untag();
                throw std::runtime_error("Cannot preserve a pending native edit");
            }
            draft.tagged = true;
        }
        previous = active;
        active = this;
    }
    bool Current(const Draft& draft) const noexcept {
        return draft.tagged && IsWindow(draft.edit) && GetParent(draft.edit) == draft.parent &&
            GetDlgCtrlID(draft.edit) == draft.id && GetPropW(draft.edit, kDraftOwner) == reinterpret_cast<HANDLE>(const_cast<Impl*>(this));
    }
    void Untag() noexcept {
        for (auto& draft : drafts) if (Current(draft)) {
            if (draft.previousOwner) SetPropW(draft.edit, kDraftOwner, draft.previousOwner);
            else RemovePropW(draft.edit, kDraftOwner);
            draft.tagged = false;
        }
    }
    ~Impl() {
        bake::ScopedUiInputGate input;
        for (auto& draft : drafts) if (Current(draft)) {
            const int length = GetWindowTextLengthW(draft.edit);
            bool same = length >= 0 && std::size_t(length) == draft.text.size();
            if (same) {
                const int copied = GetWindowTextW(draft.edit, draft.comparison.data(), length + 1);
                same = copied == length && std::equal(draft.text.begin(), draft.text.end(), draft.comparison.begin());
            }
            if (!same) SetWindowTextW(draft.edit, draft.text.c_str());
            if (!Current(draft)) continue;
            DWORD first = 0, last = 0;
            SendMessageW(draft.edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
            if (first != draft.first || last != draft.last)
                SendMessageW(draft.edit, EM_SETSEL, draft.first, draft.last);
            if (!SendMessageW(draft.edit, EM_GETMODIFY, 0, 0))
                SendMessageW(draft.edit, EM_SETMODIFY, TRUE, 0);
        }
        Untag();
        active = previous;
    }
};
ScopedFollowReadoutDrafts::ScopedFollowReadoutDrafts(MMDApp& app) {
    // The command captures before publication. Its nested readout helpers
    // reuse that scope and cannot allocate another draft after data commits.
    for (auto* saved = Impl::active; saved; saved = saved->previous)
        if (saved->app == &app) return;
    impl_ = std::make_unique<Impl>(app);
}
ScopedFollowReadoutDrafts::~ScopedFollowReadoutDrafts() = default;

void RefreshBoundCameraFollowFlags(MMDApp& app) noexcept {
    bake::ScopedUiInputGate input;
    const bool follow = app.state.followCameraEnabled != 0;
    const auto main = app.MainWindow();
    if (main && IsWindow(main)) {
        CheckButton(main, panel::kFollowCameraCheckbox, follow);
        if (const auto menu = GetMenu(main)) {
            const auto state = GetMenuState(menu, 247, MF_BYCOMMAND);
            if (state != UINT(-1) && bool(state & MF_CHECKED) != follow)
                CheckMenuItem(menu, 247, MF_BYCOMMAND | (follow ? MF_CHECKED : MF_UNCHECKED));
        }
    }
    const auto floating = app.FloatingWindow();
    if (floating && floating != main && IsWindow(floating))
        CheckButton(floating, panel::kFollowCameraCheckbox, follow);
}
void RefreshBoundCameraFollowReadout(MMDApp& app) noexcept {
    RefreshBoundCameraFollowFlags(app);
    Boundary([&]() -> MmsResult {
        bake::ScopedUiInputGate input;
        const auto main = app.MainWindow();
        const auto floating = app.FloatingWindow();
        if (floating && floating != main && IsWindow(floating)) {
            FloatingGlobalControls(app, floating);
        }
        GravityControls(app);
        return MMS_OK;
    });
}
}
#endif
