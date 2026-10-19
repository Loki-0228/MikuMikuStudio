#include "mikumikustudio/display_capacity.hpp"
// ===========================================================================
// model_query_gaps.cpp - model queries / facial-panel / edit-target glue
// ===========================================================================
// Ports the remaining small model-side helpers of the 0x410000..0x440000
// band that had no body yet.  All __thiscall members below take the MMDApp
// object as `this` (verified at the call sites inside sub_46B090, e.g. the
// `mov ecx, ebx` immediately before the 0x42D3A0 call at 0x46FF02).
//
// Shared field map used here (decimal offsets as in the decompiles):
//   app+2320    kByteSlotidx  - active model-slot order index
//   app+1920    ModelSlots()  - slot -> model pointer array
//   app+646512  kBufBuf9ddx   - the 255-entry display-object array (0x9DD70)
//   app+645704  DisplayObjectListScrollPosition() - object-list scroll position
//   app+647536  kByte9e170    - selected display-object slot
//   app+656356..656359 (0xA03E4..0xA03E7) - edit-branch mode flags
//   app+658796  kDwordMsgseen - "message seen" latch (key edge detector)
//   app+657080  kPtrHwnd      - main window HWND
//   model+9916  bone records (604 B)          model+11652 bone count
//   model+9936  morph name table (101 B/rec, flag at +100)
//   model+9940  morph table entry count (byte)
//   model+9944  display-group records (46 B: +40 table idx, +42 WORD bone)
//   model+11664 selected bone index           model+11668 bone selection flags
//   model+11692 facial separator row count    model+11696 group record count
//   model+1181  display-frame index (edit branch)
//   model+1196  show flag
//   model+12716 list scroll position (display-list auto-scroll reference)
//   model+14592 base (root) bone index
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <io.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {
namespace {

// ---- tiny raw accessors (offsets are model/app byte offsets) ---------------
inline std::int32_t RdI32(unsigned char* p, std::size_t off) {
    return *reinterpret_cast<std::int32_t*>(p + off);
}
inline std::uint16_t RdU16(unsigned char* p, std::size_t off) {
    return *reinterpret_cast<std::uint16_t*>(p + off);
}
inline unsigned char*& RdPtr(unsigned char* p, std::size_t off) {
    return *reinterpret_cast<unsigned char**>(p + off);
}
inline std::int32_t& WrI32(unsigned char* p, std::size_t off) {
    return *reinterpret_cast<std::int32_t*>(p + off);
}

inline unsigned char* SlotModel(MMDApp* app) {
    return app->SelectedModel();
}

constexpr int kDisplayObjectCount = 255;
constexpr std::size_t kDisplayOrder = 1181;
constexpr std::size_t kDisplayActive = 1196;

inline unsigned char* DisplayObjectAt(MMDApp* app, int slot) {
    return static_cast<unsigned char*>(app->ObjectSlot(slot));
}
inline std::uint8_t& DisplayOrder(unsigned char* object) {
    return object[kDisplayOrder];
}
inline std::uint8_t& DisplayObjectActive(unsigned char* object) {
    return object[kDisplayActive];
}

inline HWND AppHwnd(MMDApp* app) {
    return app->state.hwnd;
}

// VA 0x0040E3D0 - single-key edge detector, inlined into PollKeyboardStates below
// (its only caller).  State cells hold 0=up, 1=just pressed, 3=held,
// 2=just released; any 0->1 / active->2 transition latches app+658796=1.
inline void KeyEdgeScan(MMDApp* app, int vk, std::uint32_t* cell) {
    if ((GetKeyState(vk) & 0x8000) != 0) {
        if (*cell != 0u) {
            *cell = 3;                               // 0x40E401 held repeat
        } else {
            *cell = 1;                               // 0x40E3ED press edge
            app->state.messageSeen = 1;  // 0x40E3F3
        }
    } else if (*cell == 1u || *cell == 3u) {
        *cell = 2;                                   // 0x40E425 release edge
        app->state.messageSeen = 1;      // 0x40E42B
    } else {
        *cell = 0;                                   // 0x40E41B stayed up
    }
}

}  // namespace

// ===========================================================================
// VA 0x0041A1A0 - ReadBeWord (was Sub41A1A0)  (__stdcall, no `this`)
// ===========================================================================
// Reads `nbytes` single bytes from `fh` and accumulates them big-endian
// (v = byte + (v << 8), 0x41A1C9) into *outVal; nbytes <= 0 just stores 0.
// Returns the last _read() result (or the outVal pointer in the empty case,
// mirroring the original - no caller uses the value).
// =========================================================================//
int ReadBeWord(int fh, int* outVal, int nbytes) {
    std::int32_t acc = 0;
    int result;
    if (nbytes <= 0) {                                // 0x41A1AA
        result = static_cast<int>(
            reinterpret_cast<std::uintptr_t>(outVal));  // original returns a2
        *outVal = 0;                                  // 0x41A1E1
    } else {
        int byteBuf = 0;
        result = 0;
        do {
            result = _read(fh, &byteBuf, 1);          // 0x41A1B9
            acc = static_cast<std::uint8_t>(byteBuf) +
                  (acc << 8);                         // 0x41A1C9
            --nbytes;
        } while (nbytes != 0);
        *outVal = acc;                                // 0x41A1D6
    }
    return result;
}

// ===========================================================================
// VA 0x0041A1F0 - ReadFixedString (was Sub41A1F0)  (__stdcall, no `this`)
// ===========================================================================
// Reads `len` bytes one at a time into a 1000-byte scratch buffer,
// NUL-terminates and sprintf_s("%s")-copies into `out` (capacity 0x3E8).
// (The original has no bounds check between len and the 1000-byte scratch;
// ported 1:1.)  Returns the sprintf_s result.
// =========================================================================//
int ReadFixedString(int fh, char* out, int len) {
    char byteBuf = 0;
    char scratch[1000];                               // v6 @0x41A1F0
    int i = 0;
    for (; i < len; ++i) {
        _read(fh, &byteBuf, 1);                       // 0x41A22B
        scratch[i] = byteBuf;                         // 0x41A234
    }
    scratch[i] = 0;                                   // 0x41A253
    return sprintf_s(out, 0x3E8u, "%s", scratch);     // 0x41A267
}

// ===========================================================================
// VA 0x0042D3A0 - PollKeyboardStates  (__thiscall, this = MMDApp)
// ===========================================================================
// Frame-driver key polling: 63 edge scans pairing virtual keys with the
// dword state cells at app+20..+196 (decompile's this[N] dword indices).
// Several cells are fed by two keys (menu accelerator + letter twin); the
// tables below cover every original 0x42D3AA..0x42D6B6 pairing, regrouped
// into named cells / letter twins / numpad twins - observationally
// identical because each scan writes only its own cell plus the
// idempotent messageSeen latch.  Called once per frame from 0x46FF02.
// =========================================================================//
void PollKeyboardStates(MMDApp* app) {  // was Sub42D3A0
    struct KeyCell { int vk; std::int32_t MMDAppState::* cell; };
    static constexpr KeyCell kNamedCells[] = {
        // 0x42D3AA..0x42D428 and the 0x42D5D6..0x42D6B6 non-numpad entries
        {VK_UP, &MMDAppState::upKeyState},
        {VK_DOWN, &MMDAppState::downKeyState},
        {VK_LEFT, &MMDAppState::leftKeyState},
        {VK_RIGHT, &MMDAppState::rightKeyState},
        {VK_SHIFT, &MMDAppState::shiftModifierState},
        {VK_SPACE, &MMDAppState::spaceKeyState},
        {VK_CONTROL, &MMDAppState::ctrlModifierState},
        {VK_DELETE, &MMDAppState::deleteKeyState},
        {VK_ESCAPE, &MMDAppState::escKeyState},
        {VK_TAB, &MMDAppState::tabKeyState},
        {VK_LBUTTON, &MMDAppState::leftMouseButtonState},
        {VK_RBUTTON, &MMDAppState::rightMouseButtonState},
        {VK_MBUTTON, &MMDAppState::middleMouseButtonState},
        {VK_RETURN, &MMDAppState::enterKeyState},
        {VK_MENU, &MMDAppState::menuKeyState},
        {221, &MMDAppState::keyState221},
        {226, &MMDAppState::keyState226},
    };
    for (const KeyCell& e : kNamedCells)
        KeyEdgeScan(app, e.vk,
                    reinterpret_cast<std::uint32_t*>(&(app->state.*e.cell)));

    // Share the same Win32 virtual-key mapping and independent edge history
    // as MouseInteractionBegin; each letter is sampled once per frame.
    PollLetterHotkeys(app);

    // 0x42D5D6..0x42D6B6 tail: VK_NUMPAD0..9 twin scans -> app+144..+180
    for (int i = 0; i < 10; ++i)
        KeyEdgeScan(app, VK_NUMPAD0 + i,
                    reinterpret_cast<std::uint32_t*>(
                        &app->state.numpadKeyState[i]));
}

// ===========================================================================
// VA 0x0041A280 - ScrollDisplayBoneIntoView  (__thiscall, this = MMDApp)
// ===========================================================================
// Called by SelectDisplayBone after a display-bone selection: walks the display list
// rows (morph table entries at model+9936, 101 B each with visibility flag
// at +100; group records at model+9944, 46 B with table idx at +40 and the
// bone index WORD at +42) until the record carrying `boneIdx` is found,
// then WM_VSCROLLs the listbox (item 427 of the main window) line by line
// so the row becomes visible.  The row count includes the +11692 separator
// rows inserted after table entry 2.  Early-out when boneIdx is the root
// bone or negative.  (The original's return value is the
// low byte of a pointer - junk every caller ignores.)
// =========================================================================//
void ScrollDisplayBoneIntoView(MMDApp* app, int boneIdx) {  // was Sub41A280
    unsigned char* m = SlotModel(app);                          // 0x41A28E
    const auto* record = mdl::Mdl(m);
    if (record->displayRootBone == static_cast<std::uint32_t>(boneIdx) ||
        boneIdx < 0)                                            // 0x41A2BB
        return;

    int rows = -1;                                              // v5
    const unsigned tblCnt =
        static_cast<std::uint32_t>(mikumikustudio::mdl::DisplayGroupCount(*mikumikustudio::mdl::Mdl(m)));                    // 0x41A2D4
    if (tblCnt <= 1u)
        return;
    const mdl::DisplayGroup* const tbl = mdl::DisplayGroups(m);

    for (unsigned k = 1; k < tblCnt; ++k) {                     // v17 / v4
        ++rows;                                                 // 0x41A2FC
        if (tbl[k].flags == 0)                                  // 0x41A2FF
            continue;
        const std::uint32_t sep = mikumikustudio::mdl::FacialFrameCount(*mikumikustudio::mdl::Mdl(m));                 // v7
        if (sep != 0 && k == 2) {                               // 0x41A31A
            rows += sep;                                        // 0x41A32D
            continue;
        }
        const int n = static_cast<int>(mdl::Mdl(m)->rigidBodyCount);
        if (n == 0)
            continue;
        const mdl::FrameGroup* const grps = mdl::RigidGroups(m);
        for (int j = 0; j < n; ++j) {                           // 0x41A34C
            if (mikumikustudio::mdl::FrameGroupIndex(grps[j]) != k)
                continue;
            ++rows;                                             // 0x41A351
            if (mikumikustudio::mdl::FrameTargetIndex(grps[j]) !=
                static_cast<std::uint32_t>(boneIdx))            // 0x41A358
                continue;
            // hit: scroll the facial list so `rows` is in view
            const int delta = rows - record->boneListPos;       // 0x41A390
            HWND hwnd = AppHwnd(app);
            if (delta >= 0) {
                RECT rc;
                GetClientRect(hwnd, &rc);                       // 0x41A3EA
                const int slack =
                    (rc.bottom - 408) / 14 - delta - 2;         // 0x41A40F
                if (slack < 0) {
                    int cnt2 = 2 - ((rc.bottom - 408) / 14 - delta);
                    for (; cnt2 != 0; --cnt2)                   // 0x41A452
                        SendMessageA(hwnd, WM_VSCROLL, 1,
                                     reinterpret_cast<LPARAM>(
                                         GetDlgItem(hwnd, panel::kTimelineVScroll)));
                }
            } else {
                int cnt2 = -delta;
                for (; cnt2 != 0; --cnt2)                       // 0x41A3D2
                    SendMessageA(hwnd, WM_VSCROLL, 0,
                                 reinterpret_cast<LPARAM>(
                                     GetDlgItem(hwnd, panel::kTimelineVScroll)));
            }
            return;                                             // 0x41A381
        }
    }
}

// ===========================================================================
// VA 0x00438CD0 - SelectDisplayBone  (__thiscall, this = MMDApp, a2 = bone)
// ===========================================================================
// Stores the selected bone index, clears the bone selection bitmap (bounded
// by the model bone count) and re-marks only the selected entry, then runs
// the display-list auto-scroll (0x41A280) and the label/view refresh pair
// (0x42F1E0 / 0x40D070).  (Original returned the trailing BOOL; unused.)
// =========================================================================//
void SelectDisplayBone(MMDApp* app, int boneIdx) {  // was Sub438CD0
    unsigned char* m = SlotModel(app);
    auto* record = mdl::Mdl(m);
    record->selectedBone = boneIdx;                             // 0x438CE5
    const int cap = static_cast<int>(record->boneCount);
    unsigned char* flags = record->boneSelection;
    for (int i = 0; i < cap; ++i)                               // 0x438D01
        flags[i] = 0;
    // Deliberate hardening: the original offsets the selection index into the
    // bitmap base and stores 1 without checking it against the model bone count
    // (verified at 0x1400712B4), so a display-list index beyond the bone table
    // writes outside the one-byte-per-bone array. The port marks the entry
    // only within [0, boneCount) and leaves the bitmap cleared otherwise.
    // Defect MB-18.
    if (boneIdx >= 0 && boneIdx < cap)
        flags[boneIdx] = 1;                                     // 0x438D3A
    ScrollDisplayBoneIntoView(app, boneIdx);                    // 0x438D41
    PostLanguageSweep(app);                                     // 0x438D48
    PostLanguageSweep2(app);                                    // 0x438D54
}

// ===========================================================================
// VA 0x00438D60 - SelectPreviousDisplayBone  (__thiscall, this = MMDApp)
// ===========================================================================
// "Previous" button: finds the display-group record carrying the currently
// selected bone:
//   * current < 0            -> select the root bone
//   * current == base        -> no-op
//   * its table group g visible and an earlier record of the same group
//     exists -> select that record (0x438F36)
//   * otherwise find the highest visible table entry below g (>=3; entries
//     0..2 fall back to the base morph, 0x438F28) and select the LAST
//     group record belonging to it (0x438EED)
//   * no match anywhere -> keep scanning later records for the current
//     morph (outer loop at LABEL_34)
// =========================================================================//
void SelectPreviousDisplayBone(MMDApp* app) {  // was Sub438D60
    unsigned char* m = SlotModel(app);
    const auto* record = mdl::Mdl(m);
    const int cur = record->selectedBone;                       // 0x438D71
    if (cur < 0) {
        SelectDisplayBone(app, record->displayRootBone);                // 0x438D92
        return;
    }
    const int rootBone = record->displayRootBone;
    if (rootBone == cur)
        return;                                                 // 0x438D8F
    const int n = static_cast<int>(mdl::Mdl(m)->rigidBodyCount);
    if (n == 0)
        return;
    const mdl::FrameGroup* const grps = mdl::RigidGroups(m);
    const mdl::DisplayGroup* const tbl = mdl::DisplayGroups(m);

    for (int i = 0; i < n; ++i) {                               // 0x438DEB
        if (mikumikustudio::mdl::FrameTargetIndex(grps[i]) != static_cast<std::uint32_t>(cur))
            continue;                                           // 0x438DF0
        int g = mikumikustudio::mdl::FrameGroupIndex(grps[i]);                             // 0x438E12

        if (tbl[g].flags != 0 && i >= 1) {                      // 0x438E1C
            // previous record of the same visible group (scan downwards)
            for (int j = i - 1; j >= 0; --j) {                  // 0x438E84
                if (mikumikustudio::mdl::FrameGroupIndex(grps[j]) == g) {
                    SelectDisplayBone(app, mikumikustudio::mdl::FrameTargetIndex(grps[j]));        // 0x438F36
                    return;
                }
            }
        }

        // LABEL_24: highest visible table entry strictly below g
        if (g > 0) {
            for (--g; g > 0 && tbl[g].flags == 0; --g) {        // 0x438ECD
            }
        }
        if (g <= 2) {
            SelectDisplayBone(app, rootBone);                           // 0x438F28
            return;
        }
        // LAST record of that group (scan downwards from n-1)
        for (int j = n - 1; j >= 0; --j) {                      // 0x438EE4
            if (mikumikustudio::mdl::FrameGroupIndex(grps[j]) == g) {
                SelectDisplayBone(app, mikumikustudio::mdl::FrameTargetIndex(grps[j]));            // 0x438EED
                return;
            }
        }
        // not found -> next outer record (LABEL_34)
    }
}

// ===========================================================================
// VA 0x00438F50 - SelectNextDisplayBone  (__thiscall, this = MMDApp)
// ===========================================================================
// "Next" button, the mirror of 0x438D60:
//   * current < 0     -> select the root bone
//   * current == base -> first visible table entry (>=1) that owns a group
//     record, selecting its FIRST record (0x43904E); falls through into
//     the record walk when nothing matches
//   * record found with its group visible -> next record of the same group
//     (0x439105), else / group hidden -> next visible table entry above g
//     and its FIRST record (0x4390D5 / 0x43914D); g is clamped by the
//     entry count at model+9940 (loop exits at LABEL_41 when exhausted)
// =========================================================================//
void SelectNextDisplayBone(MMDApp* app) {  // was Sub438F50
    unsigned char* m = SlotModel(app);
    const auto* record = mdl::Mdl(m);
    const int cur = record->selectedBone;                       // 0x438F64
    if (cur < 0) {
        SelectDisplayBone(app, record->displayRootBone);                // 0x438F8C
        return;
    }
    const mdl::FrameGroup* const grps = mdl::RigidGroups(m);
    const mdl::DisplayGroup* const tbl = mdl::DisplayGroups(m);
    const int n = static_cast<int>(mdl::Mdl(m)->rigidBodyCount);
    const int tblLast = static_cast<std::uint32_t>(mikumikustudio::mdl::DisplayGroupCount(*mikumikustudio::mdl::Mdl(m))) - 1;  // v17

    if (cur == record->displayRootBone) {                        // 0x438F96
        for (int g = 1; g <= tblLast; ++g) {                    // 0x438FE8
            if (tbl[g].flags == 0)
                continue;
            for (int j = 0; j < n; ++j) {
                if (mikumikustudio::mdl::FrameGroupIndex(grps[j]) == g) {
                    SelectDisplayBone(app, mikumikustudio::mdl::FrameTargetIndex(grps[j]));        // 0x43904E
                    return;
                }
            }
        }
    }
    if (n == 0)                                                 // 0x438FFE
        return;

    for (int i = 0; i < n; ++i) {                               // 0x439058
        if (mikumikustudio::mdl::FrameTargetIndex(grps[i]) != static_cast<std::uint32_t>(cur))
            continue;                                           // 0x43905F
        const int g = mikumikustudio::mdl::FrameGroupIndex(grps[i]);                       // 0x439081

        if (tbl[g].flags != 0 && i + 1 < n) {
            // next record of the same visible group (scan upwards)
            for (int j = i + 1; j < n; ++j) {                   // 0x439105
                if (mikumikustudio::mdl::FrameGroupIndex(grps[j]) == g) {
                    SelectDisplayBone(app, mikumikustudio::mdl::FrameTargetIndex(grps[j]));
                    return;
                }
            }
        }

        // next visible table entry above g, then its FIRST record; exits to
        // the next outer record once g reaches the last entry (LABEL_41)
        if (g < tblLast) {
            int g2 = g;
            for (;;) {                                          // 0x4390B0
                ++g2;
                if (tbl[g2].flags != 0) {
                    for (int j = 0; j < n; ++j) {
                        if (mikumikustudio::mdl::FrameGroupIndex(grps[j]) == g2) {
                            SelectDisplayBone(app, mikumikustudio::mdl::FrameTargetIndex(grps[j]));
                            return;
                        }
                    }
                }
                if (g2 >= tblLast)                              // LABEL_27/40
                    break;
            }
        }
    }
}

// ===========================================================================
// VA 0x0041A460 - ScrollModelListIntoView  (__thiscall, this = MMDApp,
//                 a2 = slot index into the app+646512 model array)
// ===========================================================================
// WM_VSCROLLs the bone/model listbox (item 427) so the model's display row
// (model+1181) is visible, measured against the edit-branch list position
// at app+645704 and the (clientHeight-452)/14 row budget.  (Original
// returned a junk LRESULT; unused by all callers.)
// =========================================================================//
void ScrollModelListIntoView(MMDApp* app, unsigned char slotIdx) {  // was Sub41A460
    unsigned char* m = DisplayObjectAt(app, slotIdx);           // 0x41A46B
    if (m == nullptr)
        return;
    const int listPos = app->DisplayObjectListScrollPosition();
    const int delta = DisplayOrder(m) - listPos;                 // 0x41A484
    HWND hwnd = AppHwnd(app);
    if (delta >= 0) {
        RECT rc;
        GetClientRect(hwnd, &rc);                               // 0x41A4DE
        const int slack = (rc.bottom - 452) / 14 - delta - 2;   // 0x41A503
        if (slack < 0) {
            int cnt = 2 - ((rc.bottom - 452) / 14 - delta);
            for (; cnt != 0; --cnt)                             // 0x41A542
                SendMessageA(hwnd, WM_VSCROLL, 1,
                             reinterpret_cast<LPARAM>(
                                 GetDlgItem(hwnd, panel::kTimelineVScroll)));
        }
    } else {
        int cnt = listPos - DisplayOrder(m);
        for (; cnt != 0; --cnt)                                 // 0x41A4C6
            SendMessageA(hwnd, WM_VSCROLL, 0,
                         reinterpret_cast<LPARAM>(GetDlgItem(hwnd, panel::kTimelineVScroll)));
    }
}

namespace {

inline void ClearAllDisplayObjectFlags(MMDApp* app) {
    for (int slot = 0; slot < kDisplayObjectCount; ++slot) {
        unsigned char* object = DisplayObjectAt(app, slot);
        if (object != nullptr)
            DisplayObjectActive(object) = 0;                     // 0x4391EF
    }
}

// Shared tail of 0x4391D0 / 0x439520: activate the display object, make it
// the selected slot, push its display row into the combo (item 471,
// CB_SETCURSEL) and refresh (0x4134E0 + 0x41A460 + sweep pair).
inline void SelectDisplayObject(MMDApp* app, int idx) {         // 0x43974F
    unsigned char* object = DisplayObjectAt(app, idx);
    DisplayObjectActive(object) = 1;
    HWND hwnd = AppHwnd(app);
    app->SelectedObjectSlot() = static_cast<unsigned char>(idx); // 0x43975D
    const WPARAM wp = DisplayOrder(object);                      // 0x439771
    SendMessageA(GetDlgItem(hwnd, panel::kAccessoryCombo), CB_SETCURSEL, wp, 0);
    SyncAccessoryEditPanel(app);  // was Sub4134E0              // 0x43978C
    ScrollModelListIntoView(app, app->SelectedObjectSlot());
    PostLanguageSweep(app);                                     // 0x4397A2
    PostLanguageSweep2(app);
}

}  // namespace

// ===========================================================================
// VA 0x4391D0 - SelectPrevEditTarget  (__thiscall, this = MMDApp)
// ===========================================================================
// "Previous target" through the edit-branch mode flags app+656356..656359
// (0xA03E4..0xA03E7): bone -> (off) -> facial -> accessory -> previous
// model display frame.  Every branch first clears the show flag (model+1196)
// of all 255 model-array entries; the model-select branch (all flags 0)
// walks the array for the previous display-frame index (model+1181)-1,
// selects it via the combo at item 471 and 0x41A460; frame 0 wraps to the
// accessory mode (656359=1); a missing current model resets to the bone
// mode (656356=1).
// =========================================================================//
void SelectPrevEditTarget(MMDApp* app) {  // was Sub4391D0
    if (app->GlobalTrackSelected(GlobalTimelineTrack::Camera) != 0) {
        ClearAllDisplayObjectFlags(app);
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
        PostLanguageSweep(app);                                 // 0x439244
        PostLanguageSweep2(app);
        return;
    }
    if (app->GlobalTrackSelected(GlobalTimelineTrack::Light) != 0) {
        ClearAllDisplayObjectFlags(app);
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
        PostLanguageSweep(app);
        PostLanguageSweep2(app);
        return;
    }
    if (app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow) != 0) {
        ClearAllDisplayObjectFlags(app);
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);
        PostLanguageSweep(app);
        PostLanguageSweep2(app);
        return;
    }
    if (app->GlobalTrackSelected(GlobalTimelineTrack::Gravity) != 0) {
        ClearAllDisplayObjectFlags(app);
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::SelfShadow);
        PostLanguageSweep(app);
        PostLanguageSweep2(app);
        return;
    }

    // all mode flags clear: previous model display frame
    ClearAllDisplayObjectFlags(app);
    const unsigned char curSlot = app->SelectedObjectSlot();    // 0x4393FB
    app->ClearGlobalTimelineTrackSelection();
    unsigned char* current = DisplayObjectAt(app, curSlot);
    if (current == nullptr) {                                   // 0x439425
        app->SelectedObjectSlot() = 0;
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
        PostLanguageSweep(app);                                 // 0x43950D
        PostLanguageSweep2(app);
        return;
    }
    const unsigned char frame = DisplayOrder(current);          // 0x43942D
    if (frame != 0) {
        const unsigned char target = frame - 1;                 // v29
        int i = 0;
        while (DisplayObjectAt(app, i) == nullptr ||
               DisplayOrder(DisplayObjectAt(app, i)) != target) { // 0x43945F
            ++i;
            if (i >= 255) {
                PostLanguageSweep(app);                         // 0x439486
                PostLanguageSweep2(app);
                return;
            }
        }
        SelectDisplayObject(app, i);                             // 0x43949C..
        return;
    }
    app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Gravity);
    ScrollModelListIntoView(app, curSlot);                                    // 0x439441
    PostLanguageSweep(app);                                     // 0x439448
    PostLanguageSweep2(app);
}

// ===========================================================================
// VA 0x439520 - SelectNextEditTarget  (__thiscall, this = MMDApp)
// ===========================================================================
// "Next target" - the forward twin of 0x4391D0.  Flag walk:
//   656356 -> 656357 -> 656358 -> 656359; with 656359 set (or reached
//   without finding a model) it hunts the first model whose display frame
//   (model+1181) is 0 and selects it; from the no-flag state it takes
//   frame (model+1181)+1, re-showing the CURRENT model when that frame
//   does not exist (0x43985A).
// =========================================================================//
void SelectNextEditTarget(MMDApp* app) {  // was Sub439520
    if (app->GlobalTrackSelected(GlobalTimelineTrack::Camera) != 0) {
        ClearAllDisplayObjectFlags(app);
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);
        PostLanguageSweep(app);                                 // 0x4395A3
        PostLanguageSweep2(app);
        return;
    }
    if (app->GlobalTrackSelected(GlobalTimelineTrack::Light) != 0) {
        ClearAllDisplayObjectFlags(app);
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::SelfShadow);
        PostLanguageSweep(app);                                 // 0x439623
        PostLanguageSweep2(app);
        return;
    }
    if (app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow) != 0) {
        ClearAllDisplayObjectFlags(app);
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Gravity);
        PostLanguageSweep(app);                                 // 0x4396A3
        PostLanguageSweep2(app);
        return;
    }

    if (app->GlobalTrackSelected(GlobalTimelineTrack::Gravity) != 0) {
        ClearAllDisplayObjectFlags(app);
        app->ClearGlobalTimelineTrackSelection();
        // first model with display frame 0
        int i = 0;
        while (DisplayObjectAt(app, i) == nullptr ||
               DisplayOrder(DisplayObjectAt(app, i)) != 0) {    // 0x439734
            ++i;
            if (i >= 255) {
                app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Gravity);
                PostLanguageSweep(app);                         // 0x4396A3
                PostLanguageSweep2(app);
                return;
            }
        }
        SelectDisplayObject(app, i);                             // 0x43974F..
        return;
    }

    // no mode flag set: next model display frame from the current slot
    ClearAllDisplayObjectFlags(app);
    const unsigned char curSlot = app->SelectedObjectSlot();    // 0x4397F8 tail
    unsigned char* cur = DisplayObjectAt(app, curSlot);         // v30 @0x439824
    app->ClearGlobalTimelineTrackSelection();
    if (cur == nullptr) {                                       // 0x439822
        app->SelectedObjectSlot() = 0;
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
        PostLanguageSweep(app);                                 // 0x439891
        PostLanguageSweep2(app);
        return;
    }
    const unsigned char target = DisplayOrder(cur) + 1;         // 0x43982D
    int i = 0;
    while (DisplayObjectAt(app, i) == nullptr ||
           DisplayOrder(DisplayObjectAt(app, i)) != target) {   // 0x439832
        ++i;
        if (i >= 255) {
            DisplayObjectActive(cur) = 1;                        // 0x43985A
            ScrollModelListIntoView(app, curSlot);
            PostLanguageSweep(app);                             // 0x439872
            PostLanguageSweep2(app);
            return;
        }
    }
    SelectDisplayObject(app, i);
}

}  // namespace mikumikustudio
