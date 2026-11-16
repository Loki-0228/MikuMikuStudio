#pragma once

#include "BulletDynamics/Dynamics/btDiscreteDynamicsWorld.h"
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "BulletCollision/CollisionDispatch/btCollisionDispatcher.h"
#include "BulletCollision/CollisionShapes/btBoxShape.h"
#include "BulletSoftBody/btSoftRigidDynamicsWorld.h"
#include <map>
#include <vector>
#include "mikumikustudio/physics_bake_settings.hpp"
#endif
#include <cstdint>

namespace mikumikustudio {

// The diagnostics use the same accumulator as Bullet's stepSimulation.
// Access the protected member through its owning class, independent of ABI.
#if MMD_ORIGINAL_PROFILE
using DynamicsWorldBase = btDiscreteDynamicsWorld;
#else
using DynamicsWorldBase = btSoftRigidDynamicsWorld;

class SoftGroundDispatcher final : public btCollisionDispatcher {
public:
    using btCollisionDispatcher::btCollisionDispatcher;
    void SetGround(const btRigidBody* ground) noexcept { ground_ = ground; }
    const btRigidBody* Ground() const noexcept { return ground_; }

    bool needsCollision(btCollisionObject* first, btCollisionObject* second) override {
        if ((first == ground_ && second->getInternalType() == btCollisionObject::CO_SOFT_BODY) ||
            (second == ground_ && first->getInternalType() == btCollisionObject::CO_SOFT_BODY))
            return false;
        return btCollisionDispatcher::needsCollision(first, second);
    }
private:
    const btRigidBody* ground_ = nullptr;
};
#endif
class DynamicsWorld final : public DynamicsWorldBase {
public:
    using DynamicsWorldBase::DynamicsWorldBase;
    btScalar LocalTime() const { return m_localTime; }
    void ResetLocalTime() { m_localTime = btScalar(0); }
#if !MMD_ORIGINAL_PROFILE
    using StepHook = void (*)(btDiscreteDynamicsWorld&);
    inline static StepHook beforeStep = nullptr;
    using SoftBodySolver = bool (*)(btSoftBody&);
    inline static SoftBodySolver solveSoftBody = nullptr;
    using ClusterSolver = bool (*)(btAlignedObjectArray<btSoftBody*>&);
    inline static ClusterSolver solveClusters = nullptr;
    void SetRateCompensation(int rate) noexcept {
        softStepRatio_ = rate > 0 ? 60.0 / rate : 1.0;
    }
    void SetGround(btRigidBody* ground, SoftGroundDispatcher& dispatcher) noexcept {
        softGroundDispatcher_ = &dispatcher;
        dispatcher.SetGround(ground);
    }
    void removeRigidBody(btRigidBody* body) override {
        if (softGroundDispatcher_ && softGroundDispatcher_->Ground() == body)
            softGroundDispatcher_->SetGround(nullptr);
        DynamicsWorldBase::removeRigidBody(body);
    }
    void removeSoftBody(btSoftBody* body) {
        const auto contact = softGround_.find(body);
        if (contact != softGround_.end()) {
            getWorldInfo().m_sparsesdf.RemoveReferences(&contact->second.shape);
            softGround_.erase(contact);
        }
        DynamicsWorldBase::removeSoftBody(body);
    }
    void removeCollisionObject(btCollisionObject* object) override {
        if (auto* body = btSoftBody::upcast(object)) removeSoftBody(body);
        else DynamicsWorldBase::removeCollisionObject(object);
    }
    // History reserves the existing arrays during prepare, then replaces
    // pointer entries without destroying/recreating Bullet bodies or proxies.
    btAlignedObjectArray<btRigidBody*>& HistoryBodies() noexcept { return m_nonStaticRigidBodies; }
    btAlignedObjectArray<btTypedConstraint*>& HistoryConstraints() noexcept { return m_constraints; }
    bool HasActions() const noexcept { return m_actions.size() != 0; }
    std::uint64_t StepSerial() const noexcept { return stepSerial_; }
    int stepSimulation(btScalar dt, int steps = 1,
                       btScalar fixed = btScalar(1.) / btScalar(60.)) override {
        ++stepSerial_;
        getWorldInfo().m_gravity = getGravity();
        return DynamicsWorldBase::stepSimulation(dt, steps, fixed);
    }
    void performDiscreteCollisionDetection() override {
        ++stepSerial_;
        btDiscreteDynamicsWorld::performDiscreteCollisionDetection();
    }
private:
    static constexpr btScalar kGroundHalfSpan = 64;
    static constexpr btScalar kGroundHalfDepth = 10;
    struct SoftGroundContact {
        btBoxShape shape{btVector3(kGroundHalfSpan, kGroundHalfDepth, kGroundHalfSpan)};
        btCollisionObject object;

        SoftGroundContact() {
            object.setCollisionShape(&shape);
            object.setCollisionFlags(btCollisionObject::CF_STATIC_OBJECT);
        }
    };
    void AddSoftGroundContacts(btAlignedObjectArray<btSoftBody*>& bodies) {
        const auto* ground = softGroundDispatcher_ ? softGroundDispatcher_->Ground() : nullptr;
        if (!bodies.size() || !ground || !ground->hasContactResponse()) return;
        const auto floor = ground->getWorldTransform();
        const auto inverse = floor.inverse();
        // The floor is a scene setting. The PMX mask still filters every
        // registered model pair, including model rigid bodies in group 16.
        for (int i = 0; i < bodies.size(); ++i) {
            auto* body = bodies[i];
            if (!body->isActive()) continue;
            btVector3 low, high;
            body->getAabb(low, high);
            const auto center = inverse * ((low + high) * btScalar(.5));
            const auto halfSpan = inverse.getBasis().absolute() * ((high - low) * btScalar(.5));
            const auto margin = body->getCollisionShape()->getMargin();
            if (center.y() - halfSpan.y() > margin + btScalar(.1)) continue;

            // Keep the support shape local to the soft body. A world-sized
            // box makes Bullet 2.75's distance calculation lose precision.
            auto& contact = softGround_.try_emplace(body).first->second;
            const auto scale = contact.shape.getLocalScaling();
            const btVector3 required(
                btMax(scale.x(), (halfSpan.x() + margin + 4) / kGroundHalfSpan),
                1,
                btMax(scale.z(), (halfSpan.z() + margin + 4) / kGroundHalfSpan));
            if (required != scale) {
                getWorldInfo().m_sparsesdf.RemoveReferences(&contact.shape);
                contact.shape.setLocalScaling(required);
            }
            auto transform = floor;
            transform.setOrigin(floor * btVector3(center.x(), -kGroundHalfDepth, center.z()));
            contact.object.setWorldTransform(transform);
            contact.object.setInterpolationWorldTransform(transform);
            contact.object.setFriction(ground->getFriction());
            contact.object.setRestitution(ground->getRestitution());
            body->defaultCollisionHandler(&contact.object);
        }
    }
    void internalSingleStepSimulation(btScalar dt) override {
        if (beforeStep) beforeStep(*this);
        // PMX soft damping is applied once per step; rigid-body damping already
        // uses pow(1-damping, dt) in Bullet and must not be converted twice.
        auto& soft = getSoftBodyArray();
        const bool compensate = softStepRatio_ != 1.0;
        if (compensate) {
            savedDamping_.resize(soft.size());
            for (int i = 0; i < soft.size(); ++i) {
                savedDamping_[i] = soft[i]->m_cfg.kDP;
                soft[i]->m_cfg.kDP = bake::PerStepFraction(soft[i]->m_cfg.kDP, softStepRatio_);
            }
        }
        struct Restore {
            btAlignedObjectArray<btSoftBody*>& bodies;
            const std::vector<btScalar>& damping;
            bool active;
            ~Restore() { if (active) for (int i = 0; i < bodies.size(); ++i) bodies[i]->m_cfg.kDP = damping[i]; }
        } restore{soft, savedDamping_, compensate};
        if (!solveSoftBody && (!softGroundDispatcher_ || !softGroundDispatcher_->Ground())) {
            DynamicsWorldBase::internalSingleStepSimulation(dt);
            return;
        }
        // Keep Bullet's prediction, collision and cluster stages in the same
        // order. Only an accepted body's constraint iterations move to compute.
        btDiscreteDynamicsWorld::internalSingleStepSimulation(dt);
        auto& bodies=getSoftBodyArray();
        AddSoftGroundContacts(bodies);
        if(bodies.size() && (!solveClusters || !solveClusters(bodies)))btSoftBody::solveClusters(bodies);
        for(int i=0;i<bodies.size();++i)
            if(!solveSoftBody || !solveSoftBody(*bodies[i]))bodies[i]->solveConstraints();
        for(int i=0;i<bodies.size();++i)bodies[i]->defaultCollisionHandler(bodies[i]);
        updateSoftBodies();
    }
    std::uint64_t stepSerial_ = 0;
    std::map<btSoftBody*, SoftGroundContact> softGround_;
    SoftGroundDispatcher* softGroundDispatcher_ = nullptr;
    double softStepRatio_ = 1.0;
    std::vector<btScalar> savedDamping_;
#endif
};

}  // namespace mikumikustudio
