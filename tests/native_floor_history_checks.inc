// Include after native_physics_mode_checks.inc and the black-background fixture.
// Real owned Bullet worlds and menu messages; no global keyboard/mouse input.
#include "../src/plugins/native_floor_history.hpp"
#include <cmath>
namespace floor_history_checks {
using namespace plugins;
using Mode = SharedHistory::RecordingMode;
int Flags(bool enabled) { return enabled ? int(btCollisionObject::CF_STATIC_OBJECT) : -1; }
struct Scene : black_background_checks::Fixture {
    PhysicsFixture simulation, isolated;
    btStaticPlaneShape plane{btVector3(0,1,0),0};
    btDefaultMotionState motion{btTransform::getIdentity()};
    std::unique_ptr<btRigidBody> ground;
    PhysicsScene physics{};
    Scene() {
        ground=std::make_unique<btRigidBody>(0,&motion,&plane,btVector3(0,0,0));
        simulation.world.addRigidBody(ground.get(),static_cast<short>(0x8000),static_cast<short>(-1));
        ground->setRestitution(.88f);
        physics.world=&simulation.world;physics.dispatcher=&simulation.dispatcher;physics.broadphase=&simulation.broadphase;
        physics.solver=&simulation.solver;physics.collisionConfig=&simulation.config;physics.groundBody=ground.get();
        app->Physics()=&physics;app->state.floorVisible=1;app->HInstance()=GetModuleHandleW(nullptr);
        Check(AppendMenuW(menu,MF_STRING,285,L"floor"),"create the actual floor menu item");
        simulation.world.performDiscreteCollisionDetection();RefreshBoundFloorReadout(*app);
    }
    ~Scene() {
        history.Reset();history.Collect();app->Physics()=nullptr;physics.groundBody=nullptr;
        if(ground)simulation.world.removeRigidBody(ground.get());
    }
    void Menu() {
        struct Restore {MMDApp* before;~Restore(){g_Block=before;}} restore{g_Block};
        g_Block=app.get();CommandDispatch(nullptr,MAKEWPARAM(285,0));
    }
    bool Echo() const {const auto state=GetMenuState(menu,285,MF_BYCOMMAND);return state!=UINT(-1)&&bool(state&MF_CHECKED)==bool(app->state.floorVisible);}
    void Initial(unsigned char raw,int flags) {app->state.floorVisible=raw;ground->setCollisionFlags(flags);RefreshBoundFloorReadout(*app);}
};
std::uint64_t WorldDigest(PhysicsFixture& fixture, const btRigidBody* exempt=nullptr) {
    std::uint64_t hash=14695981039346656037ull;
    const auto add=[&](const void* data,std::size_t count){const auto* p=static_cast<const unsigned char*>(data);while(count--){hash^=*p++;hash*=1099511628211ull;}};
    const auto scalar=[&](btScalar x){add(&x,sizeof x);};
    const auto vector=[&](const btVector3& v){scalar(v.x());scalar(v.y());scalar(v.z());};
    const auto transform=[&](const btTransform& t){for(int i=0;i<3;++i)vector(t.getBasis()[i]);vector(t.getOrigin());};
    const auto serial=fixture.world.StepSerial();add(&serial,sizeof serial);scalar(fixture.world.LocalTime());vector(fixture.world.getGravity());
    const auto& objects=fixture.world.getCollisionObjectArray();const auto count=objects.size(),capacity=objects.capacity();
    add(&count,sizeof count);add(&capacity,sizeof capacity);const auto* array=count?&objects[0]:nullptr;add(&array,sizeof array);
    for(int i=0;i<count;++i){const auto* object=objects[i];add(&object,sizeof object);transform(object->getWorldTransform());transform(object->getInterpolationWorldTransform());
        if(object!=exempt){const int flags=object->getCollisionFlags();add(&flags,sizeof flags);}
        auto* body=btRigidBody::upcast(objects[i]);if(body){vector(body->getLinearVelocity());vector(body->getAngularVelocity());vector(body->getTotalForce());vector(body->getTotalTorque());}
        const auto activation=object->getActivationState();add(&activation,sizeof activation);scalar(object->getDeactivationTime());
        const auto* shape=object->getCollisionShape();add(&shape,sizeof shape);const auto* proxy=object->getBroadphaseHandle();add(&proxy,sizeof proxy);
        if(proxy){add(&proxy->m_uniqueId,sizeof proxy->m_uniqueId);add(&proxy->m_collisionFilterGroup,sizeof proxy->m_collisionFilterGroup);
            add(&proxy->m_collisionFilterMask,sizeof proxy->m_collisionFilterMask);vector(proxy->m_aabbMin);vector(proxy->m_aabbMax);}}
    const auto& bodies=fixture.world.HistoryBodies();const int bodyCount=bodies.size(),bodyCapacity=bodies.capacity();
    add(&bodyCount,sizeof bodyCount);add(&bodyCapacity,sizeof bodyCapacity);for(int i=0;i<bodies.size();++i)add(&bodies[i],sizeof bodies[i]);
    const auto& constraints=fixture.world.HistoryConstraints();const int constraintCount=constraints.size(),constraintCapacity=constraints.capacity();
    add(&constraintCount,sizeof constraintCount);add(&constraintCapacity,sizeof constraintCapacity);
    for(int i=0;i<constraints.size();++i){add(&constraints[i],sizeof constraints[i]);const auto uid=constraints[i]->getUid();add(&uid,sizeof uid);}
    auto* cache=fixture.broadphase.Cache();add(&cache,sizeof cache);const int pairs=cache->getNumOverlappingPairs();add(&pairs,sizeof pairs);
    const auto& pairArray=cache->getOverlappingPairArray();
    for(int i=0;i<pairArray.size();++i){const auto& pair=pairArray[i];add(&pair.m_pProxy0,sizeof pair.m_pProxy0);add(&pair.m_pProxy1,sizeof pair.m_pProxy1);add(&pair.m_algorithm,sizeof pair.m_algorithm);}
    const auto manifolds=fixture.dispatcher.getNumManifolds();add(&manifolds,sizeof manifolds);
    for(int i=0;i<manifolds;++i){const auto* manifold=fixture.dispatcher.getManifoldByIndexInternal(i);add(&manifold,sizeof manifold);const auto contacts=manifold->getNumContacts();add(&contacts,sizeof contacts);
        for(int j=0;j<contacts;++j){const auto& point=manifold->getContactPoint(j);vector(point.m_localPointA);vector(point.m_localPointB);scalar(point.m_appliedImpulse);}}
    return hash;
}
struct Source {
    Scene& f;
    black_background_checks::Source author;
    const std::uint64_t world, isolated;
    explicit Source(Scene& value):f(value),author(f),world(WorldDigest(f.simulation,f.ground.get())),isolated(WorldDigest(f.isolated)){}
    void Unchanged(int frame=-1) const {
        author.Unchanged(frame);
        Check(WorldDigest(f.simulation,f.ground.get())==world&&WorldDigest(f.isolated)==isolated,
            "floor publication preserves both real CPU worlds, arrays, contacts, transforms, velocities, activation and accumulators");
    }
};
void RoutingAndRawFlags() {
    for(unsigned char raw:{0,1,7,255}) {
        Scene f;NativePoseTestBinding bound(*f.app,f.history);f.Initial(raw,Flags(raw!=0));bool changed=true;
        Check(ApplyBoundFloor(*f.app,raw!=0,changed)==MMS_OK&&!changed&&f.app->state.floorVisible==raw&&!f.history.UndoCount(),
            "matching logical floor and actual flags preserve the exact noncanonical source byte without history");
        const int originalFlags=btCollisionObject::CF_STATIC_OBJECT|btCollisionObject::CF_CUSTOM_MATERIAL_CALLBACK|
            (raw?0:btCollisionObject::CF_NO_CONTACT_RESPONSE);
        f.Initial(raw,originalFlags);Source source(f);const auto target=raw?0:1;f.Menu();
        Check(f.app->state.floorVisible==target&&f.ground->getCollisionFlags()==Flags(target!=0)&&f.history.UndoCount()==1&&
            f.Echo()&&f.app->SceneModified()&&!f.history.LastPublicationRequiresPhysicsReset(),
            "actual menu 285 publishes the authored byte and live Bullet flags as one drawing-only adjustment");source.Unchanged();
        f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;f.app->CurrentFrame()=49;
        Check(TryBoundHistoryCommand(f.app.get(),false)&&f.app->state.floorVisible==raw&&f.ground->getCollisionFlags()==originalFlags&&
            !f.history.UndoCount()&&f.history.RedoCount()==1&&f.Echo(),"real common undo restores the exact original floor byte and all collision flag bits");source.Unchanged(49);
        Check(TryBoundHistoryCommand(f.app.get(),true)&&f.app->state.floorVisible==target&&f.ground->getCollisionFlags()==Flags(target!=0)&&f.Echo(),
            "real common redo keeps the current model, camera mode and playhead");source.Unchanged(49);
        Check(f.history.History(false)==MMS_OK,"seed floor redo for no-op");f.Initial(raw,Flags(raw!=0));
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        Check(ApplyBoundFloor(*f.app,raw!=0,changed)==MMS_OK&&!changed&&f.history.RedoCount()==1&&!f.app->SceneModified()&&
            !f.app->RecordedFrameCountByte1(),"floor no-op preserves a live redo branch and dirty flags");
    }
    Scene f;NativePoseTestBinding bound(*f.app,f.history);f.Initial(7,Flags(false));Source source(f);bool changed=false;
    Check(ApplyBoundFloor(*f.app,true,changed)==MMS_OK&&changed&&f.app->state.floorVisible==7&&f.ground->getCollisionFlags()==Flags(true)&&
        f.history.UndoCount()==1,"same logical floor with incorrect actual flags is a real recorded repair without normalizing its source byte");
    Check(f.history.History(false)==MMS_OK&&f.app->state.floorVisible==7&&f.ground->getCollisionFlags()==Flags(false),
        "undo restores the exact previously inconsistent physical projection");source.Unchanged();
}
void RecordingModes() {
    Scene f;NativePoseTestBinding bound(*f.app,f.history);Source source(f);f.Menu();
    Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"seed floor redo before explicit-only adjustments");
    Check(f.history.SetMode(Mode::ExplicitOnly)==MMS_OK,"select explicit-only floor history mode");
    std::uint64_t revision=0,next=0;Check(f.history.Revision(revision)==MMS_OK,"capture floor revision");f.Menu();
    Check(!f.app->state.floorVisible&&f.ground->getCollisionFlags()==Flags(false)&&!f.history.UndoCount()&&f.history.RedoCount()==1&&
        f.history.Revision(next)==MMS_OK&&next>revision&&f.Echo(),"explicit-only actual floor menu updates both fields and revision while retaining redo without a node");
    Check(f.history.History(true)==MMS_OK&&!f.app->state.floorVisible,"retained floor redo remains usable after unrecorded adjustment");
    Check(f.history.Configure(Mode::ExplicitOnly,128,1)==MMS_OK,"configure floor budget below one history node");f.Menu();
    Check(f.app->state.floorVisible==1&&f.ground->getCollisionFlags()==Flags(true)&&!f.history.UndoCount()&&!f.history.RetainedBytes(),
        "explicit-only floor adjustment needs no retained node budget");
    Check(f.history.Configure(Mode::AllOperations,128,1024*1024)==MMS_OK,"restore floor all-operations mode");f.Menu();
    Check(f.history.UndoCount()==1&&f.history.History(false)==MMS_OK&&f.app->state.floorVisible==1&&f.ground->getCollisionFlags()==Flags(true),
        "all-operations floor adjustment records the preceding unrecorded state");source.Unchanged();
}
void BudgetAndAllocationFailures() {
    {
        Scene f;NativePoseTestBinding bound(*f.app,f.history);f.Initial(7,9);Source source(f);int value=0;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("seed",std::make_shared<black_background_checks::TinyChange>(value),transaction)==MMS_OK&&
            f.history.CommitNative(std::move(transaction))==MMS_OK&&f.history.History(false)==MMS_OK,"seed smaller floor redo before budget refusal");
        const auto budget=f.history.RetainedBytes();Check(budget&&f.history.Configure(Mode::AllOperations,128,budget)==MMS_OK,"limit floor budget while keeping the smaller redo");
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;CheckMenuItem(f.menu,285,MF_UNCHECKED);f.Menu();
        Check(f.app->state.floorVisible==7&&f.ground->getCollisionFlags()==9&&!f.history.UndoCount()&&f.history.RedoCount()==1&&
            f.history.RetainedBytes()==budget&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.Echo(),
            "actual floor menu budget refusal preserves exact bytes, flags, redo, menu and dirty flags");source.Unchanged();
        Check(f.history.History(true)==MMS_OK&&value==1,"smaller redo survives the rejected floor edit");
    }
    Scene f;NativePoseTestBinding bound(*f.app,f.history);f.Initial(7,9);f.Menu();
    Check(f.history.History(false)==MMS_OK,"seed exact floor redo before C++ allocation sweep");
    Source source(f);f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
    const auto bytes=f.history.RetainedBytes();unsigned failures=0;bool success=false;
    for(long limit=0;limit<160;++limit){CheckMenuItem(f.menu,285,MF_UNCHECKED);failAfter=limit;f.Menu();failAfter=-1;
        if(f.history.UndoCount()){success=true;break;}++failures;
        Check(f.app->state.floorVisible==7&&f.ground->getCollisionFlags()==9&&f.history.RedoCount()==1&&f.history.RetainedBytes()==bytes&&
            !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.Echo(),"every floor preparation allocation failure leaves exact live state, old redo and UI echo intact");source.Unchanged();
        Check(f.history.Enter()==MMS_OK,"floor allocation refusal releases its history gate");f.history.Leave();}
    Check(success&&failures>=5&&!f.app->state.floorVisible&&f.ground->getCollisionFlags()==-1&&!f.history.RedoCount(),"floor allocation sweep reaches one complete two-field publication");
    Check(f.history.History(false)==MMS_OK&&f.app->state.floorVisible==7&&f.ground->getCollisionFlags()==9,"floor remains exactly undoable after allocation failures");source.Unchanged();
}
void GatesAndOrdinaryPath() {
    for(unsigned gate=0;gate<8;++gate){Scene f;NativePoseTestBinding bound(*f.app,f.history);f.Initial(7,9);Source source(f);
        if(gate==0)f.app->PlaybackActive()=1;
        if(gate==1)f.app->FrameStepPlayback()=1;
        if(gate==2)f.app->state.recordPlaybackStartPending=true;
        if(gate==3)f.app->FrameRangeDialog()=reinterpret_cast<HWND>(INT_PTR(1));
        if(gate==4)Check(f.history.Enter()==MMS_OK,"hold floor journal for native busy rejection");
        if(gate==5)f.app->GravitySettingDialog()=reinterpret_cast<HWND>(INT_PTR(1));
        if(gate==6)f.app->GroundShadowColorDialog()=reinterpret_cast<HWND>(INT_PTR(1));
        if(gate==7)f.app->state.accessoryEditDialogOpen=true;
        f.Menu();Check(f.app->state.floorVisible==7&&f.ground->getCollisionFlags()==9&&!f.history.UndoCount()&&!f.app->SceneModified()&&f.Echo(),
            "bound floor playback, modal and busy refusal consumes the actual menu without legacy writes");
        f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=false;f.app->FrameRangeDialog()=nullptr;
        f.app->GravitySettingDialog()=f.app->GroundShadowColorDialog()=nullptr;f.app->state.accessoryEditDialogOpen=false;if(gate==4)f.history.Leave();source.Unchanged();}
    Scene f;bool changed=true;
    Check(!TryToggleBoundFloor(f.app.get())&&ApplyBoundFloor(*f.app,false,changed)==MMS_E_NOT_READY&&!changed,"unbound floor helpers preserve the ordinary native route");
    f.Initial(7,9);const auto world=WorldDigest(f.simulation,f.ground.get());f.Menu();
    Check(!f.app->state.floorVisible&&f.ground->getCollisionFlags()==-1&&!f.history.UndoCount()&&f.Echo()&&f.app->state.dialogFlags[15]==1,
        "ordinary actual menu 285 keeps its old physical flags, menu echo and consumed-input flag");
    Check(WorldDigest(f.simulation,f.ground.get())==world,"ordinary floor toggle does not reconstruct the real world");
}
void RootAndResourceIdentity() {
    {
        Scene f;NativePoseTestBinding bound(*f.app,f.history);f.Menu();
        const auto capacity=mdl::KeyPoolCapacity(f.app->CameraKeys());auto* grown=mdl::KeyPoolCreate<mdl::CameraKey>(capacity+8);
        std::memcpy(grown,f.app->CameraKeys(),capacity*sizeof(mdl::CameraKey));mdl::KeyPoolCommitGrowth(f.app->CameraKeys(),grown);
        Check(f.history.History(false)==MMS_OK&&f.app->state.floorVisible==1&&f.history.History(true)==MMS_OK,"floor replay survives identity-preserving project key-pool growth");
        mdl::KeyPoolDestroy(f.app->CameraKeys());f.app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(capacity+8);
        Check(f.history.History(false)==MMS_E_STALE&&!f.app->state.floorVisible&&f.history.UndoCount()==1,"replacement project root refuses stale floor replay without moving the journal");
    }
    for(unsigned replacement=0;replacement<8;++replacement){Scene f;NativePoseTestBinding bound(*f.app,f.history);f.Menu();
        auto* oldScene=f.app->Physics();auto* oldWorld=f.physics.world;auto* oldGround=f.physics.groundBody;
        const auto oldMask=f.ground->getBroadphaseHandle()->m_collisionFilterMask;
        if(replacement==0)f.app->Physics()=nullptr;
        if(replacement==1)f.physics.world=&f.isolated.world;
        if(replacement==2)f.physics.groundBody=f.simulation.bodies[0].get();
        if(replacement==3){f.simulation.world.removeRigidBody(f.ground.get());f.ground.reset();} // stale scene pointer is intentionally not dereferenced
        if(replacement==4){ // same body allocation, shape/motion, sweep handle and UID
            f.simulation.world.removeRigidBody(f.ground.get());auto* memory=f.ground.get();memory->~btRigidBody();
            ::new(static_cast<void*>(memory)) btRigidBody(0,&f.motion,&f.plane,btVector3(0,0,0));
            f.simulation.world.addRigidBody(memory,static_cast<short>(0x8000),static_cast<short>(-1));memory->setCollisionFlags(-1);
            Check(f.physics.groundBody==oldGround,"exercise ground replacement in the identical body allocation");}
        if(replacement==5)f.app->AdvanceSceneGeneration();
        if(replacement==6)f.ground->setCollisionShape(&f.simulation.shape);
        if(replacement==7)f.ground->getBroadphaseHandle()->m_collisionFilterMask=0;
        const auto world=WorldDigest(f.simulation,f.ground.get()),isolated=WorldDigest(f.isolated);
        Check(f.history.History(false)==MMS_E_STALE&&!f.app->state.floorVisible&&f.history.UndoCount()==1&&!f.history.RedoCount(),
            "changed scene, world, ground member, lifetime, generation, shape or mask refuses floor replay before old resource access");
        Check(WorldDigest(f.simulation,f.ground.get())==world&&WorldDigest(f.isolated)==isolated,"stale floor replay changes neither current physical world");
        f.app->Physics()=oldScene;f.physics.world=oldWorld;
        f.physics.groundBody=f.ground?f.ground.get():nullptr;
        if(replacement==6)f.ground->setCollisionShape(&f.plane);
        if(replacement==7)f.ground->getBroadphaseHandle()->m_collisionFilterMask=oldMask;
    }
}
void CandidatesAndNoAllocation() {
    for(unsigned mutation=0;mutation<5;++mutation){Scene f;NativePoseTestBinding bound(*f.app,f.history);std::shared_ptr<HistoryChange> change;
        Check(PlanFloor(*f.app,false,change)==MMS_OK&&change,"prepare a private floor change");std::unique_ptr<PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"prepare real floor publication before stale-source mutation");
        if(mutation==0)++f.app->CurrentFrame();
        if(mutation==1)f.ground->setCollisionFlags(9);
        if(mutation==2)f.simulation.world.performDiscreteCollisionDetection();
        if(mutation==3){auto& objects=f.simulation.world.getCollisionObjectArray();std::swap(objects[0],objects[1]);}
        if(mutation==4){NativePoseTestBinding rebound(*f.app,f.history);}
        const auto world=WorldDigest(f.simulation,f.ground.get());Check(!candidate->IsCurrent(),"frame, flags, world step, member order or binding change expires a short floor candidate");
        candidate.reset();Check(f.app->state.floorVisible==1&&!f.history.UndoCount()&&WorldDigest(f.simulation,f.ground.get())==world,"cancelling a stale floor candidate performs no author or world write");}
    Scene f;NativePoseTestBinding bound(*f.app,f.history);f.Initial(7,9);Source source(f);
    f.Menu();Check(f.history.History(false)==MMS_OK,"seed floor redo before cancelling a valid reservation");
    const auto retained=f.history.RetainedBytes();std::shared_ptr<HistoryChange> cancelled;
    Check(PlanFloor(*f.app,false,cancelled)==MMS_OK&&cancelled,"plan a cancellable two-field floor adjustment");
    std::unique_ptr<SharedHistory::NativeTransaction> abandoned;
    Check(f.history.PrepareNative("Floor collision",cancelled,abandoned,SharedHistory::NativeIntent::Adjustment)==MMS_OK,
        "prepare the actual floor journal transaction before cancellation");
    abandoned.reset();
    Check(f.app->state.floorVisible==7&&f.ground->getCollisionFlags()==9&&!f.history.UndoCount()&&f.history.RedoCount()==1&&f.history.RetainedBytes()==retained,
        "cancelling a valid floor reservation preserves exact state, redo and retained budget");source.Unchanged();
    std::shared_ptr<HistoryChange> change;
    Check(PlanFloor(*f.app,false,change)==MMS_OK&&change,"capture reusable floor history participant");
    for(unsigned i=0;i<6;++i){const bool forward=(i%2)==0;std::unique_ptr<PreparedHistoryChange> candidate;
        Check(change->Prepare(forward,candidate)==MMS_OK&&candidate->IsCurrent(),"prepare one floor direction before disabling allocators");
        bulletAllocations=0;trackBulletAlloc=true;failAfter=0;const bool current=candidate->IsCurrent();if(current)candidate->Publish();failAfter=-1;trackBulletAlloc=false;
        Check(current&&!bulletAllocations&&f.app->state.floorVisible==(forward?0:7)&&f.ground->getCollisionFlags()==(forward?-1:9),
            "floor publication and source verification allocate neither C++ nor Bullet memory in either direction");source.Unchanged();}
    std::shared_ptr<HistoryChange> journal;Check(PlanFloor(*f.app,false,journal)==MMS_OK&&journal,"capture floor for actual no-allocation journal publication");
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;Check(f.history.PrepareNative("Floor collision",journal,transaction,SharedHistory::NativeIntent::Adjustment)==MMS_OK,"prepare the complete floor journal reservation");
    bulletAllocations=0;trackBulletAlloc=true;failAfter=0;const auto result=f.history.CommitNative(std::move(transaction));failAfter=-1;trackBulletAlloc=false;
    Check(result==MMS_OK&&!bulletAllocations&&f.history.UndoCount()==1,"actual floor journal commit publishes without either allocator");
    Check(f.history.History(false)==MMS_OK&&f.app->state.floorVisible==7&&f.ground->getCollisionFlags()==9,"journal floor undo retains full raw state after allocation-free publication");source.Unchanged();
}
void PendingAndMixedOrder() {
    Scene f;NativePoseTestBinding bound(*f.app,f.history);
    const auto commitMain=[&](int id,const char* value){struct Restore {MMDApp& app;HWND window;~Restore(){app.FloatingWindow()=window;}} restore{*f.app,f.app->FloatingWindow()};
        f.app->FloatingWindow()=nullptr;f.Commit(id,value);};
    commitMain(panel::kReadoutPosYEdit,"3.25");Check(f.first.bones[0].trans[1]==3.25f&&f.history.UndoCount()==1,"seed actual model A node for mixed floor history");
    f.Menu();Check(f.history.UndoCount()==2&&!f.app->state.floorVisible,"seed the actual floor node before plugin edit");
    camera_multiply_checks::PluginState plugin;StorageClient storage;storage.id="test.floor-history";f.storage.Api(storage);auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof serializer,&provider)==MMS_OK,"register the real mixed-floor plugin serializer");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;const auto api=edits.Api(client);std::uint64_t revision=0,next=0;MmsHandle tx=0;
    Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,Text("floor plugin"),revision,&tx)==MMS_OK&&edits.HasPending(),"begin actual plugin draft while floor is disabled");
    const auto bytes=f.history.RetainedBytes();const auto world=WorldDigest(f.simulation);bool changed=true;
    Check(ApplyBoundFloor(*f.app,true,changed)==MMS_E_BUSY&&!changed,"pending plugin draft excludes direct floor edit admission");f.Menu();
    Check(!f.app->state.floorVisible&&f.ground->getCollisionFlags()==-1&&f.history.UndoCount()==2&&!f.history.RedoCount()&&
        f.history.RetainedBytes()==bytes&&f.history.Revision(next)==MMS_OK&&next==revision&&edits.HasPending()&&f.Echo()&&WorldDigest(f.simulation)==world,
        "pending plugin draft excludes actual menu 285 without fallback, cancellation, revision or physical changes");
    const auto participant=plugin.Participant(Text(client.id));
    Check(api.join(api.context,tx,&participant)==MMS_OK&&api.prepare(api.context,tx)==MMS_OK&&api.commit(api.context,tx)==MMS_OK&&plugin.value==42,
        "original floor-blocking plugin transaction remains usable and commits into the same history");
    f.app->SelectedModelSlot()=1;commitMain(panel::kReadoutPosZEdit,"8.5");Check(f.second.bones[0].trans[2]==8.5f&&f.history.UndoCount()==4,"seed actual model B node after mixed plugin publication");f.Menu();
    Check(f.history.UndoCount()==5&&f.app->state.floorVisible==1,"two models, real plugin and two floor edits share chronological history");
    f.first.bones[1].trans[2]=17.125f;f.second.bones[1].trans[0]=-3.75f;f.app->CameraMode()=1;f.app->CurrentFrame()=73;f.app->PhysicsResetPending()=7;Source later(f);
    Check(TryBoundHistoryCommand(f.app.get(),false)&&!f.app->state.floorVisible&&f.ground->getCollisionFlags()==-1&&f.Echo(),"common UI undo routes floor after later unregistered poses");later.Unchanged();
    Check(f.history.History(false)==MMS_OK&&f.second.bones[0].trans[2]==0&&f.history.History(false)==MMS_OK&&plugin.value==0&&
        f.history.History(false)==MMS_OK&&f.app->state.floorVisible==1&&f.history.History(false)==MMS_OK&&f.first.bones[0].trans[1]==0,
        "mixed floor history undoes model B, plugin, first floor and model A in their real order");
    for(unsigned i=0;i<5;++i)Check(f.history.History(true)==MMS_OK,"redo one actual model/plugin/floor node");
    Check(f.first.bones[0].trans[1]==3.25f&&f.second.bones[0].trans[2]==8.5f&&plugin.value==42&&f.app->state.floorVisible==1&&
        f.ground->getCollisionFlags()==Flags(true),"mixed floor redo restores exact target fields and plugin state");
    Check(f.history.Reset()==MMS_OK,"retire mixed floor records before serializer cleanup");f.history.Collect();Check(f.storage.Unregister(storage,provider)==MMS_OK,"release mixed floor serializer after history cleanup");
}
void NotificationFailure() {
    Scene f;NativePoseTestBinding bound(*f.app,f.history);Source source(f);black_background_checks::RequestProbe probe(f);bool changed=false;
    Check(ApplyBoundFloor(*f.app,false,changed)==MMS_OK&&changed&&probe.requests==1&&!probe.geometry&&!probe.poseReadouts&&f.Echo(),
        "initial floor edit forwards only a real drawing request and never enters pose readout");source.Unchanged();
    probe.throwAfterForward=true;
    Check(ApplyBoundFloor(*f.app,true,changed)==MMS_OK&&changed&&probe.requests==2&&!probe.poseReadouts&&f.history.UndoCount()==2&&f.Echo()&&f.ground->getCollisionFlags()==Flags(true),
        "notification exception after the real floor request cannot undo or misreport an already-published adjustment");source.Unchanged();
    probe.throwAfterForward=false;f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
    Check(ApplyBoundFloor(*f.app,true,changed)==MMS_OK&&!changed&&probe.requests==2&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"floor no-op requests neither drawing nor author changes");
}
bool Same(const btVector3& a,const btVector3& b) {return std::fabs(a.x()-b.x())<1e-6f&&std::fabs(a.y()-b.y())<1e-6f&&std::fabs(a.z()-b.z())<1e-6f;}
void CompareWorlds(Scene& actual,Scene& ordinary) {
    Check(actual.simulation.world.LocalTime()==ordinary.simulation.world.LocalTime(),"controlled and ordinary floor use the same real Bullet accumulator");
    for(unsigned i=0;i<actual.simulation.bodies.size();++i){const auto& a=*actual.simulation.bodies[i];const auto& b=*ordinary.simulation.bodies[i];
        Check(Same(a.getWorldTransform().getOrigin(),b.getWorldTransform().getOrigin())&&Same(a.getLinearVelocity(),b.getLinearVelocity())&&
            Same(a.getAngularVelocity(),b.getAngularVelocity())&&a.getCollisionFlags()==b.getCollisionFlags()&&a.getActivationState()==b.getActivationState(),
            "actual next CPU step matches the ordinary floor command for every resident dynamic body");}
}
void CpuCollisionSteps() {
    std::array<btScalar,2> height{},velocity{};
    for(unsigned enabled=0;enabled<2;++enabled){Scene actual,ordinary;
        const auto position=[&](Scene& f){auto* body=f.simulation.bodies[3].get();auto transform=btTransform::getIdentity();transform.setOrigin(btVector3(-20,.45f,0));
            body->setWorldTransform(transform);body->setInterpolationWorldTransform(transform);body->setLinearVelocity(btVector3(0,0,0));body->setAngularVelocity(btVector3(0,0,0));
            f.simulation.world.performDiscreteCollisionDetection();f.Initial(static_cast<unsigned char>(!enabled),Flags(!enabled));};
        position(actual);position(ordinary);NativePoseTestBinding bound(*actual.app,actual.history);
        black_background_checks::Source poses(actual);Source initial(actual);actual.Menu();ordinary.Menu();
        Check(actual.history.UndoCount()==1&&actual.app->state.floorVisible==enabled&&actual.ground->hasContactResponse()==bool(enabled)&&
            actual.ground->getCollisionFlags()==ordinary.ground->getCollisionFlags(),"real floor menu target matches the ordinary collision-response flags before the next step");initial.Unchanged();
        const auto step=actual.simulation.world.StepSerial(),isolated=WorldDigest(actual.isolated);
        actual.simulation.world.stepSimulation(btScalar(1)/60,1,btScalar(1)/60);ordinary.simulation.world.stepSimulation(btScalar(1)/60,1,btScalar(1)/60);
        Check(actual.simulation.world.StepSerial()>step&&WorldDigest(actual.isolated)==isolated,"exercise an actual CPU world step without touching the unrelated world");CompareWorlds(actual,ordinary);poses.Unchanged();
        height[enabled]=actual.simulation.bodies[3]->getWorldTransform().getOrigin().y();velocity[enabled]=actual.simulation.bodies[3]->getLinearVelocity().y();
        const auto afterStep=WorldDigest(actual.simulation,actual.ground.get());
        Check(actual.history.History(false)==MMS_OK&&WorldDigest(actual.simulation,actual.ground.get())==afterStep,
            "floor undo after simulation preserves every newer physical state and restores only the two target fields");ordinary.Menu();
        actual.simulation.world.stepSimulation(btScalar(1)/60,1,btScalar(1)/60);ordinary.simulation.world.stepSimulation(btScalar(1)/60,1,btScalar(1)/60);
        CompareWorlds(actual,ordinary);poses.Unchanged();
    }
    Check(height[1]>height[0]+.001f&&velocity[1]>velocity[0]+.01f,"the real enabled CPU floor supports the penetrating sphere while disabled floor allows it to fall");
}
}
void NativeFloorHistoryChecks() {
    struct BulletAllocators {BulletAllocators(){btAlignedAllocSetCustom(HistoryAlloc,HistoryFree);btAlignedAllocSetCustomAligned(HistoryAlignedAlloc,HistoryAlignedFree);}
        ~BulletAllocators(){btAlignedAllocSetCustomAligned(nullptr,nullptr);btAlignedAllocSetCustom(nullptr,nullptr);}} allocationHooks;
    floor_history_checks::RoutingAndRawFlags();floor_history_checks::RecordingModes();
    floor_history_checks::BudgetAndAllocationFailures();floor_history_checks::GatesAndOrdinaryPath();
    floor_history_checks::RootAndResourceIdentity();floor_history_checks::CandidatesAndNoAllocation();
    floor_history_checks::PendingAndMixedOrder();floor_history_checks::NotificationFailure();floor_history_checks::CpuCollisionSteps();
}
