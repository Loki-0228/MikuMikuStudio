#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cstdint>

namespace mikumikustudio {
enum class FrameViewportMode { Docked, Floating, Fullscreen };
struct ViewportToolbarRows {
    int percent = 100;
    int top = 25;
    int bottom = 32;
};

inline ViewportToolbarRows FitViewportToolbarRows(int height, int percent) noexcept {
    // Leave room for the scene in a short editor window.
    const int budget = std::clamp(height, 153, 210) - 96;
    const int effective = std::clamp(percent, 100, budget * 100 / 57);
    return {effective, MulDiv(25, effective, 100), MulDiv(32, effective, 100)};
}

struct FrameViewportGeometry {
    RECT available{}, presentationBounds{}, scene{};
};
inline bool FrameRectWithin(const RECT& rect,const RECT& bounds) noexcept {
    return rect.left>=bounds.left&&rect.top>=bounds.top&&rect.right<=bounds.right&&rect.bottom<=bounds.bottom&&
        rect.right>rect.left&&rect.bottom>rect.top;
}
// Full-profile editor geometry only. Keep the original integer centering when
// it yields real pixels; insufficient room has no substitute 1px viewport.
inline bool CalculateFrameViewport(const RECT& client,int sidebar,int toolbar,FrameViewportMode mode,
        int renderWidth,int renderHeight,FrameViewportGeometry& result,int propertyExtensions = 0,
        int leftExtensions = 0,int rightExtensions = 0,int bottomExtension = 0,
        ViewportToolbarRows rows = {}) noexcept {
    result={};
    if(client.left||client.top||client.right<=0||client.bottom<=0||renderWidth<=0||renderHeight<=0)return false;
    using Wide=std::int64_t;
    const Wide width=client.right,height=client.bottom;
    const bool docked=mode==FrameViewportMode::Docked,fullscreen=mode==FrameViewportMode::Fullscreen;
    // The sidebar margin stays independent of the docked plugin columns; a
    // negative column width reserves nothing. Wide keeps extreme sums wrap-free.
    const Wide left=docked?std::max<Wide>(0,Wide(sidebar)+9)+std::max<Wide>(0,Wide(leftExtensions)):0;
    const Wide right=docked?width-std::max<Wide>(0,Wide(rightExtensions)):width;
    const Wide top=fullscreen?0:rows.top+(docked?std::max<Wide>(0,toolbar):0);
    const Wide property=docked?std::max<Wide>(0,propertyExtensions):0;
    const Wide nativePanels=docked?160+std::max<Wide>(0,bottomExtension):0;
    const Wide bottom=height-nativePanels-(fullscreen?0:rows.bottom)-property;
    if(left>=right||top>=bottom)return false;
    FrameViewportGeometry next;
    next.available={LONG(left),LONG(top),LONG(right),LONG(bottom)};
    // The native bottom panels begin at clientHeight-160. A compressed
    // viewport strip may reclaim up to 8px of its own 32px row, never this band.
    next.presentationBounds={LONG(left),LONG(top),LONG(right),LONG(height-nativePanels-property)};
    const Wide availableWidth=right-left,availableHeight=bottom-top;
    const Wide fittedWidth=availableHeight*Wide(renderWidth)/renderHeight;
    Wide sceneLeft=left,sceneRight=right,sceneTop=top,sceneBottom=bottom;
    if(availableWidth>=fittedWidth){
        const Wide center=availableWidth/2+left;
        sceneLeft=center-fittedWidth/2;sceneRight=center+fittedWidth/2;
    }else{
        const Wide fittedHeight=availableWidth*Wide(renderHeight)/renderWidth;
        const Wide center=availableHeight/2+top;
        sceneTop=center-fittedHeight/2;sceneBottom=center+fittedHeight/2;
    }
    next.scene={LONG(sceneLeft),LONG(sceneTop),LONG(sceneRight),LONG(sceneBottom)};
    if(!FrameRectWithin(next.scene,next.available))return false;
    result=next;return true;
}
} // namespace mikumikustudio
