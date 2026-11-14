#include "../src/animation/negative_paste_authoring.hpp"
#include "../src/animation/native_camera_sampling.hpp"

namespace {
namespace signed_paste_authoring_checks {
namespace au = mikumikustudio::negative_frames::authoring;
namespace ac = mikumikustudio::animation_curve;
namespace camera = mikumikustudio::native_camera_sampling;

void Run() {
    using mikumikustudio::frame_limits::minimum;
    Check(!au::TranslatePaste({},-10,-30),"legacy rows do not invent an author");
    const auto native=au::Make(au::Family::Light,au::Mode::Native,{});
    Check(au::TranslatePaste(native,-10,-30)==native,"native controls need no temporal rewrite");

    au::Track data;data.family=au::Family::Camera;
    for(unsigned i=0;i<11;++i) {
        au::Lane lane;lane.index=i;lane.track.discrete=i>=8;
        ac::Key a,b;a.frame=-30;b.frame=-10;a.value=1;b.value=3;
        lane.track.keys={a,b};
        if(i==0) {
            lane.nativeCameraTiming=true;
            mikumikustudio::curve_editor::NativeCameraInterval interval;
            interval.first=a.frame;interval.last=b.frame;interval.sourceFirst=-80;interval.sourceLast=20;
            interval.sampleFirst=-35.5;interval.sampleLast=-15.5;
            interval.sourceValueFirst=-5;interval.sourceValueLast=8;
            interval.controls={15,65,95,119};interval.geometry={a.value,a.outWeight,a.outSlope,b.value,b.inWeight,b.inSlope};
            lane.preservedCameraIntervals.push_back(interval);
        }
        data.lanes.push_back(std::move(lane));
    }
    au::NativeData raw;raw.cameraControlsPresent=true;raw.cameraControls.fill(19);
    const auto before=au::Make(data.family,au::Mode::Authored,raw,au::Freeze(data));
    Check(au::TranslatePaste(before,-10,-10)==before,"same frame reuses immutable authoring");
    const auto after=au::TranslatePaste(before,-10,-40);
    Check(after && !au::Same(before,after) && after->Native().cameraControls==raw.cameraControls,
        "moved authoring owns new coordinates while retaining raw camera controls");
    const auto& shifted=after->Author()->Data();
    for(const auto& lane:shifted.lanes)Check(lane.track.keys[0].frame==-60 && lane.track.keys[1].frame==-40,
        "every lane keeps its absolute offset and key spacing");
    const auto& oldLane=before->Author()->Data().lanes[0];
    const auto& lane=shifted.lanes[0];
    const auto& interval=lane.preservedCameraIntervals[0];
    Check(interval.first==-60 && interval.last==-40 && interval.sourceFirst==-80 && interval.sourceLast==20 &&
        interval.sampleFirst==-35.5 && interval.sampleLast==-15.5,"camera slice moves only its display-time coordinates");
    for(double time:{-29.75,-20.0,-10.25}) {
        const auto old=camera::Sample(oldLane,time,true), now=camera::Sample(lane,time-30,true);
        Check(old.value==now.value && old.derivative==now.derivative,"translated exact camera slice retains value and derivative");
    }
    Check(oldLane.track.keys[0].frame==-30 && oldLane.preservedCameraIntervals[0].first==-30,
        "paste never mutates the source authoring");
    bool rejected=false;
    try { au::TranslatePaste(before,-10,minimum); } catch(const std::invalid_argument&) { rejected=true; }
    Check(rejected,"reject the whole author when even one curve endpoint would leave the editor range");
}
}
}

void NativeSignedPasteAuthoringChecks() { signed_paste_authoring_checks::Run(); }
