#pragma once
#include "build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <mms/plugin.h>
#include <cstdint>
#include <memory>
namespace mikumikustudio::plugins::resource_budget { class Collector; }
#endif
class btRigidBody;
class btCollisionShape;
class btGeneric6DofConstraint;
class btTransform;
namespace mikumikustudio::physics_scale {
// Update physical shapes and constraint frames from native bone scaling.
void Sync(unsigned char *model, bool reseat = false);
bool Follow(const unsigned char *model, int rigid, float matrix[16]);
void Readback(unsigned char *model);
void FinishReadback(unsigned char *model);
void Forget(unsigned char *model);
const btCollisionShape *BaseShape(const btRigidBody *body);
bool CollisionTransform(const btRigidBody *body, float matrix[16]);
void JointBaseline(const btGeneric6DofConstraint &joint, btTransform &a, btTransform &b);
float JointRadiusBaseline(const unsigned char *model, int joint);
#if !MMD_ORIGINAL_PROFILE
// Allocation-free bound for Capture's wrapper, private Impl and control block.
// The active scale node is borrowed; no scale map or native resource is copied.
// Failure preserves bytes. CRT/driver/process allocation metadata is excluded.
MmsResult MeasureResourceStateCapturePreparation(std::uint64_t& bytes) noexcept;
// Own the complete scale-derived state while its native resource version is
// parked. Active states stay in the ordinary lookup table, preserving all old
// physics-scale queries. Switching uses preexisting map nodes only.
class ResourceState : public std::enable_shared_from_this<ResourceState> {
    friend MmsResult MeasureResourceStateCapturePreparation(std::uint64_t&) noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit ResourceState(std::unique_ptr<Impl>);
public:
    class Exchange {
        struct Impl;
        std::unique_ptr<Impl> impl_;
        explicit Exchange(std::unique_ptr<Impl>);
        friend class ResourceState;
    public:
        ~Exchange();
        bool IsCurrent() const noexcept;
        void Publish() noexcept;
    };
    ~ResourceState();
    static std::shared_ptr<ResourceState> Capture(unsigned char* model);
    static std::shared_ptr<ResourceState> Empty(unsigned char* model);
    static std::shared_ptr<ResourceState> TakePrivate(unsigned char* scratch, unsigned char* model);
    // Prepare newly built private Bullet objects using the live scale runtime,
    // including its original authored geometry (not readback compensation).
    static void PreparePrivate(const unsigned char* source, unsigned char* scratch);
    std::unique_ptr<Exchange> PrepareExchange(const std::shared_ptr<ResourceState>& target);
    void Retire() noexcept;
    void MeasureResources(plugins::resource_budget::Collector&) const;
    void MeasureBookkeeping(plugins::resource_budget::Collector&) const;
};
#endif
} // namespace mikumikustudio::physics_scale
