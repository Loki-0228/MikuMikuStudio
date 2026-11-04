#pragma once
// CPU-only regression for the native unsigned-track boundary. No window,
// renderer, fixture model file, or continuous negative playback is required.
#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include <array>
#include <limits>
#include <stdexcept>

namespace negative_cursor_checks {
using namespace mikumikustudio;
inline unsigned checks = 0;
inline void Require(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
struct Fixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{};
    mdl::MorphRecord morph{};
    std::array<mdl::BoneKey,2> boneKeys{};
    std::array<mdl::MorphKey,2> morphKeys{};
    mdl::DisplayKey display{};
    std::uint32_t boneCursor = 0, morphCursor = 0;
    unsigned char boneActive = 0, morphActive = 0, selected = 1, dirty = 0;
    MMDApp* previous = g_Block;
    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    Fixture() {
        app->ModelSlot(0) = Raw(); g_Block = app.get();
        app->CurrentFrame() = 123;
        model->boneCount = model->morphCount = 1;
        model->boneTable = &bone; model->morphs = &morph;
        model->boneKeys = boneKeys.data(); model->morphKeys = morphKeys.data();
        model->displayKeys = &display; display.visible = 1;
        model->boneKeyCursors = &boneCursor; model->morphKeyCursors = &morphCursor;
        model->boneTrackActive = &boneActive; model->morphTrackActive = &morphActive;
        model->boneSelection = &selected; model->bonePhysicsState = &dirty;
        bone.type = mdl::BoneType::RotateMove; bone.slotIndex = -1;
        bone.parent = -1; bone.rotQuat[3] = 1;
        for (auto& key : boneKeys) key.rotation[3] = 1;
        boneKeys[0].next = morphKeys[0].next = 1;
        boneKeys[1].frame = morphKeys[1].frame = 30;
        boneKeys[1].position[0] = 12; morphKeys[1].value = .6f;
    }
    ~Fixture() {
        negative_frames::Reset(app.get());
#if !MMD_ORIGINAL_PROFILE
        bone_scale::Forget(Raw()); bone_curve::Forget(Raw()); morph_curve::Forget(Raw());
#endif
        app->ModelSlot(0) = nullptr; g_Block = previous;
    }
    std::array<float,8> Pose() const {
        return {bone.trans[0],bone.trans[1],bone.trans[2],bone.rotQuat[0],
                bone.rotQuat[1],bone.rotQuat[2],bone.rotQuat[3],morph.value};
    }
    void Finite() const {
        for (float value : Pose()) Require(std::isfinite(value),"negative time cannot produce a nonfinite bone or morph value");
    }
    void Key(int frame, float value) {
        bone.trans[0] = value; morph.value = value / 100.f;
        Require(negative_frames::RegisterBone(Raw(),0,frame),"register independent negative bone key");
        Require(negative_frames::RegisterMorph(Raw(),0,frame,morph.value),"register independent negative morph key");
    }
};
inline int Run() {
    checks = 0;
    try {
        Fixture f;
        f.Key(frame_limits::minimum,90); f.Key(frame_limits::minimum+30,60);
        f.Key(-30,30); f.Key(-1,1);
        const auto registered = negative_frames::Count(f.app.get());
        const auto modified = f.app->SceneModified();
        struct Sample { double seconds; int frame; };
        const double first = double(frame_limits::minimum)/30.0;
        const Sample samples[]{
            {-1.0,-30}, {-1.0/30.0,-1}, {-.5/30.0,-1},
            {std::nextafter(-1.0/30.0,-std::numeric_limits<double>::infinity()),-1},
            {(-1.0-.0004)/30.0,-1}, {(-1.0-.0006)/30.0,-2},
            {-.0004/30.0,0}, {-.0006/30.0,-1},
            {-std::numeric_limits<double>::denorm_min(),0},
            {first,frame_limits::minimum}, {first-1.0,frame_limits::minimum},
            {(double(frame_limits::minimum)+1)/30.0,frame_limits::minimum+1},
            {-std::numeric_limits<double>::max(),frame_limits::minimum}
        };
        for (const auto& sample : samples) {
            SeekModelFrame(f.Raw(),sample.frame,0);
            const auto expected = f.Pose();
            // Simulate a previous session already parked at the last native key.
            f.boneCursor = f.morphCursor = 1; f.boneActive = f.morphActive = 0;
            f.bone.trans[0] = 999; f.morph.value = 999;
            InitModelTrackCursors(f.Raw(),sample.seconds,0);
            Require(f.Pose()==expected,"negative seconds map to the correct bounded integer frame");
            Require(f.boneCursor==0&&f.morphCursor==0&&f.boneActive&&f.morphActive,
                    "negative initialization prepares native cursors for returning to frame zero");
            f.Finite();
            const auto before = f.Pose();
            AdvanceModelKeyframes(f.Raw(),sample.seconds,0);
            Require(f.Pose()==before,"negative advance holds the sought pose rather than extrapolating unsigned tracks");
            f.Finite();
        }
        // A normal positive evaluation must not depend on which negative pose
        // was sought, or on cursors left by a previous nonnegative session.
        for (double seconds : {0.0,.5,1.0}) {
            InitModelTrackCursors(f.Raw(),0,0); AdvanceModelKeyframes(f.Raw(),seconds,0);
            const auto reference = f.Pose();
            InitModelTrackCursors(f.Raw(),-1,0); AdvanceModelKeyframes(f.Raw(),seconds,0);
            Require(f.Pose()==reference,"returning to nonnegative time preserves native interpolation");
            f.Finite();
        }
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
            const auto before = f.Pose();
            const auto boneCursor = f.boneCursor, morphCursor = f.morphCursor;
            InitModelTrackCursors(f.Raw(),invalid,0); AdvanceModelKeyframes(f.Raw(),invalid,0);
            Require(f.Pose()==before&&boneCursor==f.boneCursor&&morphCursor==f.morphCursor,
                    "nonfinite times are rejected without editing pose or cursors");
        }
        for (double seconds : {-1.0,0.0,std::numeric_limits<double>::quiet_NaN()}) {
            InitModelTrackCursors(nullptr,seconds,0); AdvanceModelKeyframes(nullptr,seconds,0);
        }
        auto empty = std::make_unique<mdl::ModelRecord>();
        auto* raw = reinterpret_cast<unsigned char*>(empty.get());
        InitModelTrackCursors(raw,-1,0); AdvanceModelKeyframes(raw,-1,0);
        empty->displayKeys = &f.display; empty->boneCount = 1;
        InitModelTrackCursors(raw,-1,0); AdvanceModelKeyframes(raw,-1,0);
        Require(empty->displayTrackActive==0,"incomplete negative-time model is rejected before cursor mutation");
        Require(f.app->CurrentFrame()==123&&f.app->SceneModified()==modified&&
                negative_frames::Count(f.app.get())==registered,
                "negative initialization neither moves the editor playhead nor edits registered data");
        std::printf("PASS negative cursor checks=%u (bounded seek and hold; no negative playback)\n",checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"FAIL negative cursor checks=%u: %s\n",checks,error.what());
        return 1;
    }
}
} // namespace negative_cursor_checks
