#pragma once
// ===========================================================================
// 视口操作柄停靠角 -> 矩形映射（公共头文件，纯函数）
//
// 绘制、命中和光标归位共用同一布局。XYZ 的轴向顺序始终保持不变。
// 左侧停靠时等比缩放位于 XYZ 右侧；上方停靠时坐标条位于缩放行下方。
// 右下角保持原始布局。units 是距 XYZ 行右下锚点的 HUD 单位距离。
// ===========================================================================
#include <Windows.h>

namespace mikumikustudio {
namespace viewport_tool_layout {

// 四个停靠角。数值顺序 = 帮助菜单田字选择器的格子顺序
//（左上 -> 右上 -> 左下 -> 右下）：格子下标直接 static_cast 成 Corner。
enum class Corner : int {
    TopLeft = 0,
    TopRight = 1,
    BottomLeft = 2,
    BottomRight = 3,   // 默认，保持移植现状
};

inline constexpr Corner kDefaultCorner = Corner::BottomRight;

// 操作柄块的外廓（HUD 单位，乘 scale 后是设备像素）。
// Kx 最大 170（等比缩放格的左缘）、Ky 最大 167（坐标条命中带顶缘），
// 两贴边再留 kEdgeMarginUnits 的白边。
inline constexpr float kBlockWidthUnits = 180.0f;   // 170 + 10
inline constexpr float kBlockHeightUnits = 177.0f;  // 167 + 10
inline constexpr float kEdgeMarginUnits = 10.0f;

struct Anchor {
    float x;      // 等价于现状的 view.right
    float y;      // 等价于现状的 view.bottom
    float scale;  // 归一化后的 HUD 缩放
    bool leftSide;
    bool topSide;

    // 块内偏移 -> 设备坐标。units 是距锚点的 HUD 单位距离。
    float X(float units) const { return x - units * scale; }
    float Y(float units) const { return y - units * scale; }
    // Logical column 0 = uniform, 1/2/3 = X/Y/Z. UVs and operation IDs
    // retain this order even when the uniform cell changes visual position.
    float ScaleLeftUnits(int column) const {
        return column == 0 ? (leftSide ? 10.0f : 170.0f)
                           : 170.0f - 40.0f * column;
    }
    float ScaleCenterX(int column) const { return X(ScaleLeftUnits(column) - 20.0f); }
    float CoordDrawTop() const { return Y(topSide ? -5.0f : 165.0f); }
    float CoordHitTop() const { return Y(topSide ? -3.0f : 167.0f); }
    float CoordHitBottom() const { return Y(topSide ? -37.0f : 125.0f); }
};

// scale 非法时回退 1.0，与 frame_modes.cpp 的 ViewportScale 同规。
inline float NormalizeScale(float scale) {
    return scale > 0.0f ? scale : 1.0f;
}

// 角落 -> 锚点。
inline Anchor Make(const RECT& view, float scale, Corner corner) {
    const float s = NormalizeScale(scale);
    const bool rightSide = corner == Corner::TopRight ||
                           corner == Corner::BottomRight;
    const bool bottomSide = corner == Corner::BottomLeft ||
                            corner == Corner::BottomRight;
    Anchor anchor;
    anchor.x = rightSide
        ? static_cast<float>(view.right)
        : static_cast<float>(view.left) + 140.0f * s;
    anchor.y = bottomSide
        ? static_cast<float>(view.bottom)
        : static_cast<float>(view.top) + 130.0f * s;
    anchor.scale = s;
    anchor.leftSide = !rightSide;
    anchor.topSide = !bottomSide;
    return anchor;
}

// 只要两个锚点标量时的便捷入口（接线点就是把
// `view.right` / `view.bottom` 换成这两个返回值）。
inline float AnchorRight(const RECT& view, float scale, Corner corner) {
    return Make(view, scale, corner).x;
}
inline float AnchorBottom(const RECT& view, float scale, Corner corner) {
    return Make(view, scale, corner).y;
}

// ---- 操作柄各元素的 HUD 单位常量（绘制与命中共用的唯一真源） ----------
// 数值与移植现状一一对应：右下角停靠时换算结果与旧代码逐点相等。

// 旋转行（操作 9..11 / 18..20）与位移行（12..14 / 15..17）。
inline constexpr float kRotateRowTopUnits = 120.0f;     // 命中开区间 (120, 90)
inline constexpr float kRotateRowBottomUnits = 90.0f;
inline constexpr float kMoveRowTopUnits = 80.0f;        // 命中开区间 (80, 50)
inline constexpr float kMoveRowBottomUnits = 50.0f;
inline constexpr float kRowLeftUnits = 130.0f;          // 行绘制左缘
inline constexpr float kRowRightUnits = 10.0f;          // 行绘制右缘

// 三条列带（列 0=X、列 1=Y、列 2=Z），命中用内缩 3 单位的窄带。
inline constexpr float kColumnHitLeftUnits[3] = {127.0f, 86.0f, 48.0f};
inline constexpr float kColumnHitRightUnits[3] = {92.0f, 51.0f, 13.0f};

// 缩放行（操作 22..25）：四格 40x30，等比格在最左。
inline constexpr float kScaleRowTopUnits = 40.0f;       // 命中开区间 (40, 10)
inline constexpr float kScaleRowBottomUnits = 10.0f;
inline constexpr float kScaleUniformHitLeftUnits = 167.0f;
inline constexpr float kScaleUniformHitRightUnits = 133.0f;
inline constexpr float kScaleCellWidthUnits = 40.0f;
inline constexpr float kScaleCellHeightUnits = 30.0f;
inline constexpr float kScaleUniformDrawLeftUnits = 170.0f;

// 等比/X/Y/Z 四格格心（RestoreViewportToolCursor 用）。
inline constexpr float kScaleCenterXUnits[4] = {150.0f, 110.0f, 70.0f, 30.0f};
inline constexpr float kScaleCenterYUnits = 25.0f;

// 旋转 / 位移三列格心与两行行心（RestoreViewportToolCursor 用）。
inline constexpr float kColumnCenterXUnits[3] = {110.0f, 70.0f, 30.0f};
inline constexpr float kRotateRowCenterYUnits = 105.0f;
inline constexpr float kMoveRowCenterYUnits = 65.0f;

// 坐标系文字条（操作 21）。命中带比绘制矩形上下各多 2 单位：
// 现状命中 top = bottom-167、底 = bottom-125，绘制 top = bottom-165、120x30。
inline constexpr float kCoordStripLeftUnits = 130.0f;
inline constexpr float kCoordStripRightUnits = 10.0f;
inline constexpr float kCoordHitTopUnits = 167.0f;
inline constexpr float kCoordHitBottomUnits = 125.0f;
inline constexpr float kCoordDrawTopUnits = 165.0f;
inline constexpr float kCoordDrawWidthUnits = 120.0f;
inline constexpr float kCoordDrawHeightUnits = 30.0f;

}  // namespace viewport_tool_layout
}  // namespace mikumikustudio
