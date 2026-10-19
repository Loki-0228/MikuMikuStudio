#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/skinning_menu.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace mikumikustudio {
namespace {
using processor::Mode;
using processor::Selection;

const wchar_t* Text(MMDApp* app, const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
    return app->EnglishUI() == kUiChinese ? zh : app->EnglishUI() == kUiEnglish ? en : ja;
}
std::wstring MenuSafe(std::wstring text) {
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == L'&') text.insert(i++, 1, L'&');
    return text;
}

// The outcome of the last SaveRequested is kept as a state plus the error text,
// not as a pre-rendered sentence, so the popup can word it in whichever
// language is showing when the user reopens the menu.
enum class SaveState { None, Saved, Failed };
SaveState gSaveState = SaveState::None;
std::wstring gSaveError;

// ---- no handle cache -----------------------------------------------------
// Deleting a popup item also destroys the submenu hanging off it, so a refresh
// that rebuilt the whole popup tore down the level the user had open and the
// menu flickered.  Nothing is cached here: every refresh starts from the menu
// tree the host actually has right now, adopts the submenus it finds in it, and
// afterwards only writes text, greyed and checked state.  An item is only
// replaced when the list of devices behind it changed.

std::wstring DeviceName(std::uint64_t adapterId, const std::wstring& adapterName) {
    if (!adapterName.empty()) return MenuSafe(adapterName);
    if (adapterId != 0) return L"GPU #" + std::to_wstring(adapterId);
    return std::wstring();
}

std::wstring AdapterLabel(const std::vector<processor::AdapterEntry>& adapters, std::size_t index) {
    const auto& entry = adapters[index];
    std::wstring label = DeviceName(entry.adapterId, entry.adapterName);
    const auto sameName = [&](const processor::AdapterEntry& other) {
        return !entry.adapterName.empty() && other.adapterName == entry.adapterName;
    };
    if (std::count_if(adapters.begin(), adapters.end(), sameName) > 1) {
        const auto ordinal = std::count_if(adapters.begin(), adapters.begin() + index + 1, sameName);
        label += L" (" + std::to_wstring(ordinal) + L")";
    }
    return label;
}

// One info row for a device the current D3D9 enumeration does not provide.
// The marker wording is the point: a DXGI detection is displayed as a
// detection and never as a device the main renderer could use.
std::wstring ExtraDeviceLabel(MMDApp* app,
                              const std::vector<processor::AdapterEntry>& computes,
                              unsigned row) {
    return AdapterLabel(computes, row) + L"  " +
           Text(app, L"（当前 D3D9 未提供）",
                L"(not provided by the current D3D9)",
                L"（現在の D3D9 では未提供）");
}

// A listed device without a LUID cannot be matched back at the next launch, so
// it is shown but never offered as a choice.
bool Selectable(const processor::AdapterEntry& entry) { return entry.adapterId != 0; }

Selection FromEntry(const processor::AdapterEntry& entry) {
    Selection selection;
    selection.mode = Mode::Adapter;
    selection.adapterId = entry.adapterId;
    selection.adapterName = entry.adapterName;
    selection.identity = entry.identity;
    return selection;
}

// A non-device mode is a clean choice: a fresh Selection drops whatever card the
// previous pick remembered.
Selection FreshMode(Mode mode) {
    Selection selection;
    selection.mode = mode;
    return selection;
}

// Which command the saved request maps to, and whether it can be shown at all.
// The device matching is delegated to processor::ResolveFromTable on purpose:
// several cards of one model share one hardware identity, so the saved LUID is
// what tells them apart inside that identity set.  Taking the first entry with
// an equal identity would silently move the radio onto a different physical
// card.  A request that is missing or ambiguous resolves with fellBack, and a
// mode this menu has no item for maps to nothing - in both cases nothing is
// checked and the reason is shown, because falling back onto Automatic would
// silently rewrite what the user asked for.
struct RadioChoice { UINT id = 0; bool savedUnavailable = false; };

RadioChoice ChooseRole(const Selection& selection,
                       unsigned firstFixedId, unsigned secondFixedId,
                       Mode firstMode, Mode secondMode,
                       unsigned adapterFirstId, unsigned adapterLimit,
                       const std::vector<processor::AdapterEntry>& adapters) {
    RadioChoice choice;
    if (selection.mode == firstMode) { choice.id = firstFixedId; return choice; }
    if (selection.mode == secondMode && secondFixedId) { choice.id = secondFixedId; return choice; }
    if (selection.mode != Mode::Adapter) { choice.savedUnavailable = true; return choice; }
    const auto resolved = processor::ResolveFromTable(selection, adapters);
    if (resolved.fellBack) { choice.savedUnavailable = true; return choice; }
    for (std::size_t i = 0; i < std::min<std::size_t>(adapters.size(), adapterLimit); ++i) {
        const auto& entry = adapters[i];
        if (!Selectable(entry)) continue;
        if (entry.ordinal == resolved.adapterOrdinal ||
            (entry.adapterId == resolved.adapterId && entry.identity == resolved.identity)) {
            choice.id = adapterFirstId + static_cast<UINT>(i);
            return choice;
        }
    }
    choice.savedUnavailable = true;
    return choice;
}

// ---- what this run actually uses, per role -------------------------------

// InitD3D reports a REF software rasterizer through softwareDevice, not
// through mode, so that flag is checked first.  A failed creation never shows a
// GPU: no preselected card is displayed as running.
std::wstring RendererCurrent(MMDApp* app) {
    const auto& info = processor::ActualRenderer();
    if (info.failed)
        return std::wstring(Text(app, L"创建失败：", L"device creation failed: ", L"作成失敗：")) +
               MenuSafe(info.error);
    if (info.softwareDevice) return L"CPU";
    if (info.mode == Mode::Cpu) return L"CPU";
    const auto name = DeviceName(info.adapterId, info.adapterName);
    if (name.empty()) return Text(app, L"尚未确定", L"not resolved yet", L"未確定");
    std::wstring text = name;
    if (info.fellBack)
        text += L"  " + std::wstring(Text(app, L"（已回退：", L"(fell back: ", L"（フォールバック：")) +
                MenuSafe(info.fallbackReason) + L")";
    return text;
}

// This is the global most recent model computation, not the currently selected
// model, and it is never presented as a global performance measurement.
std::wstring VerticesCurrent(MMDApp* app) {
    const auto& status = skinning::GetStatus();
    switch (status.result) {
    case skinning::Result::Gpu: return MenuSafe(status.adapterName);
    case skinning::Result::Cpu: return L"CPU";
    case skinning::Result::Unsupported:
        return Text(app, L"CPU（该模型使用兼容回退）", L"CPU (model compatibility fallback)",
                    L"CPU（モデル互換フォールバック）");
    case skinning::Result::Unavailable:
        return Text(app, L"CPU（GPU 不可用）", L"CPU (GPU unavailable)", L"CPU（GPU 使用不可）");
    default:
        return Text(app, L"等待模型运算", L"Waiting for a model computation", L"モデル演算待ち");
    }
}

std::wstring MmvCurrent(MMDApp* app) {
    const auto& info = processor::ActualMmv();
    const auto selection = processor::ResolvedMmv();
    std::wstring text;
    if (processor::Requested().mmv.mode == Mode::Automatic ||
        (selection.mode == Mode::Adapter && selection.adapterId == 0 &&
         selection.adapterName.empty())) {
        text = Text(app, L"自动 / 等待处理", L"Automatic / waiting to run", L"自動 / 処理待ち");
    } else if (selection.mode == Mode::FollowRenderer) {
        text = Text(app, L"等待主渲染设备", L"Waiting for the main renderer", L"主描画デバイス待ち");
    } else if (selection.mode == Mode::Cpu) {
        text = L"CPU";
    } else {
        const auto name = DeviceName(selection.adapterId, selection.adapterName);
        text = name.empty() ? Text(app, L"自动 / 等待处理", L"Automatic / waiting to run",
                                   L"自動 / 処理待ち") : name;
    }
    if (info.fellBack && !info.fallbackReason.empty())
        text += L"  " + std::wstring(Text(app, L"（已回退：", L"(fell back: ", L"（フォールバック：")) +
                MenuSafe(info.fallbackReason) + L")";
    return text;
}

// ---- what the next launch will use ---------------------------------------

std::wstring IntentText(MMDApp* app, const Selection& selection, bool allowFollow,
                        bool savedUnavailable) {
    std::wstring text;
    switch (selection.mode) {
    case Mode::Cpu: text = L"CPU"; break;
    case Mode::FollowRenderer:
        text = allowFollow ? Text(app, L"跟随主渲染", L"Follow the main renderer", L"主描画に追従")
                           : Text(app, L"自动（优先高性能）", L"Automatic (best performance)",
                                  L"自動（性能優先）");
        break;
    case Mode::Adapter: {
        const auto name = DeviceName(selection.adapterId, selection.adapterName);
        text = name.empty() ? std::wstring()
                            : std::wstring(Text(app, L"显卡：", L"GPU: ", L"GPU: ")) + name;
        break;
    }
    default:
        text = Text(app, L"自动（优先高性能）", L"Automatic (best performance)", L"自動（性能優先）");
        break;
    }
    if (savedUnavailable) {
        std::wstring reason = Text(app, L"已保存设备不可用", L"Saved device unavailable",
                                   L"保存済みデバイスは利用できません");
        if (!text.empty()) reason += L"（" + text + L"）";
        return reason;
    }
    return text;
}

// ---- the items a role submenu should hold --------------------------------

// A plan entry carries only what identifies an item.  Greyed and checked are
// state and are written on every refresh; only an identity change forces the
// item to be replaced.
struct ItemPlan {
    UINT id = 0;
    bool separator = false;
    bool greyed = false;
    std::wstring text;
};

// Layout of every role submenu, fixed for the lifetime of the program:
// position 0 is the greyed current-state line, position 1 the greyed
// next-launch line, position 2 a separator, then the choice list.  Tests read
// these by position.
constexpr int kCurrentLine = 0;
constexpr int kPendingLine = 1;

// Only the shape of the list is compared - separators and command ids.  Text is
// state and is rewritten in place on every refresh, so it must not decide
// whether an item is replaced; otherwise a changing device name or a language
// switch would rebuild the level the user is navigating.  The command id is
// compared for every row, id 0 included: a greyed info line and a device
// command are never allowed to trade places without a rebuild.
bool SameItems(HMENU menu, const std::vector<ItemPlan>& plan) {
    if (GetMenuItemCount(menu) != static_cast<int>(plan.size())) return false;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        MENUITEMINFOW item{sizeof item};
        item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item)) return false;
        const bool separator = (item.fType & MFT_SEPARATOR) != 0;
        if (separator != plan[i].separator) return false;
        if (!separator && item.hSubMenu != nullptr) return false;
        if (!separator && item.wID != plan[i].id) return false;
    }
    return true;
}

void ApplyItems(HMENU menu, const std::vector<ItemPlan>& plan) {
    while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
    for (const auto& entry : plan) {
        if (entry.separator) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); continue; }
        AppendMenuW(menu, MF_STRING | (entry.greyed ? MF_GRAYED : 0),
                    entry.id, entry.text.c_str());
    }
}

void SetItemText(HMENU menu, int position, const std::wstring& text) {
    MENUITEMINFOW item{sizeof item};
    item.fMask = MIIM_STRING;
    item.dwTypeData = const_cast<wchar_t*>(text.c_str());
    SetMenuItemInfoW(menu, position, TRUE, &item);
}

void SetGreyed(HMENU menu, UINT id, bool greyed) {
    EnableMenuItem(menu, id, MF_BYCOMMAND | (greyed ? MF_GRAYED : MF_ENABLED));
}
}

// ---- devices DXGI detects but the D3D9 list does not provide -------------
// Pure classification shared by the renderer submenu and its tests.  The
// tests declare this function themselves instead of extending
// skinning_menu.hpp, so the signature must stay exactly as written here.
//
// A compute entry is dropped when the current D3D9 list provides the same
// device:
//   - by LUID when both sides know their LUID (the per-boot device identity);
//   - otherwise, when at least one side's LUID is unknown, by a full hardware
//     identity match that is unique on BOTH sides.  An ambiguous identity
//     never merges: several cards of one model share one identity, and
//     collapsing them would hide a real device.  The adapter name is never a
//     match key.
// Everything the D3D9 list cannot be shown to provide becomes an extra row.
// Extra rows repeating an already listed non-zero LUID collapse into one row;
// rows without a LUID are never collapsed with each other because without a
// LUID they may be two cards of one model.  Indices come back in compute
// order.
std::vector<unsigned> RendererExtraDeviceRows(
    const std::vector<processor::AdapterEntry>& rendererAdapters,
    const std::vector<processor::AdapterEntry>& computeAdapters) {
    const auto known = [](const processor::DeviceIdentity& identity) {
        return identity != processor::DeviceIdentity{};
    };
    std::vector<unsigned> rows;
    std::vector<std::uint64_t> listedLuids;
    for (std::size_t i = 0; i < computeAdapters.size(); ++i) {
        const auto& entry = computeAdapters[i];
        bool provided = false;
        if (entry.adapterId != 0) {
            for (const auto& renderer : rendererAdapters) {
                if (renderer.adapterId == entry.adapterId) { provided = true; break; }
            }
        }
        if (!provided && known(entry.identity)) {
            const auto sameIdentity = [&](const processor::AdapterEntry& other) {
                return other.identity == entry.identity;
            };
            const auto rendererMatches = std::count_if(
                rendererAdapters.begin(), rendererAdapters.end(), sameIdentity);
            const auto computeMatches = std::count_if(
                computeAdapters.begin(), computeAdapters.end(), sameIdentity);
            const bool luidUnknownOnOneSide =
                entry.adapterId == 0 ||
                std::any_of(rendererAdapters.begin(), rendererAdapters.end(),
                            [&](const processor::AdapterEntry& other) {
                                return sameIdentity(other) && other.adapterId == 0;
                            });
            provided = rendererMatches == 1 && computeMatches == 1 &&
                       luidUnknownOnOneSide;
        }
        if (provided) continue;
        if (entry.adapterId != 0) {
            if (std::find(listedLuids.begin(), listedLuids.end(), entry.adapterId) !=
                listedLuids.end())
                continue;
            listedLuids.push_back(entry.adapterId);
        }
        rows.push_back(static_cast<unsigned>(i));
    }
    return rows;
}

namespace {
// Menu refreshes never re-enumerate a device: they read the processor's cached
// adapter tables, and this cache recomputes the classification only when one
// of those tables changes (each table is enumerated once per run).
struct ExtraRowsCache {
    const void* rendererData = nullptr;
    std::size_t rendererSize = 0;
    const void* computeData = nullptr;
    std::size_t computeSize = 0;
    std::vector<unsigned> rows;
};
ExtraRowsCache gExtraRows;

const std::vector<unsigned>& CachedExtraRows(
    const std::vector<processor::AdapterEntry>& renderers,
    const std::vector<processor::AdapterEntry>& computes) {
    if (gExtraRows.rendererData != renderers.data() ||
        gExtraRows.rendererSize != renderers.size() ||
        gExtraRows.computeData != computes.data() ||
        gExtraRows.computeSize != computes.size()) {
        gExtraRows.rendererData = renderers.data();
        gExtraRows.rendererSize = renderers.size();
        gExtraRows.computeData = computes.data();
        gExtraRows.computeSize = computes.size();
        gExtraRows.rows = RendererExtraDeviceRows(renderers, computes);
    }
    return gExtraRows.rows;
}
}

// ---- finding the Help popup ---------------------------------------------
// Help is resolved through its About command (201), the convention already
// used by enhance_model_io.cpp, physics_bake_dialog.cpp, ui_init.cpp and
// ui_model_reload.cpp.  A top-level index is not usable: the baking popup sits
// directly in front of Help and MME/MMV append further top-level menus after
// it, so neither "index 7" nor "the last one" is stable.
constexpr UINT kHelpAboutCommand = 201;

int HelpMenuPosition(HMENU bar) {
    const int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW item{sizeof item};
        item.fMask = MIIM_SUBMENU;
        if (!GetMenuItemInfoW(bar, UINT(i), TRUE, &item) || !item.hSubMenu) continue;
        if (GetMenuState(item.hSubMenu, kHelpAboutCommand, MF_BYCOMMAND) != UINT(-1)) return i;
    }
    return -1;
}

// Where the entry goes inside Help: on the separator in front of About, so
// About keeps its place as the last entry.  With no About to anchor on the
// entry is appended.
int HelpInsertPosition(HMENU help) {
    const int count = GetMenuItemCount(help);
    int about = count;
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW item{sizeof item};
        item.fMask = MIIM_ID;
        if (GetMenuItemInfoW(help, UINT(i), TRUE, &item) && item.wID == kHelpAboutCommand) {
            about = i;
            break;
        }
    }
    if (about > 0) {
        MENUITEMINFOW item{sizeof item};
        item.fMask = MIIM_FTYPE;
        if (GetMenuItemInfoW(help, UINT(about - 1), TRUE, &item) && (item.fType & MFT_SEPARATOR))
            return about - 1;
    }
    return about;
}

// Which Help entry is ours, by content and not by text or by cached handle: a
// submenu that carries one of this menu's own commands.  That survives a
// language switch and identifies the entry even after the host rebuilt its bar.
int ProcessorPopupPosition(HMENU help) {
    const int count = GetMenuItemCount(help);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW item{sizeof item};
        item.fMask = MIIM_SUBMENU;
        if (!GetMenuItemInfoW(help, UINT(i), TRUE, &item) || !item.hSubMenu) continue;
        if (GetMenuState(item.hSubMenu, kSkinningAutomaticId, MF_BYCOMMAND) != UINT(-1) ||
            GetMenuState(item.hSubMenu, kRendererAutomaticId, MF_BYCOMMAND) != UINT(-1))
            return i;
    }
    return -1;
}

void RefreshSkinningMenu(MMDApp* app) {
    if (!app || !app->MainWindow()) return;
    HMENU bar = GetMenu(app->MainWindow());
    if (!bar) return;
    const int helpPos = HelpMenuPosition(bar);
    // With no Help popup nothing is installed anywhere.  Falling back to View
    // would put the entry back where the user asked to move it away from.
    if (helpPos < 0) return;
    HMENU help = GetSubMenu(bar, helpPos);
    if (!help) return;

    // Start from what Help actually holds right now.  A stale handle from an
    // earlier refresh is never reused: if the host rebuilt its menu the lookup
    // below simply misses and a fresh popup is created.  The entry is inserted
    // only after its contents are filled, so it is identifiable the moment it
    // enters the menu tree and is never inserted twice.
    const int ours = ProcessorPopupPosition(help);
    HMENU popup = nullptr;
    if (ours >= 0) {
        MENUITEMINFOW item{sizeof item};
        item.fMask = MIIM_SUBMENU;
        if (GetMenuItemInfoW(help, UINT(ours), TRUE, &item)) popup = item.hSubMenu;
    }
    const bool created = popup == nullptr;
    if (created) {
        popup = CreatePopupMenu();
        if (!popup) return;
    }

    const auto& requested = processor::Requested();
    const auto& renderers = processor::RendererAdapters();
    const auto& computes = processor::ComputeAdapters();

    struct Role {
        const wchar_t* zh; const wchar_t* en; const wchar_t* ja;
        const Selection* intent;
        const std::vector<processor::AdapterEntry>* adapters;
        unsigned firstFixedId; const wchar_t* firstFixedText;
        unsigned secondFixedId; const wchar_t* secondFixedText;
        unsigned adapterFirstId, adapterLimit;
        Mode firstMode, secondMode;
        bool allowFollow;
        const wchar_t* currentLabel;
        std::wstring (*current)(MMDApp*);
    };
    const Role allRoles[3] = {
        {L"主渲染设备", L"Main renderer", L"主描画デバイス",
         &requested.renderer, &renderers,
         kRendererAutomaticId,
         Text(app, L"自动（优先高性能）", L"Automatic (best performance)", L"自動（性能優先）"),
         0, nullptr, kRendererAdapterFirstId, kRendererAdapterLimit,
         Mode::Automatic, Mode::Cpu, false,
         Text(app, L"当前设备：", L"Current device: ", L"現在のデバイス："), RendererCurrent},
        {L"顶点计算设备", L"Vertex compute", L"頂点演算デバイス",
         &requested.vertices, &computes,
         kSkinningAutomaticId,
         Text(app, L"自动（首次测速）", L"Automatic (benchmark once)", L"自動（初回に測定）"),
         kSkinningCpuId, L"CPU", kSkinningAdapterFirstId, kSkinningAdapterLimit,
         Mode::Automatic, Mode::Cpu, false,
         Text(app, L"最近一次模型运算：", L"Last model computation: ", L"直近のモデル演算："),
         VerticesCurrent},
    };
    const unsigned roleCount = 2;
    const Role* roles = allRoles;

    // The popup holds the available role submenus, a separator and two status
    // lines for the renderer and native vertex processor.
    // Adopt whatever already sits there; only a wrong shape is rebuilt.
    bool shaped = GetMenuItemCount(popup) == int(roleCount + 3);
    if (shaped) {
        for (unsigned i = 0; i < roleCount && shaped; ++i) {
            MENUITEMINFOW item{sizeof item};
            item.fMask = MIIM_SUBMENU | MIIM_FTYPE;
            if (!GetMenuItemInfoW(popup, i, TRUE, &item) || item.hSubMenu == nullptr ||
                (item.fType & MFT_SEPARATOR)) shaped = false;
        }
    }
    if (!shaped) {
        while (GetMenuItemCount(popup) > 0) DeleteMenu(popup, 0, MF_BYPOSITION);
        for (unsigned roleIndex = 0; roleIndex < roleCount; ++roleIndex) {
            const auto& role = roles[roleIndex];
            HMENU sub = CreatePopupMenu();
            if (!sub) continue;
            AppendMenuW(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(sub),
                        Text(app, role.zh, role.en, role.ja));
        }
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING | MF_GRAYED, 0, L"");
        AppendMenuW(popup, MF_STRING | MF_GRAYED, 0, L"");
    }

    for (unsigned index = 0; index < roleCount; ++index) {
        const auto& role = roles[index];
        MENUITEMINFOW item{sizeof item};
        item.fMask = MIIM_SUBMENU;
        HMENU sub = GetMenuItemInfoW(popup, index, TRUE, &item) ? item.hSubMenu : nullptr;
        if (sub == nullptr) {
            sub = CreatePopupMenu();
            if (!sub) continue;
            item.fMask = MIIM_SUBMENU | MIIM_STRING;
            item.hSubMenu = sub;
            item.dwTypeData = const_cast<wchar_t*>(Text(app, role.zh, role.en, role.ja));
            SetMenuItemInfoW(popup, index, TRUE, &item);
        }
        // The title is rewritten every refresh so a language switch is picked up
        // without touching the submenu handle.
        SetItemText(popup, static_cast<int>(index), Text(app, role.zh, role.en, role.ja));

        // The main renderer submenu additionally lists the devices DXGI
        // detects that the current D3D9 enumeration does not provide.  Only
        // that role gains rows; the other roles keep their original lists.
        std::vector<std::wstring> extraTexts;
        if (index == 0) {
            for (unsigned row : CachedExtraRows(renderers, computes))
                extraTexts.push_back(ExtraDeviceLabel(app, computes, row));
        }

        std::vector<ItemPlan> plan;
        plan.push_back({0, false, true, std::wstring(role.currentLabel) + role.current(app)});
        plan.push_back({0, false, true,
                        std::wstring(Text(app, L"下次启动：", L"Next launch: ", L"次回起動：")) + L""});
        plan.push_back({0, true, false, std::wstring()});
        if (role.firstFixedId) plan.push_back({role.firstFixedId, false, false, role.firstFixedText});
        if (role.secondFixedId) plan.push_back({role.secondFixedId, false, false, role.secondFixedText});
        for (std::size_t i = 0; i < std::min<std::size_t>(role.adapters->size(), role.adapterLimit); ++i)
            plan.push_back({role.adapterFirstId + static_cast<UINT>(i), false,
                            !Selectable((*role.adapters)[i]), AdapterLabel(*role.adapters, i)});
        // The extra rows are independent greyed info lines with no command id
        // (id 0), so they can never be picked and never collide with the
        // renderer device commands or their selection mapping.
        for (const auto& text : extraTexts) plan.push_back({0, false, true, text});

        if (!SameItems(sub, plan)) ApplyItems(sub, plan);

        const auto choice = ChooseRole(*role.intent, role.firstFixedId, role.secondFixedId,
                                       role.firstMode, role.secondMode,
                                       role.adapterFirstId, role.adapterLimit, *role.adapters);
        SetItemText(sub, kCurrentLine, plan[kCurrentLine].text);
        SetItemText(sub, kPendingLine,
                    std::wstring(Text(app, L"下次启动：", L"Next launch: ", L"次回起動：")) +
                    IntentText(app, *role.intent, role.allowFollow, choice.savedUnavailable));
        // Fixed choice texts are rewritten in place too, so a language switch
        // never leaves them behind in the old language.
        unsigned planIndex = 3;
        if (role.firstFixedId) { SetItemText(sub, static_cast<int>(planIndex), role.firstFixedText); ++planIndex; }
        if (role.secondFixedId) { SetItemText(sub, static_cast<int>(planIndex), role.secondFixedText); ++planIndex; }
        for (std::size_t i = 0; i < std::min<std::size_t>(role.adapters->size(), role.adapterLimit); ++i) {
            SetItemText(sub, static_cast<int>(planIndex), AdapterLabel(*role.adapters, i));
            SetGreyed(sub, role.adapterFirstId + static_cast<UINT>(i), !Selectable((*role.adapters)[i]));
            ++planIndex;
        }
        // The extra device rows carry no command and stay greyed - forced by
        // position every refresh, so such a row can never be chosen; only its
        // text follows the language.
        for (const auto& text : extraTexts) {
            SetItemText(sub, static_cast<int>(planIndex), text);
            EnableMenuItem(sub, planIndex, MF_BYPOSITION | MF_GRAYED);
            ++planIndex;
        }
        CheckMenuRadioItem(sub, role.firstFixedId,
                           role.adapterFirstId + role.adapterLimit, choice.id, MF_BYCOMMAND);
    }

    SetItemText(popup, roleCount + 1, Text(app, L"更改保存在本机用户设置",
                               L"Saved for this Windows user",
                               L"この Windows ユーザーに保存"));
    std::wstring status;
    if (gSaveState == SaveState::Saved) {
        status = Text(app, L"已保存，下次启动生效", L"Saved; takes effect on the next launch",
                      L"保存しました。次回起動時に有効");
    } else if (gSaveState == SaveState::Failed) {
        status = std::wstring(Text(app, L"保存失败，已保持原选择：",
                                   L"Save failed, the previous choice is kept: ",
                                   L"保存失敗、元の選択を維持：")) + gSaveError;
    } else {
        status = Text(app, L"下次启动生效", L"Takes effect on the next launch", L"次回起動時に有効");
    }
    SetItemText(popup, roleCount + 2, status);

    if (created) {
        // Inserted last, once the contents already carry the commands that
        // identify this entry, so a later refresh finds it instead of adding a
        // second one.  MIIM_ID keeps the entry addressable by its command id,
        // which is what the rest of the port and the tests use.
        MENUITEMINFOW entry{sizeof entry};
        entry.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        entry.wID = kSkinningMenuId;
        entry.hSubMenu = popup;
        entry.dwTypeData = const_cast<wchar_t*>(Text(app, L"运算器", L"Processors", L"演算器"));
        if (!InsertMenuItemW(help, static_cast<UINT>(HelpInsertPosition(help)), TRUE, &entry)) {
            DestroyMenu(popup);
            return;
        }
    } else {
        SetItemText(help, ours, Text(app, L"运算器", L"Processors", L"演算器"));
    }
    // The entry used to live under View.  Anything still there is a leftover of
    // the previous layout, and it is removed only now that the Help entry is in
    // place, so the level the user has open is never pulled away first.
    HMENU view = GetSubMenu(bar, 2);
    if (view) DeleteMenu(view, kSkinningMenuId, MF_BYCOMMAND);
}

bool HandleSkinningCommand(MMDApp* app, unsigned command) {
    processor::Configuration configuration = processor::Requested();
    Selection* target = nullptr;
    const std::vector<processor::AdapterEntry>* adapters = nullptr;
    unsigned firstId = 0, limit = 0;

    if (command == kRendererAutomaticId) {
        target = &configuration.renderer; *target = FreshMode(Mode::Automatic);
    } else if (command >= kRendererAdapterFirstId &&
               command < kRendererAdapterFirstId + kRendererAdapterLimit) {
        target = &configuration.renderer; adapters = &processor::RendererAdapters();
        firstId = kRendererAdapterFirstId; limit = kRendererAdapterLimit;
    } else if (command == kSkinningAutomaticId) {
        target = &configuration.vertices; *target = FreshMode(Mode::Automatic);
    } else if (command == kSkinningCpuId) {
        target = &configuration.vertices; *target = FreshMode(Mode::Cpu);
    } else if (command >= kSkinningAdapterFirstId &&
               command < kSkinningAdapterFirstId + kSkinningAdapterLimit) {
        target = &configuration.vertices; adapters = &processor::ComputeAdapters();
        firstId = kSkinningAdapterFirstId; limit = kSkinningAdapterLimit;
    } else {
        return false;
    }

    if (adapters) {
        const std::size_t index = command - firstId;
        if (index >= std::min<std::size_t>(adapters->size(), limit)) return true;
        const auto& entry = (*adapters)[index];
        // A zero LUID cannot be matched back at the next launch.
        if (!Selectable(entry)) return true;
        *target = FromEntry(entry);
    }

    // The only write: the per-user processor.ini.  No SetPreference, no
    // EnableD3D12Native, no pmm_settings::ProcessorChanged, no SceneModified,
    // no device rebuild and no restart - Active stays frozen for this run.
    std::wstring error;
    if (processor::SaveRequested(configuration, error)) {
        gSaveState = SaveState::Saved;
        gSaveError.clear();
    } else {
        gSaveState = SaveState::Failed;
        gSaveError = error;
    }
    RefreshSkinningMenu(app);
    return true;
}
}
