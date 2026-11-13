#include "model_physics_resources.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_physics_resource_owner.hpp"
#include "mikumikustudio/owned_text.hpp"
#include "mikumikustudio/physics_scale.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include <btBulletDynamicsCommon.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mikumikustudio::plugins {
namespace {
struct OwnerEntry {
    std::weak_ptr<ModelPhysicsResources> observed;
    std::shared_ptr<ModelPhysicsResources> adopted;
};
std::map<unsigned char*,OwnerEntry> owners;
template<class T> void Unique(std::vector<T*>& values) {
    std::sort(values.begin(),values.end(),std::less<T*>{});
    values.erase(std::unique(values.begin(),values.end()),values.end());
}
// These allocation shapes follow the repository's VS 2022 14.44 STL headers.
// Other STL revisions must be audited before admitting a fresh capture; a
// reusable owner does not depend on any allocation layout. No guessed fallback.
#if !MMD_ORIGINAL_PROFILE
#if defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION == 143 && _MSVC_STL_UPDATE == 202503L && !_HAS_CXX23
struct CaptureBytes {
    std::uint64_t value=0;bool valid=true;
    void Add(std::uint64_t bytes)noexcept{
        if(!valid)return;
        if(bytes>=UINT64_MAX-value){valid=false;return;}
        value+=bytes;
    }
    template<class T>void Allocation(std::size_t count)noexcept{
        if(!valid)return;
        if(count>(std::numeric_limits<std::size_t>::max)()/sizeof(T)){valid=false;return;}
        auto bytes=std::uint64_t(count)*sizeof(T);
        // std::allocator adds this header/alignment slack for large x86/x64
        // allocations. General CRT heap metadata remains outside this ledger.
#if defined(_M_IX86) || defined(_M_X64)
        if(bytes>=std::_Big_allocation_threshold
#ifdef __cpp_aligned_new
            &&alignof(T)<=__STDCPP_DEFAULT_NEW_ALIGNMENT__
#endif
        ){
            if(bytes>(std::numeric_limits<std::size_t>::max)()-std::_Non_user_size){valid=false;return;}
            bytes+=std::_Non_user_size;
        }
#endif
        Add(bytes);
    }
    void VectorProxy()noexcept{
#if _ITERATOR_DEBUG_LEVEL != 0
        Allocation<std::_Container_proxy>(1);
#endif
    }
    template<class T>static std::size_t VectorLimit()noexcept{
        return (std::min)(std::size_t((std::numeric_limits<std::ptrdiff_t>::max)()),
            (std::numeric_limits<std::size_t>::max)()/sizeof(T));
    }
    template<class T>void Reserved(std::size_t count)noexcept{
        if(count>VectorLimit<T>()){valid=false;return;}
        VectorProxy();if(count)Allocation<T>(count);
    }
    template<class T>void Growing(std::size_t count)noexcept{
        const auto limit=VectorLimit<T>();if(count>limit){valid=false;return;}
        VectorProxy();std::size_t capacity=0;
        // Each push_back reallocation uses max(size+1,capacity+capacity/2).
        // Sum every actual capacity, so retired buffers are not refunded and
        // simultaneous old/new storage is bounded without a safety multiplier.
        while(valid&&capacity<count){const auto required=capacity+1;
            capacity=capacity>limit-capacity/2?limit:(std::max)(required,capacity+capacity/2);
            Allocation<T>(capacity);
        }
    }
    template<class T>void SeparateShared()noexcept{Add(sizeof(T));Add(sizeof(std::_Ref_count<T>));}
};
#endif
struct CaptureCounts { std::size_t bodies=0,motions=0,joints=0; };
MmsResult InspectCapture(const mdl::ModelRecord& model,btDiscreteDynamicsWorld* world,CaptureCounts& counts)noexcept{
    if((model.rigidCount&&!model.rigidTable)||(model.jointCount&&!model.jointTable)||
        ((model.rigidCount||model.jointCount)&&!world))return MMS_E_ARGUMENT;
    for(unsigned i=0;i<model.rigidCount;++i){
        auto* body=static_cast<btRigidBody*>(model.rigidTable[i].body);if(!body)continue;
        for(unsigned previous=0;previous<i;++previous)
            if(model.rigidTable[previous].body==body)return MMS_E_ARGUMENT;
        if(!physics_scale::BaseShape(body))return MMS_E_ARGUMENT;
        ++counts.bodies;if(body->getMotionState())++counts.motions;
    }
    const auto contains=[&](const btRigidBody* body){
        for(unsigned i=0;i<model.rigidCount;++i)if(model.rigidTable[i].body==body)return true;
        return false;
    };
    const int worldCount=world?world->getNumConstraints():0;if(worldCount<0)return MMS_E_ARGUMENT;
    for(unsigned i=0;i<model.jointCount;++i){
        const auto& joint=model.jointTable[i];const auto id=joint.constraint;if(id<0)continue;
        btTypedConstraint* found=nullptr;
        for(int j=0;j<worldCount;++j){auto* candidate=world->getConstraint(j);if(!candidate)return MMS_E_ARGUMENT;
            if(candidate->getUid()==id){if(found)return MMS_E_ARGUMENT;found=candidate;}}
        if(!found){
            const auto a=joint.rigidA,b=joint.rigidB;
            if(a<0||b<0||unsigned(a)>=model.rigidCount||unsigned(b)>=model.rigidCount||
                !model.rigidTable[a].body||!model.rigidTable[b].body)continue;
            return MMS_E_ARGUMENT;
        }
        for(unsigned previous=0;previous<i;++previous)
            if(model.jointTable[previous].constraint==id)return MMS_E_ARGUMENT;
        if(!contains(&found->getRigidBodyA())||!contains(&found->getRigidBodyB()))return MMS_E_ARGUMENT;
        ++counts.joints;
    }
    if(auto* dynamics=dynamic_cast<DynamicsWorld*>(world)){
        // Retained::Impl uses a checked reference cast to this exact family.
        if(!dynamic_cast<history_physics::Broadphase*>(dynamics->getBroadphase()))return MMS_E_NOT_READY;
    }else if(counts.bodies||counts.joints)return MMS_E_NOT_READY;
    return MMS_OK;
}
#endif
}
struct ModelPhysicsVersion::Impl {
    mdl::RigidRecord* rigids;
    mdl::JointRecord* jointTable;
    unsigned rigidCount,jointCount;
    std::vector<void*> bindings;
    std::vector<int> jointIds;
    std::vector<btRigidBody*> bodies;
    std::vector<btCollisionShape*> bodyShapes;
    std::vector<btMotionState*> bodyMotions;
    std::vector<btCollisionShape*> shapes;
    std::vector<btMotionState*> motions;
    std::vector<btTypedConstraint*> joints;
    std::shared_ptr<history_physics::Retained> membership;
    std::shared_ptr<history_physics::ImportScene> privateScene;
    std::shared_ptr<physics_scale::ResourceState> scale;
    std::weak_ptr<ModelPhysicsResources> owner;
    btDiscreteDynamicsWorld* world;
    PhysicsScene* sourceScene;
    bool owned=false, published=false;
    Impl(mdl::ModelRecord& model,btDiscreteDynamicsWorld* w):rigids(model.rigidTable),jointTable(model.jointTable),
        rigidCount(model.rigidCount),jointCount(model.jointCount),world(w),sourceScene(model.scenePtr) {
        if((rigidCount&&!rigids)||(jointCount&&!jointTable)||((rigidCount||jointCount)&&!world))
            throw std::invalid_argument("incomplete model physics resources");
        bindings.reserve(rigidCount);jointIds.reserve(jointCount);
        for(unsigned i=0;i<rigidCount;++i) {
            auto* body=static_cast<btRigidBody*>(rigids[i].body);bindings.push_back(body);
            if(!body)continue;
            if(std::find(bodies.begin(),bodies.end(),body)!=bodies.end())throw std::invalid_argument("aliased model rigid body");
            bodies.push_back(body);
            auto* base=const_cast<btCollisionShape*>(physics_scale::BaseShape(body));
            if(!base)throw std::invalid_argument("missing model collision shape");
            bodyShapes.push_back(base);bodyMotions.push_back(body->getMotionState());
            shapes.push_back(base);
            if(auto* motion=body->getMotionState())motions.push_back(motion);
        }
        Unique(shapes);Unique(motions);
        for(unsigned i=0;i<jointCount;++i) {
            const auto id=jointTable[i].constraint;jointIds.push_back(id);
            if(id<0)continue;
            btTypedConstraint* found=nullptr;
            for(int j=0;j<world->getNumConstraints();++j) if(world->getConstraint(j)->getUid()==id) {
                if(found)throw std::invalid_argument("ambiguous model constraint UID");
                found=world->getConstraint(j);
            }
            if(!found) {
                const auto a=jointTable[i].rigidA,b=jointTable[i].rigidB;
                if(a<0||b<0||unsigned(a)>=rigidCount||unsigned(b)>=rigidCount||!rigids[a].body||!rigids[b].body)continue;
                throw std::invalid_argument("missing model constraint resource");
            }
            if(std::find(joints.begin(),joints.end(),found)!=joints.end()||
                std::find(bodies.begin(),bodies.end(),&found->getRigidBodyA())==bodies.end()||
                std::find(bodies.begin(),bodies.end(),&found->getRigidBodyB())==bodies.end())
                throw std::invalid_argument("aliased or external model constraint");
            joints.push_back(found);
        }
    }
    bool Tables(const mdl::ModelRecord& m)const noexcept {
        if(m.rigidTable!=rigids||m.jointTable!=jointTable||m.rigidCount!=rigidCount||m.jointCount!=jointCount)return false;
        for(unsigned i=0;i<rigidCount;++i)if(rigids[i].body!=bindings[i])return false;
        for(unsigned i=0;i<jointCount;++i)if(jointTable[i].constraint!=jointIds[i])return false;
        for(size_t i=0;i<bodies.size();++i)if(physics_scale::BaseShape(bodies[i])!=bodyShapes[i]||bodies[i]->getMotionState()!=bodyMotions[i])return false;
        return true;
    }
    wchar_t* Name(size_t index)const noexcept {
        if(index<size_t(rigidCount)*2)return (index&1)?rigids[index/2].enText:rigids[index/2].jpText;
        index-=size_t(rigidCount)*2;return (index&1)?jointTable[index/2].enText:jointTable[index/2].jpText;
    }
    ~Impl() {
        if(!owned)return;
        // Scale nodes own affine wrappers and refer to bodies/joints. Release
        // their indexes/wrappers first, without reevaluating the model/world.
        scale.reset();
        if(world)for(auto* joint:joints)world->removeConstraint(joint);
        for(auto* joint:joints)delete joint;
        if(world)for(auto* body:bodies)world->removeRigidBody(body);
        for(auto* body:bodies)delete body;
        for(auto* motion:motions)delete motion;
        for(auto* shape:shapes)delete shape;
        const auto count=(size_t(rigidCount)+jointCount)*2;
        for(size_t i=0;i<count;++i) {
            auto* name=Name(i);if(!name)continue;
            bool previous=false;for(size_t j=0;j<i;++j)if(Name(j)==name){previous=true;break;}
            if(!previous)owned_text::Free(name);
        }
        std::free(rigids);std::free(jointTable);
    }
    void MeasureBookkeeping(resource_budget::Collector& out,bool includeMembership)const {
        out.Array(this,1);
        out.Vector(bindings);out.Vector(jointIds);out.Vector(bodies);out.Vector(shapes);out.Vector(motions);out.Vector(joints);
        out.Vector(bodyShapes);out.Vector(bodyMotions);
        if(scale)scale->MeasureBookkeeping(out);
        if(includeMembership&&membership)membership->MeasureResources(out);
    }
    void Measure(resource_budget::Collector& out,bool includeMembership)const {
        MeasureBookkeeping(out,includeMembership);out.Array(rigids,rigidCount);out.Array(jointTable,jointCount);
        if(scale)scale->MeasureResources(out);
        for(unsigned i=0;i<rigidCount;++i){out.RawText(rigids[i].jpText);out.RawText(rigids[i].enText);}
        for(unsigned i=0;i<jointCount;++i){out.RawText(jointTable[i].jpText);out.RawText(jointTable[i].enText);}
        for(auto* body:bodies)resource_budget::Body(body,out);
        for(auto* shape:shapes)resource_budget::Shape(shape,out);
        for(auto* joint:joints)resource_budget::Joint(joint,out);
    }
};
struct ModelPhysicsResources::Impl {
    unsigned char* model;
    PhysicsScene* destination;
    std::shared_ptr<ModelPhysicsVersion> current;
    bool bound=true;
    Impl(unsigned char* m,PhysicsScene* scene,std::shared_ptr<ModelPhysicsVersion> value):model(m),destination(scene),current(std::move(value)){}
    bool Current(const std::shared_ptr<ModelPhysicsVersion>& value)const noexcept {
        if(!bound||!value||current!=value)return false;
        const auto& m=*mdl::Mdl(model);const auto& v=*value->impl_;
        return m.scenePtr==(v.published?destination:v.sourceScene)&&v.Tables(m);
    }
    void Measure(resource_budget::Collector& out,bool membership)const {
        out.Array(this,1);
        const auto found=owners.find(model);
        if(found!=owners.end())out.Add(&*found,1,sizeof(*found)+4*sizeof(void*));
        current->MeasureResources(out,membership);
    }
    struct Candidate final:PreparedHistoryChange {
        std::shared_ptr<ModelPhysicsResources> owner;
        std::shared_ptr<ModelPhysicsVersion> from,to;
        std::unique_ptr<history_physics::Retained::Transition> replay;
        std::unique_ptr<history_physics::ImportScene::Publication> initial;
        std::shared_ptr<history_physics::Retained> incoming;
        std::unique_ptr<physics_scale::ResourceState::Exchange> scale;
        std::shared_ptr<resource_budget::Collector> resources;
        std::shared_ptr<history_physics::ImportScene> cleanupScene;
        uint64_t bytes=0;
        bool consumed=false;
        void Measure(resource_budget::Collector& out)const {
            out.Array(this,1);out.Array(owner.get(),1);out.Array(owner->impl_.get(),1);
            const auto found=owners.find(owner->impl_->model);
            if(found!=owners.end())out.Add(&*found,1,sizeof(*found)+4*sizeof(void*));
            from->MeasureResources(out,false);to->MeasureResources(out,false);
            const bool complete=initial?initial->MeasureTargetResources(out):replay->MeasureTargetResources(out);
            if(!complete)throw std::invalid_argument("stale model physics membership resources");
        }
        bool IsCurrent()const noexcept override {
            if(consumed||!owner->impl_->Current(from)||!scale->IsCurrent()||
                (initial?!initial->IsCurrent():!replay->IsCurrent()))return false;
            return resources->Covers([&](auto& out){Measure(out);});
        }
        void Publish()noexcept override {
            if(!IsCurrent())std::terminate();
            // The model owns a merely captured live baseline. This is the
            // exact publication at which old resources become history-owned.
            owner->AdoptOwnership();
            if(initial) {
                if(!initial->Publish())std::terminate();
                to->impl_->membership.swap(incoming);
                to->impl_->world=owner->impl_->destination->world;
                to->impl_->published=true;
                cleanupScene.swap(to->impl_->privateScene);
            } else if(!replay->Publish())std::terminate();
            scale->Publish();
            auto& m=*mdl::Mdl(owner->impl_->model);
            m.rigidTable=to->impl_->rigids;m.rigidCount=to->impl_->rigidCount;
            m.jointTable=to->impl_->jointTable;m.jointCount=to->impl_->jointCount;
            owner->impl_->current=to;
            consumed=true;
        }
        bool TargetRetainedBytes(uint64_t& out)const noexcept override {out=bytes;return true;}
    };
};
ModelPhysicsVersion::ModelPhysicsVersion(std::unique_ptr<Impl> value):impl_(std::move(value)){}
ModelPhysicsVersion::~ModelPhysicsVersion()=default;
mdl::RigidRecord* ModelPhysicsVersion::Rigids()const noexcept{return impl_->rigids;}
mdl::JointRecord* ModelPhysicsVersion::Joints()const noexcept{return impl_->jointTable;}
unsigned ModelPhysicsVersion::RigidCount()const noexcept{return impl_->rigidCount;}
unsigned ModelPhysicsVersion::JointCount()const noexcept{return impl_->jointCount;}
std::shared_ptr<history_physics::Retained> ModelPhysicsVersion::Membership()const noexcept{return impl_->membership;}
void ModelPhysicsVersion::MeasureResources(resource_budget::Collector& out,bool membership)const {out.Array(this,1);impl_->Measure(out,membership);}
void ModelPhysicsVersion::MeasureBookkeeping(resource_budget::Collector& out,bool membership)const {out.Array(this,1);impl_->MeasureBookkeeping(out,membership);}
ModelPhysicsResources::ModelPhysicsResources(std::unique_ptr<Impl> value):impl_(std::move(value)){}
ModelPhysicsResources::~ModelPhysicsResources() {
    const auto found=owners.find(impl_->model);
    if(found!=owners.end()&&!found->second.adopted&&found->second.observed.expired())owners.erase(found);
}
std::shared_ptr<ModelPhysicsResources> ModelPhysicsResources::Find(unsigned char* model)noexcept {
    const auto found=owners.find(model);return found==owners.end()?nullptr:found->second.observed.lock();
}
#if !MMD_ORIGINAL_PROFILE
MmsResult ModelPhysicsResources::MeasureCapturePreparation(unsigned char* model,std::uint64_t& bytes)noexcept{
    if(!model)return MMS_E_ARGUMENT;
    if(auto found=Find(model)){
        if(!found->IsCurrent(found->Current()))return MMS_E_STALE;
        bytes=0;return MMS_OK;
    }
#if defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION == 143 && _MSVC_STL_UPDATE == 202503L && !_HAS_CXX23
    const auto& native=*mdl::Mdl(model);auto* world=native.scenePtr?native.scenePtr->world:nullptr;
    CaptureBytes measured;
    measured.Add(sizeof(ModelPhysicsVersion::Impl));measured.SeparateShared<ModelPhysicsVersion>();
    measured.Add(sizeof(Impl));measured.SeparateShared<ModelPhysicsResources>();
    measured.Reserved<void*>(native.rigidCount);measured.Reserved<int>(native.jointCount);
    if(!measured.valid)return MMS_E_MEMORY;
    CaptureCounts counts;if(const auto result=InspectCapture(native,world,counts))return result;
    measured.Growing<btRigidBody*>(counts.bodies);
    measured.Growing<btCollisionShape*>(counts.bodies); // bodyShapes
    measured.Growing<btMotionState*>(counts.bodies); // bodyMotions includes nulls
    measured.Growing<btCollisionShape*>(counts.bodies); // shapes capacity before Unique
    measured.Growing<btMotionState*>(counts.motions); // motions capacity before Unique
    measured.Growing<btTypedConstraint*>(counts.joints);
    if(!measured.valid)return MMS_E_MEMORY;
    if(owners.find(model)==owners.end()){
        if(owners.size()==owners.max_size())return MMS_E_MEMORY;
        measured.Allocation<std::_Tree_node<decltype(owners)::value_type,void*>>(1);
    }
    if(!measured.valid)return MMS_E_MEMORY;
    std::uint64_t nested=0;
    if(dynamic_cast<DynamicsWorld*>(world)){
        if(const auto result=history_physics::MeasureRetainedCapturePreparation(counts.bodies,counts.joints,nested))return result;
        measured.Add(nested);if(!measured.valid)return MMS_E_MEMORY;
    }
    if(const auto result=physics_scale::MeasureResourceStateCapturePreparation(nested))return result;
    measured.Add(nested);if(!measured.valid)return MMS_E_MEMORY;
    bytes=measured.value;return MMS_OK;
#else
    return MMS_E_NOT_READY;
#endif
}
#endif
std::shared_ptr<ModelPhysicsResources> ModelPhysicsResources::Capture(unsigned char* model) {
    if(!model)throw std::invalid_argument("null model physics owner");
    if(auto found=Find(model)) {
        if(!found->IsCurrent(found->Current()))throw std::invalid_argument("model physics owner was replaced");
        return found;
    }
    auto& m=*mdl::Mdl(model);auto* world=m.scenePtr?m.scenePtr->world:nullptr;
    auto value=std::unique_ptr<ModelPhysicsVersion::Impl>(new ModelPhysicsVersion::Impl(m,world));
    if(auto* dynamics=dynamic_cast<DynamicsWorld*>(world))value->membership=std::make_shared<history_physics::Retained>(*dynamics,value->bodies,value->joints);
    else if(!value->bodies.empty()||!value->joints.empty())throw std::invalid_argument("unsupported model physics world");
    value->scale=physics_scale::ResourceState::Capture(model);value->published=true;
    auto version=std::shared_ptr<ModelPhysicsVersion>(new ModelPhysicsVersion(std::move(value)));
    auto result=std::shared_ptr<ModelPhysicsResources>(new ModelPhysicsResources(std::make_unique<Impl>(model,m.scenePtr,version)));
    version->impl_->owner=result;
    owners.insert_or_assign(model,OwnerEntry{result,{}});
    model_physics_resource_owner::dispose=DisposeModel;
    return result;
}
std::shared_ptr<ModelPhysicsResources> ModelPhysicsResources::CaptureImport(unsigned char* model,PhysicsScene& destination,
    std::shared_ptr<history_physics::ImportScene> scene,std::shared_ptr<history_physics::Retained> membership) {
    if(!model||!scene||!membership||Find(model))throw std::invalid_argument("invalid imported physics owner");
    auto& m=*mdl::Mdl(model);
    if(m.scenePtr!=&scene->Scene())throw std::invalid_argument("private model physics scene mismatch");
    auto value=std::unique_ptr<ModelPhysicsVersion::Impl>(new ModelPhysicsVersion::Impl(m,m.scenePtr->world));
    value->membership=std::move(membership);value->scale=physics_scale::ResourceState::Capture(model);
    auto version=std::shared_ptr<ModelPhysicsVersion>(new ModelPhysicsVersion(std::move(value)));
    auto result=std::shared_ptr<ModelPhysicsResources>(new ModelPhysicsResources(std::make_unique<Impl>(model,&destination,version)));
    version->impl_->owner=result;
    owners.insert_or_assign(model,OwnerEntry{result,{}});model_physics_resource_owner::dispose=DisposeModel;
    return result;
}
void ModelPhysicsResources::AdoptOwnership()noexcept {
    const auto found=owners.find(impl_->model);
    if(found==owners.end())std::terminate();
    auto owner=found->second.observed.lock();if(owner.get()!=this)std::terminate();
    impl_->current->impl_->owned=true;found->second.adopted=std::move(owner);
}
void ModelPhysicsResources::ReleaseBorrowedParkedJoints()noexcept {
    auto& v=*impl_->current->impl_;if(v.owned)return;
    physics_scale::Forget(impl_->model);
    for(auto* joint:v.joints){if(v.world)v.world->removeConstraint(joint);delete joint;}
}
void ModelPhysicsResources::PublishedImport()noexcept {
    auto& v=*impl_->current->impl_;v.published=true;v.world=impl_->destination->world;
}
std::shared_ptr<ModelPhysicsVersion> ModelPhysicsResources::Current()const noexcept{return impl_->current;}
bool ModelPhysicsResources::IsCurrent(const std::shared_ptr<ModelPhysicsVersion>& value)const noexcept{return impl_->Current(value);}
std::shared_ptr<ModelPhysicsVersion> ModelPhysicsResources::AdoptPrivate(mdl::ModelRecord& scratch,const std::shared_ptr<history_physics::ImportScene>& scene) {
    if(!impl_->Current(impl_->current)||!scene||scratch.scenePtr!=&scene->Scene()||!impl_->destination||!impl_->destination->world)
        throw std::invalid_argument("stale/private model physics scene");
    if(scratch.rigidTable&&scratch.rigidTable==impl_->current->Rigids())throw std::invalid_argument("aliased private rigid table");
    if(scratch.jointTable&&scratch.jointTable==impl_->current->Joints())throw std::invalid_argument("aliased private joint table");
    auto value=std::unique_ptr<ModelPhysicsVersion::Impl>(new ModelPhysicsVersion::Impl(scratch,scratch.scenePtr->world));
    value->privateScene=scene;
    const auto& before=*impl_->current->impl_;
    for(auto* body:value->bodies)if(std::find(before.bodies.begin(),before.bodies.end(),body)!=before.bodies.end())throw std::invalid_argument("aliased private native physics");
    for(auto* shape:value->shapes)if(std::find(before.shapes.begin(),before.shapes.end(),shape)!=before.shapes.end())throw std::invalid_argument("aliased private collision shape");
    for(auto* motion:value->motions)if(std::find(before.motions.begin(),before.motions.end(),motion)!=before.motions.end())throw std::invalid_argument("aliased private motion state");
    for(size_t i=0;i<(size_t(value->rigidCount)+value->jointCount)*2;++i)if(auto* name=value->Name(i))
        for(size_t j=0;j<(size_t(before.rigidCount)+before.jointCount)*2;++j)if(before.Name(j)==name)throw std::invalid_argument("aliased private physics name");
    auto result=std::shared_ptr<ModelPhysicsVersion>(new ModelPhysicsVersion(std::move(value)));
    result->impl_->owner=shared_from_this();
    // Last fallible operation precedes extracting the scratch scale node and
    // clearing its native fields. No live object/world is changed by adoption.
    result->impl_->scale=physics_scale::ResourceState::TakePrivate(reinterpret_cast<unsigned char*>(&scratch),impl_->model);
    result->impl_->owned=true;
    scratch.rigidTable=nullptr;scratch.rigidCount=0;scratch.jointTable=nullptr;scratch.jointCount=0;
    return result;
}
std::unique_ptr<PreparedHistoryChange> ModelPhysicsResources::PrepareExchange(
    const std::shared_ptr<ModelPhysicsVersion>& from,const std::shared_ptr<ModelPhysicsVersion>& to) {
    if(!impl_->Current(from)||!to||from==to||!to->impl_->owned||to->impl_->owner.lock().get()!=this)throw std::invalid_argument("invalid model physics version exchange");
    auto next=std::make_unique<Impl::Candidate>();next->owner=shared_from_this();next->from=from;next->to=to;
    next->scale=from->impl_->scale->PrepareExchange(to->impl_->scale);
    if(!to->impl_->published) {
        if(!to->impl_->privateScene)throw std::invalid_argument("missing private physics version world");
        std::vector<history_physics::Retained*> outgoing;
        if(from->impl_->membership)outgoing.push_back(from->impl_->membership.get());
        next->initial=to->impl_->privateScene->PrepareReplacing(outgoing);
        next->incoming=next->initial->RetainedOwner();
    } else {
        if(!from->impl_->membership||!to->impl_->membership)throw std::invalid_argument("missing retained physics membership");
        next->replay=history_physics::Retained::PrepareExchange({{from->impl_->membership.get(),false},{to->impl_->membership.get(),true}});
    }
    next->resources=std::make_shared<resource_budget::Collector>();next->Measure(*next->resources);
    if(!next->resources->Result().Complete())throw std::invalid_argument("incomplete model physics resource budget");
    next->bytes=next->resources->Result().totalBytes;
    if(!next->IsCurrent())throw std::invalid_argument("stale prepared physics version");
    return next;
}
void ModelPhysicsResources::MeasureResources(resource_budget::Collector& out,bool membership)const {out.Array(this,1);impl_->Measure(out,membership);}
void ModelPhysicsResources::MeasureBookkeeping(resource_budget::Collector& out,bool membership)const {
    out.Array(this,1);out.Array(impl_.get(),1);
    const auto found=owners.find(impl_->model);if(found!=owners.end())out.Add(&*found,1,sizeof(*found)+4*sizeof(void*));
    impl_->current->MeasureBookkeeping(out,membership);
}
bool ModelPhysicsResources::DisposeModel(unsigned char* model)noexcept {
    const auto found=owners.find(model);if(found==owners.end())return false;
    auto owner=found->second.observed.lock();owners.erase(found);
    if(!owner)return false;
    auto& o=*owner->impl_;o.bound=false;
    if(o.current->impl_->owned) {
        o.current->impl_->scale->Retire();
        if(auto* world=o.current->impl_->world) {
            for(auto* joint:o.current->impl_->joints)world->removeConstraint(joint);
            for(auto* body:o.current->impl_->bodies)world->removeRigidBody(body);
        }
        auto& m=*mdl::Mdl(model);
        m.rigidTable=nullptr;m.rigidCount=0;m.jointTable=nullptr;m.jointCount=0;
        return true;
    }
    return false;
}
}
