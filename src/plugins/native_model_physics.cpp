#include "native_model_physics.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "model_physics_resources.hpp"
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/owned_text.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/physics_scale.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <btBulletDynamicsCommon.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mikumikustudio::plugins {
namespace {
thread_local const MMDApp* publication = nullptr;
constexpr std::size_t maxRecords = 100000;
bool Ready(MMDApp& app, bool replay) noexcept {
    const bool authorized = replay && (publication == &app || NativeObjectPublicationAuthorized(&app));
    return (!bake::UiLocked() || authorized) && (!app.SceneMutationInProgress() || authorized) &&
        !SceneSaveBlocksEdits() && !NativeObjectUiPending(&app) && !GetCapture() &&
        !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.RecordingWindow() && !app.FrameRangeDialog() &&
        (!app.state.frameCopyDialog || BoundPhysicsModelDraft(&app)) && !app.GravitySettingDialog() &&
        !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen &&
        !NativeModelOutlineDialogBlocks(app) && app.MainWindow() &&
        GetWindowThreadProcessId(app.MainWindow(), nullptr) == GetCurrentThreadId();
}
mdl::RigidRecord Authoring(mdl::RigidRecord r) noexcept {
    r.jpText = r.enText = nullptr; r.body = r.keyData = nullptr;
    std::memset(r.invTransform, 0, sizeof(r.invTransform));
    r.mode = r.staticFlag ? 0 : r.kinematicFlag ? 2 : 1;
    r.staticFlag = r.mode == 0; r.kinematicFlag = r.mode == 2;
    return r;
}
mdl::JointRecord Authoring(mdl::JointRecord r) noexcept {
    r.jpText = r.enText = nullptr; r.constraint = r.constraint < 0 ? -1 : 0; r.radiusBound = 0;
    return r;
}
bool NameValid(const std::vector<wchar_t>& value) noexcept {
    if (value.empty()) return true;
    std::size_t i = 0;
    for (; i < value.size() && value[i]; ++i) {
        const auto c = std::uint16_t(value[i]);
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == value.size() || std::uint16_t(value[i]) < 0xdc00 || std::uint16_t(value[i]) > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return i < value.size();
}
std::vector<wchar_t> Name(const wchar_t* p) {
    if (!p) return {};
    owned_text::Allocation info;
    if (!owned_text::Find(p, info) || info.bytes < sizeof(wchar_t) || info.bytes % sizeof(wchar_t))
        throw std::invalid_argument("unknown physics name allocation");
    std::vector<wchar_t> value(info.bytes / sizeof(wchar_t));
    std::memcpy(value.data(), p, info.bytes);
    if (!NameValid(value)) throw std::invalid_argument("invalid physics name");
    return value;
}
bool NameMatches(const wchar_t* p, const std::vector<wchar_t>& value) noexcept {
    if (!p || value.empty()) return !p && value.empty();
    owned_text::Allocation info;
    return owned_text::Find(p, info) && info.bytes == value.size() * sizeof(wchar_t) &&
        !std::memcmp(p, value.data(), info.bytes);
}
wchar_t* CloneName(const std::vector<wchar_t>& value) {
    if (value.empty()) return nullptr;
    auto* p = static_cast<wchar_t*>(std::malloc(value.size() * sizeof(wchar_t)));
    if (!p) throw std::bad_alloc();
    try { owned_text::Remember(p, value.size() * sizeof(wchar_t)); }
    catch (...) { std::free(p); throw; }
    std::memcpy(p, value.data(), value.size() * sizeof(wchar_t)); return p;
}
ModelPhysicsDraft Capture(const mdl::ModelRecord& model) {
    if (model.rigidCount > maxRecords || model.jointCount > maxRecords ||
        (model.rigidCount && !model.rigidTable) || (model.jointCount && !model.jointTable))
        throw std::invalid_argument("physics table extent");
    ModelPhysicsDraft out;
    for (unsigned i = 0; i < model.rigidCount; ++i) {
        out.rigids.push_back(Authoring(model.rigidTable[i]));
        out.rigidNames.push_back({Name(model.rigidTable[i].jpText), Name(model.rigidTable[i].enText)});
    }
    for (unsigned i = 0; i < model.jointCount; ++i) {
        out.joints.push_back(Authoring(model.jointTable[i]));
        out.jointNames.push_back({Name(model.jointTable[i].jpText), Name(model.jointTable[i].enText)});
    }
    return out;
}
bool Matches(const mdl::ModelRecord& m, const ModelPhysicsDraft& d) noexcept {
    if (m.rigidCount != d.rigids.size() || m.jointCount != d.joints.size() ||
        (m.rigidCount && !m.rigidTable) || (m.jointCount && !m.jointTable)) return false;
    for (std::size_t i = 0; i < d.rigids.size(); ++i) {
        const auto value = Authoring(m.rigidTable[i]);
        if (std::memcmp(&value, &d.rigids[i], sizeof(value)) ||
            !NameMatches(m.rigidTable[i].jpText, d.rigidNames[i][0]) || !NameMatches(m.rigidTable[i].enText, d.rigidNames[i][1])) return false;
    }
    for (std::size_t i = 0; i < d.joints.size(); ++i) {
        const auto value = Authoring(m.jointTable[i]);
        if (std::memcmp(&value, &d.joints[i], sizeof(value)) ||
            !NameMatches(m.jointTable[i].jpText, d.jointNames[i][0]) || !NameMatches(m.jointTable[i].enText, d.jointNames[i][1])) return false;
    }
    return true;
}
bool Equal(const ModelPhysicsDraft& a, const ModelPhysicsDraft& b) noexcept {
    return a.rigids.size() == b.rigids.size() && a.joints.size() == b.joints.size() &&
        (a.rigids.empty() || !std::memcmp(a.rigids.data(), b.rigids.data(), a.rigids.size() * sizeof(mdl::RigidRecord))) &&
        (a.joints.empty() || !std::memcmp(a.joints.data(), b.joints.data(), a.joints.size() * sizeof(mdl::JointRecord))) &&
        a.rigidNames == b.rigidNames && a.jointNames == b.jointNames;
}
void MeasureDraft(const ModelPhysicsDraft& d, resource_budget::Collector& c) {
    c.Vector(d.rigids); c.Vector(d.joints); c.Vector(d.rigidNames); c.Vector(d.jointNames);
    for (const auto* names : {&d.rigidNames, &d.jointNames})
        for (const auto& pair : *names) for (const auto& name : pair) c.Vector(name);
}
template<std::size_t N> bool Finite(const float (&v)[N]) noexcept {
    for (auto n : v) if (!std::isfinite(n)) return false; return true;
}
bool Valid(const ModelPhysicsDraft& d) noexcept {
    if (d.rigids.size() > maxRecords || d.joints.size() > maxRecords ||
        d.rigids.size() != d.rigidNames.size() || d.joints.size() != d.jointNames.size()) return false;
    for (const auto* names : {&d.rigidNames, &d.jointNames})
        for (const auto& pair : *names) for (const auto& name : pair) if (!NameValid(name)) return false;
    for (const auto& r : d.rigids) {
        if (r.mode > 2 || r.group > 15 || r.shape > 2 || !Finite(r.size) || !Finite(r.position) || !Finite(r.rotation) ||
            !std::isfinite(r.mass) || r.mass < 0 || !std::isfinite(r.linearDamping) || !std::isfinite(r.angularDamping) ||
            !std::isfinite(r.restitution) || !std::isfinite(r.friction)) return false;
        for (auto size : r.size) if (size < 0) return false;
    }
    for (const auto& j : d.joints)
        if (j.constraint >= 0 && (j.rigidA < 0 || j.rigidB < 0 || unsigned(j.rigidA) >= d.rigids.size() || unsigned(j.rigidB) >= d.rigids.size() ||
            !Finite(j.position) || !Finite(j.rotation) || !Finite(j.limits) || !Finite(j.springs))) return false;
    return true;
}
struct BoneSource { std::array<float, 3> position; std::array<float, 16> initial; };
struct PrivatePhysics {
    std::shared_ptr<history_physics::ImportScene> scene;
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones;
    PrivatePhysics(PhysicsScene& live, const mdl::ModelRecord& source) : scene(std::make_shared<history_physics::ImportScene>(live)) {
        if (source.boneCount) bones.assign(source.boneTable, source.boneTable + source.boneCount);
        model->scenePtr = &scene->Scene(); model->boneCount = source.boneCount;
        model->boneTable = bones.empty() ? nullptr : bones.data(); model->physicsMode = source.physicsMode;
    }
    ~PrivatePhysics() {
        auto& m = *model; auto* world = scene->Scene().world;
        auto* raw = reinterpret_cast<unsigned char*>(model.get());
        physics_scale::Forget(raw); bone_scale::Forget(raw);
        // AdoptPrivate empties both tables only on success. In that case its
        // version also owns this private world and every native allocation.
        if (!m.rigidTable && !m.jointTable) return;
        for (int i = world->getNumConstraints() - 1; i >= 0; --i) {
            auto* joint = world->getConstraint(i); world->removeConstraint(joint); delete joint;
        }
        for (unsigned i = 0; i < m.rigidCount; ++i) {
            auto& r = m.rigidTable[i];
            if (auto* body = static_cast<btRigidBody*>(r.body)) {
                world->removeRigidBody(body); delete body->getMotionState(); delete body->getCollisionShape(); delete body;
            }
            owned_text::Free(r.jpText); owned_text::Free(r.enText);
        }
        for (unsigned i = 0; i < m.jointCount; ++i) { owned_text::Free(m.jointTable[i].jpText); owned_text::Free(m.jointTable[i].enText); }
        std::free(m.rigidTable); std::free(m.jointTable);
    }
};
void TransformPoint(float out[3], const float point[3], const float matrix[16]) noexcept {
    for (unsigned col = 0; col < 3; ++col)
        out[col] = point[0] * matrix[col] + point[1] * matrix[4 + col] + point[2] * matrix[8 + col] + matrix[12 + col];
}
void Build(PrivatePhysics& p, const ModelPhysicsDraft& draft, const std::vector<BoneSource>& bones) {
    auto& m = *p.model; auto& dx = d3dx::Get(); using Mat = d3dx::D3DXMATRIXF;
    if (!draft.rigids.empty()) {
        m.rigidTable = static_cast<mdl::RigidRecord*>(std::calloc(draft.rigids.size(), sizeof(mdl::RigidRecord)));
        if (!m.rigidTable) throw std::bad_alloc();
    }
    m.rigidCount = unsigned(draft.rigids.size());
    if (!draft.joints.empty()) {
        m.jointTable = static_cast<mdl::JointRecord*>(std::calloc(draft.joints.size(), sizeof(mdl::JointRecord)));
        if (!m.jointTable) throw std::bad_alloc();
    }
    m.jointCount = unsigned(draft.joints.size());
    BoneSource identity{}; identity.initial[0] = identity.initial[5] = identity.initial[10] = identity.initial[15] = 1;
    for (unsigned i = 0; i < m.rigidCount; ++i) {
        auto& r = m.rigidTable[i]; r = draft.rigids[i]; r.jpText = r.enText = nullptr; r.body = r.keyData = nullptr;
        r.jpText = CloneName(draft.rigidNames[i][0]); r.enText = CloneName(draft.rigidNames[i][1]);
        const bool linked = r.boneIndex >= 0 && unsigned(r.boneIndex) < bones.size();
        const auto& bone = linked ? bones[unsigned(r.boneIndex)] : bones.empty() ? identity : bones[0];
        Mat matrix, temp, initial;
        dx.rotZ(&matrix, r.rotation[2]); dx.rotX(&temp, r.rotation[0]); dx.multiply(&matrix, &matrix, &temp);
        dx.rotY(&temp, r.rotation[1]); dx.multiply(&matrix, &matrix, &temp);
        dx.translation(&temp, r.position[0], r.position[1], r.position[2]); dx.multiply(&matrix, &matrix, &temp);
        dx.translation(&temp, bone.position[0], bone.position[1], bone.position[2]); dx.multiply(&matrix, &matrix, &temp);
        std::memcpy(&initial, bone.initial.data(), sizeof(initial)); dx.multiply(&matrix, &matrix, &initial);
        void* result[2]{};
        CreateRigidBody(&p.scene->Scene(), result, r.shape, r.size[0], r.size[1], r.size[2], &matrix.m[0][0],
            r.mode, r.mass, r.linearDamping, r.angularDamping, r.restitution, r.friction, char(r.group), r.noCollapse);
        r.keyData = result[0]; r.body = result[1];
        if (!r.body) throw std::invalid_argument("physics draft body could not be constructed");
        Mat inverse;
        dx.translation(&inverse, -bone.position[0], -bone.position[1], -bone.position[2]);
        dx.translation(&temp, -r.position[0], -r.position[1], -r.position[2]); dx.multiply(&inverse, &inverse, &temp);
        dx.rotY(&temp, -r.rotation[1]); dx.multiply(&inverse, &inverse, &temp);
        dx.rotX(&temp, -r.rotation[0]); dx.multiply(&inverse, &inverse, &temp);
        dx.rotZ(&temp, -r.rotation[2]); dx.multiply(&inverse, &inverse, &temp);
        std::memcpy(r.invTransform, &inverse, sizeof(inverse));
    }
    for (unsigned i = 0; i < m.jointCount; ++i) {
        auto& j = m.jointTable[i]; j = draft.joints[i]; j.jpText = j.enText = nullptr;
        j.jpText = CloneName(draft.jointNames[i][0]); j.enText = CloneName(draft.jointNames[i][1]);
        if (j.constraint < 0) continue;
        const auto& a = m.rigidTable[j.rigidA]; const auto& b = m.rigidTable[j.rigidB];
        float pivot[2][3], quat[2][4], radius2 = 0; TransformPoint(pivot[0], j.position, a.invTransform); TransformPoint(pivot[1], j.position, b.invTransform);
        j.radiusBound = 0;
        for (const auto& point : pivot) j.radiusBound += std::sqrt(point[0]*point[0] + point[1]*point[1] + point[2]*point[2]);
        for (unsigned axis = 0; axis < 3; ++axis) { const auto limit = std::max(std::fabs(j.limits[axis]), std::fabs(j.limits[axis + 3])); radius2 += limit * limit; }
        j.radiusBound += std::sqrt(radius2);
        for (unsigned side = 0; side < 2; ++side) {
            const auto& r = side ? b : a; Mat rotation, temp;
            dx.rotZ(&rotation, j.rotation[2]); dx.rotX(&temp, j.rotation[0]); dx.multiply(&rotation, &rotation, &temp);
            dx.rotY(&temp, j.rotation[1]); dx.multiply(&rotation, &rotation, &temp);
            dx.rotY(&temp, -r.rotation[1]); dx.multiply(&rotation, &rotation, &temp);
            dx.rotX(&temp, -r.rotation[0]); dx.multiply(&rotation, &rotation, &temp);
            dx.rotZ(&temp, -r.rotation[2]); dx.multiply(&rotation, &rotation, &temp); dx.quatFromMatrix(quat[side], &rotation);
        }
        j.constraint = CreatePhysJoint(&p.scene->Scene(), a.body, b.body,
            pivot[0][0], pivot[0][1], pivot[0][2], quat[0][0], quat[0][1], quat[0][2], quat[0][3],
            pivot[1][0], pivot[1][1], pivot[1][2], quat[1][0], quat[1][1], quat[1][2], quat[1][3],
            j.limits[0],j.limits[1],j.limits[2],j.limits[3],j.limits[4],j.limits[5],
            j.limits[6],j.limits[7],j.limits[8],j.limits[9],j.limits[10],j.limits[11],
            j.springs[0],j.springs[1],j.springs[2],j.springs[3],j.springs[4],j.springs[5]);
    }
}
struct PublicationScope {
    const MMDApp* before;
    explicit PublicationScope(MMDApp& app) : before(publication) { publication = &app; }
    ~PublicationScope() { publication = before; }
};
}

struct NativeModelPhysicsPlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    MMDApp& app; SharedHistory* const history; const unsigned slot;
    unsigned char* const object; const std::uint64_t scene, binding, revision;
    const MmsObjectRef initial;
    std::shared_ptr<RetainedSceneObject> owner;
    std::shared_ptr<ModelPhysicsResources> resources;
    std::shared_ptr<ModelPhysicsVersion> before, after;
    mdl::BoneRecord* boneTable = nullptr;
    std::vector<BoneSource> bones;
    bone_scale::Values initialPose;
    std::vector<unsigned char> beforeFlags, afterFlags;
    ModelPhysicsDraft beforeDraft, afterDraft;
    std::uint64_t serial = 1, fixedBytes = 0;
    bool staged = false, finished = false, published = false, applied = false;
    State(MMDApp& a, SharedHistory* h, unsigned s, std::uint64_t r) : app(a), history(h), slot(s), object(a.ModelSlot(s)),
        scene(a.SceneGeneration()), binding(BoundNativePoseGeneration(&a)), revision(r), initial(SceneObjectIdentity(object)) {}
    bool Source(bool forward, bool first) const noexcept {
        if (BoundNativePoseHistory(&app) != history || app.SceneGeneration() != scene || app.ModelSlot(slot) != object ||
            !owner || !owner->Present() || !SceneObjectPresent(object, initial.id) || !resources ||
            !resources->IsCurrent(forward ? before : after) ||
            (first && (BoundNativePoseGeneration(&app) != binding || SceneObjectIdentity(object).generation != initial.generation))) return false;
        if (first && !bone_scale::PoseValuesEqual(object, initialPose)) return false;
        const auto& m = *mdl::Mdl(object); const auto& flags = forward ? beforeFlags : afterFlags;
        if (m.boneTable != boneTable || m.boneCount != bones.size() || !Matches(m, forward ? beforeDraft : afterDraft)) return false;
        for (std::size_t i = 0; i < bones.size(); ++i)
            if (std::memcmp(boneTable[i].position, bones[i].position.data(), sizeof(boneTable[i].position)) ||
                (first && std::memcmp(boneTable[i].matInit, bones[i].initial.data(), sizeof(boneTable[i].matInit))) ||
                boneTable[i].hasRigidBody != flags[i]) return false;
        return true;
    }
    bool InitialCurrent() const noexcept {
        if (!Source(true, true)) return false;
        if (!history) return true;
        std::uint64_t now = 0; return !history->Revision(now) && now == revision;
    }
    void Measure(resource_budget::Collector& c) const {
        c.Add(this, sizeof(State) + 4*sizeof(void*), 1); c.Vector(bones); c.Vector(beforeFlags); c.Vector(afterFlags);
        c.Nodes(initialPose);
        MeasureDraft(beforeDraft, c); MeasureDraft(afterDraft, c);
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> state;
        std::unique_ptr<PreparedHistoryChange> physics;
        mutable resource_budget::Collector extra;
        const bool forward, first;
        const std::uint64_t serial, binding, generation;
        std::uint64_t bytes = 0;
        bool consumed = false;
        Candidate(std::shared_ptr<State> s, bool f) : state(std::move(s)), forward(f), first(!state->published), serial(state->serial),
            binding(BoundNativePoseGeneration(&state->app)), generation(SceneObjectIdentity(state->object).generation) {}
        bool Metadata() const noexcept {
            return !consumed && state->serial == serial && state->applied != forward && state->Source(forward, first) &&
                BoundNativePoseGeneration(&state->app) == binding && SceneObjectIdentity(state->object).generation == generation && Ready(state->app, true);
        }
        bool IsCurrent() const noexcept override {
            return Metadata() && physics && physics->IsCurrent() && extra.Covers([&](auto& c) { state->Measure(c); }) &&
                state->owner->CoversBookkeeping(extra) && Metadata();
        }
        void Publish() noexcept override {
            if (!Metadata() || !physics->IsCurrent()) std::terminate();
            physics->Publish();
            const auto& flags = forward ? state->afterFlags : state->beforeFlags;
            for (std::size_t i = 0; i < flags.size(); ++i) state->boneTable[i].hasRigidBody = flags[i];
            state->app.EnhancedModelDirty() = 1;
            state->applied = forward; state->published = true; ++state->serial; consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!staged || !finished || applied == forward || !Source(forward, !published) || (!published && !InitialCurrent())) return MMS_E_STALE;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            if (!Ready(app, true)) return MMS_E_BUSY;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward);
            next->physics = resources->PrepareExchange(forward ? before : after, forward ? after : before);
            if (!next->physics) return MMS_E_NOT_READY;
            Measure(next->extra); if (auto r = owner->MeasureBookkeeping(next->extra)) return r;
            next->extra.Add(next.get(), sizeof(Candidate) + 4*sizeof(void*), 1);
            std::uint64_t nativeBytes = 0;
            if (!next->extra.Result().Complete() || !next->physics->TargetRetainedBytes(nativeBytes)) return MMS_E_NOT_READY;
            next->bytes = resource_budget::Collector::Sum(nativeBytes, next->extra.Result().totalBytes);
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return fixedBytes; }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};

MmsResult NativeModelPhysicsPlan::Begin(MMDApp& app, unsigned slot) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        if (slot >= kModelSlotCount || !app.ModelSlot(slot)) return MMS_E_ARGUMENT;
        if (!Ready(app, false)) return MMS_E_BUSY;
        auto* history = BoundNativePoseHistory(&app); std::uint64_t revision = 0;
        if (history) { if (auto r = history->CheckNativeEdit()) return r; if (auto r = history->Revision(revision)) return r; }
        auto s = std::make_shared<State>(app, history, slot, revision);
        s->owner = RetainSceneObject(app, MMS_OBJECT_MODEL, slot);
        s->resources = ModelPhysicsResources::Capture(s->object); s->before = s->resources->Current();
        s->initialPose = bone_scale::PoseValues(s->object);
        const auto& m = *mdl::Mdl(s->object); s->boneTable = m.boneTable;
        if (m.boneCount && !m.boneTable) return MMS_E_STALE;
        for (unsigned i = 0; i < m.boneCount; ++i) {
            BoneSource b; std::memcpy(b.position.data(), m.boneTable[i].position, sizeof(m.boneTable[i].position));
            std::memcpy(b.initial.data(), m.boneTable[i].matInit, sizeof(m.boneTable[i].matInit));
            for (auto n : b.position) if (!std::isfinite(n)) return MMS_E_ARGUMENT;
            for (auto n : b.initial) if (!std::isfinite(n)) return MMS_E_ARGUMENT;
            s->bones.push_back(b); s->beforeFlags.push_back(m.boneTable[i].hasRigidBody);
        }
        s->beforeDraft = Capture(m); s->afterDraft = s->beforeDraft; s->afterFlags = s->beforeFlags;
        if (!s->InitialCurrent()) return MMS_E_STALE;
        state_ = std::move(s); return MMS_OK;
    });
}
bool NativeModelPhysicsPlan::IsCurrent() const noexcept { return state_ && !state_->finished && state_->InitialCurrent(); }
MmsResult NativeModelPhysicsPlan::Read(ModelPhysicsDraft& out) const noexcept {
    if (!state_) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        if (!IsCurrent()) return MMS_E_STALE;
        auto value = state_->afterDraft; out = std::move(value); return MMS_OK;
    });
}
MmsResult NativeModelPhysicsPlan::Set(const ModelPhysicsDraft& draft) noexcept {
    if (!state_ || state_->finished) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto& s = *state_; if (!s.InitialCurrent()) return MMS_E_STALE;
        if (!Ready(s.app, false)) return MMS_E_BUSY;
        if (!Valid(draft)) return MMS_E_ARGUMENT;
        auto value = draft;
        for (auto& r : value.rigids) {
            // The mode is the editor's authoritative field; derive both flags
            // before removing runtime identity from the private record.
            r.staticFlag = r.mode == 0; r.kinematicFlag = r.mode == 2; r = Authoring(r);
        }
        for (auto& j : value.joints) j = Authoring(j);
        std::vector<unsigned char> flags(s.bones.size(), 0);
        for (const auto& r : value.rigids) if (r.mode && r.boneIndex >= 0 && unsigned(r.boneIndex) < flags.size()) flags[unsigned(r.boneIndex)] = 1;
        if (!s.InitialCurrent()) return MMS_E_STALE;
        s.afterDraft = std::move(value); s.afterFlags = std::move(flags); s.staged = true; return MMS_OK;
    });
}
MmsResult NativeModelPhysicsPlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    if (!state_ || !state_->staged || state_->finished) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto& s = *state_; if (!s.InitialCurrent()) return MMS_E_STALE;
        if (!Ready(s.app, false)) return MMS_E_BUSY;
        if (Equal(s.beforeDraft, s.afterDraft)) { state_.reset(); return MMS_OK; }
        if (!d3dx::Get().Load()) return MMS_E_NOT_READY;
        auto* live = mdl::Mdl(s.object)->scenePtr; if (!live || !live->world) return MMS_E_NOT_READY;
        PrivatePhysics prepared(*live, *mdl::Mdl(s.object)); Build(prepared, s.afterDraft, s.bones);
        physics_scale::ResourceState::PreparePrivate(s.object, reinterpret_cast<unsigned char*>(prepared.model.get()));
        if (!s.InitialCurrent()) return MMS_E_STALE;
        auto after = s.resources->AdoptPrivate(*prepared.model, prepared.scene);
        if (!after || !s.InitialCurrent()) return MMS_E_STALE;
        resource_budget::Collector cost; s.Measure(cost); after->MeasureResources(cost); s.before->MeasureResources(cost);
        if (!cost.Result().Complete()) return MMS_E_NOT_READY;
        s.after = std::move(after); s.fixedBytes = cost.Result().totalBytes; s.finished = true;
        out = std::move(state_); return MMS_OK;
    });
}
MmsResult CommitBoundModelPhysics(MMDApp& app, SharedHistory& history, std::shared_ptr<HistoryChange> change) noexcept {
    if (!change) return MMS_OK;
    return Boundary([&]() -> MmsResult {
        if (BoundNativePoseHistory(&app) != &history || BoundPhysicsModelDraft(&app) || app.state.frameCopyDialog) return MMS_E_STALE;
        { PublicationScope authorized(app); bake::ScopedUiInputGate gate;
            std::unique_ptr<SharedHistory::NativeTransaction> token;
            if (auto r = history.PrepareNative("Model physics resources", std::move(change), token, SharedHistory::NativeIntent::ExplicitCommit)) {
                runtime_log::Trace("PLUGIN_ABI physics publication stage=prepare-history result=%d", r); return r;
            }
            if (auto r = history.CommitNative(std::move(token))) {
                runtime_log::Trace("PLUGIN_ABI physics publication stage=commit-history result=%d", r); return r;
            }
        }
        PublishBoundPoseEdit(app, history, false); RefreshBoundObjectUi(app, history); return MMS_OK;
    });
}
MmsResult CommitUnboundModelPhysics(MMDApp& app, std::shared_ptr<HistoryChange> change) noexcept {
    if (!change) return MMS_OK;
    return Boundary([&]() -> MmsResult {
        if (BoundNativePoseHistory(&app) || BoundPhysicsModelDraft(&app) || app.state.frameCopyDialog) return MMS_E_STALE;
        PublicationScope authorized(app); bake::ScopedUiInputGate gate;
        std::unique_ptr<PreparedHistoryChange> next;
        if (auto r = change->Prepare(true, next)) return r;
        if (!next || !next->IsCurrent()) return MMS_E_STALE;
        next->Publish(); next.reset(); app.SceneModified() = 1; return MMS_OK;
    });
}
}
#endif
