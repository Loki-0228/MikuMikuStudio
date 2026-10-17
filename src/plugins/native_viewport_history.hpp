#pragma once
#include "mikudancestudio/build_profile.hpp"
#include <array>
#include <cstdint>
namespace mikudancestudio::plugins {
// The restored and controlled pumps share this detached calculator. Values
// retain the original float constants, including their historical rounding.
struct ViewportPresetState {
    std::array<float,3> cameraPosition{},rotation{};
    float offsetX=0,offsetY=0,distance=0,fov=0;
    std::array<float,16> basis{};
    std::uint8_t suppressed=0,perspective=0;
    int parentModel=-1,parentBone=0;
};
struct ViewportPresetContext {
    bool cameraStyle=false,selectedBoneReference=false,suppressAttachment=false;
};
inline bool ComputeNumpadViewPreset(const ViewportPresetContext& context,unsigned key,
    const ViewportPresetState& before,ViewportPresetState& out,const ViewportPresetState* resetCamera=nullptr) noexcept {
    constexpr float halfPi=1.5707999467849731f,pi=3.1415920257568359f;
    float pitch=0,yaw=0;
    switch(key){case 2:break;case 4:yaw=halfPi;break;case 6:yaw=-halfPi;break;
    case 8:yaw=pi;break;case 5:pitch=-halfPi;break;case 0:case 1:pitch=halfPi;break;default:return false;}
    auto target=before;
    if(!context.cameraStyle&&key==0){
        if(!resetCamera)return false;
        target.cameraPosition=resetCamera->cameraPosition;target.rotation=resetCamera->rotation;
        target.distance=resetCamera->distance;target.fov=resetCamera->fov;
        target.perspective=resetCamera->perspective;target.parentModel=resetCamera->parentModel;target.parentBone=resetCamera->parentBone;
        target.offsetX=target.offsetY=0;target.suppressed=0;
    }else{
        target.rotation={pitch,yaw,0};target.distance=-45.0f;
        if(!context.cameraStyle){
            if(context.suppressAttachment)target.suppressed=1;
            target.offsetX=0;target.offsetY=key!=5&&key!=1&&!context.selectedBoneReference?10.0f:0.0f;
            if(key==1)target.distance=-30.0f;
            for(unsigned i=0;i<16;++i)target.basis[i]=i%5==0?1.0f:0.0f;
        }
    }
    out=target;return true;
}
}
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Camera right/middle drags reserve once, preview in place, then publish once.
// No history/storage lock is retained across window messages.
MmsResult BeginViewportCameraGesture(MMDApp&, SharedHistory&, HWND owner, UINT button) noexcept;
bool AllowViewportCameraPreview(MMDApp&, UINT button) noexcept;
bool AcceptViewportCameraPreview(MMDApp&) noexcept;
MmsResult EndViewportCameraGesture(MMDApp&, bool& changed) noexcept;
bool CancelViewportCameraGesture(MMDApp&) noexcept;
// A wheel message is one adjustment. Main/float legacy math differs, and
// parented main-window cameras adjust target Z instead of distance.
MmsResult AdjustViewportCameraWheel(MMDApp&, SharedHistory&, std::int16_t delta,
    bool floating, bool& changed) noexcept;
// One pump batch, in original 2/4/6/8/5/0/1 order, is one Adjustment.
// Bits name digits (1u<<digit); a final no-op preserves the redo branch.
MmsResult PlanViewportNumpadPresets(MMDApp&, std::uint16_t pressedKeys,
    std::shared_ptr<HistoryChange>& out) noexcept;
MmsResult AdjustViewportNumpadPresets(MMDApp&, SharedHistory&, std::uint16_t pressedKeys,
    bool& changed) noexcept;
bool HandleBoundViewportMessage(MMDApp&, HWND, UINT, WPARAM, LPARAM) noexcept;
}
#endif
