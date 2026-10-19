// ===========================================================================
// VA 0x0042AE80 - InitSceneFontTexture  (original: sub_42AE80, 0x12C0 bytes)
// ===========================================================================
// Builds the in-scene HUD font atlas: a 512x512 A8R8G8B8 texture whose DC is
// filled with the overlay character set (verified draw list):
//   row 0   : axes glyphs + "Model:" "Bone:"
//   row 32  : "camera/light/accessory"
//   row 64  : "center" "X:" "angle" "X:" "distance" "Y:" "Z:"
//   row 96  : digits, "X:", ".", "X:"
//   row 128 : "Global" "Local" "Recording(Esc:Stop)" "Playing"
//   row 192 : digits "fps" "." "-"
//   row 224 : "1fps or less" "1fps "
//   row 256 : digits "frame ("
// then re-locks the destination texture and converts pixels to white with
// alpha = source blue channel.
//
// Original quirk (not replicated): the pixel loop writes past the declared
// stack frame via the return-address slot - a Ghidra artifact of a 1MB stack
// buffer.  The port uses an explicit heap buffer.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_translation.hpp"

namespace mikumikustudio {
namespace {

struct Glyph { const char* s; short x, y; };

// exact draw list from the decompilation (size 30, color 255,255,255, normal);
// SJIS bytes decoded from the original .rdata (row 0 spells
// カメラ照明アクセサリ glyph-by-glyph for per-char positioning)
constexpr Glyph kAtlas[] = {
    {"\xb6", 0, 0}, {"\xd2", 16, 0}, {"\xd7", 32, 0}, {"\xa5", 48, 0},
    {"\x8f\xc6\x96\xbe", 64, 0},   // 照明
    {"\xa5", 128, 0}, {"\xb1", 144, 0}, {"\xb8", 160, 0},
    {"\xbe", 176, 0}, {"\xbb", 192, 0}, {"\xd8", 208, 0},
    {"M", 256, 0}, {"o", 272, 0}, {"d", 288, 0}, {"e", 304, 0},
    {"l", 320, 0}, {":", 336, 0}, {"B", 352, 0}, {"o", 368, 0},
    {"n", 384, 0}, {"e", 400, 0}, {":", 416, 0},
    {"c", 0, 32}, {"a", 16, 32}, {"m", 32, 32}, {"e", 48, 32},
    {"r", 64, 32}, {"a", 80, 32}, {"/", 96, 32}, {"l", 112, 32},
    {"i", 128, 32}, {"g", 144, 32}, {"h", 160, 32}, {"t", 176, 32},
    {"/", 192, 32}, {"a", 208, 32}, {"c", 224, 32}, {"c", 240, 32},
    {"e", 256, 32}, {"s", 272, 32}, {"s", 288, 32}, {"o", 304, 32},
    {"r", 320, 32}, {"y", 336, 32},
    {"c", 0, 64}, {"e", 16, 64}, {"n", 32, 64}, {"t", 48, 64},
    {"e", 64, 64}, {"r", 80, 64}, {"X", 112, 64}, {":", 128, 64},
    {"a", 160, 64}, {"n", 176, 64}, {"g", 192, 64}, {"l", 208, 64},
    {"e", 224, 64}, {"X", 256, 64}, {":", 272, 64}, {"d", 288, 64},
    {"i", 304, 64}, {"s", 320, 64}, {"t", 336, 64}, {"a", 352, 64},
    {"n", 368, 64}, {"c", 384, 64}, {"e", 400, 64}, {"Y", 416, 64},
    {":", 432, 64}, {"Z", 448, 64}, {":", 464, 64},
    {"0", 0, 96}, {"1", 16, 96}, {"2", 32, 96}, {"3", 48, 96},
    {"4", 64, 96}, {"5", 80, 96}, {"6", 96, 96}, {"7", 112, 96},
    {"8", 128, 96}, {"9", 144, 96}, {"\x92\x86\x90\x53", 160, 96},  // 中心
    {"X", 240, 96}, {":", 256, 96}, {"\x8ap\x93x", 288, 96},  // 視点度
    {"X", 368, 96}, {":", 384, 96}, {"\x8b\x97\x97\xa3", 416, 96},  // 距離
    {"Global", 0, 128}, {"Local", 96, 128},
    // Coordinate-selector states, on a spare row so the selector can use the
    // scene font: the model/bone panel drags along the screen axes ("View"),
    // the camera/light/accessory panel edits the selected accessory
    // ("Accessory").  A camera has no view state of its own - its own frame is
    // the view - which is why the accessory state carries the original sheet's
    // "accessory" word.
    {"View", 0, 288}, {"Accessory", 96, 288},
    {"Recording(Esc:Stop)", 176, 128},
    {"\x98^\x89\xe6\x92\x86(Esc:\x92\x86\x92" "\xf)", 0, 160},  // 録画中(Esc:中断)
    {"P", 256, 160}, {"l", 272, 160}, {"a", 288, 160}, {"y", 304, 160},
    {"i", 320, 160}, {"n", 336, 160}, {"g", 352, 160},
    {"\x8d\xc4\x90\xb6\x92\x86", 384, 160},  // 再生中
    {"0", 0, 192}, {"1", 16, 192}, {"2", 32, 192}, {"3", 48, 192},
    {"4", 64, 192}, {"5", 80, 192}, {"6", 96, 192}, {"7", 112, 192},
    {"8", 128, 192}, {"9", 144, 192}, {"fps", 160, 192},
    // Chinese "fps" is translated to three full-width glyphs (帧/秒), so it
    // needs a wider atlas cell than the original Latin abbreviation. Keep the
    // following punctuation clear of that localized run.
    {".", 280, 192}, {"-", 304, 192},
    {"1fps or less", 0, 224},
    {"1fps \x96\xA2\x96\xBA", 208, 224},  // JP 未満 (x64 0x7FF7CB54B430)

    {"0", 0, 256}, {"1", 16, 256}, {"2", 32, 256}, {"3", 48, 256},
    {"4", 64, 256}, {"5", 80, 256}, {"6", 96, 256}, {"7", 112, 256},
    {"8", 128, 256}, {"9", 144, 256}, {"frame (", 160, 256},
    {":", 288, 256}, {":", 336, 256}, {")", 384, 256},
};

IDirect3DDevice9* DeviceOf(MMDApp* app) {
    D3DRenderer* sub = app->Renderer();
    if (sub == nullptr)
        return nullptr;
    return sub->device;
}

// Measured extents of the atlas captions. GDI draws proportional text, and a
// wide (Chinese) face reserves a full em per character, so neither a fixed
// cell pitch nor the reported advance shows where the ink actually falls.
// Ink columns are read back from the painted atlas instead, which keeps every
// language centred without hard-coded pixel counts.
struct MeasuredLabel {
    const char* text;
    short x;          // x anchor in the atlas where the string is drawn
    short y;          // atlas row where the string is drawn
    short band;       // sampled window height (the tier's row pitch)
    short scan;       // atlas rows scanned to find ink
    short advance;    // GDI advance; bounds the ink scan window
    short inkLeft;    // first painted column relative to x (-1 = none painted yet)
    short inkRight;   // last painted column relative to x
    short inkTop;     // first painted row relative to y
    short inkBottom;  // last painted row relative to y
    short windowTop;  // first atlas row sampled by the HUD
    short windowBottom;
};
constexpr int kMaxMeasuredLabels = 12;
MeasuredLabel g_measuredLabels[kMaxMeasuredLabels];
int g_measuredLabelCount = 0;

// The four coordinate-system captions appear twice in the atlas: once at the
// original 30-pixel size, and once at 60 pixels in a two-row band.
// The HUD samples whichever copy is closer to the size it will paint, so a
// 2560-wide desktop (HUD scale about 2.0) reads the 60-pixel copy almost
// one-to-one instead of magnifying the 30-pixel copy twofold.
//
// The large copy samples a 64-row WINDOW rather than the rows it was drawn on:
// 64 rows is twice the small copy's 32-row band, so the same window-to-quad
// ratio keeps the caption's painted size identical in both tiers. The window
// is centred on the painted ink afterwards, because GDI places a 60-pixel run
// a few rows lower than twice a 30-pixel run (the Chinese renderer's three-pixel
// lift is not proportional either), and a window taken straight from the draw
// origin both clipped the descender of "Accessory" and sampled the last row of
// the caption above it.
constexpr int kCaptionLargeSize = 60;
constexpr short kCaptionSmallBand = 32;
constexpr short kCaptionSmallScan = 28;
constexpr short kCaptionLargeBand = 64;
// Rows reserved per large cell: the scan must reach a 60-pixel run's descender,
// and the two bands must not sample each other's ink.
constexpr short kCaptionLargePitch = 72;
constexpr short kCaptionLargeScan = kCaptionLargePitch - 2;
// Below the last row of the 288 row: the Chinese captions there paint their ink
// down to row 320, so a large cell may not start higher than row 321.
constexpr short kCaptionLargeFirstRow = 322;
// sqrt(2): above this HUD scale the 60-pixel copy is the closer of the two.
constexpr float kCaptionLargeTierScale = 1.41421356f;
constexpr Glyph kLargeCaptionAtlas[] = {
    {"Global", 0, 322}, {"Local", 256, 322},
    {"Accessory", 0, 394},
    {"View", 256, 394},
};

// Only the captions the HUD addresses as a whole string need measuring: the
// digit rows are sampled with their fixed 16-pixel cell pitch.  They are also
// the atlas' only serif runs - the original sheet paints its coordinate-system
// caption (local / global / accessory) with a serif face, so the per-language
// redraw, which spells them "Local" / "Global" / "View" / "Accessory", keeps
// that face in the English and Japanese UI.  The Chinese UI paints the
// translated Han wording with its label face instead (see UiCaptionFaceName).
bool IsHudCaption(const char* text) {
    static const char* const kCaptions[] = {"Local", "Global", "View",
                                            "Accessory"};
    for (const char* caption : kCaptions)
        if (std::strcmp(text, caption) == 0)
            return true;
    return false;
}

void RecordMeasuredLabel(const char* text, int x, int y, int band, int scan,
                         int advance) {
    if (text == nullptr || advance <= 0 || !IsHudCaption(text) ||
        g_measuredLabelCount >= kMaxMeasuredLabels)
        return;
    MeasuredLabel& slot = g_measuredLabels[g_measuredLabelCount++];
    slot.text = text;
    slot.x = static_cast<short>(x);
    slot.y = static_cast<short>(y);
    slot.band = static_cast<short>(band);
    slot.scan = static_cast<short>(scan);
    slot.advance = static_cast<short>(advance);
    slot.inkLeft = -1;
    slot.inkRight = -1;
    slot.inkTop = -1;
    slot.inkBottom = -1;
    // The small tier samples the band it was drawn into.  The large tier gets a
    // window centred on its ink below, once the ink has been measured.
    slot.windowTop = static_cast<short>(y);
    slot.windowBottom = static_cast<short>(y + band);
}

// Records the painted box of every caption on the source surface (the glyphs
// are rendered in pure blue, so byte 0 of an X8R8G8B8 pixel holds the coverage
// that the conversion pass turns into alpha). The scan starts at the caption's
// own row and runs for the tier's scan height, which stops short of the next
// cell: the Chinese renderer shifts its glyphs up by three pixels and paints
// one row past its band, so scanning the full pitch would read the caption
// above.
void MeasureLabelInk(const unsigned char* source, int pitch) {
    for (int i = 0; i < g_measuredLabelCount; ++i) {
        MeasuredLabel& label = g_measuredLabels[i];
        const int left = label.x;
        const int right = label.x + label.advance < 512 ? label.x + label.advance
                                                        : 512;
        const int top = label.y;
        const int bottom = label.y + label.scan < 512 ? label.y + label.scan
                                                      : 512;
        for (int row = top; row < bottom; ++row) {
            const auto* line = source + row * pitch;
            for (int col = left; col < right; ++col) {
                if (line[col * 4] <= 8)
                    continue;
                const int offset = col - label.x;
                if (label.inkLeft < 0 || offset < label.inkLeft)
                    label.inkLeft = static_cast<short>(offset);
                if (offset > label.inkRight)
                    label.inkRight = static_cast<short>(offset);
                const int lineOffset = row - label.y;
                if (label.inkTop < 0 || lineOffset < label.inkTop)
                    label.inkTop = static_cast<short>(lineOffset);
                if (lineOffset > label.inkBottom)
                    label.inkBottom = static_cast<short>(lineOffset);
            }
        }
        if (label.band == kCaptionLargeBand && label.inkBottom >= 0) {
            const int centre = label.y + (label.inkTop + label.inkBottom) / 2;
            int windowTop = centre - kCaptionLargeBand / 2;
            if (windowTop < kCaptionLargeFirstRow)
                windowTop = kCaptionLargeFirstRow;
            label.windowTop = static_cast<short>(windowTop);
            label.windowBottom = static_cast<short>(windowTop +
                                                    kCaptionLargeBand);
        }
    }
}

}  // namespace

bool SceneFontLabelCell(const char* text, float scale, SceneFontCell* out) {
    if (text == nullptr || out == nullptr)
        return false;
    // Every caption is stored twice in the atlas.
    // Pick the copy whose baked size is closest to the size being painted for the sharpest result.
    const short band = scale >= kCaptionLargeTierScale ? kCaptionLargeBand
                                                       : kCaptionSmallBand;
    for (int i = 0; i < g_measuredLabelCount; ++i) {
        const MeasuredLabel& label = g_measuredLabels[i];
        if (label.band != band || std::strcmp(label.text, text) != 0)
            continue;
        // The horizontal extent spans only the painted ink, while the vertical extent
        // is the cell's window: the draw band for the small tier, and a
        // 64-row window centred on the ink for the large one. In either case
        // the caption is centred in the selector by its window, so the three states
        // and both tiers share a single baseline.
        const int first = label.inkLeft >= 0 ? label.inkLeft : 0;
        const int last = label.inkRight >= 0 ? label.inkRight
                                             : label.advance - 1;
        out->u0 = static_cast<float>(label.x + first) / kSceneFontAtlasSize;
        out->u1 = static_cast<float>(label.x + last + 1) / kSceneFontAtlasSize;
        out->v0 = static_cast<float>(label.windowTop) / kSceneFontAtlasSize;
        out->v1 = static_cast<float>(label.windowBottom) /
                  kSceneFontAtlasSize;
        out->band = static_cast<float>(label.band);
        return true;
    }
    return false;
}

bool InitSceneFontTexture(MMDApp* app) {
    auto& s = *app;
    IDirect3DDevice9* device = DeviceOf(app);
    if (device == nullptr)
        return false;    // original returns 0 when surface creation fails

    // The atlas is rebuilt on every language change, so the measured caption
    // extents must be replaced rather than appended: a stale entry kept the
    // previous language's caption width and pushed that caption off centre.
    g_measuredLabelCount = 0;

    // The original creates a SYSTEMMEM X8R8G8B8 texture, obtains level 0,
    // and lets GDI draw into that surface.  Keeping the texture alive is
    // essential: its pixels are the source of the conversion pass below.
    IDirect3DTexture9* sourceTex = nullptr;
    if (FAILED(device->CreateTexture(
            512, 512, 1, 0, D3DFMT_X8R8G8B8 /*22*/, D3DPOOL_SYSTEMMEM /*2*/,
            &sourceTex, nullptr)))
        return false;

    IDirect3DSurface9* surface = nullptr;
    if (FAILED(sourceTex->GetSurfaceLevel(0, &surface))) {
        sourceTex->Release();
        return false;
    }

    HDC hdc = nullptr;
    if (FAILED(surface->GetDC(&hdc))) {
        surface->Release();
        sourceTex->Release();
        return false;
    }

    HPEN pen = CreatePen(PS_SOLID /*0*/, 1, 0);
    HBRUSH brush = CreateSolidBrush(0);
    HGDIOBJ prevPen = SelectObject(hdc, pen);
    HGDIOBJ prevBrush = SelectObject(hdc, brush);
    Rectangle(hdc, 0, 0, 512, 512);
    SelectObject(hdc, prevBrush);
    DeleteObject(brush);
    SelectObject(hdc, prevPen);
    DeleteObject(pen);

    for (const Glyph& g : kAtlas) {
        const int advance = DrawUiGlyph(
            app, g.s, hdc, 30, g.x, g.y, 0, 0, 255, 1,
            IsHudCaption(g.s) ? UiGlyphFace::Caption : UiGlyphFace::Label);
        RecordMeasuredLabel(g.s, g.x, g.y, kCaptionSmallBand, kCaptionSmallScan,
                            advance);
    }

    // High-resolution copies of the coordinate captions, painted in the free
    // rows beneath the original draw list. The band-to-font ratio matches the small
    // copy, so the HUD renders the caption at the same size from either tier.
    for (const Glyph& g : kLargeCaptionAtlas) {
        const int advance = DrawUiGlyph(app, g.s, hdc, kCaptionLargeSize, g.x,
                                        g.y, 0, 0, 255, 1,
                                        UiGlyphFace::Caption);
        RecordMeasuredLabel(g.s, g.x, g.y, kCaptionLargeBand, kCaptionLargeScan,
                            advance);
    }

    // The surface owns this DC, so ReleaseDC is the matching call: it hands the
    // DC back to Direct3D's pool.  DeleteDC destroyed a DC that
    // IDirect3DSurface9::GetDC had only lent out, the mirror image of the
    // convention used at bg_overlay.cpp:320.
    // Defect RB-07.
    surface->ReleaseDC(hdc);
    surface->Release();

    // destination texture slot (this+650784), recreated as SYSTEMMEM
    if (s.state.sceneFontTexture != nullptr) {
        reinterpret_cast<IDirect3DTexture9*>(
            s.state.sceneFontTexture)->Release();
        s.state.sceneFontTexture = nullptr;
    }
    IDirect3DTexture9* fontTex = nullptr;
    if (FAILED(device->CreateTexture(512, 512, 1, 0, D3DFMT_A8R8G8B8 /*21*/,
                                     D3DPOOL_MANAGED /*1*/, &fontTex, nullptr))) {
        sourceTex->Release();
        return false;
    }
    s.state.sceneFontTexture = fontTex;

    // 0x42C0B0..0x42C124: destination RGB is white and alpha comes from the
    // pure-blue GDI glyph coverage in byte 0 of X8R8G8B8 pixels.
    D3DLOCKED_RECT srcRect{};
    D3DLOCKED_RECT dstRect{};
    const HRESULT srcHr = sourceTex->LockRect(0, &srcRect, nullptr,
                                              D3DLOCK_READONLY);
    const HRESULT dstHr = fontTex->LockRect(0, &dstRect, nullptr, 0);
    if (SUCCEEDED(srcHr) && SUCCEEDED(dstHr)) {
        MeasureLabelInk(static_cast<const unsigned char*>(srcRect.pBits),
                        srcRect.Pitch);
        for (int row = 0; row < 512; ++row) {
            auto* src = static_cast<const unsigned char*>(srcRect.pBits) +
                        row * srcRect.Pitch;
            auto* dst = static_cast<unsigned char*>(dstRect.pBits) +
                        row * dstRect.Pitch;
            for (int col = 0; col < 512; ++col) {
                const unsigned char alpha = src[col * 4];
                unsigned char* out = dst + col * 4;
                out[0] = out[1] = out[2] = 0xFF;
                out[3] = alpha;
            }
        }
    }
    if (SUCCEEDED(dstHr))
        fontTex->UnlockRect(0);
    if (SUCCEEDED(srcHr))
        sourceTex->UnlockRect(0);
    sourceTex->Release();
    return true;
}

}  // namespace mikumikustudio

