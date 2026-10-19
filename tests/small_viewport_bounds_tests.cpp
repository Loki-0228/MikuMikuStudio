// Pure editor geometry; the separate host checks use the real hidden windows,
// production refresh/present gates, D3D target and history/key pools.
#include "mikudancestudio/viewport_bounds.hpp"
#include "mikudancestudio/viewport_panel_layout.hpp"
#include <climits>
#include <cstdio>
#include <cstdlib>

using namespace mikudancestudio;
namespace {
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
bool Empty(const RECT& rect){return !rect.left&&!rect.top&&!rect.right&&!rect.bottom;}
void Empty(const FrameViewportGeometry& geometry){
    Check(Empty(geometry.available)&&Empty(geometry.presentationBounds)&&Empty(geometry.scene),
        "rejected geometry publishes no stale or substituted rectangle");
}
bool Intersects(const RECT& a,const RECT& b){RECT overlap{};return IntersectRect(&overlap,&a,&b)!=FALSE;}
void Bounds(int width,int height,int sidebar,int toolbar,FrameViewportMode mode,int renderWidth,int renderHeight){
    const RECT client{0,0,width,height};
    FrameViewportGeometry geometry{{1,2,3,4},{5,6,7,8},{9,10,11,12}};
    if(!CalculateFrameViewport(client,sidebar,toolbar,mode,renderWidth,renderHeight,geometry)){Empty(geometry);return;}
    Check(FrameRectWithin(geometry.available,client)&&FrameRectWithin(geometry.scene,geometry.available)&&
        FrameRectWithin(geometry.presentationBounds,client),"all admitted rectangles are positive and inside the client");
    for(bool model:{false,true})for(int caption:{36,64}){
        const auto panels=MakeViewportPanelLayout(geometry.available,geometry.scene,mode!=FrameViewportMode::Fullscreen,model,caption);
        const bool display=FrameRectWithin(panels.overlay,geometry.presentationBounds);
        if(!display)continue; // The production gate rejects an unusable adaptive strip.
        Check(FrameRectWithin(panels.overlay,client),"admitted presentation never escapes the current client");
        if(mode!=FrameViewportMode::Docked)continue;
        const RECT nativePanel{0,height-160,width,height};
        Check(!Intersects(geometry.scene,nativePanel)&&!Intersects(panels.overlay,nativePanel),
            "scene and adaptive presentation both exclude the complete native bottom panel");
        // Actual unexpanded-camera FOV and expanded-light trackbar rectangles.
        // Bottom-panel width expansion does not change their vertical band.
        const RECT sliders[]={
            {363,height-95,514,height-75},
            {397,height-140,533,height-122},{397,height-122,533,height-104},{397,height-104,533,height-86},
            {397,height-86,529,height-66},{397,height-66,529,height-46},{397,height-46,529,height-26}
        };
        for(const auto& slider:sliders)Check(!Intersects(geometry.scene,slider)&&!Intersects(panels.overlay,slider),
            "none of the seven real slider bands intersects scene or presentation");
    }
}
void Golden(){
    struct Case {int width,height,sidebar,toolbar;FrameViewportMode mode;RECT scene,available;};
    const Case cases[]={
        {1200,820,250,0,FrameViewportMode::Docked,{259,62,1200,590},{259,25,1200,628}},
        {1500,900,250,0,FrameViewportMode::Docked,{272,25,1486,708},{259,25,1500,708}},
        {820,1100,250,0,FrameViewportMode::Docked,{259,309,820,623},{259,25,820,908}},
        {1200,820,250,90,FrameViewportMode::Docked,{273,115,1185,628},{259,115,1200,628}},
        {1200,820,0,0,FrameViewportMode::Floating,{0,69,1200,743},{0,25,1200,788}},
        {1200,820,0,0,FrameViewportMode::Fullscreen,{0,73,1200,747},{0,0,1200,820}}
    };
    for(const auto& c:cases){FrameViewportGeometry geometry;
        Check(CalculateFrameViewport({0,0,c.width,c.height},c.sidebar,c.toolbar,c.mode,1920,1080,geometry),
            "normal docked/floating/fullscreen geometry remains renderable");
        Check(EqualRect(&geometry.scene,&c.scene)&&EqualRect(&geometry.available,&c.available),
            "normal integer centering and every original scene pixel remain exact");
    }
    FrameViewportGeometry geometry;
    Check(!CalculateFrameViewport({0,0,760,160},250,0,FrameViewportMode::Docked,1920,1080,geometry),
        "760x160 regression is unavailable instead of a 6px Present over red slider455");Empty(geometry);
    for(int width:{259,260,261,262}){
        Check(!CalculateFrameViewport({0,0,width,220},250,0,FrameViewportMode::Docked,1920,1080,geometry),
            "no width or an aspect-fit rounded to zero never becomes a forged 1px scene");Empty(geometry);
    }
    Check(!CalculateFrameViewport({0,0,1200,820},250,INT_MAX,FrameViewportMode::Docked,1920,1080,geometry),
        "toolbar reservation cannot wrap into the native bottom panel");Empty(geometry);
    Check(!CalculateFrameViewport({0,0,1200,820},INT_MAX,0,FrameViewportMode::Docked,1920,1080,geometry),
        "sidebar plus its 9px margin cannot overflow into a visible viewport");Empty(geometry);
    Check(!CalculateFrameViewport({0,0,1200,820},250,0,FrameViewportMode::Docked,0,1080,geometry),
        "zero render dimensions have no viewport");Empty(geometry);
    Check(!CalculateFrameViewport({0,0,1200,820},250,0,FrameViewportMode::Docked,1920,-1,geometry),
        "negative render dimensions have no viewport");Empty(geometry);
    // A hidden or DPI-virtualized window supplies coordinates in its own
    // window context. Geometry takes those units unchanged; it has no screen
    // origin, visibility, activation or hardcoded monitor-DPI dependency.
    for(int dpi:{96,168,192}){
        const int width=MulDiv(760,dpi,96),height=MulDiv(160,dpi,96);
        Bounds(width,height,MulDiv(250,dpi,96),MulDiv(31,dpi,96),FrameViewportMode::Docked,1920,1080);
        Bounds(width,height,0,0,FrameViewportMode::Floating,1920,1080);
    }
    Check(CalculateFrameViewport({0,0,INT_MAX,INT_MAX},0,0,FrameViewportMode::Docked,INT_MAX,INT_MAX,geometry)&&
        FrameRectWithin(geometry.scene,{0,0,INT_MAX,INT_MAX}),"aspect arithmetic uses a wide product before checked coordinates");
}
}
int main(){
    Golden();
    for(int width=0;width<=1100;width+=13)for(int height=0;height<=900;height+=17)
        for(int sidebar:{0,250,width-5})for(int toolbar:{0,31,height})
            for(auto mode:{FrameViewportMode::Docked,FrameViewportMode::Floating,FrameViewportMode::Fullscreen}){
                Bounds(width,height,sidebar,toolbar,mode,1920,1080);
                Bounds(width,height,sidebar,toolbar,mode,1,1000000);
                Bounds(width,height,sidebar,toolbar,mode,1000000,1);
            }
    std::printf("PASS small editor viewport bounds: %u checks\n",checks);return 0;
}
