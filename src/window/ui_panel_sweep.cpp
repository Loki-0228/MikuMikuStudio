#include "mikumikustudio/display_capacity.hpp"
// ===========================================================================
// VA 0x0042F1E0 - PostLanguageSweep  (original: sub_42F1E0, ~0x1320 bytes)
// ===========================================================================
// Right-panel label-column sweep: shared tail of LocalizeUI (0x441AD0), the
// list-scroll handlers (0x440AC0 / 0x44BB30 / 0x44AEE0), HandleWindowSize
// (0x443300), HandleLButtonDown (0x446A70) and the model-load / command
// paths (0x460430, 0x458F80, 0x47E8A0, 0x46B090).  Everything draws into
// the offscreen panel DC (this+0x2D4) label column x 0..91, which WM_PAINT
// (0x47C0A0) later blits to the window at (6,145).
//
//   1. 0x42F1F2  compat DC over the panel back-store bitmap (this+0x2DC);
//      SRCCOPY of (0,0) size (91, listH @ sub+0x1D4E8) erases stale label
//      text, then DeleteDC; the 200 line-highlight flags (this+0xA04F8)
//      are zeroed.
//   2. 0x42F24C  bone-edit mode (optflag[0] @0x2F8 == 0), current model slot
//      (this+0x910 -> slots @0x780): root line = 604-byte-stride name at
//      model+0x26BC indexed by model+0x3900 (+20 for EN), highlighted when
//      the flag byte model+0x2D94[rootIdx] is set.  Link tables
//      model+0x2DB4 / 0x2DB8 are zeroed, the per-line type bytes
//      (model+0x2DC0) cleared and the 20-byte display records at
//      model+0x2E88 reset to -999; record 0 = model+0x3900, iteration
//      counter model+0x31A8 = 1.
//   3. 0x42FA13  display-frame iteration (count @0x26D4, byte): per frame
//      the 101-byte record (JP name @+0, EN name @+50, flag @+100) at
//      model+0x26D0; flag==0 frames list their bones ("+"/"＋" 0x52C95C
//      glyph for frame > 1), flag!=0 frames go to the face (frame 2) /
//      rigid-group sections ("-"/"－" 0x52C958).  Rows draw in the theme
//      normal colour (0xA065C) or highlight (0xA0660) with the flag byte
//      this+0xA04F8[line] set for highlighted rows.  Face records
//      (model+0x26DC, 46B: bone@+40, morph@+42 u16, flag@+44) and rigid
//      groups (model+0x26D8, 46B: bone@+40, rigid@+42 u16) back-link their
//      morph/rigid index to the line number via model+0x2DB8 / 0x2DB4
//      (-line); each drawn line's record value at model+0x2E88+4*line is
//      the rigid index or -1-morph.  Frame 1's head line is stored at
//      model+0x2DBC.
//   4. 0x42F252  display mode (optflag[0] != 0): fixed headers camera / light
//      / "s shadow" / gravity at y = 17/31/45/59 (EN literals, JP
//      Shift-JIS カメラ/照 明/セルフ影/重 力 @0x52C980..0x52C964),
//      highlighted per the toggle bytes this+0xA03E4..0xA03E7.
//   5. 0x42F5F6  joint list: the 255 joint pointers (this+0x9DD70) are
//      walked per type byte (record+0x49D) 0..254; rows below the joint
//      scroll gate (this+0x9DA48) draw the name (record+0x238) at
//      y = 73+14k, highlight per record+0x4AC, filling this+0x9DA50[line]
//      = joint index; this+0x9DA4C counts matches.
//   6. 0x42F730  tail call: PanelPaint (0x414610) repaints the icon/band
//      column, list scrollbar and selection box.
//
// Reference: ../translated/MikuMikuDance/fcn_0042f1e0.cpp is a mismatched
// decompiler dump (physics-debug renderer, not this function); the port
// follows the live IDA pseudocode/disassembly of VA 0x0042F1E0.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"

namespace mikumikustudio {

// Timeline rows are only 14 pixels high. Use a bounded Unicode cell rather
// than an ANSI TextOut baseline whose fallback font can spill into the next
// row or the frame-zero column on Chinese Windows.
void DrawTimelineLabel(MMDApp* app, HDC dc, const wchar_t* text,
                       int x, int iconY, COLORREF color) {
    LOGFONTW lf{};
    // Request character height, not cell height: YaHei UI's internal leading
    // otherwise shrinks Japanese/Chinese glyphs to about 8 pixels in this row.
    lf.lfHeight = -12;
    lf.lfWeight = FW_BOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, app->EnglishUI() == 2 ? L"Microsoft YaHei UI"
                         : app->EnglishUI() == 1 ? L"Arial" : L"MS PGothic");
    HFONT font = CreateFontIndirectW(&lf);
    const int saved = SaveDC(dc);
    SelectObject(dc, font);
    SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT cell{x, iconY - 1, 91, iconY + 13};
    DrawTextW(dc, text, -1, &cell,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    RestoreDC(dc, saved);
    DeleteObject(font);
}

namespace {

// --- panel layout constants --------------------------------------------------
constexpr std::size_t kListLeft = 91;         // 0x5B  label column width

// --- model display-tree offsets (model object, PMD/PMX chain) ---------------
// The per-line bookkeeping lives in ModelRecord::boneListSelLine /
// boneListRowType / boneListRowRecord (typed fields since the x86-raw
// constants drifted from the x64 layout: the old 0x2E88 -999 reset run
// landed on x64 boneCount/facialFrameCount and corrupted live models).
// The display-object rows below read mdl::AccessoryRecord fields directly
// (name / order / rowSelected) - the former raw x86 offsets 0x238/0x49D/
// 0x4AC sat 16 bytes off the x64 record and matched no order byte.

// JP panel texts (Shift-JIS; original byte_52C95C/52C958/52C980/52C978/
// 52C96C/52C964).  EN texts are inline literals (asc_52C960/52C6D0,
// aCamera/aLight/aSShadow/aGravity).
constexpr char kJpPlus[] = "\x81\x7b";                     // ＋ (0x52C95C)
constexpr char kJpMinus[] = "\x81\x7c";                    // － (0x52C958)
constexpr char kJpCamera[] = "\x83\x4a\x83\x81\x83\x89";   // カメラ (0x52C980)
constexpr char kJpLight[] = "\x8f\xc6\x81\x40\x96\xbe";    // 照 明 (0x52C978)
constexpr char kJpShadow[] = "\x83\x5a\x83\x8b\x83\x74\x89\x65";  // セルフ影 (0x52C96C)
constexpr char kJpGravity[] = "\x8f\x64\x81\x40\x97\xcd";  // 重 力 (0x52C964)

// Current model slot for the slot-order index (this+0x910) - 0x42F74B.
unsigned char* CurrentModel(MMDApp* app, std::size_t slotIdx) {
    return app->ModelSlot(static_cast<int>(slotIdx));
}

// Colour from the color.txt COLORREF `color` (themeColors[33] = 0xA065C
// normal / themeColors[34] = 0xA0660 highlight), split into the three bytes
// the original pushes.  COLORREF = 0x00BBGGRR, so little-endian byte 0 = R,
// 1 = G, 2 = B - the same bytes the former per-offset reads fetched.
void DrawPanelText(MMDApp* app, const char* text, HDC hdc, int size, int x,
                   int y, const std::uint32_t& color) {
    (void)size;
    std::wstring wide;
    text_encoding::Decode(text, 932, wide);
    DrawTimelineLabel(app, hdc, wide.c_str(), x, y, color);
}

// Display-frame name row (0x42FB18/0x42FB98 and siblings).
void DrawFrameName(MMDApp* app, const mdl::DisplayGroup& frame,
                   HDC hdc, int y, const std::uint32_t& color) {
    // Model-internal names follow the shared preference: Auto uses English
    // only with an English UI, while Japanese/Chinese UIs show model names.
    const bool english = UiUsesEnglishNames(app->EnglishUI());
    const auto name = text_encoding::DisplayGroupLabel(frame, english);
    DrawTimelineLabel(app, hdc, name.c_str(), 12, y, color);
}

// Rigid-group / face record name row (0x430401/0x430443, 0x43020C/0x430247).
void DrawRecordName(MMDApp* app, const mdl::ModelRecord& model,
                    const mdl::FrameGroup& record, bool morph, HDC hdc, int y,
                    const std::uint32_t& color) {
    const bool english = UiUsesEnglishNames(app->EnglishUI());
    // FrameGroup holds only a truncated SJIS mirror; PMX keeps the original
    // Unicode on the bone/morph itself. Match the names used by the combos.
    const auto name = morph
        ? text_encoding::MorphLabel(model.morphs[mikumikustudio::mdl::FrameTargetIndex(record)], english)
        : text_encoding::BoneLabel(model.boneTable[mikumikustudio::mdl::FrameTargetIndex(record)], english);
    DrawTimelineLabel(app, hdc, name.c_str(), 15, y, color);
}

// Fixed header label of the display-mode band column (0x42F292 etc.).
void DrawHeader(MMDApp* app, HDC hdc, const char* text, int y,
                const std::uint32_t& color) {
    if (app->EnglishUI() == 2) {
        if (const auto* translated = ChineseUiText(text)) {
            DrawTimelineLabel(app, hdc, translated, 12, y, color);
            return;
        }
    }
    DrawPanelText(app, text, hdc, 12, 12, y, color);
}

}  // namespace

void PanelPaint(MMDApp* app);  // VA 0x00414610 (defined in ui_panel_paint.cpp)

void PostLanguageSweep(MMDApp* app) {
    auto& s = *app;
    const bool english = UiUsesEnglishNames(s.EnglishUI());
    const std::size_t slotIdx = s.SelectedModelSlot();  // 2320
    HDC panel = s.PanelDC();                                       // 724
    // listH = the 0x1D574 render wrapper's "list height" field
    // (screenHeight, sub+0x1D4E8).
    const std::int32_t listH = s.Renderer()->screenHeight;

    // --- 1. erase the label column from the panel bitmap (0x42F1F2) --------
    HDC dc = CreateCompatibleDC(nullptr);
    SelectObject(dc, reinterpret_cast<HGDIOBJ>(s.state.bmpPanelSpare));  // 732
    BitBlt(panel, 0, 0, kListLeft, listH, dc, 0, 0, SRCCOPY);
    DeleteDC(dc);

    std::memset(s.PanelRowFlags(), 0, 0xC8);     // 200 flags

    if (s.state.optflag[0] == 0) {
        // === bone-edit mode: current model display tree (0x42F744) ========
        unsigned char* model = CurrentModel(app, slotIdx);
        mdl::ModelRecord* const record = mdl::Mdl(model);
        // All selection paths refresh this panel (list, viewport, box and
        // group selection). Keep the active edit target in the highlighted
        // set: deselecting it must not disable drags or leave numeric edits
        // pointing at an unselected bone. Preserve it while still selected.
        const int active = record->selectedBone;
        if (active < 0 || static_cast<unsigned>(active) >= record->boneCount ||
            record->boneSelection[active] == 0) {
            record->selectedBone = -1;
            for (std::uint32_t i = 0; i < record->boneCount; ++i) {
                if (record->boneSelection[i] != 0) {
                    record->selectedBone = static_cast<int>(i);
                    break;
                }
            }
        }
        auto* const boneLineByIndex =
            reinterpret_cast<std::int32_t*>(record->boneKeyIndices);
        auto* const morphLineByIndex =
            reinterpret_cast<std::int32_t*>(record->morphKeyIndices);
        const std::int32_t rootIdx =
            static_cast<std::int32_t>(record->displayRootBone);
        unsigned char* const flagTab = record->boneSelection;
        const mdl::BoneRecord& rootBone = mdl::Bones(model)[rootIdx];
        const auto rootName = text_encoding::BoneLabel(rootBone, english);
        if (flagTab[rootIdx] != 0) {
            DrawTimelineLabel(app, panel, rootName.c_str(), 12, 17,
                          s.state.themeColors[34]);
            s.PanelRowFlags()[0] = 1;
        } else {
            DrawTimelineLabel(app, panel, rootName.c_str(), 12, 17,
                          s.state.themeColors[33]);
        }

        // --- zero link tables, clear line records (0x42F88C..0x42F9FB) -----
        std::memset(boneLineByIndex, 0,
                    sizeof(std::uint32_t) * record->boneCount);
        std::memset(morphLineByIndex, 0,
                    sizeof(std::uint32_t) * record->morphCount);
        record->boneListSelLine = 0;
        for (int line0 = 0; line0 < 200; ++line0) {
            mdl::SetDisplayRowType(*record, line0, 0);
            record->boneListRowRecord[line0] = -999;
        }
        record->boneListRowRecord[0] = rootIdx;
        record->boneListRows = 1;

        // --- display-frame iteration (0x42FA13..0x4304F7) -------------------
        // Restructured from the original's LABEL_76/86/122/123/139/140 web
        // (Wave5-C): LABEL_76/86 were skip-the-highlight gotos (now the
        // anyFaceSelected / anyRigidVisible guards), LABEL_122 skipped the
        // row flag for unselected faces, LABEL_123/139 formed the face-list
        // loop and LABEL_140 the frame advance at the loop tail.  The
        // original's `v20 = v72` re-syncs were no-ops (both always equal -
        // only LABEL_140's `v72 = ++v20` changes either).
        const mdl::DisplayGroup* const frames = mdl::DisplayGroups(model);
        const mdl::FrameGroup* const faces = mdl::DisplayFrames(model);
        const mdl::FrameGroup* const groups = mdl::RigidGroups(model);
        const std::int32_t groupCnt =
            static_cast<std::int32_t>(record->rigidBodyCount);
        std::uint32_t frameIdx = 1;   // display-frame index
        std::uint32_t frameType = 1;  // frame type carried onto the next line
        int line = 1;                // tree line counter
        std::uint32_t faceIdx = 0;    // face index (0x430138)
        if (mikumikustudio::mdl::DisplayGroupCount(*mikumikustudio::mdl::Mdl(model)) <= 1) {
            PanelPaint(app);
            return;
        }
        for (;;) {
            const int frameNo = frameIdx;  // frame index fixed for this pass
            const bool visible = frames[frameIdx].flags == 0;
            const std::int32_t iter = record->boneListRows;
            const std::int32_t iterMax = record->boneListPos;

            if (visible) {
                // frame lists bones (0x42FA7E)
                if (iter > iterMax && line < 200) {
                    if (english) {
                        if (frameIdx > 1)
                            DrawPanelText(app, "+", panel, 12, 1, 14 * line + 17, 0);
                        DrawFrameName(app, frames[frameIdx], panel, 14 * line + 17,
                                      s.state.themeColors[33]);
                    } else {
                        if (frameIdx > 1)
                            DrawPanelText(app, kJpPlus, panel, 12, 1, 14 * line + 17, 0);
                        DrawFrameName(app, frames[frameIdx], panel, 14 * line + 17,
                                      s.state.themeColors[33]);
                    }
                    if (((frameType == 1) & record->displayKeyframesPresent) != 0) {
                        DrawFrameName(app, frames[frameIdx], panel, 14 * line + 17,
                                      s.state.themeColors[34]);
                        s.PanelRowFlags()[line] = 1;
                    }
                    if (mikumikustudio::mdl::FacialFrameCount(*record) != 0 && frameType == 2) {
                        // frame 2: highlight when any face flag (+44) is
                        // set - first face with a live flag (0x42FC57,
                        // LABEL_76 skipped when none is live).
                        bool anyFaceSelected = false;
                        for (unsigned int f = 0;
                             f < mikumikustudio::mdl::FacialFrameCount(*mikumikustudio::mdl::Mdl(model)); ++f) {
                            if (faces[f].selected != 0) {
                                anyFaceSelected = true;
                                break;
                            }
                        }
                        if (anyFaceSelected) {
                            DrawFrameName(app, frames[frameIdx], panel,
                                          14 * line + 17,
                                          s.state.themeColors[34]);
                            s.PanelRowFlags()[line] = 1;
                        }
                    }
                    // highlight when a rigid group of this frame is visible
                    // (0x42FD26; LABEL_86 skipped when none matched).
                    if (groupCnt) {
                        bool anyRigidVisible = false;
                        for (int g = 0; g < groupCnt; ++g) {
                            if (mikumikustudio::mdl::FrameGroupIndex(groups[g]) == frameNo &&
                                flagTab[mikumikustudio::mdl::FrameTargetIndex(groups[g])] != 0) {
                                anyRigidVisible = true;
                                break;
                            }
                        }
                        if (anyRigidVisible) {
                            DrawFrameName(app, frames[frameIdx], panel,
                                          14 * line + 17,
                                          s.state.themeColors[34]);
                            s.PanelRowFlags()[line] = 1;
                        }
                    }
                    // line type byte + morph/rigid back-links (0x42FE37)
                    mdl::SetDisplayRowType(*record, line, frameType);
                    if (frameType == 1)
                        record->boneListSelLine = line;
                    for (std::uint32_t j = 0;
                         j < mikumikustudio::mdl::FacialFrameCount(*mikumikustudio::mdl::Mdl(model)); ++j) {
                        const mdl::FrameGroup& face = faces[j];
                        if (mikumikustudio::mdl::FrameGroupIndex(face) + 1 == frameNo)
                            morphLineByIndex[mikumikustudio::mdl::FrameTargetIndex(face)] = -line;
                    }
                    if (groupCnt) {
                        for (unsigned int g = 0;
                             g < static_cast<unsigned int>(groupCnt); ++g) {
                            const mdl::FrameGroup& rigid = groups[g];
                            if (mikumikustudio::mdl::FrameGroupIndex(rigid) == frameNo)
                                boneLineByIndex[mikumikustudio::mdl::FrameTargetIndex(rigid)] = -line;
                        }
                    }
                    frameIdx = frameType;
                    ++line;
                }
                ++record->boneListRows;
            } else {
                // frame lists faces / rigid groups (0x42FF94)
                if (iter > iterMax && line < 200) {
                    if (english) {
                        if (frameIdx > 1)
                            DrawPanelText(app, "-", panel, 12, 1, 14 * line + 17, 0);
                        DrawFrameName(app, frames[frameIdx], panel, 14 * line + 17,
                                      s.state.themeColors[33]);
                    } else {
                        if (frameIdx > 1)
                            DrawPanelText(app, kJpMinus, panel, 12, 1, 14 * line + 17, 0);
                        DrawFrameName(app, frames[frameIdx], panel, 14 * line + 17,
                                      s.state.themeColors[33]);
                    }
                    mdl::SetDisplayRowType(*record, line++, frameType);
                    frameIdx = frameType;
                }
                ++record->boneListRows;
                if (frameIdx == 1) {
                    record->boneListSelLine = line - 1;
                } else if (mikumikustudio::mdl::FacialFrameCount(*record) != 0 && frameIdx == 2) {
                    // face section (frame 2) - list the frame's face records
                    // (0x430153..0x43030B; LABEL_122/123/139 were the skip /
                    // advance / exhausted edges of this loop).
                    faceIdx = 0;  // 0x430138
                    int recOff = line;  // index into boneListRowRecord
                    int rowY = 14 * line + 17;
                    for (;;) {
                        if (record->boneListRows > record->boneListPos &&
                            line < 200) {
                            const mdl::FrameGroup& face = faces[faceIdx];
                            DrawRecordName(app, *record, face, true, panel, rowY,
                                s.state.themeColors[face.selected != 0 ? 34 : 33]);
                            if (face.selected != 0)  // LABEL_122 skip
                                s.PanelRowFlags()[line] = 1;
                            record->boneListRowRecord[recOff] =
                                -1 - mikumikustudio::mdl::FrameTargetIndex(face);
                            morphLineByIndex[mikumikustudio::mdl::FrameTargetIndex(face)] = line++;
                            ++recOff;
                            rowY += 14;
                        }
                        ++record->boneListRows;  // LABEL_123
                        if (++faceIdx >= mikumikustudio::mdl::FacialFrameCount(*record))
                            break;               // LABEL_139
                    }
                } else if (groupCnt) {
                    // rigid-group section (0x43031D..0x4304D8)
                    int recOff = line;  // index into boneListRowRecord
                    int rowY = 14 * line + 17;
                    for (unsigned int g = 0;
                         g < static_cast<unsigned int>(groupCnt); ++g) {
                        const mdl::FrameGroup& rigid = groups[g];
                        if (mikumikustudio::mdl::FrameGroupIndex(rigid) == frameNo) {
                            if (record->boneListRows > record->boneListPos &&
                                line < 200) {
                                if (flagTab[mikumikustudio::mdl::FrameTargetIndex(rigid)] != 0) {
                                    DrawRecordName(app, *record, rigid, false, panel, rowY,
                                                   s.state.themeColors[34]);
                                    s.PanelRowFlags()[line] = 1;
                                } else {
                                    DrawRecordName(app, *record, rigid, false, panel, rowY,
                                                   s.state.themeColors[33]);
                                }
                                record->boneListRowRecord[recOff] =
                                    mikumikustudio::mdl::FrameTargetIndex(rigid);
                                boneLineByIndex[mikumikustudio::mdl::FrameTargetIndex(rigid)] = line++;
                                ++recOff;
                                rowY += 14;
                            }
                            ++record->boneListRows;
                        }
                    }
                }
            }

            // LABEL_140 advance: next display frame
            frameType = ++frameIdx;
            if (frameIdx >= mikumikustudio::mdl::DisplayGroupCount(*mikumikustudio::mdl::Mdl(model))) {
                PanelPaint(app);
                return;
            }
        }
    } else {
        // === display mode: fixed headers camera/light/shadow/gravity =======
        // (0x42F252..0x42F5F0). ChineseUiText uses English lookup keys for
        // these built-in captions; model names keep their own language rule.
        if (s.EnglishUI() != 0) {
            if (s.GlobalTrackSelected(GlobalTimelineTrack::Camera)) {
                DrawHeader(app, panel, "camera", 17, s.state.themeColors[34]);
                s.PanelRowFlags()[0] = 1;
            } else {
                DrawHeader(app, panel, "camera", 17, s.state.themeColors[33]);
            }
            if (s.GlobalTrackSelected(GlobalTimelineTrack::Light)) {
                DrawHeader(app, panel, "light", 31, s.state.themeColors[34]);
                s.PanelRowFlags()[1] = 1;
            } else {
                DrawHeader(app, panel, "light", 31, s.state.themeColors[33]);
            }
            if (s.GlobalTrackSelected(GlobalTimelineTrack::SelfShadow)) {
                s.PanelRowFlags()[2] = 1;
                DrawHeader(app, panel, "s shadow", 45, s.state.themeColors[34]);
            } else {
                DrawHeader(app, panel, "s shadow", 45, s.state.themeColors[33]);
            }
            if (s.GlobalTrackSelected(GlobalTimelineTrack::Gravity)) {
                s.PanelRowFlags()[3] = 1;
                DrawHeader(app, panel, "gravity", 59, s.state.themeColors[34]);
            } else {
                DrawHeader(app, panel, "gravity", 59, s.state.themeColors[33]);
            }
        } else {
            if (s.GlobalTrackSelected(GlobalTimelineTrack::Camera)) {
                DrawHeader(app, panel, kJpCamera, 17, s.state.themeColors[34]);
                s.PanelRowFlags()[0] = 1;
            } else {
                DrawHeader(app, panel, kJpCamera, 17, s.state.themeColors[33]);
            }
            if (s.GlobalTrackSelected(GlobalTimelineTrack::Light)) {
                DrawHeader(app, panel, kJpLight, 31, s.state.themeColors[34]);
                s.PanelRowFlags()[1] = 1;
            } else {
                DrawHeader(app, panel, kJpLight, 31, s.state.themeColors[33]);
            }
            if (s.GlobalTrackSelected(GlobalTimelineTrack::SelfShadow)) {
                s.PanelRowFlags()[2] = 1;
                DrawHeader(app, panel, kJpShadow, 45, s.state.themeColors[34]);
            } else {
                DrawHeader(app, panel, kJpShadow, 45, s.state.themeColors[33]);
            }
            if (s.GlobalTrackSelected(GlobalTimelineTrack::Gravity)) {
                s.PanelRowFlags()[3] = 1;
                DrawHeader(app, panel, kJpGravity, 59, s.state.themeColors[34]);
            } else {
                DrawHeader(app, panel, kJpGravity, 59, s.state.themeColors[33]);
            }
        }
    }

    // === accessory list below the fixed bands (0x42F5F6..0x42F72A) ==========
    // Walks the accessory slots grouped by their order byte 0..254 and
    // appends one panel row per loaded accessory, recording slot -> row in
    // jointLineMap for the click/hover handlers.
    std::memset(s.state.jointLineMap, 0xFF, 0x320);  // 200 ints
    s.DisplayObjectListMatchCount() = 1;
    std::uint8_t order = 0;                        // accessory order byte 0..254
    int ordinal = 0;                               // order counter (scroll gate)
    int line = 0;                                  // row below the fixed bands
    do {
        int slot = 0;                              // accessory slot index
        int rowY = 14 * line + 73;
        std::int32_t* map = s.state.jointLineMap + line;
        do {
            mdl::AccessoryRecord* accessory =
                static_cast<mdl::AccessoryRecord*>(s.ObjectSlot(slot));
            if (accessory != nullptr && accessory->order == order) {
                if (ordinal >= s.DisplayObjectListScrollPosition() &&
                    line < 200) {
                    if (accessory->rowSelected != 0) {
                        DrawTimelineLabel(app, panel, text_encoding::Display(accessory->name).c_str(), 12, rowY,
                                      s.state.themeColors[34]);
                        s.PanelRowFlags()[4 + line] = 1;
                    } else {
                        DrawTimelineLabel(app, panel, text_encoding::Display(accessory->name).c_str(), 12, rowY,
                                      s.state.themeColors[33]);
                    }
                    *map = slot;
                    ++line;
                    ++map;
                    rowY += 14;
                }
                ++s.DisplayObjectListMatchCount();
            }
            ++slot;
        } while (slot < 255);
        ++ordinal;
        ++order;
    } while (order != 0xFF);

    PanelPaint(app);  // 0x42F730 tail call (0x414610)
}

}  // namespace mikumikustudio
