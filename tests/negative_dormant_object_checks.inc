namespace {
struct NegativeDormantObjectFixture : NegativeGlobalFixture {
    NegativeDormantObjectFixture() {
        for(unsigned slot=0;slot<2;++slot) {
            auto* object=new mdl::AccessoryRecord{};
            object->scale=object->opacity=1;object->parentModel=-1;object->order=slot;
            app->AccessorySlot(slot)=object;
            app->AccessoryKeys(slot)=mdl::KeyPoolCreate<mdl::AccessoryKey>(4);
            auto& zero=app->AccessoryKeys(slot)[0];
            zero.scale=zero.opacity=1;zero.parentModel=-1;zero.visible=1;
        }
        app->AccessoryRenderSplitOrder()=2;
        app->CameraFov()=45;app->CurrentFrame()=-10;
    }
    ~NegativeDormantObjectFixture() {
        history.Reset();negative_frames::Reset(app.get());history.Collect();
        for(unsigned slot=0;slot<2;++slot) {
            if(auto* object=app->AccessorySlot(slot))DeleteAccessory(object,1);
            app->AccessorySlot(slot)=nullptr;
            mdl::KeyPoolDestroy(app->AccessoryKeys(slot));app->AccessoryKeys(slot)=nullptr;
        }
        curve_host::Reset(app.get());
    }
};

void NegativeDormantCandidateStaleChecks();
void NegativeDormantModelHistoryChecks();
void NegativeDormantObjectHistoryChecks() {
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(bool editCamera:{true,false}) {
        std::printf("RUN HIS-03-A dormant A / %s / %s\n",editCamera?"camera":"accessory B",
            mode==Mode::AllOperations?"all operations":"explicit only");std::fflush(stdout);
        NegativeDormantObjectFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"HIS-03-A choose common history mode");
        auto* a=f.app->AccessorySlot(0);auto* b=f.app->AccessorySlot(1);
        auto* aKeys=f.app->AccessoryKeys(0);
        const auto identity=plugins::SceneObjectIdentity(a);
        a->position[0]=4;
        Check(negative_frames::RegisterAccessory(f.app.get(),0,-10),"HIS-03-A register A negative key before real deletion");
        const unsigned target=editCamera?2:1001;
        const auto edit=[&](float value) {
            if(editCamera) {
                f.app->CameraPositionX()=value;
                return negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10);
            }
            b->position[0]=value;
            return negative_frames::RegisterAccessory(f.app.get(),1,-10);
        };
        const auto pose=[&](){return editCamera?f.app->CameraPositionX():b->position[0];};
        double value=0;
        Check(edit(6) && f.history.UndoCount()==2 && negative_frames::Count(f.app.get())==2,
            "HIS-03-A seed surviving negative track in the same journal");
        auto owner=plugins::RetainSceneObject(*f.app,MMS_OBJECT_ACCESSORY,0);
        std::weak_ptr<plugins::RetainedSceneObject> retained=owner;
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("HIS-03-A delete A",owner->CaptureRemoval(1024*1024),tx)==MMS_OK &&
            f.history.CommitNative(std::move(tx))==MMS_OK,
            "HIS-03-A commit real retained object removal through common history");
        Check(!f.app->AccessorySlot(0) && !owner->Present() && !plugins::SceneObjectPresent(a,identity.id) &&
            f.app->AccessoryKeys(0)!=aKeys && f.app->AccessorySlot(1)==b && b->order==0 &&
            f.app->AccessoryRenderSplitOrder()==1 && f.history.UndoCount()==3,
            "HIS-03-A deletion publishes membership identity key-pool exchange and survivor ordering");
        owner.reset();
        Check(!retained.expired() && negative_frames::Count(f.app.get())==2 &&
            !NegativeTestGlobal(*f.app,1000,-10,value),
            "HIS-03-A common history alone retains absent A while its dormant key stays out of active capture");

        Check(edit(9),editCamera?
            "HIS-03-A RED: camera negative registration succeeds while deleted A is retained by common history":
            "HIS-03-A RED: B negative registration succeeds while deleted A is retained by common history");
        Check(f.history.UndoCount()==4 && !f.history.RedoCount() && negative_frames::Count(f.app.get())==2 &&
            NegativeTestGlobal(*f.app,target,-10,value) && value==9 && pose()==9 && !f.app->AccessorySlot(0),
            "HIS-03-A survivor edit adds one node and preserves dormant A");
        Check(f.history.History(false)==MMS_OK && f.history.UndoCount()==3 && f.history.RedoCount()==1 &&
            pose()==6 && NegativeTestGlobal(*f.app,target,-10,value) && value==6 && !f.app->AccessorySlot(0),
            "HIS-03-A undo survivor edit restores its previous negative key while A remains deleted");
        Check(f.history.History(false)==MMS_OK && f.history.UndoCount()==2 && f.history.RedoCount()==2 &&
            f.app->AccessorySlot(0)==a && f.app->AccessoryKeys(0)==aKeys &&
            plugins::SceneObjectPresent(a,identity.id) && b->order==1 && f.app->AccessoryRenderSplitOrder()==2 &&
            NegativeTestGlobal(*f.app,1000,-10,value) && value==4,
            "HIS-03-A undo deletion restores original A identity pool ordering and exact negative key");
        a->position[0]=99;
        negative_frames::EvaluateGlobals(f.app.get(),-10);
        Check(a->position[0]==4 && pose()==6 && f.app->CurrentFrame()==-10,
            "HIS-03-A restored A negative key evaluates to authored value without moving the playhead");
        Check(f.history.History(true)==MMS_OK && !f.app->AccessorySlot(0) && !retained.expired() &&
            f.history.UndoCount()==3 && f.history.RedoCount()==1,
            "HIS-03-A redo deletion parks A again in common history");
        Check(f.history.History(true)==MMS_OK && pose()==9 &&
            NegativeTestGlobal(*f.app,target,-10,value) && value==9 && f.history.UndoCount()==4,
            "HIS-03-A redo survivor negative edit also succeeds with dormant A");
        Check(f.history.History(false)==MMS_OK && pose()==6,
            "HIS-03-A undo survivor edit before branching after redo deletion");
        Check(edit(12) && f.history.UndoCount()==4 && !f.history.RedoCount() && !f.app->AccessorySlot(0) &&
            NegativeTestGlobal(*f.app,target,-10,value) && value==12 && pose()==12,
            "HIS-03-A fresh negative registration after redo deletion replaces the redo branch");
        Check(f.history.History(false)==MMS_OK && pose()==6 && f.history.History(false)==MMS_OK &&
            f.app->AccessorySlot(0)==a && NegativeTestGlobal(*f.app,1000,-10,value) && value==4,
            "HIS-03-A branched edit still undoes before deletion and preserves A negative payload");
    }
    NegativeDormantCandidateStaleChecks();
    NegativeDormantModelHistoryChecks();
}

void NegativeDormantCandidateStaleChecks() {
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(bool restoreDormant:{false,true}) {
        std::printf("RUN HIS-03-A candidate stale / %s / %s\n",
            restoreDormant?"dormant A returns":"live B leaves",
            mode==Mode::AllOperations?"all operations":"explicit only");std::fflush(stdout);
        NegativeDormantObjectFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"HIS-03-A stale candidate chooses common history mode");
        auto* a=f.app->AccessorySlot(0);auto* b=f.app->AccessorySlot(1);
        a->position[0]=4;b->position[0]=6;f.app->CameraPositionX()=8;
        Check(negative_frames::RegisterAccessory(f.app.get(),0,-10) &&
            negative_frames::RegisterAccessory(f.app.get(),1,-10) &&
            negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10),
            "HIS-03-A seed A B and camera negative keys for membership invalidation");
        auto owner=plugins::RetainSceneObject(*f.app,MMS_OBJECT_ACCESSORY,0);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("HIS-03-A stale setup delete A",owner->CaptureRemoval(1024*1024),transaction)==MMS_OK &&
            f.history.CommitNative(std::move(transaction))==MMS_OK && !f.app->AccessorySlot(0) && !owner->Present(),
            "HIS-03-A stale setup uses real common-history deletion of A");
        owner.reset();
        f.app->CurrentFrame()=20;
        Check(negative_frames::SelectCell(f.app.get(),-10,0,false),
            "HIS-03-A select negative camera key while playhead is positive");
        const auto keys=NegativeTestRecord(*f.app,negative_frames::kPmmKey);
        const auto selection=NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey);
        const auto count=negative_frames::Count(f.app.get());
        const auto undo=f.history.UndoCount(),redo=f.history.RedoCount();
        std::uint64_t revision=0,afterRevision=0;
        Check(count==3 && undo==4 && f.history.Revision(revision)==MMS_OK,
            "HIS-03-A snapshot includes dormant key and real deletion history");
        std::shared_ptr<plugins::HistoryChange> change;
        Check(negative_frames::PlanSelectedDeletion(*f.app,change)==MMS_OK && change &&
            f.history.PrepareNative("HIS-03-A positive-playhead negative deletion",change,transaction)==MMS_OK && transaction,
            "HIS-03-A prepare negative camera deletion at positive playhead with dormant A and live B");
        f.app->SceneModified()=0;
        // Only simulate an external membership change between preparation and
        // publication here; A's original deletion above uses the real journal.
        if(restoreDormant)f.app->AccessorySlot(0)=a;
        else f.app->AccessorySlot(1)=nullptr;
        const auto externalKeys=NegativeTestRecord(*f.app,negative_frames::kPmmKey);
        const auto externalSelection=NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey);
        const auto result=f.history.CommitNative(std::move(transaction));
        const bool keptExternalState=
            f.app->AccessorySlot(0)==(restoreDormant?a:nullptr) &&
            f.app->AccessorySlot(1)==(restoreDormant?b:nullptr) &&
            NegativeTestRecord(*f.app,negative_frames::kPmmKey)==externalKeys &&
            NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==externalSelection;
        // Restore the simulated external change before fixture/history teardown.
        f.app->AccessorySlot(0)=nullptr;f.app->AccessorySlot(1)=b;
        Check(result==MMS_E_STALE,restoreDormant?
            "HIS-03-A restored dormant A rejects prepared negative deletion as STALE at positive playhead":
            "HIS-03-A departed live B rejects prepared negative deletion as STALE at positive playhead");
        Check(keptExternalState && negative_frames::Count(f.app.get())==count &&
            NegativeTestRecord(*f.app,negative_frames::kPmmKey)==keys &&
            NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==selection &&
            f.history.UndoCount()==undo && f.history.RedoCount()==redo &&
            f.history.Revision(afterRevision)==MMS_OK && afterRevision==revision &&
            !f.app->SceneModified() && f.app->CurrentFrame()==20 &&
            a->position[0]==4 && b->position[0]==6 && f.app->CameraPositionX()==8,
            "HIS-03-A stale membership candidate preserves keys selection journal revision dirty state poses and playhead");
        double value=0;
        Check(f.history.History(false)==MMS_OK && f.app->AccessorySlot(0)==a &&
            NegativeTestGlobal(*f.app,1000,-10,value) && value==4 &&
            NegativeTestGlobal(*f.app,1001,-10,value) && value==6 &&
            NegativeTestGlobal(*f.app,2,-10,value) && value==8,
            "HIS-03-A rejected candidate leaves real deletion undoable and all three negative payloads intact");
    }
}

struct NegativeDormantModelFixture : NegativeGlobalFixture {
    // Slots 0/1 belong to the base's stack-backed fixtures. Only these separate
    // heap models may be retained or passed to the production DeleteModel.
    static constexpr unsigned aSlot=4,bSlot=5;
    NegativeDormantModelFixture() {
        // The base pose-only models have no external-parent order tables.
        // Declare that empty scope so real removal can validate every survivor.
        first.m->boneOrderCount=second.m->boneOrderCount=0;
        for(unsigned slot:{aSlot,bSlot}) {
            auto* model=static_cast<mdl::ModelRecord*>(std::calloc(1,sizeof(mdl::ModelRecord)));
            Check(model!=nullptr,"HIS-03-A allocate owned model record");
            app->ModelSlot(slot)=reinterpret_cast<unsigned char*>(model);
            model->boneCount=1;
            model->boneTable=static_cast<mdl::BoneRecord*>(std::calloc(1,sizeof(mdl::BoneRecord)));
            model->bonePhysicsState=static_cast<unsigned char*>(std::calloc(1,1));
            model->boneSelection=static_cast<unsigned char*>(std::calloc(1,1));
            Check(model->boneTable && model->bonePhysicsState && model->boneSelection,
                "HIS-03-A allocate model-owned bone and flag arrays");
            model->boneKeys=mdl::BoneKeyPoolCreate(4);
            model->morphKeys=mdl::MorphKeyPoolCreate(4);
            model->displayKeys=mdl::KeyPoolCreate<mdl::DisplayKey>(1);
            Check(model->boneKeys && model->morphKeys && model->displayKeys,
                "HIS-03-A allocate tracked native model key pools");
            model->boneTable[0].parent=-1;model->boneTable[0].slotIndex=-1;
            model->boneTable[0].rotQuat[3]=1;model->boneKeys[0].rotation[3]=1;
            for(int i=0;i<4;++i)model->boneTable[0].matInit[i*5]=1;
            model->comboSelIndex=model->comboSelIndex2=static_cast<unsigned char>(slot-2);
            model->displayKeys[0].visible=1;model->loadComplete=1;
        }
        app->CameraFov()=45;app->CameraParentModel()=-1;app->CurrentFrame()=-10;
    }
    ~NegativeDormantModelFixture() {
        // The journal disposes parked heap models; the scene still owns live
        // ones. Never free a raw pointer cached before history collection.
        history.Reset();negative_frames::Reset(app.get());history.Collect();
        for(unsigned slot:{aSlot,bSlot}) {
            if(auto* model=app->ModelSlot(slot))DeleteModel(model,1);
            app->ModelSlot(slot)=nullptr;
        }
    }
};

void NegativeDormantModelHistoryChecks() {
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(bool editCamera:{true,false}) {
        std::printf("RUN HIS-03-A retained model A / %s / %s\n",editCamera?"camera":"model B bone",
            mode==Mode::AllOperations?"all operations":"explicit only");std::fflush(stdout);
        NegativeDormantModelFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"HIS-03-A model deletion chooses common history mode");
        auto* a=f.app->ModelSlot(f.aSlot);auto* b=f.app->ModelSlot(f.bSlot);
        auto& model=*mdl::Mdl(a);auto& bone=model.boneTable[0];
        auto* boneTable=model.boneTable;auto* boneKeys=model.boneKeys;
        const auto identity=plugins::SceneObjectIdentity(a);
        model.boneKeys[3].frame=23;model.boneKeys[3].position[0]=17;
        bone.trans[0]=4;bone.rotQuat[0]=.6f;bone.rotQuat[3]=.8f;
        Check(bone_scale::SetPose(a,0,Scale(2)) && negative_frames::RegisterBone(a,0,-10),
            "HIS-03-A register owned model A negative bone position rotation and scale");
        const auto edit=[&](float value) {
            if(editCamera) {
                f.app->CameraPositionX()=value;
                return negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10);
            }
            mdl::Mdl(b)->boneTable[0].trans[0]=value;
            return negative_frames::RegisterBone(b,0,-10);
        };
        const auto pose=[&](){return editCamera?f.app->CameraPositionX():mdl::Mdl(b)->boneTable[0].trans[0];};
        const auto key=[&](double expected) {
            double value=0;
            return (editCamera?NegativeTestGlobal(*f.app,2,-10,value):NegativeTestBone(*f.app,f.bSlot,0,-10,value)) &&
                value==expected;
        };
        Check(edit(6) && f.history.UndoCount()==2 && negative_frames::Count(f.app.get())==2,
            "HIS-03-A seed camera or model B negative track before deleting model A");
        auto owner=plugins::RetainSceneObject(*f.app,MMS_OBJECT_MODEL,f.aSlot);
        std::weak_ptr<plugins::RetainedSceneObject> retained=owner;
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("HIS-03-A delete owned model A",owner->CaptureRemoval(1024*1024),transaction)==MMS_OK &&
            f.history.CommitNative(std::move(transaction))==MMS_OK && !owner->Present() &&
            !f.app->ModelSlot(f.aSlot) && !plugins::SceneObjectPresent(a,identity.id) &&
            f.app->ModelSlot(f.bSlot)==b && f.history.UndoCount()==3,
            "HIS-03-A real model removal publishes absent membership and identity in common history");
        owner.reset();double value=0;
        Check(!retained.expired() && negative_frames::Count(f.app.get())==2 &&
            !NegativeTestBone(*f.app,f.aSlot,0,-10,value),
            "HIS-03-A history alone retains deleted heap model and its dormant bone key");
        Check(edit(9) && key(9) && pose()==9 && f.history.UndoCount()==4 && !f.history.RedoCount() &&
            !f.app->ModelSlot(f.aSlot) && negative_frames::Count(f.app.get())==2,
            "HIS-03-A camera or B bone registration succeeds with a retained deleted model");
        Check(f.history.History(false)==MMS_OK && key(6) && pose()==6 && !f.app->ModelSlot(f.aSlot) &&
            f.history.UndoCount()==3 && f.history.RedoCount()==1,
            "HIS-03-A undo surviving edit restores its negative payload and pose before model deletion");
        Check(f.history.History(false)==MMS_OK && f.app->ModelSlot(f.aSlot)==a &&
            plugins::SceneObjectPresent(a,identity.id) && model.boneTable==boneTable && model.boneKeys==boneKeys &&
            model.boneKeys[3].frame==23 && model.boneKeys[3].position[0]==17 &&
            NegativeTestBone(*f.app,f.aSlot,0,-10,value) && value==4 &&
            bone.trans[0]==4 && bone.rotQuat[0]==.6f && bone.rotQuat[3]==.8f && bone_scale::Pose(a,0)==Scale(2) &&
            f.history.UndoCount()==2 && f.history.RedoCount()==2,
            "HIS-03-A undo model deletion restores original allocations native keys negative bone pose and scale");
        bone.trans[0]=99;bone.rotQuat[0]=0;bone.rotQuat[3]=1;
        Check(bone_scale::SetPose(a,0,Scale(7)),"HIS-03-A disturb restored model pose to verify negative-key evaluation");
        negative_frames::EvaluateModel(a,-10);
        Check(bone.trans[0]==4 && bone.rotQuat[0]==.6f && bone.rotQuat[3]==.8f &&
            bone_scale::Pose(a,0)==Scale(2) && pose()==6 && f.app->CurrentFrame()==-10,
            "HIS-03-A restored model negative payload evaluates authored translation rotation and scale");
        Check(f.history.History(true)==MMS_OK && !f.app->ModelSlot(f.aSlot) && !retained.expired() &&
            f.history.UndoCount()==3 && f.history.RedoCount()==1,
            "HIS-03-A redo real model deletion retains A again");
        Check(f.history.History(true)==MMS_OK && key(9) && pose()==9 && f.history.UndoCount()==4 && !f.history.RedoCount(),
            "HIS-03-A redo camera or B bone edit succeeds while model A stays absent");
        Check(edit(12) && key(12) && pose()==12 && !f.app->ModelSlot(f.aSlot) && f.history.UndoCount()==5,
            "HIS-03-A fresh camera or B bone registration remains editable after redo model deletion");
        Check(f.history.History(false)==MMS_OK && key(9) && pose()==9 &&
            f.history.History(false)==MMS_OK && key(6) && pose()==6 &&
            f.history.History(false)==MMS_OK && f.app->ModelSlot(f.aSlot)==a &&
            NegativeTestBone(*f.app,f.aSlot,0,-10,value) && value==4 && bone_scale::Pose(a,0)==Scale(2),
            "HIS-03-A subsequent edits still undo in order before restoring model A and its bone payload");
        Check(f.history.History(true)==MMS_OK && !f.app->ModelSlot(f.aSlot) && !retained.expired(),
            "HIS-03-A leave model A genuinely parked for retained-owner disposal");
    }
}
}
