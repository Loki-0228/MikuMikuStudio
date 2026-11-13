// Included by native_key_commands.cpp inside the plugins namespace.
namespace signed_key_delete {
using Ledger=resource_budget::Collector;
struct Failure {MmsResult result;};
constexpr std::uint64_t preparationLimit=64ull*1024*1024;
struct Change final:HistoryChange,std::enable_shared_from_this<Change> {
    MMDApp& app;SharedHistory& history;
    const std::uint64_t binding,scene,epoch;
    const int initialFrame;
    unsigned modelSlot=0;
    std::shared_ptr<GlobalKeyDeletion> globals;
    std::shared_ptr<ModelKeyDeletion> model;
    std::shared_ptr<negative_frames::SelectedDeletion> negative;
    std::uint64_t serial=1;
    bool published=false,applied=false;
    Change(MMDApp& a,SharedHistory& h):app(a),history(h),binding(BoundNativePoseGeneration(&a)),
        scene(a.SceneGeneration()),epoch(h.NativeEpoch()),initialFrame(a.CurrentFrame()){}
    bool Current() const noexcept {
        return BoundNativePoseHistory(&app)==&history&&(published||BoundNativePoseGeneration(&app)==binding)&&
            app.SceneGeneration()==scene&&history.NativeEpoch()==epoch&&negative_frames::DeletionInteractionReady(app,published);
    }
    void VisitOwners(void* context,ModelDeletionOwnerVisitor visit)const {
        if(globals)globals->VisitRetainedOwners(context,visit);
        if(model)model->VisitRetainedModels(context,visit);
    }
    struct Candidate final:PreparedHistoryChange {
        std::shared_ptr<Change> source;
        const std::uint64_t serial,binding;
        const int frame;
        const bool forward,initial;
        bool consumed=false;
        std::unique_ptr<PreparedGlobalKeyDeletion> globals;
        std::unique_ptr<PreparedModelKeyDeletion> model;
        std::unique_ptr<PreparedHistoryChange> negative;
        mutable Ledger owners;
        std::uint64_t bytes=0;
        Candidate(std::shared_ptr<Change> s,bool f,bool i):source(std::move(s)),serial(source->serial),binding(BoundNativePoseGeneration(&source->app)),
            frame(source->app.CurrentFrame()),forward(f),initial(i){}
        void VisitOwners(void* context,ModelDeletionOwnerVisitor visit)const {
            source->VisitOwners(context,visit);
            if(globals)globals->VisitRetainedOwners(context,visit);
            if(model)model->VisitRetainedModels(context,visit);
        }
        MmsResult Measure() {
            MmsResult result=MMS_OK;
            struct Context {Ledger& ledger;MmsResult& result;} context{owners,result};
            VisitOwners(&context,[](void* raw,const std::shared_ptr<RetainedSceneObject>& owner){
                auto& c=*static_cast<Context*>(raw);
                if(c.result==MMS_OK)c.result=owner?owner->MeasureBookkeeping(c.ledger):MMS_E_STALE;
            });
            if(result)return result;
            return owners.Result().Complete()?MMS_OK:MMS_E_NOT_READY;
        }
        bool IsCurrent()const noexcept override {
            const auto& s=*source;
            if(consumed||serial!=s.serial||binding!=BoundNativePoseGeneration(&s.app)||forward==s.applied||initial==s.published||!s.Current()||
                frame!=s.app.CurrentFrame()||(initial&&frame!=s.initialFrame)||
                (globals&&!globals->IsCurrent())||(model&&!model->IsCurrent())||
                !negative||!negative->IsCurrent()||!owners.Result().Complete())return false;
            bool covered=true;
            struct Context {Ledger& ledger;bool& covered;} context{owners,covered};
            VisitOwners(&context,[](void* raw,const std::shared_ptr<RetainedSceneObject>& owner){
                auto& c=*static_cast<Context*>(raw);
                if(c.covered)c.covered=owner&&owner->CoversBookkeeping(c.ledger);
            });
            return covered;
        }
        void Publish()noexcept override {
            if(!IsCurrent())std::terminate();
            // Both guards refer to the old source. Native publication changes
            // its zero-frame dependency, so the negative half publishes the
            // already prepared result without revalidating that old baseline.
            if(globals)globals->Publish();
            if(model)model->Publish();
            negative->Publish();
            source->applied=forward;source->published=true;++source->serial;consumed=true;
        }
        bool TargetRetainedBytes(std::uint64_t& out)const noexcept override {out=bytes;return true;}
    };
    MmsResult PrepareImpl(bool forward,bool initial,std::unique_ptr<PreparedHistoryChange>& out) noexcept {
        out.reset();
        try {
            if(!Current()||initial==published||forward==applied||(initial&&app.CurrentFrame()!=initialFrame))return MMS_E_STALE;
            if(serial==UINT64_MAX)return MMS_E_NOT_READY;
            const auto fixed=RetainedBytes(),limit=history.MaxBytes();
            if(fixed>=limit)return MMS_E_MEMORY;
            std::uint64_t spent=sizeof(Candidate),ledgerBytes=0;
            if(spent>(std::min)(preparationLimit,limit-fixed))return MMS_E_MEMORY;
            auto candidate=std::make_unique<Candidate>(shared_from_this(),forward,initial);
            const auto available=[&]()->std::uint64_t {
                const auto retained=Ledger::Sum(fixed,candidate->owners.Result().totalBytes);
                if(retained>limit)throw Failure{MMS_E_MEMORY};
                const auto cap=(std::min)(preparationLimit,limit-retained);
                if(spent>cap)throw Failure{MMS_E_MEMORY};
                return cap-spent;
            };
            const auto measure=[&] {
                if(const auto result=candidate->Measure())throw Failure{result};
                const auto current=candidate->owners.StorageBytes();
                spent=Ledger::Sum(spent,current-ledgerBytes);ledgerBytes=current;
                (void)available();
            };
            measure();
            auto remaining=available();const auto granted=remaining;
            if(globals){if(auto result=globals->PrepareSignedBudgeted(forward,remaining,candidate->globals))return result;}
            if(model){if(auto result=model->PrepareSignedBudgeted(forward,remaining,candidate->model))return result;}
            spent=Ledger::Sum(spent,granted-remaining);
            // Late pose parents are charged before the negative participant can
            // consume the remainder. Shared model/parent holders count once.
            measure();
            negative_frames::DeletionTargets targets{candidate->globals.get(),candidate->model.get(),modelSlot};
            remaining=available();const auto negativeGranted=remaining;
            if(auto result=negative->PrepareSigned(forward,initial,targets,remaining,candidate->negative))return result;
            spent=Ledger::Sum(spent,negativeGranted-remaining);
            measure();
            candidate->bytes=Ledger::Sum(fixed,candidate->owners.Result().totalBytes);
            if(!candidate->IsCurrent())return MMS_E_STALE;
            out=std::move(candidate);return MMS_OK;
        }catch(const Failure& f){return f.result;}
        catch(const std::bad_alloc&){return MMS_E_MEMORY;}
        catch(const std::length_error&){return MMS_E_MEMORY;}
        catch(const std::invalid_argument&){return MMS_E_ARGUMENT;}
        catch(...){return MMS_E_FAILED;}
    }
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{return PrepareImpl(forward,false,out);}
    MmsResult PrepareInitial(SharedHistory& requested,bool,std::unique_ptr<PreparedHistoryChange>& out)override {
        out.reset();if(&requested!=&history)return MMS_E_ARGUMENT;
        return PrepareImpl(true,true,out);
    }
    std::uint64_t RetainedBytes()const noexcept override {
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);
        const auto add=[&](const auto& part){if(part){const auto amount=part->RetainedBytes();bytes=amount>UINT64_MAX-bytes?UINT64_MAX:bytes+amount;}};
        add(globals);add(model);add(negative);return bytes;
    }
    bool RequiresPhysicsReset()const noexcept override {
        return (globals&&globals->RequiresPhysicsReset())||(model&&model->RequiresPhysicsReset())||(negative&&negative->RequiresPhysicsReset());
    }
    HistoryEffects Effects()const noexcept override {
        auto effects=HistoryEffects::None;
        if(globals)effects=effects|globals->Effects();if(model)effects=effects|model->Effects();
        if(negative)effects=effects|negative->Effects();return effects;
    }
};
}
MmsResult PlanSignedKeyDeletion(MMDApp& app,SharedHistory& history,bool automaticInterpolation,
    bool selectedAtCurrentFrame,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();negative_frames::DeletionSelection selection;
    if(auto result=negative_frames::QueryDeletionSelection(app,history,selection))return result;
    return Boundary([&]()->MmsResult {
        auto change=std::make_shared<signed_key_delete::Change>(app,history);
        const bool fallback=selectedAtCurrentFrame&&!selection.negative&&!selection.positive;
        if(app.CameraMode()) {
            if(auto result=PlanSignedGlobalKeyDeletion(app,change->globals,fallback))return result;
        }else {
            change->modelSlot=app.SelectedModelSlot();
            if(auto result=PlanSignedModelKeyDeletion(app,change->modelSlot,automaticInterpolation,change->model,fallback))return result;
        }
        if(auto result=negative_frames::PlanSignedSelectedDeletion(app,change->negative,bool(change->globals||change->model)))return result;
        if(!change->globals&&!change->model&&!change->negative)return MMS_OK;
        if(!change->negative)return MMS_E_FAILED;
        out=std::move(change);return MMS_OK;
    });
}
