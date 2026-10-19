// ===========================================================================
// VA 0x00423420 / 0x004757C3 - text and selection-line overlay producers
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/viewport_corner_settings.hpp"
#include "mikumikustudio/viewport_tool_layout.hpp"
#include "vb_dump.hpp"

namespace mikumikustudio {

// MMS addition: the bone-scale readout of the top status bar (ui_palette.cpp).
// One change-guarded tick per frame keeps that painted text following bone
// selection, frame and coordinate-mode changes; it repaints only when the
// formatted values change.

namespace {

struct TextVertex {
    float x, y, z;
    std::uint32_t rhw;
    D3DCOLOR color;
    float u, v;
};
static_assert(sizeof(TextVertex) == 28);

struct LineVertex {
    float x, y, z, rhw;
    D3DCOLOR color;
};
static_assert(sizeof(LineVertex) == 20);

void PutQuad(TextVertex*& out, float left, float top, float right,
             float bottom, float u0, float v0, float u1, float v1,
             D3DCOLOR color) {
    const TextVertex vertices[6] = {
        {right, top,    0.0f, 0x3F800000u, color, u1, v0},
        {right, bottom, 0.0f, 0x3F800000u, color, u1, v1},
        {left,  top,    0.0f, 0x3F800000u, color, u0, v0},
        {left,  top,    0.0f, 0x3F800000u, color, u0, v0},
        {right, bottom, 0.0f, 0x3F800000u, color, u1, v1},
        {left,  bottom, 0.0f, 0x3F800000u, color, u0, v1},
    };
    for (const TextVertex& vertex : vertices)
        *out++ = vertex;
}

float OverlayScale(MMDApp* app) {
    return FrameOverlayScale(app);
}

}  // namespace

void PrepareFrameTextOverlay(MMDApp* app) {  // 0x423420
    auto* vb = app->OverlayVertices();
    app->TextOverlayPrimitiveCount() = 0;
    if (vb == nullptr)
        return;

    TextVertex* out = nullptr;
    if (FAILED(vb->Lock(0, 0x6D60, reinterpret_cast<void**>(&out), 0)) ||
        out == nullptr)
        return;
    TextVertex* const begin = out;

    const float scale = OverlayScale(app);
    const RECT view = FrameOverlayRect(app);
    auto append = [&](float left, float top, float right, float bottom,
                      float u0, float v0, float u1, float v1,
                      D3DCOLOR color) {
        PutQuad(out, left, top, right, bottom, u0, v0, u1, v1, color);
        app->TextOverlayPrimitiveCount() += 2;
    };
    auto pair = [&](float left, float top, float right, float bottom,
                    float u0, float v0, float u1, float v1,
                    D3DCOLOR foreground, D3DCOLOR shifted) {
        append(left, top, right, bottom, u0, v0, u1, v1, foreground);
        append(left - 2.0f, top - 2.0f, right - 2.0f, bottom - 2.0f,
               u0, v0, u1, v1, shifted);
    };
    auto digits = [&](const char* text, float& xOffset, float top,
                      float atlasV0, float atlasV1,
                      D3DCOLOR foreground, D3DCOLOR shifted) {
        for (const char* p = text; *p != '\0'; ++p) {
            if (*p == ' ') {
                xOffset += 12.0f;
                continue;
            }
            const int digit = *p - '0';
            if (digit < 0 || digit > 9)
                continue;
            const float u0 = static_cast<float>(16 * digit) / 512.0f;
            const float left = static_cast<float>(view.left +
                static_cast<int>(xOffset * scale));
            pair(left, top, left + 12.5f * scale, top + 25.0f * scale,
                 u0, atlasV0, u0 + 15.0f / 512.0f, atlasV1,
                 foreground, shifted);
            xOffset += 12.0f;
        }
    };

    // 0x423591..0x423EBF: information/FPS. All positions first truncate
    // the scaled offset to int, matching the original x87-to-int sequence.
    if (app->FpsOverlayEnabled() != 0) {
        const int fps = app->FramesPerSecond();
        const float fpsTop = static_cast<float>(view.bottom -
            static_cast<int>(30.0f * scale));
        if (fps > 0) {
            float xOffset = 15.0f;
            const bool numeric = app->PlaybackActive() != 0 ||
                app->state.frameVolumeControlEnabled == 0;
            if (numeric) {
                char value[256]{};
                std::snprintf(value, sizeof(value), "%3d", fps);
                digits(value, xOffset, fpsTop, 0.375f, 0.43554688f,
                       0xFFE0E0E0u, 0xFF0000FFu);
            } else {
                // 0x42382F: the A4420 path substitutes two fixed glyphs.
                xOffset = 27.0f;
                for (int glyph = 0; glyph < 2; ++glyph) {
                    const float left = static_cast<float>(view.left +
                        static_cast<int>(xOffset * scale));
                    pair(left, fpsTop, left + 12.5f * scale,
                         fpsTop + 25.0f * scale,
                         0.4375f, 0.375f, 0.46679688f, 0.43554688f,
                         0xFFE0E0E0u, 0xFF0000FFu);
                    xOffset += 12.0f;
                }
            }
            xOffset += 4.0f;
            const float left = static_cast<float>(view.left +
                static_cast<int>(xOffset * scale));
            pair(left, fpsTop, left + 90.0f * scale,
                 fpsTop + 25.0f * scale,
                 0.3125f, 0.375f, 0.546875f, 0.43554688f,
                 0xFFE0E0E0u, 0xFF0000FFu);
        } else {
            // The two original captions live side by side in row 224:
            // "1fps or less" at the English anchor and "1fps 未満" at 208.
            // DrawUiGlyph substitutes the Chinese wording at the English
            // anchor, so only the Japanese UI takes the second cell; treating
            // every non-English UI as Japanese made the Chinese caption read
            // "1fps 未満" (defect reported 2026-09-22).
            const bool japanese = app->EnglishUI() == kUiJapanese;
            const float left = static_cast<float>(view.left -
                static_cast<int>(-30.5f * scale));
            const float width = (japanese ? 125.0f : 150.0f) * scale;
            pair(left, fpsTop, left + width, fpsTop + 25.0f * scale,
                 japanese ? 0.375f : 0.0f, 0.4375f,
                 japanese ? 0.68554688f : 0.37304688f, 0.49804688f,
                 0xFFE0E0E0u, 0xFF0000FFu);
        }
    }

    // 0x423EC5..0x4249F7: playback frame, bar and HH:MM:SS cells.
    if (app->FpsOverlayEnabled() != 0 &&
        app->PlaybackActive() != 0 && app->FrameStepPlayback() == 0) {
        char frameText[256]{};
        const int frame = static_cast<int>(30.0f * app->PlaybackCursorSeconds());
        std::snprintf(frameText, sizeof(frameText), "%d", frame);
        float xOffset = 15.0f;
        const float top = static_cast<float>(view.top -
            static_cast<int>(-5.0f * scale));
        digits(frameText, xOffset, top, 0.5f, 0.56054688f,
               0xFF000000u, 0xFFFFFFFFu);
        xOffset += 4.0f;
        // Keep the label and punctuation in separate atlas cells. A single
        // stretched bar overlapped the digits and clipped localized labels.
        const auto glyph = [&](int atlasX, int atlasWidth, float width) {
            const float left = view.left + xOffset * scale;
            pair(left, top, left + width * scale, top + 25.0f * scale,
                 atlasX / 512.0f, 0.5f, (atlasX + atlasWidth) / 512.0f,
                 0.56054688f, 0xFF000000u, 0xFFFFFFFFu);
            xOffset += width;
        };
        glyph(160, 127, 75.0f);
        const int seconds = static_cast<int>(app->PlaybackCursorSeconds());
        const int parts[] = {seconds / 3600, (seconds / 60) % 60, seconds % 60};
        for (int i = 0; i < 3; ++i) {
            char timePart[32]{};
            std::snprintf(timePart, sizeof(timePart), "%02d", parts[i]);
            digits(timePart, xOffset, top, 0.5f, 0.56054688f,
                   0xFF000000u, 0xFFFFFFFFu);
            if (i < 2) glyph(288, 15, 12.5f);
        }
        glyph(384, 15, 12.5f);
    }

    const bool playback = app->PlaybackActive() != 0;

    // Coordinate-space selector caption (original sprite strip 0x46C414). The
    // caption uses one measured atlas cell per state, centred in the 120x30
    // selector rectangle; the strings use the proportional HUD font
    // (Times New Roman; Microsoft YaHei UI for the Chinese wording), so
    // per-character cells neither align nor share the same width. This left the
    // three states at different offsets and pushed the Chinese captions off
    // centre.
    //
    // The panel determines the third state. With the model/bone panel the state
    // follows the screen axes ("View"); with the camera/light/accessory panel it
    // edits the selected accessory ("Accessory"), which is why a camera has no
    // view state - its own frame IS the view. That accessory
    // state only exists while the panel has an accessory to edit, so an empty
    // list keeps the caption at Local/Global and prevents a stale state byte from
    // labeling the selector with a state it can no longer reach (EffectiveCoordinateState
    // applies the same clamp as the drag handlers and the gizmo).
    if (!playback) {
        static const char* const kModelCoordinateLabels[3] = {
            "Local", "Global", "View"};
        static const char* const kAccessoryCoordinateLabels[3] = {
            "Local", "Global", "Accessory"};
        const char* const* labels = app->state.optflag[0] != 0
            ? kAccessoryCoordinateLabels
            : kModelCoordinateLabels;
        const int index = EffectiveCoordinateState(app);
        SceneFontCell cell{};
        if (SceneFontLabelCell(labels[index], scale, &cell)) {
            // MMS addition: the caption strip belongs to the operation handle
            // block (rotate/move/scale rows + their hit test) and docks to the
            // same viewport corner, so its rectangle maps through the shared
            // viewport_tool_layout anchor.  Bottom-right docking keeps the
            // original numbers exactly; the FPS text and the status icons in
            // this file keep the real viewport corners and are untouched.
            const viewport_tool_layout::Anchor hud =
                viewport_tool_layout::Make(
                    view, scale, viewport_corner_settings::Current());
            const float stripLeft = hud.x - 130.0f * scale;
            // Top corners put this strip below the scale row.
            const float stripTop = hud.CoordDrawTop();
            const float stripWidth = 120.0f * scale;
            const float stripHeight = 30.0f * scale;
            // The cell comes from one of two caption tiers, so the u extent is
            // scaled by the same factor as the v extent: a 64-row cell holds the
            // same caption at twice the resolution.
            const float atlasToDevice =
                kSceneFontRowHeight * scale / cell.band;
            const float labelWidth = (cell.u1 - cell.u0) *
                                     kSceneFontAtlasSize * atlasToDevice;
            const float labelHeight = kSceneFontRowHeight * scale;
            // Centre horizontally on the live caption width and vertically on
            // the cell's row band so every state lands on the same pixels.
            const float left = stripLeft + (stripWidth - labelWidth) * 0.5f;
            const float top = stripTop + (stripHeight - labelHeight) * 0.5f;
            const float right = left + labelWidth;
            const float bottom = top + labelHeight;
            // Eight unit offsets of black behind the white face: a one-device-
            // pixel ring that keeps the caption readable over both bright and
            // dark viewport content.
            //
            // The ring must not follow the HUD scale.  It used to be a 1.25 *
            // scale offset in the four diagonal directions only, so on a 2560
            // pixel desktop (HUD scale 2.0) it drew a 2.5-pixel halo that
            // closed the counters of the 30-pixel glyph and turned the caption
            // into a white blob - the reported "the caption is drawn unclearly
            // in every language".  A fixed pixel weight keeps the same crisp
            // outline at every window size, and the four cardinal directions
            // cover the flat edges the diagonals alone left bare.
            static const float kOutlineOffsets[8][2] = {
                {-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f},
                {-0.71f, -0.71f}, {0.71f, -0.71f}, {-0.71f, 0.71f},
                {0.71f, 0.71f}};
            for (int pass = 0; pass < 2; ++pass) {
                const D3DCOLOR color = pass == 0 ? 0xFF000000u : 0xFFFFFFFFu;
                if (pass == 1) {
                    append(left, top, right, bottom, cell.u0, cell.v0,
                           cell.u1, cell.v1, color);
                    continue;
                }
                for (const auto& outline : kOutlineOffsets) {
                    append(left + outline[0], top + outline[1],
                           right + outline[0], bottom + outline[1],
                           cell.u0, cell.v0, cell.u1, cell.v1, color);
                }
            }
        }
    }

    // 0x424A1F..0x424D8F: transient renderer-state indication. These
    // overlays are deliberately unscaled and use alpha-byte color patterns.
    const std::uint8_t status = app->state.autoRepeat;
    if (status > 0 && status < 4) {
        const float left = static_cast<float>(view.right) - 160.0f;
        const float top = static_cast<float>(view.top) + 30.0f;
        const float u0 = static_cast<float>(16 * (4 - status)) / 512.0f;
        pair(left, top, left + 40.0f, top + 60.0f,
             u0, 96.0f / 512.0f, u0 + 15.0f / 512.0f,
             127.0f / 512.0f, 0xBB000000u, 0xBBFFFFFFu);
    } else if (status == 4) {
        const float left = static_cast<float>(view.right) - 200.0f;
        const float top = static_cast<float>(view.top) + 120.0f;
        append(left, top, left + 120.0f, top + 20.0f,
               176.0f / 512.0f, 130.0f / 512.0f,
               319.0f / 512.0f, 159.0f / 512.0f, 0xFFFFFFFFu);
    }

    DumpVertexBatch("text", begin,
        app->TextOverlayPrimitiveCount(), 84);
    vb->Unlock();
}

void PrepareFrameLineOverlay(MMDApp* app) {  // 0x4757C3..0x4759D8
    app->LineOverlayPrimitiveCount() = 0;
    if (!app->LeftMouseButtonHeld() ||
        app->state.viewportInputActive == 0 ||
        app->BoneBoxSelectionActive() == 0)
        return;

    D3DRenderer* sub = app->Renderer();
    if (sub == nullptr)
        return;
    // The renderer owns the selection and guide-line vertex buffer.
    auto* vb = sub->lineVertexBuffer;
    if (vb == nullptr)
        return;

    LineVertex* out = nullptr;
    if (FAILED(vb->Lock(0, 8 * sizeof(LineVertex),
                        reinterpret_cast<void**>(&out), 0)) || out == nullptr)
        return;

    const float left = static_cast<float>(std::min(
        app->BoneBoxStartX(), app->MouseX()));
    const float right = static_cast<float>(std::max(
        app->BoneBoxStartX(), app->MouseX()));
    const float top = static_cast<float>(std::min(
        app->BoneBoxStartY(), app->MouseY()));
    const float bottom = static_cast<float>(std::max(
        app->BoneBoxStartY(), app->MouseY()));
    const LineVertex vertices[8] = {
        {left,  top,    0.0f, 1.0f, 0xFFFFFFFFu},
        {left,  bottom, 0.0f, 1.0f, 0xFFFFFFFFu},
        {left,  bottom, 0.0f, 1.0f, 0xFFFFFFFFu},
        {right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu},
        {right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu},
        {right, top,    0.0f, 1.0f, 0xFFFFFFFFu},
        {right, top,    0.0f, 1.0f, 0xFFFFFFFFu},
        {left,  top,    0.0f, 1.0f, 0xFFFFFFFFu},
    };
    std::copy(std::begin(vertices), std::end(vertices), out);
    app->LineOverlayPrimitiveCount() = 4;
    DumpVertexBatch("line_selection", out, 4, 40);
    vb->Unlock();
}

}  // namespace mikumikustudio
