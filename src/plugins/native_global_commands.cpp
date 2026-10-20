#include "native_global_commands.hpp"
#include "native_camera_follow_ui.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_global_registration.hpp"
#include "native_accessory_registration.hpp"
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ui_control_readout.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <CommCtrl.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace mikumikustudio::plugins {
namespace {
bool Ready(MMDApp& app, HWND shadowDialog = nullptr, HWND authorSlider = nullptr) noexcept {
    const bool ownShadow = shadowDialog && shadowDialog == app.GroundShadowColorDialog();
    const auto capture = GetCapture();
    const bool ownAuthorSlider = authorSlider && app.MainWindow() &&
        (authorSlider == GetDlgItem(app.MainWindow(), panel::kSelfShadowRangeSlider) ||
         authorSlider == GetDlgItem(app.MainWindow(), panel::kFovSlider) ||
         (GetDlgCtrlID(authorSlider) >= 455 && GetDlgCtrlID(authorSlider) <= 460 &&
          authorSlider == GetDlgItem(app.MainWindow(), GetDlgCtrlID(authorSlider))));
    return !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending && !app.SceneMutationInProgress() &&
        !bake::UiLocked() && !SceneSaveBlocksEdits() &&
        (!capture || (ownShadow && capture == GetDlgItem(shadowDialog, panel::kGroundShadowSlider)) ||
            (ownAuthorSlider && capture == authorSlider)) &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && (!app.GroundShadowColorDialog() || ownShadow) && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog;
}
std::uint8_t& ProjectDisplayField(MMDApp& app,unsigned field) noexcept {
    if(field==1)return app.GroundShadowEnabled();
    if(field==2)return app.ProjectedShadowBlendEnabled();
    return app.GroundGridEnabled();
}
std::array<std::uint8_t,3> ProjectDisplayValues(MMDApp& app) noexcept {
    return {app.GroundGridEnabled(),app.GroundShadowEnabled(),app.ProjectedShadowBlendEnabled()};
}
void RefreshProjectDisplayReadout(MMDApp& app) noexcept {
    const auto flags=ProjectDisplayValues(app);
    const auto main=app.MainWindow();if(!main)return;
    const UINT menus[]={0xD7,0xDD,0xFE};
    for(unsigned i=0;i<3;++i)CheckMenuItem(GetMenu(main),menus[i],MF_BYCOMMAND|(flags[i]?MF_CHECKED:MF_UNCHECKED));
    const HWND windows[]={main,app.FloatingWindow()};
    for(unsigned i=0;i<2;++i){
        if(!windows[i] || (i && windows[i]==main))continue;
        const auto button=GetDlgItem(windows[i],panel::kCoordAxisCheckbox);
        const WPARAM checked=flags[0]?BST_CHECKED:BST_UNCHECKED;
        if(button && SendMessageW(button,BM_GETCHECK,0,0)!=checked){SendMessageW(button,BM_SETCHECK,checked,0);InvalidateRect(button,nullptr,FALSE);}
    }
}
struct ProjectDisplayChange final : HistoryChange, std::enable_shared_from_this<ProjectDisplayChange> {
    MMDApp& app;
    const unsigned field;
    const std::uint64_t pool;
    const std::array<std::uint8_t,3> before;
    const std::uint8_t after;
    std::uint64_t generation=1;
    bool applied=false,published=false;
    ProjectDisplayChange(MMDApp& a,unsigned component,bool enabled) : app(a),field(component),pool(mdl::KeyPoolIdentity(a.CameraKeys())),before(ProjectDisplayValues(a)),after(enabled?1:0) {}
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<ProjectDisplayChange> source;
        const bool forward;
        const std::uint64_t generation;
        const int frame;
        const std::array<std::uint8_t,3> expected;
        bool consumed=false;
        Candidate(std::shared_ptr<ProjectDisplayChange> value,bool direction) : source(std::move(value)),forward(direction),generation(source->generation),frame(source->app.CurrentFrame()),expected(ProjectDisplayValues(source->app)) {}
        bool IsCurrent() const noexcept override {
            return !consumed && source->generation==generation && forward!=source->applied &&
                mdl::KeyPoolIdentity(source->app.CameraKeys())==source->pool && source->app.CurrentFrame()==frame && ProjectDisplayValues(source->app)==expected;
        }
        void Publish() noexcept override {
            if(!IsCurrent())std::terminate();
            ProjectDisplayField(source->app,source->field)=forward?source->after:source->before[source->field];
            source->published=true;source->applied=forward;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();return Boundary([&]() -> MmsResult {
            if(!pool || mdl::KeyPoolIdentity(app.CameraKeys())!=pool || forward==applied || (!published && ProjectDisplayValues(app)!=before))return MMS_E_STALE;
            if(generation==UINT64_MAX)return MMS_E_NOT_READY;
            out=std::make_unique<Candidate>(shared_from_this(),forward);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {return sizeof(*this)+4*sizeof(void*);}
};
using AmbientValues = std::array<float,4>;
AmbientValues ShadowAmbientValues(MMDApp& app) noexcept {
    return {app.ProjectedShadowAmbientIntensity(),app.state.projectedShadowAmbientG,
        app.state.projectedShadowAmbientB,app.state.projectedShadowAmbientA};
}
void WriteShadowAmbient(MMDApp& app,const AmbientValues& values) noexcept {
    app.ProjectedShadowAmbientIntensity()=values[0];app.state.projectedShadowAmbientG=values[1];
    app.state.projectedShadowAmbientB=values[2];app.state.projectedShadowAmbientA=values[3];
}
void RefreshShadowAmbientReadout(MMDApp& app,bool replacePending=false,int sliderPosition=-1) noexcept {
    const auto dialog=app.GroundShadowColorDialog();if(!dialog)return;
    const auto value=app.ProjectedShadowAmbientIntensity();if(!std::isfinite(value))return;
    const auto edit=GetDlgItem(dialog,panel::kGroundShadowEdit);
    if(edit && (replacePending || !SendMessageW(edit,EM_GETMODIFY,0,0))){
        char text[64]{};sprintf_s(text,"%3.2f",double(value));SetWindowTextA(edit,text);SendMessageW(edit,EM_SETMODIFY,FALSE,0);
    }
    // Text commit used float multiplication in the old subclass. Do not
    // replace it with double and move values such as 0.53 to another tick.
    const auto slider=GetDlgItem(dialog,panel::kGroundShadowSlider);
    if(slider){
        const auto current=SendMessageW(slider,TBM_GETPOS,0,0);
        const bool matchingTick=!replacePending && current>=0 && current<=200 && static_cast<float>(double(current)/100.0)==value;
        const auto tick=sliderPosition>=0?sliderPosition:(matchingTick?current:static_cast<LPARAM>(std::clamp(value,0.f,2.f)*100.f));
        if(current!=tick)SendMessageW(slider,TBM_SETPOS,TRUE,tick);
    }
}
struct ShadowAmbientChange final : HistoryChange, std::enable_shared_from_this<ShadowAmbientChange> {
    MMDApp& app;const std::uint64_t pool;const AmbientValues before,after;
    std::uint64_t generation=1;bool applied=false,published=false;
    ShadowAmbientChange(MMDApp& a,float value):app(a),pool(mdl::KeyPoolIdentity(a.CameraKeys())),before(ShadowAmbientValues(a)),after{value,value,value,value}{}
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<ShadowAmbientChange> source;const bool forward;const std::uint64_t generation;const int frame;
        const AmbientValues expected;bool consumed=false;
        Candidate(std::shared_ptr<ShadowAmbientChange> s,bool f):source(std::move(s)),forward(f),generation(source->generation),frame(source->app.CurrentFrame()),expected(ShadowAmbientValues(source->app)){}
        bool IsCurrent() const noexcept override {
            return !consumed && generation==source->generation && forward!=source->applied &&
                mdl::KeyPoolIdentity(source->app.CameraKeys())==source->pool && source->app.CurrentFrame()==frame && ShadowAmbientValues(source->app)==expected;
        }
        void Publish() noexcept override {
            if(!IsCurrent())std::terminate();WriteShadowAmbient(source->app,forward?source->after:source->before);
            source->published=true;source->applied=forward;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();return Boundary([&]()->MmsResult{
            if(!pool || mdl::KeyPoolIdentity(app.CameraKeys())!=pool || forward==applied || (!published && ShadowAmbientValues(app)!=before))return MMS_E_STALE;
            if(generation==UINT64_MAX)return MMS_E_NOT_READY;
            out=std::make_unique<Candidate>(shared_from_this(),forward);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {return sizeof(*this)+4*sizeof(void*);}
};
struct CameraPoseChange final : HistoryChange, std::enable_shared_from_this<CameraPoseChange> {
    MMDApp& app;
    const std::uint64_t poolIdentity;
    const std::array<float, 7> before, after;
    const unsigned changed;
    std::uint64_t generation = 1;
    bool published = false, applied = false;
    static std::array<float, 7> Read(MMDApp& app) noexcept {
        return {app.CameraPositionX(), app.CameraPositionY(), app.CameraPositionZ(),
            app.CameraPitch(), app.CameraYaw(), app.CameraRoll(), app.CameraDistance()};
    }
    CameraPoseChange(MMDApp& value, std::array<float, 7> source, std::array<float, 7> target, unsigned fields)
        : app(value), poolIdentity(mdl::KeyPoolIdentity(value.CameraKeys())), before(source), after(target), changed(fields) {}
    bool SameScene() const noexcept { return poolIdentity && mdl::KeyPoolIdentity(app.CameraKeys()) == poolIdentity; }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<CameraPoseChange> source;
        const bool forward;
        const std::uint64_t generation;
        const int frame;
        const std::array<float, 7> expected;
        bool consumed = false;
        Candidate(std::shared_ptr<CameraPoseChange> value, bool direction)
            : source(std::move(value)), forward(direction), generation(source->generation),
              frame(source->app.CurrentFrame()), expected(Read(source->app)) {}
        bool IsCurrent() const noexcept override {
            const auto live = Read(source->app);
            return !consumed && source->generation == generation && forward != source->applied &&
                source->SameScene() && source->app.CurrentFrame() == frame &&
                !std::memcmp(live.data(), expected.data(), sizeof(expected));
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            for (unsigned field = 0; field < 7; ++field) if (source->changed & (1u << field)) {
                const auto value = forward ? source->after[field] : source->before[field];
                if (field < 3) source->app.CameraPositionAxis(field) = value;
                else if (field < 6) source->app.CameraRotation()[field - 3] = value;
                else source->app.CameraDistance() = value;
            }
            source->applied = forward; source->published = true; ++source->generation;
            consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        return Boundary([&]() -> MmsResult {
            if (!SameScene() || forward == applied) return MMS_E_STALE;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            const auto current = Read(app);
            for (float value : current) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
            if (!published && std::memcmp(current.data(), before.data(), sizeof(before))) return MMS_E_STALE;
            out = std::make_unique<Candidate>(shared_from_this(), forward); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
};
struct LightPoseChange final : HistoryChange, std::enable_shared_from_this<LightPoseChange> {
    MMDApp& app;
    const std::uint64_t poolIdentity;
    const std::array<float, 6> before, after;
    const unsigned changed;
    std::uint64_t generation = 1;
    bool published = false, applied = false;
    static std::array<float, 6> Read(MMDApp& app) noexcept {
        return {app.LightColor()[0], app.LightColor()[1], app.LightColor()[2],
            app.LightDirection()[0], app.LightDirection()[1], app.LightDirection()[2]};
    }
    LightPoseChange(MMDApp& value, std::array<float, 6> source, std::array<float, 6> target, unsigned fields)
        : app(value), poolIdentity(mdl::KeyPoolIdentity(value.LightKeys())), before(source), after(target), changed(fields) {}
    bool SameScene() const noexcept { return poolIdentity && mdl::KeyPoolIdentity(app.LightKeys()) == poolIdentity; }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<LightPoseChange> source;
        const bool forward;
        const std::uint64_t generation;
        const int frame;
        const std::array<float, 6> expected;
        const D3DLIGHT9 expectedLight;
        bool consumed = false;
        Candidate(std::shared_ptr<LightPoseChange> value, bool direction)
            : source(std::move(value)), forward(direction), generation(source->generation),
              frame(source->app.CurrentFrame()), expected(Read(source->app)), expectedLight(source->app.SceneLight()) {}
        bool IsCurrent() const noexcept override {
            const auto live = Read(source->app);
            return !consumed && source->generation == generation && forward != source->applied &&
                source->SameScene() && source->app.CurrentFrame() == frame &&
                !std::memcmp(live.data(), expected.data(), sizeof(expected)) &&
                !std::memcmp(&source->app.SceneLight(), &expectedLight, sizeof(expectedLight));
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            for (unsigned field = 0; field < 6; ++field) if (source->changed & (1u << field)) {
                const auto value = forward ? source->after[field] : source->before[field];
                if (field < 3) source->app.LightColor()[field] = value;
                else source->app.LightDirection()[field - 3] = value;
            }
            source->app.ApplyTimelineLightState();
            source->applied = forward; source->published = true; ++source->generation;
            consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        return Boundary([&]() -> MmsResult {
            if (!SameScene() || forward == applied) return MMS_E_STALE;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            const auto current = Read(app);
            for (float value : current) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
            if (!published && std::memcmp(current.data(), before.data(), sizeof(before))) return MMS_E_STALE;
            out = std::make_unique<Candidate>(shared_from_this(), forward); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
};
struct CameraProjectionChange final : HistoryChange, std::enable_shared_from_this<CameraProjectionChange> {
    MMDApp& app;
    const std::uint64_t pool;
    const float beforeFov, afterFov;
    const std::uint8_t beforePerspective, afterPerspective;
    const bool editPerspective;
    std::uint64_t generation = 1;
    bool applied = false, published = false;
    CameraProjectionChange(MMDApp& a, float fov, std::uint8_t perspective, bool modeField) : app(a), pool(mdl::KeyPoolIdentity(a.CameraKeys())),
        beforeFov(a.CameraFov()), afterFov(fov), beforePerspective(a.CameraPerspective()), afterPerspective(perspective), editPerspective(modeField) {}
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<CameraProjectionChange> source;
        bool forward, consumed = false;
        const std::uint64_t generation;
        const int frame;
        const float fov, distance;
        const std::uint8_t perspective;
        Candidate(std::shared_ptr<CameraProjectionChange> value, bool direction) : source(std::move(value)), forward(direction),
            generation(source->generation), frame(source->app.CurrentFrame()), fov(source->app.CameraFov()),
            distance(source->app.CameraDistance()), perspective(source->app.CameraPerspective()) {}
        bool IsCurrent() const noexcept override {
            const float currentDistance = source->app.CameraDistance();
            return !consumed && source->generation == generation && forward != source->applied &&
                mdl::KeyPoolIdentity(source->app.CameraKeys()) == source->pool && source->app.CurrentFrame() == frame &&
                source->app.CameraFov() == fov && source->app.CameraPerspective() == perspective &&
                !std::memcmp(&currentDistance, &distance, sizeof(distance));
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            if (source->editPerspective) source->app.CameraPerspective() = forward ? source->afterPerspective : source->beforePerspective;
            else source->app.CameraFov() = forward ? source->afterFov : source->beforeFov;
            source->app.cameraProjectionRefreshPending = true;
            source->published = true; source->applied = forward; ++source->generation; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!pool || mdl::KeyPoolIdentity(app.CameraKeys()) != pool || forward == applied ||
                (!published && (app.CameraFov() != beforeFov || app.CameraPerspective() != beforePerspective))) return MMS_E_STALE;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            if (!std::isfinite(app.CameraFov())) return MMS_E_ARGUMENT;
            out = std::make_unique<Candidate>(shared_from_this(), forward); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
};
struct ShadowPoseChange final : HistoryChange, std::enable_shared_from_this<ShadowPoseChange> {
    MMDApp& app;
    const std::uint64_t pool;
    const float beforeDistance, afterDistance;
    const int beforeMode, afterMode;
    const bool editMode;
    std::uint64_t generation = 1;
    bool applied = false, published = false;
    ShadowPoseChange(MMDApp& a, float distance, int mode, bool modeField) : app(a), pool(mdl::KeyPoolIdentity(a.ShadowKeys())),
        beforeDistance(a.ShadowDistance()), afterDistance(distance), beforeMode(a.SelfShadowMode()), afterMode(mode), editMode(modeField) {}
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<ShadowPoseChange> source;
        const bool forward;
        const std::uint64_t generation;
        const int frame, mode;
        const float distance;
        bool consumed = false;
        Candidate(std::shared_ptr<ShadowPoseChange> value, bool direction) : source(std::move(value)), forward(direction),
            generation(source->generation), frame(source->app.CurrentFrame()), mode(source->app.SelfShadowMode()), distance(source->app.ShadowDistance()) {}
        bool IsCurrent() const noexcept override {
            return !consumed && source->generation == generation && forward != source->applied &&
                mdl::KeyPoolIdentity(source->app.ShadowKeys()) == source->pool && source->app.CurrentFrame() == frame &&
                source->app.SelfShadowMode() == mode && source->app.ShadowDistance() == distance;
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            if (source->editMode) source->app.SelfShadowMode() = forward ? source->afterMode : source->beforeMode;
            else source->app.ShadowDistance() = forward ? source->afterDistance : source->beforeDistance;
            source->published = true; source->applied = forward; ++source->generation; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!pool || mdl::KeyPoolIdentity(app.ShadowKeys()) != pool || forward == applied ||
                (!published && (app.ShadowDistance() != beforeDistance || app.SelfShadowMode() != beforeMode))) return MMS_E_STALE;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            if (!std::isfinite(app.ShadowDistance())) return MMS_E_ARGUMENT;
            out = std::make_unique<Candidate>(shared_from_this(), forward); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
};
// Scalar records do not own mesh/key resources. Controlled import/delete records
// retain those resources; chronological undo restores the object before its edit.
struct AccessoryPoseChange final : HistoryChange, std::enable_shared_from_this<AccessoryPoseChange> {
    MMDApp& app;
    const unsigned slot, field;
    mdl::AccessoryRecord* const object;
    const MmsId id;
    const std::uint64_t pool;
    struct Values { std::array<float, 8> numeric; std::array<std::uint8_t, 3> flags; };
    const Values before, after;
    std::uint64_t generation = 1;
    bool applied = false, published = false;
    static Values Read(const mdl::AccessoryRecord& a) noexcept {
        return {{a.position[0], a.position[1], a.position[2], a.rotation[0], a.rotation[1], a.rotation[2], a.scale, a.opacity},
            {a.visible, a.additiveBlend, a.shadowEnabled}};
    }
    static bool Same(const Values& a, const Values& b) noexcept {
        return !std::memcmp(a.numeric.data(), b.numeric.data(), sizeof(a.numeric)) && a.flags == b.flags;
    }
    AccessoryPoseChange(MMDApp& a, unsigned index, unsigned component, Values source, Values target)
        : app(a), slot(index), field(component), object(a.AccessorySlot(index)), id(SceneObjectIdentity(object).id),
          pool(mdl::KeyPoolIdentity(a.AccessoryKeys(index))), before(source), after(target) {}
    bool SameScene() const noexcept {
        // Check membership before dereferencing a possibly removed object.
        return app.AccessorySlot(slot) == object && SceneObjectPresent(object, id) && pool &&
            mdl::KeyPoolIdentity(app.AccessoryKeys(slot)) == pool;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<AccessoryPoseChange> source;
        const bool forward;
        const std::uint64_t generation, objectGeneration;
        const int frame, parentModel, parentBone;
        const Values expected;
        bool consumed = false;
        Candidate(std::shared_ptr<AccessoryPoseChange> value, bool direction) : source(std::move(value)), forward(direction),
            generation(source->generation), objectGeneration(SceneObjectIdentity(source->object).generation),
            frame(source->app.CurrentFrame()), parentModel(source->object->parentModel), parentBone(source->object->parentBone),
            expected(Read(*source->object)) {}
        bool IsCurrent() const noexcept override {
            if (consumed || source->generation != generation || forward == source->applied || !source->SameScene()) return false;
            const auto live = Read(*source->object);
            return SceneObjectIdentity(source->object).generation == objectGeneration && source->app.CurrentFrame() == frame &&
                source->object->parentModel == parentModel && source->object->parentBone == parentBone &&
                Same(live, expected);
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            auto& a = *source->object;
            const auto& values = forward ? source->after : source->before;
            if (source->field >= 8) {
                const auto flag = values.flags[source->field - 8];
                if (source->field == 8) a.visible = flag;
                else if (source->field == 9) a.additiveBlend = flag;
                else a.shadowEnabled = flag;
            } else {
                const auto value = values.numeric[source->field];
                if (source->field < 3) a.position[source->field] = value;
                else if (source->field < 6) a.rotation[source->field - 3] = value;
                else if (source->field == 6) a.scale = value;
                else a.opacity = value;
            }
            source->applied = forward; source->published = true; ++source->generation; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!SameScene() || forward == applied) return MMS_E_STALE;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            const auto current = Read(*object);
            for (float value : current.numeric) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
            if (!published && !Same(current, before)) return MMS_E_STALE;
            out = std::make_unique<Candidate>(shared_from_this(), forward); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
};
bool ReadLegacyNumber(HWND window, int id, double& value) noexcept {
    const auto edit = GetDlgItem(window, id);
    if (!edit) return false;
    // The original registration path reads at most seven ANSI characters.
    // Reject non-finite and non-numeric input before any authoring is changed.
    char text[8]{}; GetWindowTextA(edit, text, sizeof(text));
    errno = 0; char* end = nullptr; value = std::strtod(text, &end);
    return end != text && errno != ERANGE && std::isfinite(value);
}
bool SelectTrack(MMDApp& app, curve_host::Kind kind, unsigned slot) noexcept {
    // RefreshRequest preserves a multi-row selection when the target row is
    // already active; only entering an unselected row makes it exclusive.
    if (kind == curve_host::Kind::Accessory) {
        const auto* object = app.AccessorySlot(slot);
        if (object && object->rowSelected) return false;
    } else if (app.GlobalTrackSelected(kind == curve_host::Kind::Camera ? GlobalTimelineTrack::Camera :
        kind == curve_host::Kind::Light ? GlobalTimelineTrack::Light : GlobalTimelineTrack::SelfShadow)) return false;
    bool changed = false;
    for (unsigned index = 0; index < 4; ++index) {
        const auto selected = kind != curve_host::Kind::Accessory && index ==
            (kind == curve_host::Kind::Camera ? 0u : kind == curve_host::Kind::Light ? 1u : 2u);
        changed = changed || app.state.globalTrackSelected[index] != std::uint8_t(selected);
    }
    if (kind == curve_host::Kind::Accessory) app.ClearGlobalTimelineTrackSelection();
    else app.SelectGlobalTimelineTrack(kind == curve_host::Kind::Camera ? GlobalTimelineTrack::Camera :
        kind == curve_host::Kind::Light ? GlobalTimelineTrack::Light : GlobalTimelineTrack::SelfShadow);
    for (unsigned index = 0; index < kModelSlotCount; ++index)
        if (auto* object = app.AccessorySlot(index)) {
            const auto selected = kind == curve_host::Kind::Accessory && index == slot;
            changed = changed || object->rowSelected != std::uint8_t(selected);
            object->rowSelected = selected;
        }
    return changed;
}
template<class Plan>
bool Register(MMDApp* app, curve_host::Kind kind, Plan plan) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    if (!Ready(*app) || history->Enter() != MMS_OK) return true;
    history->Leave();
    Boundary([&]() -> MmsResult {
        const auto slot = unsigned(app->SelectedAccessorySlot());
        std::shared_ptr<HistoryChange> change;
        if (auto result = plan(*app, change)) return result;
        if (!change) return MMS_OK;
        const char* label = kind == curve_host::Kind::Camera ? "Register camera key" :
            kind == curve_host::Kind::Light ? "Register light key" :
            kind == curve_host::Kind::Shadow ? "Register shadow key" : "Register accessory key";
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto result = history->PrepareNative(label, std::move(change), transaction)) return result;
        if (auto result = history->CommitNative(std::move(transaction))) return result;
        // Track-row selection is a view choice, not authoring restored by undo.
        // Failure/no-op has already returned, so the original selection survives.
        const bool selectionChanged = SelectTrack(*app, kind, slot);
        if (kind == curve_host::Kind::Accessory) app->state.enterKeyState = 3;
        PublishBoundPoseEdit(*app, *history, false);
        if (app->MainWindow()) {
            // The model-mode sweep also normalizes bone selection. A global
            // registration must not change that unrelated edit target.
            if (selectionChanged && app->CameraMode() && app->Renderer() && app->PanelDC()) PostLanguageSweep(app);
            RefreshBoundGlobalReadout(*app);
            if (app->CurveDC()) SelectionReeval(app);
        }
        curve_host::NotifyRegistered(app, kind, kind == curve_host::Kind::Accessory ? int(slot) : 0);
        return MMS_OK;
    });
    return true;
}
}
struct CameraNumericEdit::Impl {
    MMDApp& app;
    SharedHistory* history;
    std::uint64_t revision = 0, pool = 0;
    int frame = 0, parentModel = -1, parentBone = 0;
    float fov = 0;
    unsigned perspective = 0;
    unsigned char* parent = nullptr;
    MmsObjectRef parentRef{};
    mdl::BoneRecord* parentBones = nullptr;
    unsigned parentCount = 0;
    std::array<float, 7> before{}, values{};
    Impl(MMDApp& a, SharedHistory& h) : app(a), history(&h) {}
    bool Current() const {
        if (BoundNativePoseHistory(&app) != history || !app.CameraMode() || !Ready(app) ||
            app.CurrentFrame() != frame || mdl::KeyPoolIdentity(app.CameraKeys()) != pool ||
            app.CameraParentModel() != parentModel || app.CameraParentBone() != parentBone ||
            app.CameraFov() != fov || app.CameraPerspective() != perspective) return false;
        std::uint64_t current = 0;
        if (history->Revision(current) != MMS_OK || current != revision) return false;
        if (parentModel != -1) {
            if (app.ModelSlot(parentModel) != parent || !SceneObjectPresent(parent, parentRef.id) ||
                SceneObjectIdentity(parent).generation != parentRef.generation) return false;
            const auto* model = mdl::Mdl(parent);
            if (model->boneTable != parentBones || model->boneCount != parentCount) return false;
        }
        const auto pose = CameraPoseChange::Read(app);
        return !std::memcmp(pose.data(), before.data(), sizeof(before));
    }
};
CameraNumericEdit::CameraNumericEdit() = default;
CameraNumericEdit::~CameraNumericEdit() = default;
std::array<float, 7> CameraNumericEdit::Values() const noexcept { return impl_ ? impl_->values : std::array<float, 7>{}; }
MmsResult CameraNumericEdit::Begin(MMDApp& app, SharedHistory& history) noexcept {
    if (impl_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        if (BoundNativePoseHistory(&app) != &history || !app.CameraMode() || !Ready(app) ||
            !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
        if (const auto result = history.CheckNativeEdit()) return result;
        auto next = std::make_unique<Impl>(app, history);
        if (const auto result = history.Revision(next->revision)) return result;
        next->pool = mdl::KeyPoolIdentity(app.CameraKeys()); next->frame = app.CurrentFrame();
        next->parentModel = app.CameraParentModel(); next->parentBone = app.CameraParentBone();
        next->fov = app.CameraFov(); next->perspective = app.CameraPerspective();
        if (next->parentModel != -1) {
            if (next->parentModel < 0 || unsigned(next->parentModel) >= kModelSlotCount ||
                !(next->parent = app.ModelSlot(next->parentModel))) return MMS_E_ARGUMENT;
            const auto* parent = mdl::Mdl(next->parent);
            if (!parent->boneTable || next->parentBone < 0 || unsigned(next->parentBone) >= parent->boneCount) return MMS_E_ARGUMENT;
            next->parentRef = SceneObjectIdentity(next->parent); next->parentBones = parent->boneTable; next->parentCount = parent->boneCount;
        }
        next->before = CameraPoseChange::Read(app); next->values = next->before;
        for (float value : next->before) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        constexpr float degrees = 180.0f / 3.14159265358979323846f;
        next->values[3] = -next->before[3] * degrees;
        next->values[4] = next->before[4] * degrees; next->values[5] = next->before[5] * degrees;
        next->values[6] = -next->before[6];
        for (float value : next->values) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        impl_ = std::move(next); return MMS_OK;
    });
}
MmsResult CameraNumericEdit::Apply(const std::array<float, 7>& input) noexcept {
    if (!impl_) return MMS_E_NOT_READY;
    auto session = std::move(impl_);
    return Boundary([&]() -> MmsResult {
        if (BoundNativePoseHistory(&session->app) != session->history) return MMS_E_STALE;
        if (const auto result = session->history->CheckNativeEdit()) return result;
        if (!session->Current()) return MMS_E_STALE;
        auto target = session->before; unsigned changed = 0;
        constexpr float radians = 3.14159265358979323846f / 180.0f;
        for (unsigned field = 0; field < 7; ++field) {
            if (!std::isfinite(input[field])) return MMS_E_ARGUMENT;
            if (input[field] != session->values[field]) {
                target[field] = field < 3 ? input[field] : field == 6 ? -input[field] :
                    (field == 3 ? -input[field] : input[field]) * radians;
                if (!std::isfinite(target[field])) return MMS_E_ARGUMENT;
                if (target[field] != session->before[field]) changed |= 1u << field;
            }
        }
        if (!changed) return MMS_OK;
        auto change = std::make_shared<CameraPoseChange>(session->app, session->before, target, changed);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = session->history->PrepareNative("Edit camera numeric pose", std::move(change), transaction, SharedHistory::NativeIntent::Adjustment)) return result;
        if (const auto result = session->history->CommitNative(std::move(transaction))) return result;
        PublishBoundPoseEdit(session->app, *session->history, false); return MMS_OK;
    });
}
MmsResult PlanCameraRotationReset(MMDApp& app, std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    return Boundary([&]() -> MmsResult {
        if (!Ready(app) || !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
        const auto before = CameraPoseChange::Read(app); auto after = before; unsigned changed = 0;
        for (float value : before) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        for (unsigned field = 3; field < 6; ++field) if (before[field] != 0.0f) {
            changed |= 1u << field; after[field] = 0.0f;
        }
        if (changed) out = std::make_shared<CameraPoseChange>(app, before, after, changed);
        return MMS_OK;
    });
}
bool TryResetBoundCameraRotation(MMDApp* app) noexcept {
    if (!app || !app->CameraMode()) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    Boundary([&]() -> MmsResult {
        if (!Ready(*app)) return MMS_E_NOT_READY;
        if (const auto result = history->CheckNativeEdit()) return result;
        std::shared_ptr<HistoryChange> change;
        if (const auto result = PlanCameraRotationReset(*app, change)) return result;
        if (!change) return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = history->PrepareNative("Reset camera rotation", std::move(change), transaction, SharedHistory::NativeIntent::Adjustment)) return result;
        if (const auto result = history->CommitNative(std::move(transaction))) return result;
        PublishBoundPoseEdit(*app, *history, false); return MMS_OK;
    });
    return true;
}
namespace {
MmsResult ApplyProjectDisplayFlag(MMDApp& app,unsigned field,bool enabled,bool& changed) noexcept {
    changed=false;return Boundary([&]() -> MmsResult {
        auto* history=BoundNativePoseHistory(&app);
        if(!history || !Ready(app) || !mdl::KeyPoolIdentity(app.CameraKeys()))return MMS_E_NOT_READY;
        if(field>=3)return MMS_E_ARGUMENT;
        if(const auto result=history->CheckNativeEdit())return result;
        if(ProjectDisplayField(app,field)==(enabled?1:0))return MMS_OK;
        auto change=std::make_shared<ProjectDisplayChange>(app,field,enabled);
        const char* labels[]={"Coordinate axis display","Projected ground shadow","Transparent ground shadow"};
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if(const auto result=history->PrepareNative(labels[field],std::move(change),transaction, SharedHistory::NativeIntent::Adjustment))return result;
        if(const auto result=history->CommitNative(std::move(transaction)))return result;
        changed=true;
        Boundary([&]() -> MmsResult {PublishBoundPoseEdit(app,*history,false);RefreshProjectDisplayReadout(app);return MMS_OK;});
        return MMS_OK;
    });
}
}
MmsResult ApplyBoundGridDisplay(MMDApp& app,bool enabled,bool& changed) noexcept {
    return ApplyProjectDisplayFlag(app,0,enabled,changed);
}
bool TryToggleBoundGridDisplay(MMDApp* app) noexcept {
    if(!app || !BoundNativePoseHistory(app))return false;
    bool changed=false;ApplyBoundGridDisplay(*app,app->GroundGridEnabled()==0,changed);
    if(!changed)RefreshProjectDisplayReadout(*app);
    return true;
}
MmsResult ApplyBoundProjectedShadowFlag(MMDApp& app,unsigned field,bool enabled,bool& changed) noexcept {
    if(field>=2){changed=false;return MMS_E_ARGUMENT;}
    return ApplyProjectDisplayFlag(app,field+1,enabled,changed);
}
bool TryToggleBoundProjectedShadowFlag(MMDApp* app,unsigned field) noexcept {
    if(!app || !BoundNativePoseHistory(app))return false;
    bool changed=false;
    if(field<2)ApplyBoundProjectedShadowFlag(*app,field,ProjectDisplayField(*app,field+1)==0,changed);
    if(!changed)RefreshProjectDisplayReadout(*app);
    return true;
}
MmsResult ApplyBoundProjectedShadowAmbient(MMDApp& app,float value,bool& changed,HWND dialog) noexcept {
    changed=false;return Boundary([&]()->MmsResult{
        auto* history=BoundNativePoseHistory(&app);if(!history)return MMS_E_NOT_READY;
        if(const auto result=history->CheckNativeEdit())return result;
        if((dialog && dialog!=app.GroundShadowColorDialog()) || !Ready(app,dialog) || !mdl::KeyPoolIdentity(app.CameraKeys()))return MMS_E_NOT_READY;
        if(!std::isfinite(value)||value<0||value>2)return MMS_E_ARGUMENT;
        const auto before=ShadowAmbientValues(app);for(float part:before)if(!std::isfinite(part))return MMS_E_ARGUMENT;
        if(before==AmbientValues{value,value,value,value})return MMS_OK;
        auto change=std::make_shared<ShadowAmbientChange>(app,value);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if(const auto result=history->PrepareNative("Projected shadow ambient",std::move(change),transaction, SharedHistory::NativeIntent::Adjustment))return result;
        if(const auto result=history->CommitNative(std::move(transaction)))return result;
        changed=true;
        Boundary([&]()->MmsResult{PublishBoundPoseEdit(app,*history,false);if(!dialog)RefreshShadowAmbientReadout(app);return MMS_OK;});return MMS_OK;
    });
}
bool TryCommitBoundProjectedShadowText(MMDApp* app,HWND edit) noexcept {
    if(!app || !BoundNativePoseHistory(app))return false;
    Boundary([&]()->MmsResult{
        const auto dialog=GetParent(edit);
        if(!dialog || dialog!=app->GroundShadowColorDialog() || GetDlgItem(dialog,panel::kGroundShadowEdit)!=edit)return MMS_E_NOT_READY;
        if(SendMessageW(edit,EM_GETMODIFY,0,0)){
            char text[64]{};GetWindowTextA(edit,text,sizeof text);char* end=nullptr;float value=std::strtof(text,&end);
            if(end!=text && std::isfinite(value)){
                value=std::clamp(value,0.f,2.f);app->state.messageSeen=1;bool changed=false;
                ApplyBoundProjectedShadowAmbient(*app,value,changed,dialog);
            }
        }
        RefreshShadowAmbientReadout(*app,true);return MMS_OK;
    });return true;
}
bool TryCommitBoundProjectedShadowSlider(MMDApp* app,HWND dialog) noexcept {
    if(!app || !BoundNativePoseHistory(app))return false;
    Boundary([&]()->MmsResult{
        if(!dialog || dialog!=app->GroundShadowColorDialog())return MMS_E_NOT_READY;
        const auto position=SendDlgItemMessageW(dialog,panel::kGroundShadowSlider,TBM_GETPOS,0,0);
        const float value=static_cast<float>(double(position)/100.0);bool changed=false;
        const auto result=ApplyBoundProjectedShadowAmbient(*app,value,changed,dialog);
        // Preserve the exact live thumb tick, including float round-trip boundaries.
        RefreshShadowAmbientReadout(*app,true,result==MMS_OK?int(position):-1);return MMS_OK;
    });return true;
}
MmsResult ApplyBoundCameraField(MMDApp& app, unsigned field, float value, bool& changed, bool activateCameraTrack) noexcept {
    changed = false;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history || !app.CameraMode() || !Ready(app) || !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
        if (field >= 7 || !std::isfinite(value)) return MMS_E_ARGUMENT;
        if (const auto result = history->CheckNativeEdit()) return result;
        const auto before = CameraPoseChange::Read(app);
        for (float source : before) if (!std::isfinite(source)) return MMS_E_ARGUMENT;
        if (before[field] == value) return MMS_OK;
        auto after = before; after[field] = value;
        auto change = std::make_shared<CameraPoseChange>(app, before, after, 1u << field);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = history->PrepareNative("Edit camera pose component", std::move(change), transaction, SharedHistory::NativeIntent::Adjustment)) return result;
        if (const auto result = history->CommitNative(std::move(transaction))) return result;
        changed = true;
        // A completed publication is not a failed edit if a later optional UI
        // notification cannot allocate. Do not invite callers to repeat it.
        Boundary([&]() -> MmsResult {
            const bool selectionChanged = activateCameraTrack && SelectTrack(app, curve_host::Kind::Camera, 0);
            PublishBoundPoseEdit(app, *history, false);
            if (selectionChanged && app.MainWindow() && app.Renderer() && app.PanelDC()) PostLanguageSweep(&app);
            return MMS_OK;
        });
        return MMS_OK;
    });
}
bool TryResetBoundCameraField(MMDApp* app, unsigned field, bool activateCameraTrack) noexcept {
    if (!app || !app->CameraMode() || !BoundNativePoseHistory(app)) return false;
    bool changed = false;
    ApplyBoundCameraField(*app, field, 0.0f, changed, activateCameraTrack);
    return true;
}
namespace {
MmsResult ApplyBoundCameraProjectionField(MMDApp& app, float fov, std::uint8_t perspective, bool editPerspective, bool& changed, HWND slider = nullptr) noexcept {
    changed = false;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history || !Ready(app, nullptr, slider) || !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
        if (!std::isfinite(fov) || !std::isfinite(app.CameraFov())) return MMS_E_ARGUMENT;
        if (const auto result = history->CheckNativeEdit()) return result;
        if (editPerspective ? app.CameraPerspective() == perspective : app.CameraFov() == fov) return MMS_OK;
        auto change = std::make_shared<CameraProjectionChange>(app, fov, perspective, editPerspective);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = history->PrepareNative(editPerspective ? "Change camera projection" : "Edit camera field of view", std::move(change), transaction, SharedHistory::NativeIntent::Adjustment)) return result;
        if (const auto result = history->CommitNative(std::move(transaction))) return result;
        changed = true;
        Boundary([&]() -> MmsResult {
            const bool selectionChanged = SelectTrack(app, curve_host::Kind::Camera, 0);
            PublishBoundPoseEdit(app, *history, false); RefreshBoundGlobalReadout(app);
            if (selectionChanged && app.CameraMode() && app.MainWindow() && app.Renderer() && app.PanelDC()) PostLanguageSweep(&app);
            return MMS_OK;
        });
        return MMS_OK;
    });
}
}
MmsResult ApplyBoundCameraFov(MMDApp& app, float value, bool& changed) noexcept {
    return ApplyBoundCameraProjectionField(app, value, app.CameraPerspective(), false, changed);
}
bool TryCommitBoundCameraFovSlider(MMDApp* app, HWND slider) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    Boundary([&]() -> MmsResult {
        if (!slider || !app->MainWindow() || slider != GetDlgItem(app->MainWindow(), panel::kFovSlider)) return MMS_E_ARGUMENT;
        // Read the trackbar's actual position, including its native range
        // clamping, exactly as the unbound WM_HSCROLL route does.
        const float fov = static_cast<float>(static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0)));
        bool changed = false;
        const auto result = ApplyBoundCameraProjectionField(*app, fov, app->CameraPerspective(), false, changed, slider);
        if (!changed) RefreshBoundGlobalReadout(*app);
        return result;
    });
    return true;
}
MmsResult ApplyBoundCameraProjection(MMDApp& app, bool orthographic, bool& changed) noexcept {
    return ApplyBoundCameraProjectionField(app, app.CameraFov(), orthographic ? 1 : 0, true, changed);
}
bool TryToggleBoundCameraPerspective(MMDApp* app) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    bool changed = false;
    ApplyBoundCameraProjection(*app, app->CameraPerspective() == 0, changed);
    if (!changed) RefreshBoundGlobalReadout(*app);
    return true;
}
namespace {
MmsResult ApplyBoundShadowField(MMDApp& app, float distance, int mode, bool editMode, bool& changed, HWND slider = nullptr) noexcept {
    changed = false;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history) return MMS_E_NOT_READY;
        if (const auto result = history->CheckNativeEdit()) return result;
        if (!Ready(app, nullptr, slider) || !mdl::KeyPoolIdentity(app.ShadowKeys())) return MMS_E_NOT_READY;
        if (!std::isfinite(distance) || !std::isfinite(app.ShadowDistance()) || (editMode && (mode < 0 || mode > 2))) return MMS_E_ARGUMENT;
        if (editMode ? app.SelfShadowMode() == mode : app.ShadowDistance() == distance) return MMS_OK;
        auto change = std::make_shared<ShadowPoseChange>(app, distance, mode, editMode);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = history->PrepareNative(editMode ? "Edit self-shadow mode" : "Edit self-shadow distance", std::move(change), transaction, SharedHistory::NativeIntent::Adjustment)) return result;
        if (const auto result = history->CommitNative(std::move(transaction))) return result;
        changed = true;
        Boundary([&]() -> MmsResult {
            const bool selectionChanged = SelectTrack(app, curve_host::Kind::Shadow, 0);
            PublishBoundPoseEdit(app, *history, false); RefreshBoundGlobalReadout(app);
            if (selectionChanged && app.CameraMode() && app.MainWindow() && app.Renderer() && app.PanelDC()) PostLanguageSweep(&app);
            return MMS_OK;
        });
        return MMS_OK;
    });
}
}
MmsResult ApplyBoundShadowDistance(MMDApp& app, float value, bool& changed) noexcept {
    return ApplyBoundShadowField(app, value, app.SelfShadowMode(), false, changed);
}
bool TryCommitBoundShadowSlider(MMDApp* app, HWND slider) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    Boundary([&]() -> MmsResult {
        if (!slider || !app->MainWindow() || slider != GetDlgItem(app->MainWindow(), panel::kSelfShadowRangeSlider)) return MMS_E_ARGUMENT;
        const auto position = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        const float distance = static_cast<float>((10000.0 - std::clamp(position, 0, 9999)) / 100000.0);
        bool changed = false;
        const auto result = ApplyBoundShadowField(*app, distance, app->SelfShadowMode(), false, changed, slider);
        if (!changed) RefreshBoundGlobalReadout(*app);
        return result;
    });
    return true;
}
bool TrySetBoundShadowMode(MMDApp* app, int mode) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    bool changed = false;
    ApplyBoundShadowField(*app, app->ShadowDistance(), mode, true, changed);
    // A native button may have toggled before its command arrived. Restore the
    // authored radio state on no-op or rejection without creating an edit.
    if (!changed) RefreshBoundGlobalReadout(*app);
    return true;
}
MmsResult ApplyBoundAccessoryField(MMDApp& app, unsigned slot, unsigned field, float value, bool& changed) noexcept {
    changed = false;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history || !Ready(app)) return MMS_E_NOT_READY;
        if (slot >= kModelSlotCount || field >= 8 || !std::isfinite(value) || (field == 7 && (value < 0 || value > 1))) return MMS_E_ARGUMENT;
        if (const auto result = history->CheckNativeEdit()) return result;
        auto* object = app.AccessorySlot(slot);
        if (!object || !mdl::KeyPoolIdentity(app.AccessoryKeys(slot))) return MMS_E_NOT_READY;
        const auto before = AccessoryPoseChange::Read(*object); auto after = before;
        for (float source : before.numeric) if (!std::isfinite(source)) return MMS_E_ARGUMENT;
        if (before.numeric[field] == value) return MMS_OK;
        after.numeric[field] = value;
        auto change = std::make_shared<AccessoryPoseChange>(app, slot, field, before, after);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = history->PrepareNative("Edit accessory field", std::move(change), transaction, SharedHistory::NativeIntent::Adjustment)) return result;
        if (const auto result = history->CommitNative(std::move(transaction))) return result;
        changed = true;
        Boundary([&]() -> MmsResult {
            const bool selectionChanged = SelectTrack(app, curve_host::Kind::Accessory, slot);
            PublishBoundPoseEdit(app, *history, false); RefreshBoundGlobalReadout(app);
            if (selectionChanged && app.CameraMode() && app.MainWindow() && app.Renderer() && app.PanelDC()) PostLanguageSweep(&app);
            return MMS_OK;
        });
        return MMS_OK;
    });
}
MmsResult ApplyBoundAccessoryFlag(MMDApp& app, unsigned slot, unsigned field, bool enabled, bool& changed) noexcept {
    changed = false;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history || !Ready(app)) return MMS_E_NOT_READY;
        if (slot >= kModelSlotCount || field >= 3) return MMS_E_ARGUMENT;
        if (const auto result = history->CheckNativeEdit()) return result;
        auto* object = app.AccessorySlot(slot);
        if (!object || !mdl::KeyPoolIdentity(app.AccessoryKeys(slot))) return MMS_E_NOT_READY;
        const auto before = AccessoryPoseChange::Read(*object); auto after = before;
        const std::uint8_t target = enabled ? 1 : 0;
        if (before.flags[field] == target) return MMS_OK;
        after.flags[field] = target;
        auto change = std::make_shared<AccessoryPoseChange>(app, slot, 8 + field, before, after);
        const char* labels[] = {"Accessory visibility", "Accessory additive blend", "Accessory shadow"};
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = history->PrepareNative(labels[field], std::move(change), transaction, SharedHistory::NativeIntent::Adjustment)) return result;
        if (const auto result = history->CommitNative(std::move(transaction))) return result;
        changed = true;
        Boundary([&]() -> MmsResult {
            // The original additive toggle does not activate the accessory row.
            const bool selectionChanged = field != 1 && SelectTrack(app, curve_host::Kind::Accessory, slot);
            PublishBoundPoseEdit(app, *history, false); RefreshBoundGlobalReadout(app);
            if (selectionChanged && app.CameraMode() && app.MainWindow() && app.Renderer() && app.PanelDC()) PostLanguageSweep(&app);
            return MMS_OK;
        });
        return MMS_OK;
    });
}
bool TryToggleBoundAccessoryFlag(MMDApp* app, unsigned field) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    bool changed = false;
    const auto slot = unsigned(app->SelectedAccessorySlot());
    if (field < 3 && slot < kModelSlotCount) if (const auto* object = app->AccessorySlot(slot)) {
        const auto before = AccessoryPoseChange::Read(*object);
        ApplyBoundAccessoryFlag(*app, slot, field, before.flags[field] == 0, changed);
    }
    // Checkbox appearance may change before WM_COMMAND; rejection restores it.
    if (!changed) RefreshBoundGlobalReadout(*app);
    return true;
}
namespace {
MmsResult ApplyBoundLightFieldImpl(MMDApp& app, unsigned field, float value, bool& changed, HWND slider = nullptr) noexcept {
    changed = false;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history || !Ready(app, nullptr, slider) || !mdl::KeyPoolIdentity(app.LightKeys())) return MMS_E_NOT_READY;
        if (field >= 6 || !std::isfinite(value)) return MMS_E_ARGUMENT;
        if (const auto result = history->CheckNativeEdit()) return result;
        const auto before = LightPoseChange::Read(app);
        for (float source : before) if (!std::isfinite(source)) return MMS_E_ARGUMENT;
        if (before[field] == value) return MMS_OK;
        auto after = before; after[field] = value;
        auto change = std::make_shared<LightPoseChange>(app, before, after, 1u << field);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = history->PrepareNative("Edit light pose component", std::move(change), transaction, SharedHistory::NativeIntent::Adjustment)) return result;
        if (const auto result = history->CommitNative(std::move(transaction))) return result;
        changed = true;
        Boundary([&]() -> MmsResult {
            const bool selectionChanged = SelectTrack(app, curve_host::Kind::Light, 0);
            PublishBoundPoseEdit(app, *history, false);
            RefreshBoundGlobalReadout(app);
            if (selectionChanged && app.CameraMode() && app.MainWindow() && app.Renderer() && app.PanelDC()) PostLanguageSweep(&app);
            return MMS_OK;
        });
        return MMS_OK;
    });
}
}
MmsResult ApplyBoundLightField(MMDApp& app, unsigned field, float value, bool& changed) noexcept {
    return ApplyBoundLightFieldImpl(app, field, value, changed);
}
bool TryCommitBoundLightSlider(MMDApp* app, HWND slider) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    Boundary([&]() -> MmsResult {
        if (!slider || !app->MainWindow()) return MMS_E_ARGUMENT;
        const auto id = GetDlgCtrlID(slider);
        if (id < 455 || id > 460 || slider != GetDlgItem(app->MainWindow(), id)) return MMS_E_ARGUMENT;
        const unsigned field = unsigned(id - 455);
        const auto position = SendMessageW(slider, TBM_GETPOS, 0, 0);
        // Preserve legacy double arithmetic before the authored float cast.
        const float value = field < 3 ? static_cast<float>(double(position) * 0.00390625) :
            static_cast<float>(double(position) / 100.0);
        bool changed = false;
        const auto result = ApplyBoundLightFieldImpl(*app, field, value, changed, slider);
        // Native controls move before notification; rejection restores the
        // authored value while an equal valid tick retains plugin redo.
        if (!changed) RefreshBoundGlobalReadout(*app);
        return result;
    });
    return true;
}
bool TryRegisterBoundCamera(MMDApp* app, HWND window) noexcept {
    return Register(app, curve_host::Kind::Camera, [window](MMDApp& host, std::shared_ptr<HistoryChange>& out) {
        const bool reset = SendMessageW(GetDlgItem(window, panel::kPhysicsFrameCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
        return PlanCameraRegistration(host, host.CurrentFrame(), reset, out);
    });
}
bool TryRegisterBoundLight(MMDApp* app, HWND) noexcept {
    return Register(app, curve_host::Kind::Light, [](MMDApp& host, std::shared_ptr<HistoryChange>& out) {
        return PlanLightRegistration(host, host.CurrentFrame(), out);
    });
}
bool TryRegisterBoundShadow(MMDApp* app, HWND window) noexcept {
    return Register(app, curve_host::Kind::Shadow, [window](MMDApp& host, std::shared_ptr<HistoryChange>& out) {
        double raw = 0;
        if (!ReadLegacyNumber(window, panel::kSelfShadowRangeEdit, raw)) return MMS_E_ARGUMENT;
        const float value = static_cast<float>(raw);
        if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        const int position = static_cast<int>((std::clamp)(value, 0.f, 9999.f));
        const float distance = static_cast<float>((10000.0 - position) / 100000.0);
        return PlanShadowRegistration(host, host.CurrentFrame(), distance, out);
    });
}
bool TryRegisterBoundAccessory(MMDApp* app, HWND window) noexcept {
    return Register(app, curve_host::Kind::Accessory, [window](MMDApp& host, std::shared_ptr<HistoryChange>& out) {
        AccessoryRegistrationInput input;
        for (unsigned index = 0; index < 7; ++index) {
            double value = 0;
            if (!ReadLegacyNumber(window, 478 + int(index), value)) return MMS_E_ARGUMENT;
            if (index >= 3 && index < 6) value = value / 180.0 * 3.141592025756836;
            const float converted = static_cast<float>(value);
            if (!std::isfinite(converted)) return MMS_E_ARGUMENT;
            if (index < 3) input.position[index] = converted;
            else if (index < 6) input.rotation[index - 3] = converted;
            else input.scale = converted;
        }
        return PlanAccessoryRegistration(host, unsigned(host.SelectedAccessorySlot()), input, out);
    });
}
void RefreshBoundGlobalReadout(MMDApp& app) noexcept {
    if (!app.MainWindow()) return;
    Boundary([&]() -> MmsResult {
        bake::ScopedUiInputGate input;
        RefreshProjectDisplayReadout(app);
        RefreshShadowAmbientReadout(app);
        if (app.cameraProjectionRefreshPending && app.Renderer() && app.Renderer()->device) {
            // Consume current-state work at the UI boundary. A zero/tiny
            // orthographic distance must not install infinities on the device.
            float aspect = app.Renderer()->aspectRatio; if (!(aspect > 0)) aspect = 4.0f / 3.0f;
            const bool usableDistance = !app.CameraPerspective() || (std::isfinite(app.CameraDistance()) && app.CameraDistance() != 0);
            const double scale = app.CameraPerspective() && usableDistance ? -2.0 / double(app.CameraDistance()) : 0;
            if (usableDistance && std::isfinite(app.CameraFov()) && (!app.CameraPerspective() ||
                (std::isfinite(scale) && std::fabs(scale) <= (std::numeric_limits<float>::max)() &&
                 std::isfinite(aspect * scale) && std::fabs(aspect * scale) <= (std::numeric_limits<float>::max)()))) ApplyFrameCameraProjection(&app);
            app.cameraProjectionRefreshPending = false;
        }
        if (app.Renderer() && app.Renderer()->device) app.Renderer()->device->SetLight(0, &app.SceneLight());
        const auto perspectiveButton = GetDlgItem(app.MainWindow(), panel::kPerspectiveCheckbox);
        const WPARAM checked = app.CameraPerspective() == 0 ? BST_CHECKED : BST_UNCHECKED;
        if (perspectiveButton && SendMessageW(perspectiveButton, BM_GETCHECK, 0, 0) != checked) {
            SendMessageW(perspectiveButton, BM_SETCHECK, checked, 0);
            InvalidateRect(perspectiveButton, nullptr, FALSE);
        }
        SyncCameraFovControls(&app);
        const double shadowPosition = 10000.0 - double(app.ShadowDistance()) * 100000.0;
        if (std::isfinite(shadowPosition) && shadowPosition >= (std::numeric_limits<int>::min)() &&
            shadowPosition <= (std::numeric_limits<int>::max)()) SyncSelfShadowControls(&app);
        // Synchronize the light's UI from its current value, never from keys.
        for (unsigned axis = 0; axis < 3; ++axis) {
            const double color = double(app.LightColor()[axis]) * 256.0;
            const double direction = app.LightDirection()[axis];
            if (!std::isfinite(color) || !std::isfinite(direction)) continue;
            const auto bounded = [](double value) {
                return static_cast<int>((std::clamp)(value, double((std::numeric_limits<int>::min)()),
                    double((std::numeric_limits<int>::max)())));
            };
            char text[64]{};
            std::snprintf(text, sizeof(text), "%3d", bounded(color));
            const auto colorEdit = GetDlgItem(app.MainWindow(), 461 + int(axis));
            if (!SendMessageW(colorEdit, EM_GETMODIFY, 0, 0))
                ui::SetReadoutTextIfChangedA(colorEdit, text);
            std::snprintf(text, sizeof(text), "%+.1f", direction);
            const auto directionEdit = GetDlgItem(app.MainWindow(), 464 + int(axis));
            if (!SendMessageW(directionEdit, EM_GETMODIFY, 0, 0))
                ui::SetReadoutTextIfChangedA(directionEdit, text);
            ui::SetTrackbarPositionIfChanged(GetDlgItem(app.MainWindow(), 455 + int(axis)), bounded(color));
            const auto directionSlider = GetDlgItem(app.MainWindow(), 458 + int(axis));
            const auto currentTick = SendMessageW(directionSlider, TBM_GETPOS, 0, 0);
            // The authored float may not multiply back to its original integer
            // tick. Keep a matching live position rather than quantizing it.
            if (static_cast<float>(double(currentTick) / 100.0) != app.LightDirection()[axis]) {
                const int nearest=bounded(std::round(direction * 100.0));
                const int restored=static_cast<float>(double(nearest)/100.0)==app.LightDirection()[axis]?
                    nearest:bounded(direction * 100.0);
                ui::SetTrackbarPositionIfChanged(directionSlider, restored);
            }
        }
        if (app.SelectedAccessorySlot() < kModelSlotCount) {
            const auto* object = app.AccessorySlot(app.SelectedAccessorySlot());
            if (object && (object->parentModel < 0 ||
                (unsigned(object->parentModel) < kModelSlotCount && app.ModelSlot(object->parentModel))))
                SyncAccessoryEditPanel(&app);
        }
        RefreshBoundCameraFollowReadout(app);
        return MMS_OK;
    });
}
}
#endif
