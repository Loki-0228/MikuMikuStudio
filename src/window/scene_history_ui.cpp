#include "mikumikustudio/scene_history_ui.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_order_projection.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/ui_translation.hpp"

namespace mikumikustudio::scene_history_ui {
namespace {
constexpr int kAccessorySlots = 255;
struct ModelRow { int slot, order; const unsigned char* object; std::wstring name; };
struct AccessoryRow { int slot, order; const mdl::AccessoryRecord* object; std::wstring name; };
struct Item { std::wstring text; LPARAM data = -1; };
struct ComboPlan {
    HWND control;
    std::vector<Item> items;
    int selected = -1;
    bool rebuild = false;
};

std::wstring Label(unsigned char language, const char* english, const wchar_t* japanese) {
    if (language == kUiJapanese) return japanese;
    if (language == kUiChinese)
        if (const auto* translated = ChineseUiText(english)) return translated;
    return std::wstring(english, english + std::char_traits<char>::length(english));
}
template<class Row> int FindObject(const std::vector<Row>& rows, const void* object) noexcept {
    if (object)
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].object == object) return static_cast<int>(i);
    return -1;
}
const ModelRow* Parent(const std::vector<ModelRow>& models, int slot) noexcept {
    for (const auto& row : models) if (row.slot == slot) return &row;
    return nullptr;
}
bool BoneItems(ComboPlan& plan, const ModelRow* parent, int selected, bool english) {
    if (!parent) return true;
    const auto& model = *mdl::Mdl(parent->object);
    if (model.boneCount && !model.boneTable) return false;
    plan.items.reserve(model.boneCount);
    for (std::uint32_t i = 0; i < model.boneCount; ++i) {
        const auto& bone = model.boneTable[i];
        if (bone.type >= mdl::BoneType::InertTip && bone.type != mdl::BoneType::FixedAxis) continue;
        if (static_cast<int>(i) == selected) plan.selected = static_cast<int>(plan.items.size());
        plan.items.push_back({text_encoding::BoneLabel(bone, english), static_cast<LPARAM>(i)});
    }
    return true;
}
// All comparison buffers are allocated before any control or selection write.
bool Inspect(ComboPlan& plan) {
    const auto count = SendMessageW(plan.control, CB_GETCOUNT, 0, 0);
    if (count < 0) return false;
    plan.rebuild = count != static_cast<LRESULT>(plan.items.size());
    for (LRESULT row = 0; !plan.rebuild && row < count; ++row) {
        const auto& item = plan.items[static_cast<std::size_t>(row)];
        const auto length = SendMessageW(plan.control, CB_GETLBTEXTLEN, row, 0);
        if (length < 0) return false;
        if (static_cast<std::size_t>(length) != item.text.size() ||
            SendMessageW(plan.control, CB_GETITEMDATA, row, 0) != item.data) {
            plan.rebuild = true;
            break;
        }
        std::wstring current(static_cast<std::size_t>(length) + 1, L'\0');
        if (SendMessageW(plan.control, CB_GETLBTEXT, row, reinterpret_cast<LPARAM>(current.data())) != length)
            return false;
        current.resize(static_cast<std::size_t>(length));
        plan.rebuild = current != item.text;
    }
    return true;
}
bool Apply(const ComboPlan& plan) noexcept {
    if (plan.rebuild) {
        if (SendMessageW(plan.control, CB_RESETCONTENT, 0, 0) < 0) return false;
        for (std::size_t i = 0; i < plan.items.size(); ++i) {
            // INSERTSTRING preserves native order even if a caller supplies a
            // sorted combo. Both CB_ERR and CB_ERRSPACE fail this equality.
            const auto row = SendMessageW(plan.control, CB_INSERTSTRING, i,
                reinterpret_cast<LPARAM>(plan.items[i].text.c_str()));
            if (row != static_cast<LRESULT>(i) ||
                SendMessageW(plan.control, CB_SETITEMDATA, row, plan.items[i].data) < 0)
                return false;
        }
    }
    if (SendMessageW(plan.control, CB_GETCURSEL, 0, 0) != plan.selected) {
        const auto row = SendMessageW(plan.control, CB_SETCURSEL, static_cast<WPARAM>(plan.selected), 0);
        // CB_ERR is the documented result of deliberately clearing selection.
        if (plan.selected >= 0 && row != plan.selected) return false;
    }
    return SendMessageW(plan.control, CB_GETCOUNT, 0, 0) == static_cast<LRESULT>(plan.items.size()) &&
        SendMessageW(plan.control, CB_GETCURSEL, 0, 0) == plan.selected;
}
void Enable(HWND control, bool value) noexcept {
    if ((IsWindowEnabled(control) != FALSE) != value) EnableWindow(control, value ? TRUE : FALSE);
}
} // namespace

Selection CaptureSelection(const MMDApp& app) noexcept {
    Selection selection;
    selection.cameraMode = app.state.optflag[0] != 0;
    if (app.SelectedModelSlot() < kModelSlotCount) selection.model = app.SelectedModel();
    if (app.SelectedObjectSlot() < kAccessorySlots) selection.accessory = app.AccessorySlot(app.SelectedObjectSlot());
    const auto main = app.state.hwnd;
    if (selection.cameraMode && main && GetWindowThreadProcessId(main, nullptr) == GetCurrentThreadId()) {
        const auto row = SendDlgItemMessageW(main, panel::kRegisterScopeCombo, CB_GETCURSEL, 0, 0);
        if (row >= 0 && row < 4) selection.globalScope = static_cast<int>(row);
        else if (row >= 4 && row < 4 + kAccessorySlots)
            for (int slot = 0; slot < kAccessorySlots; ++slot)
                if (const auto* accessory = app.AccessorySlot(slot); accessory && accessory->order == row - 4) {
                    selection.scopeAccessory = accessory;
                    break;
                }
    }
    return selection;
}

Result RefreshObjectLists(MMDApp& app, const Selection& selection, bool canUndo, bool canRedo) noexcept {
    const auto main = static_cast<HWND>(app.Hwnd());
    if (!main || !IsWindow(main)) return Result::MissingControl;
    if (GetWindowThreadProcessId(main, nullptr) != GetCurrentThreadId()) return Result::WrongThread;
    try {
        std::vector<ModelRow> models;
        std::vector<AccessoryRow> accessories;
        const auto projected = model_order::Project<kModelSlotCount>([&](std::size_t slot) noexcept {
            const auto* model = app.ModelSlot(static_cast<int>(slot));
            return model ? int(mdl::Mdl(model)->comboSelIndex) : -1;
        });
        for (std::size_t row = 1; row <= projected.count; ++row) {
            const int slot = projected.rows[row] - 1;
            for (const auto& previous : models) if (previous.object == app.ModelSlot(slot)) return Result::InvalidData;
            models.push_back({slot, static_cast<int>(row), app.ModelSlot(slot), {}});
        }
        for (int slot = 0; slot < kAccessorySlots; ++slot)
            if (const auto* accessory = app.AccessorySlot(slot)) accessories.push_back({slot, accessory->order, accessory, {}});
        std::sort(accessories.begin(), accessories.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
        for (std::size_t i = 0; i < accessories.size(); ++i)
            if (accessories[i].order != static_cast<int>(i)) return Result::InvalidOrder;
        const auto language = app.EnglishUI();
        const bool english = UiUsesEnglishNames(language);
        for (auto& row : models) row.name = text_encoding::ModelLabel(*mdl::Mdl(row.object), english);
        for (auto& row : accessories) {
            const auto& name = row.object->name;
            const std::string bounded(name, std::find(name, name + sizeof name, '\0'));
            row.name = text_encoding::Display(bounded.c_str());
        }
        const int modelIndex = FindObject(models, selection.model);
        int accessoryIndex = FindObject(accessories, selection.accessory);
        if (accessoryIndex < 0 && !accessories.empty()) accessoryIndex = 0;
        const bool cameraMode = selection.cameraMode || modelIndex < 0;
        const auto* cameraParent = Parent(models, app.CameraParentModel());
        const auto* accessory = accessoryIndex >= 0 ? accessories[accessoryIndex].object : nullptr;
        const auto* accessoryParent = accessory ? Parent(models, accessory->parentModel) : nullptr;
        std::array<ComboPlan, 7> plans{{
            {GetDlgItem(main, panel::kMainComboModel)},
            {GetDlgItem(main, panel::kAccessoryCombo)},
            {GetDlgItem(main, panel::kMainComboNormal)},
            {GetDlgItem(main, panel::kMainComboGround)},
            {GetDlgItem(main, panel::kBoneRegisterCombo)},
            {GetDlgItem(main, panel::kAttachBoneCombo)},
            {GetDlgItem(main, panel::kRegisterScopeCombo)},
        }};
        const auto planCount = cameraMode ? plans.size() : plans.size() - 1;
        const auto undo = GetDlgItem(main, panel::kUndoButton), redo = GetDlgItem(main, panel::kRedoButton);
        if (!undo || !redo) return Result::MissingControl;
        for (std::size_t i = 0; i < planCount; ++i) if (!plans[i].control) return Result::MissingControl;
        plans[0].items.push_back({Label(language, "camera/light/accessory", L"\xff76\xff92\xff97\xff65\x7167\x660e\xff65\xff71\xff78\xff7e\xff7b\xff98")});
        plans[2].items.push_back({Label(language, "non", L"\x306a\x3057")});
        plans[3].items.push_back({Label(language, "ground", L"\x5730\x9762")});
        for (const auto& row : models)
            for (const auto index : {0, 2, 3}) plans[index].items.push_back({row.name, row.slot});
        for (const auto& row : accessories) plans[1].items.push_back({row.name, row.slot});
        plans[0].selected = cameraMode ? 0 : models[modelIndex].order;
        plans[1].selected = accessoryIndex;
        plans[2].selected = cameraParent ? cameraParent->order : 0;
        plans[3].selected = accessoryParent ? accessoryParent->order : 0;
        if (!BoneItems(plans[4], cameraParent, app.CameraParentBone(), english) ||
            !BoneItems(plans[5], accessoryParent, accessory ? accessory->parentBone : -1, english))
            return Result::InvalidData;
        if (cameraMode) {
            auto& scope = plans[6];
            scope.items.push_back({Label(language, "camera", L"\x30ab\x30e1\x30e9")});
            scope.items.push_back({Label(language, "light", L"\x7167\x660e")});
            scope.items.push_back({Label(language, "s shadow", L"\x30bb\x30eb\x30d5\x5f71")});
            scope.items.push_back({Label(language, "gravity", L"\x91cd\x529b")});
            for (const auto& row : accessories) scope.items.push_back({row.name, row.slot});
            const int scopeAccessory = FindObject(accessories, selection.scopeAccessory);
            scope.selected = scopeAccessory >= 0 ? scopeAccessory + 4 :
                selection.scopeAccessory ? 0 : std::clamp(selection.globalScope, 0, 3);
        }
        for (std::size_t i = 0; i < planCount; ++i) if (!Inspect(plans[i])) return Result::ControlFailure;

        // These are selection/UI fields only. All authoring data, including
        // live unkeyed poses, parent bindings and timeline flags, stays intact.
        if ((app.CameraMode() != 0) != cameraMode)
            app.EditMode() = cameraMode ? ViewportEditMode::None : ViewportEditMode::Bone;
        app.CameraMode() = cameraMode ? 1 : 0;
        app.MainModelComboSelection() = static_cast<std::uint32_t>(plans[0].selected);
        app.SetSelectedModelSlot(static_cast<std::uint8_t>(modelIndex >= 0 ? models[modelIndex].slot : 0));
        app.SelectedAccessorySlot() = static_cast<std::uint8_t>(accessoryIndex >= 0 ? accessories[accessoryIndex].slot : 0);
        for (std::size_t i = 0; i < planCount; ++i) if (!Apply(plans[i])) return Result::ControlFailure;
        Enable(undo, canUndo);Enable(redo, canRedo);
        return Result::Ok;
    } catch (const std::bad_alloc&) { return Result::OutOfMemory; }
    catch (...) { return Result::InvalidData; }
}
} // namespace mikumikustudio::scene_history_ui
