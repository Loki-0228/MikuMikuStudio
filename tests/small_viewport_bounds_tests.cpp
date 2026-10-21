// Pure editor geometry; the separate host checks use the real hidden windows,
// production refresh/present gates, D3D target and history/key pools.
#include "mikumikustudio/viewport_bounds.hpp"
#include "mikumikustudio/viewport_panel_layout.hpp"
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace mikumikustudio;
namespace {
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
bool Empty(const RECT& rect){return !rect.left&&!rect.top&&!rect.right&&!rect.bottom;}
void Empty(const FrameViewportGeometry& geometry){
    Check(Empty(geometry.available)&&Empty(geometry.presentationBounds)&&Empty(geometry.scene),
        "rejected geometry publishes no stale or substituted rectangle");
}
bool Intersects(const RECT& a,const RECT& b){RECT overlap{};return IntersectRect(&overlap,&a,&b)!=FALSE;}
bool Same(const FrameViewportGeometry& a,const FrameViewportGeometry& b){
    return EqualRect(&a.available,&b.available)&&EqualRect(&a.presentationBounds,&b.presentationBounds)&&
        EqualRect(&a.scene,&b.scene);
}
void Exact(const FrameViewportGeometry& geometry,const RECT& available,const RECT& presentationBounds,
        const RECT& scene,const char* message){
    Check(EqualRect(&geometry.available,&available)&&EqualRect(&geometry.presentationBounds,&presentationBounds)&&
        EqualRect(&geometry.scene,&scene),message);
}
bool Viewport(int width,int height,int sidebar,int toolbar,FrameViewportMode mode,int renderWidth,
        int renderHeight,int propertyExtensions,int leftExtensions,int rightExtensions,FrameViewportGeometry& geometry){
    return CalculateFrameViewport({0,0,width,height},sidebar,toolbar,mode,renderWidth,renderHeight,geometry,
        propertyExtensions,leftExtensions,rightExtensions);
}
void Reject(int width,int height,int sidebar,int toolbar,FrameViewportMode mode,int renderWidth,int renderHeight,
        int propertyExtensions,int leftExtensions,int rightExtensions,const char* message){
    FrameViewportGeometry geometry{{1,2,3,4},{5,6,7,8},{9,10,11,12}};
    Check(!Viewport(width,height,sidebar,toolbar,mode,renderWidth,renderHeight,propertyExtensions,
        leftExtensions,rightExtensions,geometry),message);
    Empty(geometry);
}
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
    for(const auto& c:cases){
        FrameViewportGeometry geometry,explicitZero;
        Check(CalculateFrameViewport({0,0,c.width,c.height},c.sidebar,c.toolbar,c.mode,1920,1080,geometry),
            "normal docked/floating/fullscreen geometry remains renderable");
        Check(EqualRect(&geometry.scene,&c.scene)&&EqualRect(&geometry.available,&c.available),
            "normal integer centering and every original scene pixel remain exact");
        Check(CalculateFrameViewport({0,0,c.width,c.height},c.sidebar,c.toolbar,c.mode,1920,1080,explicitZero,0,0,0),
            "explicit zero plugin columns remain renderable");
        Check(Same(geometry,explicitZero),
            "zero left/right columns keep every pre-existing pixel and the default arguments");
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
void PluginColumnGoldens(){
    struct Case {int width,height,sidebar,toolbar,property,left,right;
        RECT available,presentationBounds,scene;};
    const Case cases[]={
        {1200,820,250,0,0,120,0,{379,25,1200,628},{379,25,1200,660},{379,96,1200,556}},
        {1200,820,250,0,0,0,140,{259,25,1060,628},{259,25,1060,660},{259,101,1060,551}},
        {1200,820,250,0,0,120,140,{379,25,1060,628},{379,25,1060,660},{379,135,1060,517}},
        {1200,820,250,90,48,64,80,{323,115,1120,580},{323,115,1120,612},{323,123,1120,571}},
        {1200,820,250,0,160,64,80,{323,25,1120,468},{323,25,1120,500},{328,25,1114,468}}
    };
    for(const auto& c:cases){
        FrameViewportGeometry geometry;
        Check(Viewport(c.width,c.height,c.sidebar,c.toolbar,FrameViewportMode::Docked,1920,1080,
            c.property,c.left,c.right,geometry),
            "single-sided and bilateral plugin columns keep an integer viewport");
        Exact(geometry,c.available,c.presentationBounds,c.scene,
            "plugin columns narrow available/presentation/scene to the exact docked rectangles");
        Check((geometry.scene.left==geometry.available.left&&geometry.scene.right==geometry.available.right)||
            (geometry.scene.top==geometry.available.top&&geometry.scene.bottom==geometry.available.bottom),
            "aspect scene spans the full narrowed axis it fits");
        const RECT nativePanel{0,c.height-160,c.width,c.height};
        Check(!Intersects(geometry.scene,nativePanel)&&!Intersects(geometry.presentationBounds,nativePanel),
            "property reservation plus plugin columns never covers the native bottom panel");
        for(bool model:{false,true}){
            const auto panels=MakeViewportPanelLayout(geometry.available,geometry.scene,true,model);
            if(!FrameRectWithin(panels.overlay,geometry.presentationBounds))continue;
            Check(!Intersects(panels.overlay,nativePanel)&&FrameRectWithin(panels.overlay,{0,0,c.width,c.height}),
                "adaptive presentation strip stays inside the narrowed client above the native bottom panel");
        }
    }
}
void PluginColumnBounds(){
    FrameViewportGeometry zero,negative,minusOne;
    Check(Viewport(1200,820,250,0,FrameViewportMode::Docked,1920,1080,0,0,0,zero),
        "zero plugin columns remain renderable");
    Check(Viewport(1200,820,250,0,FrameViewportMode::Docked,1920,1080,0,-160,-INT_MAX,negative),
        "negative plugin columns are ignored instead of widening the viewport");
    Check(Same(zero,negative),"negative left/right columns keep the zero-column pixels");
    Check(Viewport(1200,820,250,0,FrameViewportMode::Docked,1920,1080,0,-1,-1,minusOne)&&Same(zero,minusOne),
        "negative one pixel plugin columns reserve nothing");
    for(const auto mode:{FrameViewportMode::Floating,FrameViewportMode::Fullscreen}){
        FrameViewportGeometry base,ignored;
        Check(Viewport(1200,820,250,90,mode,1920,1080,0,0,0,base),
            "floating and fullscreen geometry remains renderable");
        Check(Viewport(1200,820,250,90,mode,1920,1080,INT_MAX,200,300,ignored),
            "floating and fullscreen geometry ignores supplied plugin columns");
        Check(Same(base,ignored),"floating and fullscreen keep their original pixels with plugin columns supplied");
        Check(base.available.left==0&&base.available.right==1200,"non-docked geometry keeps the full client width");
    }
}
void PluginColumnRejections(){
    Reject(1200,820,250,0,FrameViewportMode::Docked,1920,1080,0,INT_MAX,0,
        "a left plugin column of INT_MAX cannot overflow into a viewport");
    Reject(1200,820,250,0,FrameViewportMode::Docked,1920,1080,0,0,INT_MAX,
        "a right plugin column of INT_MAX cannot overflow into a viewport");
    Reject(1200,820,250,0,FrameViewportMode::Docked,1920,1080,0,INT_MAX,INT_MAX,
        "bilateral plugin columns of INT_MAX leave no viewport");
    Reject(1200,820,INT_MAX,0,FrameViewportMode::Docked,1920,1080,0,INT_MAX,INT_MAX,
        "sidebar and plugin columns near INT_MAX use wide arithmetic");
    Reject(INT_MAX,INT_MAX,250,0,FrameViewportMode::Docked,1920,1080,0,INT_MAX,INT_MAX,
        "an INT_MAX client with INT_MAX columns has no width");
    Reject(300,220,250,0,FrameViewportMode::Docked,1920,1080,0,40,0,
        "a one pixel narrowed strip is rejected instead of a forged viewport");
    Reject(300,220,250,0,FrameViewportMode::Docked,1920,1080,0,41,0,
        "an exhausted left reservation is rejected");
    Reject(1200,820,250,0,FrameViewportMode::Docked,1920,1080,0,0,940,
        "a one pixel right-narrowed strip is rejected");
    Reject(1200,820,250,0,FrameViewportMode::Docked,1920,1080,0,0,941,
        "an exhausted right reservation is rejected");
    Reject(1200,820,250,0,FrameViewportMode::Docked,1920,1080,0,100,842,
        "overlapping column reservations are rejected");
    Reject(1200,820,250,0,FrameViewportMode::Docked,1920,1080,INT_MAX,64,80,
        "property overflow combined with plugin columns is rejected");
}
void PluginColumnWideArithmetic(){
    FrameViewportGeometry geometry;
    Check(Viewport(INT_MAX,INT_MAX,250,0,FrameViewportMode::Docked,1920,1080,0,1000000,1000000,geometry),
        "a large client with large plugin columns keeps a wide narrowed rectangle");
    Check(geometry.available.left==1000259&&geometry.available.right==INT_MAX-1000000&&
        geometry.available.top==25&&geometry.available.bottom==INT_MAX-192,
        "wide plugin-column arithmetic publishes exact unoverflowed bounds");
    Check(FrameRectWithin(geometry.scene,geometry.available)&&
        geometry.scene.left==geometry.available.left&&geometry.scene.right==geometry.available.right,
        "the integer centered aspect scene fills the narrowed width");
    const std::int64_t availableHeight=std::int64_t(geometry.available.bottom)-geometry.available.top;
    Check(std::int64_t(geometry.scene.top)+geometry.scene.bottom==2*(availableHeight/2+geometry.available.top),
        "vertical integer centering stays exact at INT_MAX scale");
    FrameViewportGeometry defaultWide,zeroWide;
    Check(CalculateFrameViewport({0,0,INT_MAX,INT_MAX},0,0,FrameViewportMode::Docked,INT_MAX,INT_MAX,defaultWide),
        "INT_MAX render dimensions still fit");
    Check(CalculateFrameViewport({0,0,INT_MAX,INT_MAX},0,0,FrameViewportMode::Docked,INT_MAX,INT_MAX,zeroWide,0,0,0),
        "explicit zero plugin columns accept INT_MAX render dimensions");
    Check(Same(defaultWide,zeroWide),"zero plugin columns preserve the INT_MAX render behavior");
}
void PluginColumnDpiCoordinates(){
    for(int dpi:{96,120,144,168,192}){
        const int width=MulDiv(1200,dpi,96),height=MulDiv(820,dpi,96);
        const int sidebar=MulDiv(250,dpi,96),left=MulDiv(120,dpi,96),right=MulDiv(140,dpi,96);
        FrameViewportGeometry geometry;
        Check(Viewport(width,height,sidebar,0,FrameViewportMode::Docked,1920,1080,0,left,right,geometry),
            "DPI-scaled plugin columns keep a renderable docked viewport");
        Check(geometry.available.left==sidebar+9+left&&geometry.available.right==width-right&&
            geometry.available.top==25&&geometry.available.bottom==height-192,
            "geometry consumes DPI-scaled coordinates without scaling them again");
        Check(geometry.presentationBounds.left==geometry.available.left&&
            geometry.presentationBounds.right==geometry.available.right&&
            geometry.presentationBounds.bottom==height-160,
            "presentation bounds follow the DPI-scaled client and plugin columns");
        Check(FrameRectWithin(geometry.scene,geometry.available)&&
            !Intersects(geometry.scene,{0,0,geometry.available.left,height})&&
            !Intersects(geometry.scene,{geometry.available.right,0,width,height}),
            "the DPI-scaled scene stays inside the narrowed integer viewport");
    }
}
void PluginColumnCentering(){
    struct Pair {int left,right;};
    const Pair pairs[]={{0,0},{60,0},{0,80},{120,140},{200,250}};
    for(int width:{1000,1600})for(int height:{700,900})for(int sidebar:{0,250})
        for(const Pair& pair:pairs)for(int tall:{0,1}){
            const int renderWidth=tall?1080:1920,renderHeight=tall?1920:1080;
            FrameViewportGeometry geometry;
            Check(Viewport(width,height,sidebar,0,FrameViewportMode::Docked,renderWidth,renderHeight,0,
                pair.left,pair.right,geometry),
                "aspect-fit centering remains renderable inside plugin-column reservations");
            Check(geometry.available.left==sidebar+9+pair.left&&
                geometry.available.right==width-pair.right&&geometry.available.top==25&&
                geometry.available.bottom==height-192,
                "column reservations keep the exact narrowed docked bounds");
            Check(geometry.presentationBounds.left==geometry.available.left&&
                geometry.presentationBounds.right==geometry.available.right&&
                geometry.presentationBounds.bottom==height-160,
                "the adaptive presentation keeps the same narrowed columns");
            Check(!Intersects(geometry.scene,{0,0,geometry.available.left,height})&&
                !Intersects(geometry.scene,{geometry.available.right,0,width,height}),
                "the aspect scene never intersects either plugin column");
            const std::int64_t availableWidth=std::int64_t(geometry.available.right)-geometry.available.left;
            const std::int64_t availableHeight=std::int64_t(geometry.available.bottom)-geometry.available.top;
            const std::int64_t fittedWidth=availableHeight*renderWidth/renderHeight;
            if(availableWidth>=fittedWidth){
                const std::int64_t center=availableWidth/2+geometry.available.left;
                Check(std::int64_t(geometry.scene.left)+geometry.scene.right==2*center&&
                    geometry.scene.top==geometry.available.top&&
                    geometry.scene.bottom==geometry.available.bottom,
                    "horizontal aspect fit keeps the original integer centering");
            }else{
                const std::int64_t center=availableHeight/2+geometry.available.top;
                Check(std::int64_t(geometry.scene.top)+geometry.scene.bottom==2*center&&
                    geometry.scene.left==geometry.available.left&&
                    geometry.scene.right==geometry.available.right,
                    "vertical aspect fit keeps the original integer centering");
            }
        }
}
}
int main(){
    Golden();
    PluginColumnGoldens();
    PluginColumnBounds();
    PluginColumnRejections();
    PluginColumnWideArithmetic();
    PluginColumnDpiCoordinates();
    PluginColumnCentering();
    for(int width=0;width<=1100;width+=13)for(int height=0;height<=900;height+=17)
        for(int sidebar:{0,250,width-5})for(int toolbar:{0,31,height})
            for(auto mode:{FrameViewportMode::Docked,FrameViewportMode::Floating,FrameViewportMode::Fullscreen}){
                Bounds(width,height,sidebar,toolbar,mode,1920,1080);
                Bounds(width,height,sidebar,toolbar,mode,1,1000000);
                Bounds(width,height,sidebar,toolbar,mode,1000000,1);
            }
    std::printf("PASS small editor viewport bounds: %u checks\n",checks);return 0;
}
