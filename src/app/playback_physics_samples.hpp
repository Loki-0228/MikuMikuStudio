#pragma once

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/affine_skinning.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace mikumikustudio::playback_timing {

// Rolling, completed fixed-step samples. Display evaluation never becomes the
// starting state of the next simulation tick, and never changes Bullet bodies.
class PhysicsSamples {
    template<class T> static void Capture(std::vector<T>& out, const T* data, unsigned count) {
        if (data && count) out.assign(data, data + count);
        else out.clear();
    }
    template<class T> static void Restore(const std::vector<T>& data, T* out) {
        if (out && !data.empty()) std::copy(data.begin(), data.end(), out);
    }
    struct ModelSample {
        int slot = 0;
        mdl::ModelRecord* model = nullptr;
        mdl::BoneRecord* boneTable = nullptr;
        mdl::BoneKey* boneKeys = nullptr;
        std::vector<mdl::BoneRecord> bones;
        std::vector<mdl::MorphRecord> morphs;
        std::vector<mdl::BoneMorphOffsetRecord> boneMorphs;
        std::vector<mdl::IkChain> ik;
        std::vector<mdl::BoneOrderEntry> order;
        std::vector<unsigned> boneCursors, morphCursors;
        std::vector<unsigned char> boneActive, morphActive;
        unsigned displayCursor = 0;
        unsigned char displayActive = 0, visible = 0;

        void CaptureFrom(int index, mdl::ModelRecord& m) {
            slot = index; model = &m; boneTable = m.boneTable; boneKeys = m.boneKeys;
            Capture(bones, m.boneTable, m.boneCount);
            Capture(morphs, m.morphs, m.morphCount);
            Capture(boneMorphs, m.boneMorphTable, m.boneMorphCount);
            Capture(ik, m.ikChains, m.ikChainCount);
            Capture(order, static_cast<mdl::BoneOrderEntry*>(m.boneOrderTable), m.boneOrderCount);
            Capture(boneCursors, m.boneKeyCursors, m.boneCount);
            Capture(morphCursors, m.morphKeyCursors, m.morphCount);
            Capture(boneActive, m.boneTrackActive, m.boneCount);
            Capture(morphActive, m.morphTrackActive, m.morphCount);
            displayCursor = m.displayKeyCursor; displayActive = m.displayTrackActive;
            visible = m.loadComplete;
        }
        bool Matches(MMDApp& app) const {
            const auto* m = mdl::Mdl(app.ModelSlot(slot));
            return m == model && m && m->boneTable == boneTable && m->boneKeys == boneKeys &&
                m->boneCount == bones.size() && m->morphCount == morphs.size() &&
                m->ikChainCount == ik.size() && m->boneOrderCount == order.size();
        }
        void RestoreTo() const {
            auto& m = *model;
            Restore(bones, m.boneTable); Restore(morphs, m.morphs);
            Restore(boneMorphs, m.boneMorphTable); Restore(ik, m.ikChains);
            Restore(order, static_cast<mdl::BoneOrderEntry*>(m.boneOrderTable));
            Restore(boneCursors, m.boneKeyCursors); Restore(morphCursors, m.morphKeyCursors);
            Restore(boneActive, m.boneTrackActive); Restore(morphActive, m.morphTrackActive);
            m.displayKeyCursor = displayCursor; m.displayTrackActive = displayActive; m.loadComplete = visible;
        }
    };
    struct Sample {
        double time = 0;
        std::vector<ModelSample> models;
        void CaptureFrom(MMDApp& app, double seconds) {
            time = seconds;
            std::size_t index = 0;
            for (int slot = 0; slot < kModelSlotCount; ++slot) if (auto* raw = app.ModelSlot(slot)) {
                if (index == models.size()) models.emplace_back();
                models[index++].CaptureFrom(slot, *mdl::Mdl(raw));
            }
            models.resize(index);
        }
        bool Matches(MMDApp& app) const {
            unsigned count = 0;
            for (int slot = 0; slot < kModelSlotCount; ++slot) if (app.ModelSlot(slot)) ++count;
            if (count != models.size()) return false;
            for (const auto& model : models) if (!model.Matches(app)) return false;
            return true;
        }
        void RestoreTo() const { for (const auto& model : models) model.RestoreTo(); }
    } lower_, upper_;
    int rate_ = 0;

    static bool BlendMatrix(float out[16], const float a[16], const float b[16], float alpha) {
        float qa[4], qb[4], q[4], stretchA[9], stretchB[9], linearA[9], linearB[9], rotation[9];
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
            linearA[r * 3 + c] = a[r * 4 + c]; linearB[r * 3 + c] = b[r * 4 + c];
        }
        if (!affine::PolarDecompose(linearA, stretchA, qa) ||
            !affine::PolarDecompose(linearB, stretchB, qb)) {
            return false;
        }
        affine::BlendSdefQuaternions(q, qa, qb, alpha);
        affine::RotationMatrixFromQuaternion(q, rotation);
        std::memcpy(out, a, sizeof(float) * 16);
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
            out[r * 4 + c] = 0;
            for (int k = 0; k < 3; ++k)
                out[r * 4 + c] += (stretchA[r * 3 + k] * (1 - alpha) +
                    stretchB[r * 3 + k] * alpha) * rotation[k * 3 + c];
        }
        for (int c = 0; c < 3; ++c) out[12 + c] = a[12 + c] * (1 - alpha) + b[12 + c] * alpha;
        return true;
    }
public:
    void Initialize(MMDApp& app, double time, int rate) {
        rate_ = rate; upper_.CaptureFrom(app, time); lower_ = upper_;
    }
    bool Matches(MMDApp& app, int rate) const { return rate == rate_ && upper_.Matches(app) && lower_.Matches(app); }
    void RestoreSimulation() const { upper_.RestoreTo(); }
    void Complete(MMDApp& app, double time) {
        std::swap(lower_, upper_); upper_.CaptureFrom(app, time);
    }
    bool Covers(double time) const {
        const double epsilon = 4 * std::numeric_limits<double>::epsilon() * (std::max)(1.0, std::abs(time));
        return time >= lower_.time - epsilon && time <= upper_.time + epsilon;
    }
    bool Apply(MMDApp& app, double time) const {
        if (!Covers(time) || !Matches(app, rate_)) return false;
        if (time >= upper_.time) { upper_.RestoreTo(); return true; }
        lower_.RestoreTo();
        const float alpha = float(std::clamp((time - lower_.time) / (upper_.time - lower_.time), 0.0, 1.0));
        const int mode = app.NativePhysicsMode();
        for (std::size_t m = 0; m < lower_.models.size(); ++m) {
            const auto& a = lower_.models[m]; const auto& b = upper_.models[m];
            for (std::size_t i = 0; i < a.bones.size(); ++i) {
                const auto& from = a.bones[i]; const auto& to = b.bones[i];
                if (!from.hasRigidBody || (mode != 1 && (from.physicsDisabled || to.physicsDisabled))) continue;
                auto& bone = a.model->boneTable[i];
                for (int c = 0; c < 3; ++c)
                    bone.ikBackup[c] = from.ikBackup[c] * (1 - alpha) + to.ikBackup[c] * alpha;
                affine::BlendSdefQuaternions(bone.ikBackup + 3, from.ikBackup + 3, to.ikBackup + 3, alpha);
                if (!BlendMatrix(bone.matExtra, from.matExtra, to.matExtra, alpha)) return false;
            }
        }
        return true;
    }
};
}
