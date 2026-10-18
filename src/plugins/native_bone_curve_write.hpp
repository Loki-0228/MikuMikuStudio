#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikudancestudio/bone_curve.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
struct NativeBoneCurveControls {
    int frame=0;
    std::array<bone_scale::Curve,5> controls{};
};
struct BoneCurveEdit {
    unsigned bone=0;
    bool freeCurves=false;
    bone_curve::Tracks tracks;
    // Native mode supplies every existing native row in its original order.
    // Frame-zero handles remain untouched, matching the original editor.
    std::vector<NativeBoneCurveControls> nativeControls;
};
struct DisplayCurveRow {
    int frame=0;
    std::uint8_t visible=1;
    std::vector<std::uint8_t> ik;
};
using DisplayCurveRows=std::vector<DisplayCurveRow>;
// One detached multi-bone/display edit. A null display input leaves that family
// unowned. Display values retain all existing native frames and parent data.
// A successful null result is a no-op and must not clear shared redo history.
// Clean affected bones are evaluated locally at the current playhead. A dirty
// bone, including the edited bone itself, keeps its unregistered live pose;
// seeking later evaluates the new track through the normal frame path.
// Display-only visibility/IK writes leave authored bone poses intact. The
// normal viewport draw consumes IK enabled flags and rebuilds solver output.
MmsResult PlanBoneCurveWrite(MMDApp&,unsigned modelSlot,
    const std::vector<BoneCurveEdit>&,const DisplayCurveRows*,
    std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
