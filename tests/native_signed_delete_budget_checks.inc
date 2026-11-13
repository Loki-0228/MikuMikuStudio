// Internal budget integration only; command admission still requires the
// production coordinator and its separate menu/keyboard regression.
namespace signed_delete_budget_checks {
using namespace negative_global_delete_target_checks;
void SharedPreparationAllowance() {
    negative_global_delete_target_checks::Fixture f;SeedMixed(f,true);
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    auto positive=PlanPositive(f);auto negative=PlanNegative(f,true);
    for(unsigned step=0;step<3;++step) {
        const bool forward=step!=1,initial=step==0;
        const Snapshot before(f);
        std::unique_ptr<PositiveCandidate> p;
        std::unique_ptr<Candidate> n;
        std::uint64_t remaining=preparationBudget;
        Check(positive->PrepareSignedBudgeted(forward,remaining,p)==MMS_OK&&p,
            "measure positive production candidate against a shared allowance");
        const auto positiveBytes=preparationBudget-remaining;
        const auto negativeStart=remaining;
        Check(PrepareNegative(negative,forward,initial,*p,remaining,n)==MMS_OK&&n,
            "negative target consumer accepts only the positive candidate remainder");
        const auto negativeBytes=negativeStart-remaining;
        Check(positiveBytes&&negativeBytes,"both signed participants consume preparation storage");
        n.reset();p.reset();Check(before.Same(f),"measuring the combined allowance leaves all source state intact");

        const auto total=positiveBytes+negativeBytes;
        remaining=total-1;
        Check(positive->PrepareSignedBudgeted(forward,remaining,p)==MMS_OK&&p&&remaining==negativeBytes-1,
            "positive half cannot reserve the negative participant's final byte");
        const auto beforeFailure=remaining;
        Check(PrepareNegative(negative,forward,initial,*p,remaining,n)==MMS_E_MEMORY&&!n&&remaining==beforeFailure&&p->IsCurrent(),
            "combined one-byte-short admission rejects the entire negative half without consuming positive source");
        p.reset();Check(before.Same(f),"cancelling the surviving positive candidate after a budget failure preserves both halves");

        remaining=total;
        Check(positive->PrepareSignedBudgeted(forward,remaining,p)==MMS_OK&&p&&
            PrepareNegative(negative,forward,initial,*p,remaining,n)==MMS_OK&&n&&!remaining,
            "one exact allowance prepares both signed halves in each direction");
        Check(before.Same(f),"joint budget admission is still detached before publication");
        PublishTogether(*p,*n);
    }
}
}
namespace native_delete_owner_budget_checks {
void OwnerSizing() {
    auto app=std::make_unique<MMDApp>();::new(static_cast<void*>(&app->state)) MMDAppState{};
    auto model=std::make_unique<mdl::ModelRecord>();
    auto* raw=reinterpret_cast<unsigned char*>(model.get());app->ModelSlot(0)=raw;
    std::uint64_t plain=0;failAfter=0;
    const auto plainResult=plugins::MeasureGlobalDeletionParentPreparation(*app,0,plain);failAfter=-1;
    MmsObjectRef identity{};
    Check(plainResult==MMS_E_NOT_READY&&!plain&&!plugins::FindSceneObjectIdentity(raw,identity),
        "candidate preparation refuses an unregistered object before capture can leave a permanent identity");
    plugins::SceneObjectIdentity(raw);
    failAfter=0;
    const auto registeredResult=plugins::MeasureGlobalDeletionParentPreparation(*app,0,plain);failAfter=-1;
    Check(registeredResult==MMS_OK&&plain>0&&plugins::FindSceneObjectIdentity(raw,identity),
        "fresh holder sizing for an admitted object allocates nothing and preserves identity");
    btDefaultCollisionConfiguration config;btCollisionDispatcher dispatcher(&config);
    history_physics::Broadphase broadphase(btVector3(-100,-100,-100),btVector3(100,100,100),64);
    btSequentialImpulseConstraintSolver solver;DynamicsWorld world(&dispatcher,&broadphase,&solver,&config);
    PhysicsScene scene{};scene.world=&world;
    btSphereShape shape(btScalar(.5));btDefaultMotionState motion(btTransform::getIdentity());
    btRigidBody body(1,&motion,&shape);world.addRigidBody(&body,1,-1);
    mdl::RigidRecord rigid{};rigid.body=&body;
    model->scenePtr=&scene;model->rigidTable=&rigid;model->rigidCount=1;
    const auto objects=world.getNumCollisionObjects();const auto transform=body.getWorldTransform();
    std::uint64_t physical=0;failAfter=0;
    const auto physicalResult=plugins::MeasureGlobalDeletionParentPreparation(*app,0,physical);failAfter=-1;
    MmsObjectRef after{};
    Check(physicalResult==MMS_OK&&physical>plain&&plugins::FindSceneObjectIdentity(raw,after)&&
        plugins::Same(identity,after)&&
        world.getNumCollisionObjects()==objects&&body.getWorldTransform()==transform&&rigid.body==&body,
        "fresh physics holder bound uses actual body bookkeeping without changing the world or identity");
    auto owner=plugins::RetainSceneObject(*app,MMS_OBJECT_MODEL,0);
    std::uint64_t reused=1234;failAfter=0;
    const auto reuseResult=plugins::MeasureGlobalDeletionParentPreparation(*app,0,reused);
    const auto modelResult=plugins::ModelDeletionOwnerPreparationBytes(*app,0);failAfter=-1;
    Check(reuseResult==MMS_OK&&!reused&&!modelResult&&owner->Present(),
        "global and model deletion share the existing holder with zero new capture charge");
    std::uint64_t rejected=1234;failAfter=0;
    const auto badSlot=plugins::MeasureGlobalDeletionParentPreparation(*app,kModelSlotCount,rejected);
    const auto overflow=history_physics::MeasureRetainedCapturePreparation(SIZE_MAX,SIZE_MAX,rejected);failAfter=-1;
    Check(badSlot==MMS_E_ARGUMENT&&overflow==MMS_E_MEMORY&&rejected==1234,
        "invalid and overflowing capture measurements preserve output without allocating");
    owner.reset();world.removeRigidBody(&body);plugins::ForgetSceneObject(raw);app->ModelSlot(0)=nullptr;
    model->scenePtr=nullptr;model->rigidTable=nullptr;model->rigidCount=0;
}
}
void NativeSignedDeleteBudgetChecks() {
    native_delete_owner_budget_checks::OwnerSizing();
    signed_delete_budget_checks::SharedPreparationAllowance();
}
