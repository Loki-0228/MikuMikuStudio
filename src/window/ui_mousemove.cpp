#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
// ===========================================================================
// VA 0x00444CC0 - HandleMouseMove  (original: sub_444CC0)
// ===========================================================================
// WM_MOUSEMOVE handler for the editor panel (~0x1DA6 bytes, 441 basic
// blocks; called from WndProc 0x4C3A10, case 512).  Original signature:
//   HCURSOR __thiscall sub_444CC0(int this, unsigned __int16 X, unsigned
//   __int16 Y)
// The caller pushes lParam (mouse X in the low word) then lParam >> 16
// (mouse Y), i.e. X = lParam & 0xFFFF and Y = mouseY here.  The HCURSOR
// return value is discarded by the wndproc, so the port is void, but the
// early returns are preserved because they gate the hide-rect flag clears.
//
// Flow (original instruction ranges):
//   1. 0x444CC6  three hide-rect gates (bytes this+0xA06B4/0xA06B5/0xA06B6):
//      when the mouse sits inside the stored rect this+0xA0D40..0xA0D4C the
//      gate flag is cleared so later moves can re-enter; otherwise the
//      handler returns with the flag still set.
//   2. 0x444D5B  on a real move: store X/Y into this+4/this+8 (with the
//      0xEA60 -> X-0x10000 wrap), maintain the >50px jump flag
//      (byte this+0xA05D3, prev this+0xC/0x10, gate byte this+0x9F12C),
//      GetClientRect, then cursor switching:
//        IDC_SIZEWE (0x7F84): while the pointer hugs the sidebar edge
//          (sidebar <= X <= sidebar+6 && Y <= bottom-158 && 0xA0D38 == 0)
//        IDC_HAND   (0x7F89): while interaction mode (dword this+0x344) == 1
//   3. 0x444E54  sidebar drag (byte this+0xC8): sidebar this+0xA06C8 =
//      clamp(X, 250, clientRight-5), ratio float this+0xA4428, then
//      RelayoutSidebarControls + PanelPaint and three local InvalidateRect passes
//      (right strip / timeline strip / top strip).
//   4. 0x444F46  if byte this+0xA03EB != 0: hover row = (X - 0xA018C - 6)
//      / 13, byte this+0xA0B0D = 1, then the drag-frame remap over the
//      hovered row's list(s): display mode (optflag[0] @0x2F8 != 0) walks
//      the four fixed band lists (84/40/24/36-byte records via this+0x374/
//      0x378/0x37C/0x380) and the accessory lists (this+0x384, 60-byte
//      records) driven by the 16-byte offset records at this+0xA03EC..
//      0xA0410, then PanelPaint, ReloadModels, RefreshLightPanel, RefreshSelfShadowPanel,
//      ApplyGravityTrack, ApplyAccessoryTrack x255 and SyncAccessoryEditPanel; bone-edit mode walks the
//      model bone/morph/IK lists (28/20/60-byte records, offset records at
//      this+0xA0414..0xA0428) then PanelPaint + SeekModelFrame.
//   5. 0x44606F  else, if byte this+0xA0189 != 0: hover-highlight pass over
//      the five fixed-band regions (window-y bands 160..174 / 174..188 /
//      188..202 / 202..216, i.e. the four fixed bands + the accessory
//      grid; row pitch 13 starting at x = 94, column pitch 14 starting at
//      y = 209).  The visibility flags of the band records are first
//      cleared for all 200 rows (0x4461CE display / 0x44653B bone-edit,
//      gated on mode word this+0x24 != 3) and then re-set for the hovered
//      rows to (this+0xC0 != 3); the hover rect lands in
//      this+0xA05C0..0xA05CC.  Ends with PanelPaint and the rect zeroed.
//   6. 0x446A4C  if byte this+0x9DA09 != 0: Sub416280.
//
// The magic-number divisions (/13 = 0x4EC4EC4F<<2, /14 = 0x92492493<<3 with
// add) were verified numerically against the original instruction stream -
// both are plain C truncating division, so ordinary `/` is used.
//
// Reference: ../translated/MikuMikuDance/fcn_00444cc0.cpp (machine
//            translation, incomplete; IDA IDB is authoritative)
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/model.hpp"

namespace mikumikustudio {

// VA 0x00414610 - ported (ui_panel_paint.cpp).
void PanelPaint(MMDApp* app);

// Refresh helpers, ported in other translation units.
void SyncAccessoryEditPanel(MMDApp* a);              // 0x4134E0 (ui_frame_refresh.cpp)
void RefreshLightPanel(MMDApp* a);              // 0x411070 (ui_frame_refresh.cpp)
void ApplyGravityTrack(MMDApp* a);              // 0x412330 (track_apply.cpp)
void RefreshSelfShadowPanel(MMDApp* a);              // 0x411B90 (ui_frame_refresh.cpp)
void DragInterpolationControlPoint(MMDApp* a);  // 0x416280, was Sub416280 (ui_selection_reeval.cpp)
void ReloadModels(MMDApp* a);              // 0x42E640, was Sub42E640 (timeline_advance.cpp)
void RelayoutSidebarControls(MMDApp* a);              // 0x442EB0 (ui_windowsize.cpp)
int SeekModelFrame(unsigned char* model, int frame, int physicsMode);  // 0x4B4260 (model_frame_seek.cpp)
void ApplyAccessoryTrack(MMDApp* a, int idx);     // 0x413120 (accessory_paste.cpp)

namespace {

// --- deferred: the accessory hit grid stays offset-addressed -------
// (x64 xlate only carries its first 800 element entries)

// --- layout constants -------------------------------------------------------
constexpr int kRowPitch = 13;     // 0x0D  hover row pitch
constexpr int kColPitch = 14;     // 0x0E  hover column pitch (bands)
constexpr int kGridX0 = 94;       // 0x5E  first hover row x
constexpr int kColY0 = 209;       // 0xD1  first hover column y (display)
constexpr int kColY0Bone = 153;   // 0x99  first hover column y (bone-edit)


// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

unsigned char* CurrentModel(MMDApp* app) {
    return app->SelectedModel();
}

// Frame-remap walk over 16-byte offset records {idx, parent, slot, frame}
// referenced by the band lists (display mode 0x444F8C..0x445833), or by the
// model bone/morph/IK lists (bone-edit mode 0x445889..0x446040).  Every
// record that references list entry `idx` rewrites the entry's frame so the
// dragged frames land on the hovered row:
//   row == 0: frame = rec.frame                       (direct copy)
//   row <  0: parent chain via record field +4        (order-preserving)
//   row >  0: reverse walk, parent chain via field +8 (clamped, and the
//             entry frame feeds the this+0x9E16C / model+0x31B0 maxima)
// `limit` >= 0 selects the `idx >= limit` validity test (morph/IK), -1 the
// `idx > 0` test (bands/bone).  `maxModel` is model+0x31B0 for the model
// lists, nullptr for the band lists.
template <typename Key>
void RemapList(MMDApp* app, const int* recs, int count, Key* keys,
               int row, int limit, std::uint32_t* maxModel,
               int capacity = 10000) {
    if (keys == nullptr)
        return;
    // Track heads are permanent frame-zero anchors. Also normalize a head
    // left displaced by an older edit/load: merely skipping it preserves the
    // missing zero key forever. Keep its pose and links intact.
    const int headCount = std::clamp(limit >= 0 ? limit : 1, 0, capacity);
    for (int head = 0; head < headCount; ++head)
        keys[head].frame = 0;
    auto valid = [headCount, capacity](int idx) {
        return idx >= headCount && idx < capacity;
    };
#ifdef MIKUMIKUSTUDIO_DIAG
    // TEMP(debug, keyframe-drag crash): validate the record buffer before
    // the remap walks it - rec[0] must be a list index within capacity.
    // Logs and skips the whole remap instead of dereferencing a wild index.
    // (Diagnostic only: the original walks the records unvalidated.)
    for (int i = 0; i < count; ++i) {
        const int idx = recs[4 * i];
        if (idx < 0 || idx >= capacity) {
            std::fprintf(stderr,
                         "REMAP-GUARD: rec[%d/%d]={%d,%d,%d,%d} row=%d "
                         "limit=%d capacity=%d keys=%p recs=%p\n",
                         i, count, idx, recs[4 * i + 1], recs[4 * i + 2],
                         recs[4 * i + 3], row, limit, capacity,
                         static_cast<void*>(keys), static_cast<const void*>(recs));
            for (int d = 0; d < 8 && d < count; ++d)
                std::fprintf(stderr, "  rec[%d]={%d,%d,%d,%d}\n", d,
                             recs[4 * d], recs[4 * d + 1], recs[4 * d + 2],
                             recs[4 * d + 3]);
            std::fflush(stderr);
            return;
        }
    }
#endif
    if (row == 0) {
        for (int i = 0; i < count; ++i) {
            const int* rec = recs + 4 * i;
            if (valid(rec[0]))
                keys[rec[0]].frame = rec[3];
        }
    } else if (row < 0) {
        for (int i = 0; i < count; ++i) {
            const int* rec = recs + 4 * i;
            const int idx = rec[0];
            if (valid(idx)) {
                Key& key = keys[idx];
                const std::int32_t pf = static_cast<std::int32_t>(
                    keys[key.previous].frame);
                if (pf + 1 < row + rec[3]) {
                    const std::int32_t v = row + rec[3];
                    if (v > 0)
                        key.frame = v;
                } else {
                    key.frame = pf + 1;
                }
            }
        }
    } else {
        for (int i = count - 1; i >= 0; --i) {
            const int* rec = recs + 4 * i;
            const int idx = rec[0];
            if (valid(idx)) {
                Key& key = keys[idx];
                if (key.next == 0) {
                    key.frame = row + rec[3];
                } else {
                    const std::int32_t nextFrame =
                        static_cast<std::int32_t>(keys[key.next].frame);
                    if (nextFrame - 1 > row + rec[3])
                        key.frame = row + rec[3];
                    else
                        key.frame = nextFrame - 1;
                }
                const std::uint32_t frame = key.frame;
                if (maxModel != nullptr) {
                    if (*maxModel < frame)
                        *maxModel = frame;
                    if (app->LastRegisteredFrame() < *maxModel)
                        app->LastRegisteredFrame() = *maxModel;
                } else if (app->LastRegisteredFrame() < frame) {
                    app->LastRegisteredFrame() = frame;
                }
            }
        }
    }
}

// Accessory-list variant of RemapList (0x445621..0x445833): the offset
// records carry the index at +4 and the accessory slot at +8; the list base
// is this+0x384[slot] and the records are 60 bytes.
void RemapAccList(MMDApp* app, const int* recs, int count, int row) {
    for (int slot = 0; slot < 255; ++slot)
        if (auto* keys = app->AccessoryKeys(slot))
            keys[0].frame = 0;
    auto listOf = [app](int slot) {
        return app->AccessoryKeys(slot);
    };
#ifdef MIKUMIKUSTUDIO_DIAG
    // TEMP(debug, keyframe-drag crash): same validation as RemapList - the
    // accessory records carry the key index at +4 and the slot at +8.
    // (Diagnostic only: the original walks the records unvalidated.)
    for (int i = 0; i < count; ++i) {
        const int idx = recs[4 * i + 1];
        const int slot = recs[4 * i + 2];
        if (idx < 0 || slot < 0 || slot >= 255 || listOf(slot) == nullptr ||
            idx >= static_cast<int>(mdl::KeyPoolCapacity(listOf(slot)))) {
            std::fprintf(stderr,
                         "REMAPACC-GUARD: rec[%d/%d]={%d,%d,%d,%d} row=%d\n",
                         i, count, recs[4 * i], idx, slot, recs[4 * i + 3],
                         row);
            for (int d = 0; d < 8 && d < count; ++d)
                std::fprintf(stderr, "  rec[%d]={%d,%d,%d,%d}\n", d,
                             recs[4 * d], recs[4 * d + 1], recs[4 * d + 2],
                             recs[4 * d + 3]);
            std::fflush(stderr);
            return;
        }
    }
#endif
    if (row == 0) {
        for (int i = 0; i < count; ++i) {
            const int* rec = recs + 4 * i;
            const int idx = rec[1];
            if (idx > 0)
                listOf(rec[2])[idx].frame = rec[3];
        }
    } else if (row < 0) {
        for (int i = 0; i < count; ++i) {
            const int* rec = recs + 4 * i;
            const int idx = rec[1];
            if (idx > 0) {
                mdl::AccessoryKey* list = listOf(rec[2]);
                mdl::AccessoryKey& key = list[idx];
                const std::int32_t pf = static_cast<std::int32_t>(
                    list[key.previous].frame);
                if (pf + 1 < row + rec[3]) {
                    const std::int32_t v = row + rec[3];
                    if (v > 0)
                        key.frame = v;
                } else {
                    key.frame = pf + 1;
                }
            }
        }
    } else {
        for (int i = count - 1; i >= 0; --i) {
            const int* rec = recs + 4 * i;
            const int idx = rec[1];
            if (idx > 0) {
                mdl::AccessoryKey* list = listOf(rec[2]);
                mdl::AccessoryKey& key = list[idx];
                if (key.next == 0) {
                    key.frame = row + rec[3];
                } else {
                    const std::int32_t nextFrame =
                        static_cast<std::int32_t>(list[key.next].frame);
                    if (nextFrame - 1 > row + rec[3])
                        key.frame = row + rec[3];
                    else
                        key.frame = nextFrame - 1;
                }
                const std::uint32_t frame = key.frame;
                if (app->LastRegisteredFrame() < frame)
                    app->LastRegisteredFrame() = frame;
            }
        }
    }
}

// Write the hover flag value (Ctrl not active) into a band record flag byte.
inline unsigned char HoverFlag(MMDApp* app) {
    return static_cast<unsigned char>(!app->CtrlModifierActive());
}

void SetAccessorySelected(MMDApp* app, int slot, int keyIndex,
                          std::uint8_t selected) {
    auto* keys = app->AccessoryKeys(slot);
    keys[keyIndex].selected = selected;
}

}  // namespace

// ===========================================================================
// WM_MOUSEMOVE handler (see header comment for the flow map).
// ===========================================================================
void HandleMouseMove(std::uint32_t lParam, int mouseY) {
    MMDApp* app = g_Block;
    if (OwnsCenteredViewportDrag(app)) return;
    const int X = static_cast<std::int16_t>(lParam & 0xFFFFu);
    const int Y = static_cast<std::int16_t>(mouseY);

    // --- 1. hide-rect gates (0x444CC6) -------------------------------------
    if (app->state.a06B5 != 0) {
        D3DRenderer* sub = app->Renderer();
        const int ratio = static_cast<int>(
            sub->viewScale * 200.0);  // viewScale = sub+0x1D4F0 float ratio
        if (X > static_cast<int>(app->state.hideLeft) - ratio)
            return;
        app->state.a06B5 = 0;
    }
    if (app->state.a06B4 != 0) {
        if (Y > app->state.hideBottom)
            return;
        app->state.a06B4 = 0;
    }
    if (app->state.a06B6 != 0) {
        if (X > app->state.hideLeft ||
            X < app->state.hideRight)
            return;
        // Original stores (X < 0xA0D40), which is false on this path (0x444D55).
        app->state.a06B6 = 0;
    }

    // --- 2. position store + cursor switching (0x444D5B) -------------------
    if (app->MouseY() != Y || app->MouseX() != X) {
        const int previousX = app->MouseX();
        app->MouseX() = X;
        app->MouseY() = Y;
        if (X > 0xEA60)
            app->MouseX() = X - 0x10000;
        if (Y > 0xEA60)
            app->MouseY() = Y - 0x10000;
        if (app->state.separateWindowMouseSeen != 0) {
            const int x0 = app->MouseX();
            const int y0 = app->MouseY();
            if (std::abs(app->PreviousMouseX() - x0) > 50 ||
                std::abs(app->PreviousMouseY() - y0) > 50)
                app->state.mouseJumped = 1;
            app->PreviousMouseX() = x0;
            app->PreviousMouseY() = y0;
            app->state.separateWindowMouseSeen = 0;
        }
        RECT rc;
        GetClientRect(static_cast<HWND>(app->Hwnd()), &rc);
        if ((app->SidebarWidth() <=
                 app->MouseX() && app->MouseY() <= rc.bottom - 158 &&
             app->MouseX() <=
                 app->SidebarWidth() + 6) &
            (app->FloatingWindow() == nullptr)) {
            SetCursor(LoadCursorA(nullptr,
                                  reinterpret_cast<LPCSTR>(
                                      static_cast<INT_PTR>(0x7F84))));  // IDC_SIZEWE
        }

        // --- 3. sidebar drag ---------------------------------------------
        // Keep the last pointer position, but leave every control and the
        // viewport in place until WM_LBUTTONUP commits the new width.
        if (app->SidebarResizeDragging() != 0) {
            SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_SIZEWE)));
            return;
        }
        if (app->ViewportToolHovered() == 1) {
            SetCursor(LoadCursorA(nullptr,
                                  reinterpret_cast<LPCSTR>(
                                      static_cast<INT_PTR>(0x7F89))));  // IDC_HAND
        }

        // --- 4. drag-frame remap (0x444F46) --------------------------------
        if (app->SelectionBoxDragging() != 0 && app->TimelineSelectionChanged() != 0) {
            const int row =
                (app->MouseX() -
                 app->SelectionBoxAnchorX() - 6) /
                kRowPitch;  // v15 == v257 (magic /13 = truncating)
            const int previousRow =
                (previousX - app->SelectionBoxAnchorX() - 6) / kRowPitch;
            // A click or vertical/sub-cell motion does not move any keys.
            // Avoid rewriting every selected key and re-seeking physics until
            // the frame offset changes; returning to offset zero still applies.
            if (row == previousRow) return;
            app->SceneModified() = 1;
            if (app->state.optflag[0] == 0) {
                // ---- bone-edit mode (0x445887..0x446065) ------------------
                unsigned char* model = CurrentModel(app);
                mdl::ModelRecord& record = *mdl::Mdl(model);
                // bone list: 28-byte records, idx > 0 (0x44588F..0x445B30)
                RemapList(app,
                          reinterpret_cast<const int*>(app->TimelineSelectionRecords(
                              TimelineSelectionBand::ModelBone)),
                          app->TimelineSelectionCount(TimelineSelectionBand::ModelBone),
                          mdl::DisplayKeys(model), row, -1,
                          &record.maxFrame,
                          static_cast<int>(mdl::KeyPoolCapacity(mdl::DisplayKeys(model))));
                // morph list: 20-byte records, idx >= model count
                // (0x445B38..0x445D90)
                RemapList(app,
                          reinterpret_cast<const int*>(app->TimelineSelectionRecords(
                              TimelineSelectionBand::ModelMorph)),
                          app->TimelineSelectionCount(
                              TimelineSelectionBand::ModelMorph),
                          mdl::MorphKeys(model), row,
                          static_cast<std::int32_t>(record.morphCount),
                          &record.maxFrame,
                          static_cast<int>(mdl::MorphKeyCapacity(record)));
                // IK list: 60-byte records, idx >= model count
                // (0x445D9E..0x446040)
                RemapList(app,
                          reinterpret_cast<const int*>(app->TimelineSelectionRecords(
                              TimelineSelectionBand::ModelIk)),
                          app->TimelineSelectionCount(TimelineSelectionBand::ModelIk),
                          mdl::BoneKeys(model), row,
                          static_cast<std::int32_t>(record.boneCount),
                          &record.maxFrame,
                          static_cast<int>(mdl::BoneKeyCapacity(record)));
                PanelPaint(app);  // 0x446044
                // 0x446065: original __thiscall(this=model, dword0x980,
                // dword0xA0CC4).
                SeekModelFrame(model,
                          app->state.currentFrame,
                          app->state.playbackPhysicsMode);
            } else {
                // ---- display mode (0x444F8A..0x44587D) --------------------
                // four fixed band lists (84/40/24/36-byte records)
                RemapList(app,
                          reinterpret_cast<const int*>(app->TimelineSelectionRecords(
                              TimelineSelectionBand::Camera)),
                          app->TimelineSelectionCount(TimelineSelectionBand::Camera),
                          app->CameraKeys(),
                          row, -1, nullptr);
                RemapList(app,
                          reinterpret_cast<const int*>(app->TimelineSelectionRecords(
                              TimelineSelectionBand::Light)),
                          app->TimelineSelectionCount(TimelineSelectionBand::Light),
                          app->LightKeys(),
                          row, -1, nullptr);
                RemapList(app,
                          reinterpret_cast<const int*>(app->TimelineSelectionRecords(
                              TimelineSelectionBand::SelfShadow)),
                          app->TimelineSelectionCount(TimelineSelectionBand::SelfShadow),
                          app->ShadowKeys(),
                          row, -1, nullptr);
                RemapList(app,
                          reinterpret_cast<const int*>(app->TimelineSelectionRecords(
                              TimelineSelectionBand::Gravity)),
                          app->TimelineSelectionCount(TimelineSelectionBand::Gravity),
                          app->GravityKeys(),
                          row, -1, nullptr);
                RemapAccList(app,
                             reinterpret_cast<const int*>(app->TimelineSelectionRecords(
                                 TimelineSelectionBand::Accessory)),
                             app->TimelineSelectionCount(
                                 TimelineSelectionBand::Accessory), row);
                PanelPaint(app);   // 0x445837
                ReloadModels(app);    // 0x44583E
                RefreshLightPanel(app);    // 0x445845
                RefreshSelfShadowPanel(app);    // 0x44584C
                ApplyGravityTrack(app);    // 0x445853
                // per-slot refresh for every set flag (0x445858..0x445879)
                for (int i = 0; i < 255; ++i) {
                    if (app->ObjectSlot(i) != nullptr)
                        ApplyAccessoryTrack(app, i);
                }
                SyncAccessoryEditPanel(app);    // 0x44587D
            }
            mikumikustudio::curve_editor::NotifyOwnedChanged(app->Hwnd());
        } else if (app->SelectionBoxDragging() != 0) {
            // ---- 5. hover highlight (0x44606F..0x446A2C) ------------------
            // hover row/column range from the anchor (this+0xA018C/0xA0190)
            int xEnd = app->MouseX();
            int xStart;
            if (xEnd <= app->SelectionBoxAnchorX() + 6) {
                xStart = xEnd;
                xEnd = app->SelectionBoxAnchorX() + 6;
            } else {
                xStart = app->SelectionBoxAnchorX() + 6;
            }
            int yTop, yBottom;
            if (app->MouseY() <=
                app->SelectionBoxAnchorY() + 145) {
                yTop = app->MouseY();
                yBottom = app->SelectionBoxAnchorY() + 145;
            } else {
                yTop = app->SelectionBoxAnchorY() + 145;
                yBottom = app->MouseY();
            }
            int rowStart = (xStart - kGridX0) / kRowPitch;  // 0x4460BC
            if (xStart - kRowPitch * rowStart - kGridX0 <= 6)
                rowStart -= 1;
            if (rowStart < 0)
                rowStart = 0;
            int rowEnd = (xEnd - kGridX0) / kRowPitch;      // 0x4460F8
            if (xEnd - kRowPitch * rowEnd - kGridX0 > 6)
                rowEnd += 1;
            const int frameColumns = std::clamp((app->SidebarWidth() - 84) / 13, 0, 200);
            rowStart = std::clamp(rowStart, 0, frameColumns);
            rowEnd = std::clamp(rowEnd, 0, frameColumns);
            int colStart = (yTop - kColY0) / kColPitch;     // 0x446135
            if (yTop - kColPitch * colStart - kColY0 <= 7)
                colStart -= 1;
            if (colStart < 0)
                colStart = 0;
            int colEnd = (yBottom - kColY0) / kColPitch;    // 0x44617B
            if (yBottom - kColPitch * colEnd - kColY0 > 7)
                colEnd += 1;
            colStart = std::clamp(colStart, 0, 200);
            colEnd = std::clamp(colEnd, 0, 200);

            const unsigned char flag = HoverFlag(app);
            // A replacement marquee clears hidden and scrolled-out keys too.
            if (!app->ShiftModifierActive()) ClearTimelineKeySelection(app);
            if (app->state.optflag[0] != 0) {
                // ---- display-mode hover (0x4461B3) ------------------------
                // hovered-row highlight for the four fixed bands
                // (0x446342..0x44649F)
                if (yBottom >= 160 && yTop <= 174 && rowStart < rowEnd) {
                    auto* keys = app->CameraKeys();
                    int* m = app->state.rowHitBand0 + rowStart;
                    int n = rowEnd - rowStart;
                    do {
                        if (*m >= 0)
                            keys[*m].selected = flag;
                        ++m;
                    } while (--n);
                }
                if (yBottom >= 174 && yTop <= 188 && rowStart < rowEnd) {
                    auto* keys = app->LightKeys();
                    int* m = app->state.rowHitBand1 + rowStart;
                    int n = rowEnd - rowStart;
                    do {
                        if (*m >= 0)
                            keys[*m].selected = flag;
                        ++m;
                    } while (--n);
                }
                if (yBottom >= 188 && yTop <= 202 && rowStart < rowEnd) {
                    auto* keys = app->ShadowKeys();
                    int* m = app->state.rowHitBand2 + rowStart;
                    int n = rowEnd - rowStart;
                    do {
                        if (*m >= 0)
                            keys[*m].selected = flag;
                        ++m;
                    } while (--n);
                }
                if (yBottom >= 202 && yTop <= 216 && rowStart < rowEnd) {
                    auto* keys = app->GravityKeys();
                    int* m = app->state.rowHitBand3 + rowStart;
                    int n = rowEnd - rowStart;
                    do {
                        if (*m >= 0)
                            keys[*m].selected = flag;
                        ++m;
                    } while (--n);
                }
                // accessory grid rows x columns (0x4464A1..0x446534)
                if (rowStart < rowEnd) {
                    int* acc = app->state.rowHitAcc +
                               (colStart + 200 * rowStart);
                    int rows = rowEnd - rowStart;
                    do {
                        if (colStart < colEnd) {
                            int* slots = app->state.jointLineMap + colStart;
                            int cols = colEnd - colStart;
                            do {
                                if (*acc >= 0 && *slots >= 0)
                                    SetAccessorySelected(
                                        app, *slots, *acc, flag);
                                ++acc;
                                ++slots;
                            } while (--cols);
                        }
                        acc += 200 - (colEnd - colStart);
                    } while (--rows);
                }
            } else {
                // ---- bone-edit mode hover (0x44653B) ----------------------
                unsigned char* model = CurrentModel(app);
                if (model == nullptr)
                    return;
                mdl::DisplayKey* displayKeys = mdl::DisplayKeys(model);
                mdl::MorphKey* morphKeys = mdl::MorphKeys(model);
                mdl::BoneKey* boneKeys = mdl::BoneKeys(model);
                // hovered-row highlight over the morph map grid
                // (0x446815..0x446A0F)
                int colStartB = (yTop - kColY0Bone) / kColPitch;
                if (yTop - kColPitch * colStartB - kColY0Bone <= 7)
                    colStartB -= 1;
                if (colStartB < 0)
                    colStartB = 0;
                int colEndB = (yBottom - kColY0Bone) / kColPitch;
                if (yBottom - kColPitch * colEndB - kColY0Bone > 7)
                    colEndB += 1;
                colStartB = std::clamp(colStartB, 0, 200);
                colEndB = std::clamp(colEndB, 0, 200);
                if (rowStart < rowEnd) {
                    int* map = app->state.rowHitMorph +
                               (colStartB + 200 * rowStart);
                    int rows = rowEnd - rowStart;
                    do {
                        if (colStartB < colEndB) {
                            int cols = colEndB - colStartB;
                            do {
                                const int bv = map[40000];  // bone map
                                if (bv <= 0) {
                                    if (bv == -10)
                                        displayKeys[0].allocated = flag;
                                } else {
                                    displayKeys[bv].allocated = flag;
                                }
                                const int mv = map[0];      // morph map
                                if (mv <= 0) {
                                    if (mv == -10)
                                        morphKeys[0].allocated = flag;
                                } else {
                                    morphKeys[mv].allocated = flag;
                                }
                                const int iv = map[-40000]; // IK map
                                if (iv <= 0) {
                                    if (iv == -10)
                                        boneKeys[0].allocated = flag;
                                } else {
                                    boneKeys[iv].allocated = flag;
                                }
                                ++map;
                            } while (--cols);
                        }
                        // The inner loop already advanced by the selected
                        // track count. Each frame starts exactly 200 entries
                        // after the preceding frame, regardless of box height.
                        map += 200 - (colEndB - colStartB);
                    } while (--rows);
                }
                // hover rect (0x446A13..0x446A25)
                app->TimelineRangeFirstOffset() = rowStart;
                app->TimelineRangeLastOffset() = rowEnd;
                app->TimelineRangeFirstBase() = colStartB;
                app->TimelineRangeLastBase() = colEndB;
            }
            PanelPaint(app);  // 0x446A2D
            // Keep the range until mouse-up. PanelPaint also resolves keys
            // in collapsed groups; an unrelated repaint must see this same
            // range instead of clearing the selection with an empty box.
        }
    }

    // --- 6. physics-enabled hover helper (0x446A4C) -------------------------
    if (app->PendingTimelineSelectionRow() != TimelineSelectionRow::None)
        DragInterpolationControlPoint(app);
}

}  // namespace mikumikustudio
