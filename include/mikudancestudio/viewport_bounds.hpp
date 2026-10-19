#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cstdint>

namespace mikudancestudio {
enum class FrameViewportMode { Docked, Floating, Fullscreen };
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
        int renderWidth,int renderHeight,FrameViewportGeometry& result) noexcept {
    result={};
    if(client.left||client.top||client.right<=0||client.bottom<=0||renderWidth<=0||renderHeight<=0)return false;
    using Wide=std::int64_t;
    const Wide width=client.right,height=client.bottom;
    const bool docked=mode==FrameViewportMode::Docked,fullscreen=mode==FrameViewportMode::Fullscreen;
    const Wide left=docked?std::max<Wide>(0,Wide(sidebar)+9):0;
    const Wide top=fullscreen?0:25+(docked?std::max<Wide>(0,toolbar):0);
    const Wide bottom=height-(fullscreen?0:docked?192:32);
    if(left>=width||top>=bottom)return false;
    FrameViewportGeometry next;
    next.available={LONG(left),LONG(top),LONG(width),LONG(bottom)};
    // The native bottom panels begin at clientHeight-160. A compressed
    // viewport strip may reclaim up to 8px of its own 32px row, never this band.
    next.presentationBounds={LONG(left),LONG(top),LONG(width),LONG(height-(docked?160:0))};
    const Wide availableWidth=width-left,availableHeight=bottom-top;
    const Wide fittedWidth=availableHeight*Wide(renderWidth)/renderHeight;
    Wide sceneLeft=left,sceneRight=width,sceneTop=top,sceneBottom=bottom;
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
} // namespace mikudancestudio
