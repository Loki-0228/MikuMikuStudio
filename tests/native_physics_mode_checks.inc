// Include after history_physics_checks.inc and native_camera_multiply_checks.inc.
// Uses real CPU Bullet worlds, native mode menus and the actual PhysicsFrame.
#include "../src/plugins/native_physics_mode.hpp"
#include "../src/plugins/native_history_commands.hpp"
#include "../src/plugins/edit_store.hpp"
#include "mikumikustudio/physics_scale.hpp"
namespace mikumikustudio { void CmdViewMenu(MMDApp*,HWND,std::uint16_t,std::uint16_t); void PhysicsFrame(MMDApp*,unsigned char); }
namespace native_physics_mode_checks {
using namespace plugins;
using Mode = SharedHistory::RecordingMode;
unsigned ModeCommand(int mode) { return mode == 0 ? 272 : mode == 1 ? 265 : mode == 2 ? 269 : 270; }
struct Scene : BoneRegistrationFixture {
    PhysicsFixture simulation, isolated;
    PhysicsScene scene{};
    std::array<mdl::RigidRecord,2> firstRigids{},secondRigids{};
    std::array<mdl::BoneOrderEntry,1> firstOrder{},secondOrder{};
    std::array<mdl::JointRecord,1> joints{};
    HMENU menu=nullptr;
    Scene() {
        scene.world=&simulation.world; scene.dispatcher=&simulation.dispatcher; scene.broadphase=&simulation.broadphase; scene.solver=&simulation.solver;
        app->Physics()=&scene;app->HInstance()=GetModuleHandleW(nullptr);app->EnglishUI()=1;app->CameraMode()=0;
        app->CurrentFrame()=7;app->state.messageSeen=1;app->PhysicsResetPending()=0;
        unsigned side=0;
        for(auto* fixture:{&first,&second}) {
            auto& rigids=side?secondRigids:firstRigids;
            fixture->m->scenePtr=&scene;fixture->m->rigidCount=2;fixture->m->rigidTable=rigids.data();
            fixture->m->physicsMode=2;fixture->m->comboSelIndex=fixture->m->comboSelIndex2=static_cast<unsigned char>(side+1);
            fixture->m->ikChainCount=0;
            // InitBoneSortOrder always installs the global root sentinel, even
            // when no bone has an external-parent selector. Root evaluation
            // reads entry zero independently of each bone's slotIndex.
            auto& order=side?secondOrder:firstOrder;
            order[0].boneIndex=order[0].linkedModel=-1;
            fixture->m->boneOrderTable=order.data();fixture->m->boneOrderCount=1;
            for(unsigned bone=0;bone<2;++bone) {
                auto& value=fixture->bones[bone];value.parent=value.slotIndex=value.tailIdx=-1;value.rigidIdx=-296;
                value.type=mdl::BoneType::RotateMove;value.hasRigidBody=1;value.physicsDisabled=static_cast<unsigned char>((bone+side)%2);
                value.rotQuat[3]=value.rotQuat2[3]=value.ikBackup[6]=1;value.trans[0]=float(3*side+bone+1);fixture->dirty[bone]=1;
                for(int element=0;element<16;++element)value.matInit[element]=value.matLocal[element]=value.matWorld[element]=value.matExtra[element]=element%5==0?1.f:0.f;
                auto& rigid=rigids[bone];rigid.body=simulation.bodies[2*side+bone].get();rigid.boneIndex=int(bone);rigid.mass=1;rigid.size[0]=.5f;
                rigid.kinematicFlag=static_cast<unsigned char>(bone==1);rigid.mode=rigid.kinematicFlag?2:1;
                for(int element=0;element<16;++element)rigid.invTransform[element]=element%5==0?1.f:0.f;
            }
            ++side;
        }
        firstRigids[1].staticFlag=1;firstRigids[1].mode=0;
        simulation.bodies[1]->setCollisionFlags(simulation.bodies[1]->getCollisionFlags()|btCollisionObject::CF_KINEMATIC_OBJECT);
        joints[0].constraint=simulation.joint->getUid();joints[0].rigidA=0;joints[0].rigidB=1;joints[0].radiusBound=100;
        first.m->jointTable=joints.data();first.m->jointCount=1;
        simulation.bodies[0]->setLinearVelocity(btVector3(.25f,.5f,.75f));
        simulation.bodies[4]->setLinearVelocity(btVector3(.5f,-.25f,.125f));
        menu=CreateMenu();Check(menu!=nullptr,"create hidden physics-mode menu");
        for(int mode=0;mode<4;++mode)Check(AppendMenuW(menu,MF_STRING,ModeCommand(mode),L"physics mode")!=FALSE,"install actual native physics-mode command IDs");
        Check(SetMenu(app->MainWindow(),menu)!=FALSE,"attach real menu to the owned hidden fixture");
        SetInitial(2);
    }
    ~Scene() {
        history.Reset();history.Collect();physics_scale::Forget(first.raw());physics_scale::Forget(second.raw());
        app->Physics()=nullptr;SetMenu(app->MainWindow(),nullptr);DestroyMenu(menu);
        for(auto* fixture:{&first,&second}){fixture->m->scenePtr=nullptr;fixture->m->rigidTable=nullptr;fixture->m->rigidCount=0;fixture->m->jointTable=nullptr;fixture->m->jointCount=0;fixture->m->boneOrderTable=nullptr;fixture->m->boneOrderCount=0;}
    }
    void SetInitial(int mode) {
        app->PlaybackPhysicsMode()=mode;for(auto* fixture:{&first,&second})ModelBonePhysicsModeApply(fixture->raw(),mode);
        app->PhysicsResetPending()=0;RefreshBoundPhysicsModeReadout(*app);
    }
    void Menu(int mode) { auto* previous=g_Block;g_Block=app.get();CmdViewMenu(app.get(),app->MainWindow(),std::uint16_t(ModeCommand(mode)),0);g_Block=previous; }
    void Pump() {auto* previous=g_Block;g_Block=app.get();app->state.messageSeen=1;PhysicsFrame(app.get(),0);g_Block=previous;}
    bool Radios(int selected) {
        for(int mode=0;mode<4;++mode)if(bool(GetMenuState(menu,ModeCommand(mode),MF_BYCOMMAND)&MF_CHECKED)!=(mode==selected))return false;
        return true;
    }
    std::shared_ptr<HistoryChange> Plan(int target) {std::shared_ptr<HistoryChange> change;Check(PlanPhysicsMode(*app,target,change)==MMS_OK,"prepare global native physics mode privately");return change;}
    void Commit(const std::shared_ptr<HistoryChange>& change) {
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        Check(change&&history.PrepareNative("physics mode",change,transaction,SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"admit a complete explicit global physics-mode node");
        Check(CommitRegistrationNoAllocation(history,std::move(transaction))==MMS_OK,"publish global mode and every rigid projection without allocation");
    }
    std::uint64_t WorldDigest(PhysicsFixture& fixture,bool includeKinematic=false) {
        std::uint64_t hash=14695981039346656037ull;const auto add=[&](const void* data,std::size_t count){const auto* bytes=static_cast<const unsigned char*>(data);while(count--){hash^=*bytes++;hash*=1099511628211ull;}};
        const auto scalar=[&](btScalar value){add(&value,sizeof value);};const auto vector=[&](const btVector3& value){scalar(value.x());scalar(value.y());scalar(value.z());};
        const auto transform=[&](const btTransform& value){for(int row=0;row<3;++row)vector(value.getBasis()[row]);vector(value.getOrigin());};
        const auto step=fixture.world.StepSerial();add(&step,sizeof step);scalar(fixture.world.LocalTime());vector(fixture.world.getGravity());
        for(const auto& body:fixture.bodies){transform(body->getWorldTransform());transform(body->getInterpolationWorldTransform());
            vector(body->getLinearVelocity());vector(body->getAngularVelocity());vector(body->getTotalForce());vector(body->getTotalTorque());
            const auto activation=body->getActivationState();add(&activation,sizeof activation);scalar(body->getDeactivationTime());
            const int flags=includeKinematic?body->getCollisionFlags():body->getCollisionFlags()&~btCollisionObject::CF_KINEMATIC_OBJECT;add(&flags,sizeof flags);
            const auto* proxy=body->getBroadphaseHandle();add(&proxy,sizeof proxy);vector(proxy->m_aabbMin);vector(proxy->m_aabbMax);}
        const auto* cache=fixture.broadphase.Cache();add(&cache,sizeof cache);const int pairs=cache->getNumOverlappingPairs();add(&pairs,sizeof pairs);
        for(int index=0;index<fixture.dispatcher.getNumManifolds();++index){const auto* manifold=fixture.dispatcher.getManifoldByIndexInternal(index);
            add(&manifold,sizeof manifold);const auto count=manifold->getNumContacts();add(&count,sizeof count);
            for(int point=0;point<count;++point){const auto& contact=manifold->getContactPoint(point);vector(contact.m_localPointA);vector(contact.m_localPointB);scalar(contact.m_appliedImpulse);}}
        return hash;
    }
    std::uint64_t Digest() {auto hash=first.Digest()^(second.Digest()<<1)^WorldDigest(simulation,true)^WorldDigest(isolated,true);
        const auto add=[&](const void* data,std::size_t count){const auto* bytes=static_cast<const unsigned char*>(data);while(count--){hash^=*bytes++;hash*=1099511628211ull;}};
        add(&app->state,sizeof app->state);add(firstRigids.data(),sizeof firstRigids);add(secondRigids.data(),sizeof secondRigids);return hash;}
};
bool SameWorldResults(const Scene& a,const Scene& b) {
    if(a.simulation.world.LocalTime()!=b.simulation.world.LocalTime()||a.simulation.dispatcher.getNumManifolds()!=b.simulation.dispatcher.getNumManifolds())return false;
    for(unsigned index=0;index<a.simulation.bodies.size();++index) {
        const auto& x=*a.simulation.bodies[index];const auto& y=*b.simulation.bodies[index];
        if(!(x.getWorldTransform()==y.getWorldTransform())||!(x.getInterpolationWorldTransform()==y.getInterpolationWorldTransform())||
            !(x.getLinearVelocity()==y.getLinearVelocity())||!(x.getAngularVelocity()==y.getAngularVelocity())||x.getCollisionFlags()!=y.getCollisionFlags()||
            x.getActivationState()!=y.getActivationState())return false;
    }
    return true;
}
void MenuParityAndPump() {
    for(auto recording:{Mode::AllOperations,Mode::ExplicitOnly})for(int before=0;before<4;++before)for(int target=0;target<4;++target)if(before!=target) {
        std::printf("    mode=%u %d -> %d: construct bound fixture\n",unsigned(recording),before,target);
        Scene f;std::puts("    construct independent native oracle");Scene oracle;
        std::puts("    initialize modes and bind history");f.SetInitial(before);oracle.SetInitial(before);NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(recording)==MMS_OK,"choose explicit or all-operation physics-mode history");
        const auto world=f.WorldDigest(f.simulation),isolated=f.WorldDigest(f.isolated,true);const auto bones=f.first.bones,other=f.second.bones;
        const auto keys=RegistrationKeys(f.first);const auto ring=PoseRingDigest(f.first);const auto firstDirty=f.first.dirty,secondDirty=f.second.dirty;
        std::puts("    publish bound menu");f.Menu(target);std::puts("    run unbound native menu");oracle.Menu(target);
        Check(f.app->PlaybackPhysicsMode()==target&&f.history.UndoCount()==1&&f.app->SceneModified()&&f.Radios(target),"actual physics menu commits one explicit node and synchronizes all radio items in either mode");
        for(unsigned index=0;index<2;++index)Check(f.firstRigids[index].mode==oracle.firstRigids[index].mode&&f.secondRigids[index].mode==oracle.secondRigids[index].mode,
            "both model rigid modes match the ordinary native notifier");
        Check(SameWorldResults(f,oracle)&&f.app->PhysicsResetPending()==oracle.app->PhysicsResetPending(),"owned Bullet kinematic bits and native reset request match the actual unbound menu");
        Check(f.WorldDigest(f.simulation)==world&&f.WorldDigest(f.isolated,true)==isolated&&!std::memcmp(bones.data(),f.first.bones.data(),sizeof bones)&&
            !std::memcmp(other.data(),f.second.bones.data(),sizeof other)&&f.first.dirty==firstDirty&&f.second.dirty==secondDirty&&SameRegistrationKeys(f.first,keys)&&PoseRingDigest(f.first)==ring,
            "publication preserves live transforms velocity activation force contacts accumulator all bone poses and the other physics world");
        const auto step=f.simulation.world.StepSerial();std::puts("    pump bound world");f.Pump();std::puts("    pump independent native world");oracle.Pump();
        Check(SameWorldResults(f,oracle)&&f.app->PhysicsResetPending()==0,"the actual next PhysicsFrame matches native settling and consumes its enabled-mode request");
        Check(!target||f.simulation.world.StepSerial()>step,"enabled global modes exercise a real Bullet simulation pump");
        Check(f.WorldDigest(f.isolated,true)==isolated,"the native pump does not visit an unrelated independent physics world");
        for(unsigned bone=0;bone<2;++bone)Check(!std::memcmp(f.first.bones[bone].trans,bones[bone].trans,sizeof bones[bone].trans)&&
            !std::memcmp(f.first.bones[bone].rotQuat,bones[bone].rotQuat,sizeof bones[bone].rotQuat)&&f.first.dirty==firstDirty,
            "native settling retains the current unregistered bone inputs");
        f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;f.app->CurrentFrame()=37;
        f.first.bones[0].physicsDisabled^=1;f.second.bones[1].physicsDisabled^=1;
        oracle.first.bones[0].physicsDisabled=f.first.bones[0].physicsDisabled;oracle.second.bones[1].physicsDisabled=f.second.bones[1].physicsDisabled;
        oracle.app->CurrentFrame()=37;const auto currentWorld=f.WorldDigest(f.simulation);
        Check(f.history.History(false)==MMS_OK&&f.app->PlaybackPhysicsMode()==before&&f.app->CurrentFrame()==37&&f.app->SelectedModelSlot()==1,
            "undo follows retained models from another selected model and a changed playhead");
        oracle.Menu(before);Check(SameWorldResults(f,oracle)&&f.app->PhysicsResetPending()==oracle.app->PhysicsResetPending()&&f.WorldDigest(f.simulation)==currentWorld,
            "undo derives the current per-bone switches without rewinding any advanced simulation state");
        Check(f.history.History(true)==MMS_OK&&f.app->PlaybackPhysicsMode()==target,"redo reapplies the recorded logical mode");
        oracle.Menu(target);Check(SameWorldResults(f,oracle),"redo uses current switches and preserves non-owned Bullet flag bits");
    }
}
void NoopBusyBudgetAndStale() {
    for(auto recording:{Mode::AllOperations,Mode::ExplicitOnly}) {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(recording)==MMS_OK,"configure physics-mode no-op history");
        f.Commit(f.Plan(1));Check(f.history.History(false)==MMS_OK,"seed a physics-mode redo branch");
        const auto before=f.Digest();const auto redo=f.history.RedoCount();Check(!f.Plan(2),"same logical and derived physics mode is a true no-op");
        f.Menu(2);Check(f.Digest()==before&&f.history.RedoCount()==redo&&f.history.UndoCount()==0,"same-mode menu preserves all scene data and existing redo");
        const auto dirtyBeforeRepair=f.app->SceneModified();f.app->SceneModified()=0;f.firstRigids[0].mode=0;auto* body=static_cast<btRigidBody*>(f.firstRigids[0].body);
        body->setCollisionFlags(body->getCollisionFlags()|btCollisionObject::CF_KINEMATIC_OBJECT);
        const auto repairWorld=f.WorldDigest(f.simulation);const auto request=f.app->PhysicsResetPending();
        const auto repairSource=f.Digest();failAfter=0;f.Menu(2);failAfter=-1;
        Check(f.Digest()==repairSource&&f.history.RedoCount()==redo&&!f.history.UndoCount(),"same-value repair allocation refusal leaves every runtime value and history branch intact");
        std::uint64_t revision=0,afterRepair=0;Check(f.history.Revision(revision)==MMS_OK&&!f.Plan(2),"a same-value runtime repair is never returned as an authored history participant");
        f.Menu(2);Check(f.firstRigids[0].mode==1&&!(body->getCollisionFlags()&btCollisionObject::CF_KINEMATIC_OBJECT)&&
            f.history.Revision(afterRepair)==MMS_OK&&afterRepair==revision&&f.history.RedoCount()==redo&&!f.history.UndoCount()&&!f.app->SceneModified()&&
            f.app->PhysicsResetPending()==request&&f.WorldDigest(f.simulation)==repairWorld,
            "same-value menu atomically repairs derived state without EditStore history dirty changes or a new settle request");
        f.app->SceneModified()=dirtyBeforeRepair;
        f.app->PlaybackActive()=1;std::shared_ptr<HistoryChange> change;
        Check(PlanPhysicsMode(*f.app,3,change)==MMS_E_BUSY&&!change,"playback excludes a conflicting global physics edit");f.app->PlaybackActive()=0;
        change=f.Plan(3);const auto charge=change->RetainedBytes();
        Check(f.history.Configure(recording,128,charge-1)==MMS_OK,"configure a budget smaller than actual physics-mode resources");
        const auto budgetRedo=f.history.RedoCount();
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("physics budget",change,token,SharedHistory::NativeIntent::ExplicitCommit)==MMS_E_MEMORY&&!token&&f.Digest()==before&&f.history.RedoCount()==budgetRedo,
            "budget refusal cannot publish only part of a multi-model mode change");
        Check(f.history.Configure(recording,128,256ull*1024*1024)==MMS_OK,"restore physics-mode history budget");
        bool complete=false;unsigned failed=0;
        for(long allocation=0;allocation<2048&&!complete;++allocation){std::string label="physics OOM";failAfter=allocation;const auto result=f.history.PrepareNative(std::move(label),change,token,SharedHistory::NativeIntent::ExplicitCommit);failAfter=-1;
            Check(f.Digest()==before,"private allocation failure does not mutate mode world bones or journal");
            if(result==MMS_OK)complete=true;else{++failed;Check(result==MMS_E_MEMORY&&!token,"incomplete physics target releases its journal reservation");}}
        Check(complete&&failed>3,"physics candidate allocation sweep reaches a complete multi-world-guarded target");token.reset();
        Check(f.history.PrepareNative("stale physics target",change,token,SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"prepare a real physics target before changing its source");
        f.second.bones[1].physicsDisabled^=1;const auto changed=f.Digest();
        Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&f.Digest()==changed&&f.history.RedoCount()==budgetRedo,"a late per-bone switch change refuses atomic publication");
    }
    for(unsigned mutation=0;mutation<3;++mutation) {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);auto change=f.Plan(0);std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("physics source freeze",change,token)==MMS_OK,"freeze one atomic physics publication");
        if(mutation==0)f.simulation.world.performDiscreteCollisionDetection();
        else if(mutation==1)f.second.bones[0].hasRigidBody=0;
        else {NativePoseTestBinding nested(*f.app,f.history);}
        const auto changed=f.Digest();Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&f.Digest()==changed&&f.history.UndoCount()==0,
            "world collision work eligibility changes or binding recycling invalidate a prepared physics publication");
    }
}
void AliasesAndPublicationBoundary() {
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);f.Menu(1);
        Check(TryBoundHistoryCommand(f.app.get(),false)&&f.app->PlaybackPhysicsMode()==2&&f.Radios(2),
            "actual common undo refreshes all physics radio items from the restored logical mode");
        Check(TryBoundHistoryCommand(f.app.get(),true)&&f.app->PlaybackPhysicsMode()==1&&f.Radios(1),
            "actual common redo refreshes physics radio items without reloading the scene");
    }
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);auto change=f.Plan(0);std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("reserved global mode",change,token)==MMS_OK,"reserve the original physics-mode publication");
        const auto before=f.Digest();f.Menu(2);f.Menu(3);
        Check(f.Digest()==before&&f.history.UndoCount()==0,"both same-value repair and a different menu mode respect an existing prepared native transaction");
        Check(CommitRegistrationNoAllocation(f.history,std::move(token))==MMS_OK&&f.app->PlaybackPhysicsMode()==0,
            "a refused reentrant mode menu leaves the original reserved transaction committable");
    }
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);const auto original=f.secondRigids[0].body;
        f.secondRigids[0].body=f.firstRigids[0].body;const auto before=f.Digest();std::shared_ptr<HistoryChange> change;
        Check(PlanPhysicsMode(*f.app,0,change)==MMS_E_ARGUMENT&&!change&&f.Digest()==before,"shared Bullet bodies are rejected before any notifier write");
        f.secondRigids[0].body=original;
    }
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);auto change=f.Plan(0);std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("late physics alias",change,token)==MMS_OK,"prepare before an unrelated field acquires a writable alias");
        const auto old=f.second.m->boneSelection;f.second.m->boneSelection=reinterpret_cast<unsigned char*>(f.firstRigids.data());
        const auto changed=f.Digest();Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&f.Digest()==changed,"late native buffer alias invalidates the target without touching either model");
        f.second.m->boneSelection=old;
    }
    Scene f;NativePoseTestBinding binding(*f.app,f.history);f.Commit(f.Plan(0));const auto off=f.Digest();
    failAfter=0;const auto undo=f.history.History(false);failAfter=-1;
    Check(undo==MMS_E_MEMORY&&f.Digest()==off&&f.history.UndoCount()==1,"undo allocation refusal preserves the committed mode and world");
    Check(f.history.History(false)==MMS_OK,"retry physics-mode undo");const auto on=f.Digest();
    failAfter=0;const auto redo=f.history.History(true);failAfter=-1;
    Check(redo==MMS_E_MEMORY&&f.Digest()==on&&f.history.RedoCount()==1,"redo allocation refusal preserves its source and redo node");
    Check(f.history.History(true)==MMS_OK,"retry physics-mode redo");
}
void PluginAndPoseInterleave() {
    Scene f;NativePoseTestBinding binding(*f.app,f.history);camera_multiply_checks::PluginState plugin;
    StorageClient storage;storage.id="test.physics-mode";f.storage.Api(storage);auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof serializer,&provider)==MMS_OK,"register real physics-mode plugin serializer");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;auto api=edits.Api(client);auto native=f.Plan(0);
    std::uint64_t revision=0;MmsHandle transaction=0;Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,Text("mixed physics mode"),revision,&transaction)==MMS_OK,"begin real plugin and physics-mode transaction");
    std::shared_ptr<HistoryChange> absent;Check(PlanPhysicsMode(*f.app,1,absent)==MMS_E_BUSY,"active plugin draft excludes another physics menu");
    auto participant=plugin.Participant(Text(client.id));Check(api.join(api.context,transaction,&participant)==MMS_OK&&edits.JoinNative(client,transaction,native)==MMS_OK&&
        api.prepare(api.context,transaction)==MMS_OK&&api.commit(api.context,transaction)==MMS_OK&&plugin.value==42&&f.app->PlaybackPhysicsMode()==0,
        "plugin bytes and every native physics mode publish in one journal node");
    f.app->SelectedModelSlot()=1;const auto pose=f.second.bones[0].trans[1];
    NativePoseScope scope;bool changed=false;Check(scope.Begin(*f.app,f.history,1,"later other-model pose",SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"begin a later explicit pose on another model");
    f.second.bones[0].trans[1]=123;f.second.dirty[0]=1;Check(scope.Commit(changed)==MMS_OK&&changed,"commit later cross-model pose into the same journal");
    Check(api.undo(api.context)==MMS_OK&&f.second.bones[0].trans[1]==pose&&plugin.value==42&&f.app->PlaybackPhysicsMode()==0&&
        api.undo(api.context)==MMS_OK&&plugin.value==0&&f.app->PlaybackPhysicsMode()==2,"common undo visits later pose then mixed plugin/global-physics entry");
    Check(api.redo(api.context)==MMS_OK&&plugin.value==42&&f.app->PlaybackPhysicsMode()==0&&api.redo(api.context)==MMS_OK&&f.second.bones[0].trans[1]==123,"common redo restores each recorded family in chronological order");
    Check(f.history.Reset()==MMS_OK,"retire mixed physics records before serializer unload");f.history.Collect();
    Check(f.storage.Unregister(storage,provider)==MMS_OK,"unregister physics-mode regression serializer");
}
void ModelRemovalInterleave() {
    Scene f;NativePoseTestBinding binding(*f.app,f.history);f.Commit(f.Plan(0));
    auto owner=RetainSceneObject(*f.app,MMS_OBJECT_MODEL,1);Check(bool(owner),"retain the actual second-model physics owner");
    // These fixture model arrays are resident test-owned buffers. The explicit
    // overload charges that known resident test payload; production deletion
    // uses its complete native-resource measurement overload.
    auto removal=owner->CaptureRemoval(1024*1024);std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    Check(removal&&f.history.PrepareNative("remove second physics model",removal,transaction,SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,
        "prepare a real scene removal after the global mode change");
    Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK&&f.app->ModelSlot(1)==nullptr&&f.simulation.world.getNumCollisionObjects()==3,
        "the later common-history deletion really parks the second model bodies in their shared Bullet world");
    Check(f.history.History(false)==MMS_OK&&f.app->ModelSlot(1)==f.second.raw()&&f.simulation.world.getNumCollisionObjects()==5&&
        f.history.History(false)==MMS_OK&&f.app->PlaybackPhysicsMode()==2,"undo restores scene membership before replaying the earlier all-model physics mode");
    Check(f.history.History(true)==MMS_OK&&f.app->PlaybackPhysicsMode()==0&&f.history.History(true)==MMS_OK&&f.app->ModelSlot(1)==nullptr,
        "redo follows the global mode and actual model deletion in their original chronological order");
    Check(f.history.History(false)==MMS_OK&&f.app->ModelSlot(1)==f.second.raw(),"restore the resident fixture model before releasing history owners");
}
}
void NativePhysicsModeChecks() {
    Check(d3dx::Get().Load(),"load the actual D3DX functions required by the native PhysicsFrame oracle");
    std::puts("  physics mode: actual menus and real CPU physics pump");native_physics_mode_checks::MenuParityAndPump();
    std::puts("  physics mode: no-op budget allocation and source freshness");native_physics_mode_checks::NoopBusyBudgetAndStale();
    std::puts("  physics mode: aliases and allocation-free atomic publication");native_physics_mode_checks::AliasesAndPublicationBoundary();
    std::puts("  physics mode: plugin and other-model pose interleave");native_physics_mode_checks::PluginAndPoseInterleave();
    std::puts("  physics mode: real model removal and restored physics membership");native_physics_mode_checks::ModelRemovalInterleave();
}
