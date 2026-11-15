#define NOMINMAX
#include "mikumikustudio/soft_body_compute.hpp"
#include "mikumikustudio/d3d12_cloth.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "BulletSoftBody/btSoftBody.h"
#include "BulletSoftBody/btSoftBodyInternals.h"
#include "../plugins/scene_resource_budget.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace mikumikustudio::soft_compute {
namespace {
Status status;
struct Runtime {
    std::unique_ptr<gpu::Cloth> solver;
    std::uint64_t adapter=0;
    bool failed=false;
    bool Prepare(std::uint64_t id) {
        if(!solver || id!=adapter) {solver=std::make_unique<gpu::Cloth>();adapter=id;failed=false;}
        if(failed)return false;
        if(!solver->Initialize(id)) {failed=true;status.detail=solver->Error();runtime_log::Write("Cloth GPU unavailable: %s",status.detail.c_str());return false;}
        return true;
    }
} runtime;
void Store(float* out,const btVector3& v){for(int i=0;i<3;++i)out[i]=v[i];}
btVector3 Vector(const float* v){return {v[0],v[1],v[2]};}
void Matrix(float out[3][4],const btMatrix3x3& m){for(int i=0;i<3;++i)Store(out[i],m[i]);}
bool Finite(const float* v,unsigned count){for(unsigned i=0;i<count;++i)if(!std::isfinite(v[i]))return false;return true;}
double Elapsed(std::chrono::steady_clock::time_point start){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
bool Cpu(Result reason,const char* detail="") {status.result=reason;status.detail=detail;++status.cpuSteps;return false;}
}
const Status& GetStatus(){return status;}
bool SolveClusters(btAlignedObjectArray<btSoftBody*>& softBodies,const processor::Selection& selection) {
    status.clusterIterationsCpu=false;
    int iterations=0;unsigned count=0;
    for(int i=0;i<softBodies.size();++i) {
        const auto& body=*softBodies[i];iterations=std::max(iterations,body.m_cfg.citerations);
        count+=unsigned(body.m_joints.size());
        for(int j=0;j<body.m_joints.size();++j)
            if(body.m_joints[j]->Type()==btSoftBody::Joint::eType::Angular){++status.clusterCpuSteps;status.clusterIterationsCpu=true;return false;}
    }
    if(!count || !iterations)return false;
    if(selection.mode==processor::Mode::Cpu || (selection.mode!=processor::Mode::Adapter && count<128)) {
        ++status.clusterCpuSteps;status.clusterIterationsCpu=true;return false;
    }
    bool prepared=false,backendFailed=false;
    try {
        if(!runtime.Prepare(selection.mode==processor::Mode::Adapter?selection.adapterId:0)){status.clusterIterationsCpu=true;return false;}
        std::vector<btSoftBody::Body> refs;
        std::vector<gpu::ClothRigid> rigids;
        std::unordered_map<const void*,unsigned> indices;
        const auto index=[&](const btSoftBody::Body& b) {
            const void* key=b.m_soft?static_cast<void*>(b.m_soft):b.m_rigid?static_cast<void*>(b.m_rigid):b.m_collisionObject;
            auto at=indices.emplace(key,unsigned(refs.size()));
            if(at.second) {
                gpu::ClothRigid r{};r.kind=b.m_soft?1:0;
                Store(r.linear,b.linearVelocity());r.linear[3]=b.invMass();Store(r.angular,b.angularVelocity());Matrix(r.inertia,b.invWorldInertia());
                if(b.m_rigid){Store(r.linearFactor,b.m_rigid->getLinearFactor());Store(r.angularFactor,b.m_rigid->getAngularFactor());}
                if(b.m_soft) {
                    const auto& c=*b.m_soft;Store(r.center,c.m_com);
                    for(int j=0;j<2;++j){Store(r.velocityImpulse[j],c.m_vimpulses[j]);Store(r.driftImpulse[j],c.m_dimpulses[j]);}
                    r.velocityCount=c.m_nvimpulses;r.driftCount=c.m_ndimpulses;
                }
                refs.push_back(b);rigids.push_back(r);
            }return at.first->second;
        };
        std::vector<gpu::ClothCoupling> source;source.reserve(count);
        for(int i=0;i<softBodies.size();++i)softBodies[i]->prepareClusters(iterations);
        prepared=true;
        for(int i=0;i<softBodies.size();++i)for(int j=0;j<softBodies[i]->m_joints.size();++j) {
            const auto& joint=*softBodies[i]->m_joints[j];gpu::ClothCoupling c{};
            c.node=index(joint.m_bodies[0]);c.rigid=index(joint.m_bodies[1]);
            Matrix(c.matrix,joint.m_massmatrix);Store(c.drift,joint.m_drift);
            if(joint.Type()==btSoftBody::Joint::eType::Contact) {
                const auto& contact=static_cast<const btSoftBody::CJoint&>(joint);c.type=3;
                Store(c.relative,contact.m_rpos[0]);Store(c.target,contact.m_rpos[1]);
                Store(c.parameters,contact.m_normal);c.drift[3]=contact.m_friction;
                if(joint.m_bodies[0].m_soft && joint.m_bodies[0].m_soft==joint.m_bodies[1].m_soft) {
                    c.extra[0]=1;c.target[3]=joint.m_bodies[0].m_soft->m_maxSelfCollisionImpulse;
                    c.relative[3]=joint.m_bodies[0].m_soft->m_selfCollisionImpulseFactor;
                }
            } else {
                const auto& link=static_cast<const btSoftBody::LJoint&>(joint);c.type=4;
                Store(c.relative,link.m_rpos[0]);Store(c.target,link.m_rpos[1]);c.parameters[3]=joint.m_cfm;
            }
            source.push_back(c);
        }
        // Connected components preserve the exact joint order for bodies that
        // exchange impulses. Independent islands can execute concurrently.
        std::vector<unsigned> parent(count);std::iota(parent.begin(),parent.end(),0);
        const auto root=[&](unsigned n){while(parent[n]!=n){parent[n]=parent[parent[n]];n=parent[n];}return n;};
        std::unordered_map<unsigned,unsigned> owner;
        for(unsigned i=0;i<count;++i)for(unsigned b:{source[i].node,source[i].rigid}) {
            if(!rigids[b].kind && rigids[b].linear[3]==0)continue;
            auto at=owner.emplace(b,i);if(!at.second)parent[root(i)]=root(at.first->second);
        }
        std::vector<std::vector<unsigned>> batches(count);
        for(unsigned i=0;i<count;++i)batches[root(i)].push_back(i);
        std::vector<gpu::ClothCoupling> couplings;std::vector<gpu::ClothGroup> groups;
        for(const auto& batch:batches)if(!batch.empty()) {
            groups.push_back({unsigned(couplings.size()),unsigned(batch.size())});
            for(unsigned i:batch)couplings.push_back(source[i]);
        }
        for(const auto& r:rigids)if(!Finite(reinterpret_cast<const float*>(&r),48))throw std::runtime_error("invalid cluster body");
        for(const auto& c:couplings)if(!Finite(&c.matrix[0][0],24)||!Finite(c.drift,4))throw std::runtime_error("invalid cluster constraint");
        std::vector<gpu::ClothNode> dummy(1);
        std::vector<gpu::ClothPass> passes(iterations,{11,0,unsigned(groups.size())});
        if(!runtime.solver->Step(dummy,{},rigids,couplings,groups,passes,{})){backendFailed=true;throw std::runtime_error(runtime.solver->Error());}
        for(const auto& r:rigids)if(!Finite(reinterpret_cast<const float*>(&r),48))throw std::runtime_error("nonfinite cluster output");
        for(unsigned i=0;i<refs.size();++i) {
            const auto& r=rigids[i];auto& ref=refs[i];
            if(ref.m_soft) {
                auto& c=*ref.m_soft;c.m_lv=Vector(r.linear);c.m_av=Vector(r.angular);c.m_nvimpulses=r.velocityCount;
                for(int j=0;j<2;++j)c.m_vimpulses[j]=Vector(r.velocityImpulse[j]);
            } else if(ref.m_rigid && r.linear[3]!=0) {
                ref.m_rigid->setLinearVelocity(Vector(r.linear));ref.m_rigid->setAngularVelocity(Vector(r.angular));
            }
        }
        for(int i=0;i<softBodies.size();++i)softBodies[i]->cleanupClusters();
        ++status.clusterGpuSteps;return true;
    } catch(const std::exception& e) {
        if(backendFailed)runtime.failed=true;
        status.detail=e.what();runtime_log::Write("Cloth cluster GPU fallback: %s",e.what());
        ++status.clusterCpuSteps;status.clusterIterationsCpu=true;
        if(!prepared)return false;
        // Preparation changes contact lifetime. Complete this already-prepared
        // step on CPU instead of preparing it a second time.
        for(int iteration=0;iteration<iterations;++iteration)
            for(int i=0;i<softBodies.size();++i)softBodies[i]->solveClusters(btScalar(1));
        for(int i=0;i<softBodies.size();++i)softBodies[i]->cleanupClusters();
        return true;
    }
}
struct State::Impl {
    std::vector<gpu::ClothNode> nodes;
    std::vector<gpu::ClothLink> links;
    std::vector<gpu::ClothRigid> rigids;
    std::vector<gpu::ClothCoupling> couplings;
    std::vector<gpu::ClothGroup> groups,colors;
    std::vector<gpu::ClothPass> passes;
    std::vector<unsigned> order;
    std::vector<btRigidBody*> bodies;
    std::vector<btSoftBody::Node*> nodeRefs;
    const btSoftBody::Link* topology=nullptr;
    unsigned nodeCount=0;
    int decision=0;
    processor::Selection selection;
    bool Topology(btSoftBody& body) {
        const auto* first=body.m_links.size()?&body.m_links[0]:nullptr;
        if(topology==first && order.size()==unsigned(body.m_links.size()) && nodeCount==unsigned(body.m_nodes.size()))return true;
        std::vector<std::vector<unsigned>> used(body.m_nodes.size()),batches;
        for(unsigned i=0;i<unsigned(body.m_links.size());++i) {
            const auto& l=body.m_links[i];
            const auto a=l.m_n[0]-&body.m_nodes[0],b=l.m_n[1]-&body.m_nodes[0];
            if(a<0 || b<0 || a>=body.m_nodes.size() || b>=body.m_nodes.size() || a==b)return false;
            unsigned color=0;
            for(;color<batches.size();++color)
                if(std::find(used[a].begin(),used[a].end(),color)==used[a].end() &&
                   std::find(used[b].begin(),used[b].end(),color)==used[b].end())break;
            if(color>=256)return false;
            if(color==batches.size())batches.emplace_back();
            batches[color].push_back(i);used[a].push_back(color);used[b].push_back(color);
        }
        colors.clear();order.clear();
        for(const auto& batch:batches){colors.push_back({unsigned(order.size()),unsigned(batch.size())});order.insert(order.end(),batch.begin(),batch.end());}
        topology=first;nodeCount=body.m_nodes.size();decision=0;return true;
    }
    unsigned Rigid(btRigidBody* body) {
        if(!body)return ~0u;
        auto found=std::find(bodies.begin(),bodies.end(),body);
        if(found!=bodies.end())return unsigned(found-bodies.begin());
        bodies.push_back(body);gpu::ClothRigid r{};
        Store(r.linear,body->getLinearVelocity());r.linear[3]=body->getInvMass();
        Store(r.angular,body->getAngularVelocity());Store(r.linearFactor,body->getLinearFactor());
        Store(r.angularFactor,body->getAngularFactor());Matrix(r.inertia,body->getInvInertiaTensorWorld());
        rigids.push_back(r);return unsigned(bodies.size()-1);
    }
    // Each connected component is sequential, preserving Bullet's ordering
    // when contacts share a vertex or a dynamic rigid body. Other components
    // can execute independently; immovable rigid bodies need no write hazard.
    gpu::ClothGroup Components(const std::vector<gpu::ClothCoupling>& source) {
        const unsigned base=unsigned(groups.size());
        std::vector<unsigned> parent(source.size());std::iota(parent.begin(),parent.end(),0);
        const auto root=[&](unsigned i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;};
        std::unordered_map<std::uint64_t,unsigned> owner;
        for(unsigned i=0;i<source.size();++i) {
            const auto& c=source[i];
            const bool soft=c.type==2;
            const std::uint64_t keys[]={c.node,soft?c.rigid:((std::uint64_t(1)<<32)|c.rigid),c.extra[0],c.extra[1]};
            const bool dynamic=!soft && c.rigid!=~0u && rigids[c.rigid].linear[3]!=0;
            for(unsigned k=0;k<(soft?4u:dynamic?2u:1u);++k){auto found=owner.emplace(keys[k],i);if(!found.second)parent[root(i)]=root(found.first->second);}
        }
        std::vector<std::vector<unsigned>> batches(source.size());
        for(unsigned i=0;i<source.size();++i)batches[root(i)].push_back(i);
        for(const auto& batch:batches)if(!batch.empty()) {
            groups.push_back({unsigned(couplings.size()),unsigned(batch.size())});
            for(unsigned i:batch)couplings.push_back(source[i]);
        }
        return {base,unsigned(groups.size())-base};
    }
    bool Pack(btSoftBody& body) {
        nodes.resize(body.m_nodes.size());links.resize(order.size());
        bodies.clear();rigids.clear();couplings.clear();groups.clear();passes.clear();
        nodeRefs.clear();
        for(unsigned i=0;i<nodes.size();++i) {
            const auto& n=body.m_nodes[i];auto& p=nodes[i];p={};
            Store(p.position,n.m_x);p.position[3]=n.m_im;Store(p.previous,n.m_q);
            Store(p.velocity,n.m_v);Store(p.force,n.m_f);
            if(!Finite(reinterpret_cast<const float*>(&p),16))return false;
            nodeRefs.push_back(&body.m_nodes[i]);
        }
        for(unsigned i=0;i<links.size();++i) {
            const auto& l=body.m_links[order[i]];
            links[i]={unsigned(l.m_n[0]-&body.m_nodes[0]),unsigned(l.m_n[1]-&body.m_nodes[0]),l.m_c0,l.m_c1};
            if(!std::isfinite(l.m_c0) || !std::isfinite(l.m_c1))return false;
        }
        std::vector<gpu::ClothCoupling> source;
        for(int i=0;i<body.m_anchors.size();++i) {
            const auto& a=body.m_anchors[i];gpu::ClothCoupling c{};
            c.node=unsigned(a.m_node-&body.m_nodes[0]);c.rigid=Rigid(a.m_body);
            const auto relative=a.m_body->getWorldTransform().getBasis()*a.m_local;
            Matrix(c.matrix,ImpulseMatrix(body.m_sst.sdt,a.m_node->m_im,a.m_body->getInvMass(),a.m_body->getInvInertiaTensorWorld(),relative));
            Store(c.relative,relative);c.relative[3]=body.m_sst.sdt*a.m_node->m_im;
            Store(c.target,a.m_body->getInterpolationWorldTransform()*a.m_local);c.parameters[0]=body.m_cfg.kAHR;
            source.push_back(c);
        }
        const auto anchors=Components(source);source.clear();
        for(int i=0;i<body.m_rcontacts.size();++i) {
            const auto& contact=body.m_rcontacts[i];gpu::ClothCoupling c{};
            c.node=unsigned(contact.m_node-&body.m_nodes[0]);
            c.rigid=Rigid(btRigidBody::upcast(contact.m_cti.m_colObj));c.type=1;
            Matrix(c.matrix,contact.m_c0);Store(c.relative,contact.m_c1);c.relative[3]=contact.m_c2;
            Store(c.target,contact.m_cti.m_normal);c.target[3]=contact.m_cti.m_offset;
            c.parameters[0]=contact.m_c3;c.parameters[1]=contact.m_c4;c.parameters[2]=body.getCollisionShape()->getMargin();
            source.push_back(c);
        }
        const auto contacts=Components(source);
        source.clear();
        if(body.m_scontacts.size()) {
            std::unordered_map<btSoftBody::Node*,unsigned> indices;
            for(unsigned i=0;i<nodeRefs.size();++i)indices.emplace(nodeRefs[i],i);
            const auto index=[&](btSoftBody::Node* n) {
                auto at=indices.emplace(n,unsigned(nodes.size()));
                if(at.second) {
                    gpu::ClothNode p{};Store(p.position,n->m_x);p.position[3]=n->m_im;
                    Store(p.previous,n->m_q);Store(p.velocity,n->m_v);Store(p.force,n->m_f);
                    nodes.push_back(p);nodeRefs.push_back(n);
                }return at.first->second;
            };
            for(int i=0;i<body.m_scontacts.size();++i) {
                const auto& c=body.m_scontacts[i];gpu::ClothCoupling p{};p.type=2;
                p.node=index(c.m_node);p.rigid=index(c.m_face->m_n[0]);
                p.extra[0]=index(c.m_face->m_n[1]);p.extra[1]=index(c.m_face->m_n[2]);
                Store(p.relative,c.m_weights);Store(p.target,c.m_normal);p.target[3]=c.m_margin;
                p.parameters[0]=c.m_cfm[0];p.parameters[1]=c.m_cfm[1];p.parameters[2]=c.m_friction;
                source.push_back(p);
            }
        }
        const auto softContacts=Components(source);
        const gpu::ClothGroup clusterNodes{unsigned(groups.size()),nodeCount};
        if(body.m_clusters.size()) {
            std::vector<std::vector<gpu::ClothCoupling>> memberships(nodeCount);
            for(int i=0;i<body.m_clusters.size();++i) {
                const auto& c=*body.m_clusters[i];gpu::ClothRigid r{};
                Store(r.linear,c.m_lv);Store(r.angular,c.m_av);Store(r.center,c.m_com);r.center[3]=c.m_ndamping;
                for(int j=0;j<2;++j){Store(r.velocityImpulse[j],c.m_vimpulses[j]);Store(r.driftImpulse[j],c.m_dimpulses[j]);}
                r.kind=1;r.velocityCount=c.m_nvimpulses;r.driftCount=c.m_ndimpulses;
                const unsigned index=unsigned(rigids.size());rigids.push_back(r);
                for(int j=0;j<c.m_nodes.size();++j) {
                    const auto node=c.m_nodes[j]-&body.m_nodes[0];if(node<0 || node>=nodeCount)return false;
                    gpu::ClothCoupling p{};p.node=unsigned(node);p.rigid=index;p.parameters[0]=c.m_masses[j];
                    memberships[node].push_back(p);
                }
            }
            for(const auto& membership:memberships) {
                groups.push_back({unsigned(couplings.size()),unsigned(membership.size())});
                couplings.insert(couplings.end(),membership.begin(),membership.end());
            }
            passes.push_back({8,clusterNodes.first,clusterNodes.count});
        }
        const auto linear=[&](unsigned stage){for(const auto& color:colors)passes.push_back({stage,color.first,color.count});};
        const auto position=[&](const btSoftBody::tPSolverArray& sequence){
            for(int j=0;j<sequence.size();++j)switch(sequence[j]) {
            case btSoftBody::ePSolver::Linear:linear(4);break;
            case btSoftBody::ePSolver::Anchors:passes.push_back({2,anchors.first,anchors.count});break;
            case btSoftBody::ePSolver::RContacts:passes.push_back({2,contacts.first,contacts.count});break;
            case btSoftBody::ePSolver::SContacts:passes.push_back({2,softContacts.first,softContacts.count});break;
            default:return false;
            }return true;
        };
        for(int i=0;i<body.m_cfg.viterations;++i)for(int j=0;j<body.m_cfg.m_vsequence.size();++j) {
            if(body.m_cfg.m_vsequence[j]!=btSoftBody::eVSolver::Linear)return false;
            linear(0);
        }
        if(body.m_cfg.viterations)passes.push_back({1,0,nodeCount});
        for(int i=0;i<body.m_cfg.piterations;++i)if(!position(body.m_cfg.m_psequence))return false;
        if(body.m_cfg.piterations)passes.push_back({5,0,nodeCount});
        if(body.m_cfg.diterations) {
            passes.push_back({6,0,nodeCount});
            for(int i=0;i<body.m_cfg.diterations;++i)if(!position(body.m_cfg.m_dsequence))return false;
            passes.push_back({7,0,nodeCount});
        }
        if(body.m_clusters.size()) {
            passes.push_back({9,clusterNodes.first,clusterNodes.count});
            passes.push_back({10,clusterNodes.first,clusterNodes.count});
        }
        for(const auto& c:couplings)if(c.node>=nodes.size() || !Finite(&c.matrix[0][0],24))return false;
        return Valid();
    }
    bool Valid()const {
        for(const auto& n:nodes)if(!Finite(reinterpret_cast<const float*>(&n),16))return false;
        for(const auto& r:rigids)if(!Finite(reinterpret_cast<const float*>(&r),48))return false;
        return true;
    }
    void Commit(btSoftBody& body) {
        for(unsigned i=0;i<nodes.size();++i) {
            auto& n=*nodeRefs[i];const auto& p=nodes[i];
            n.m_x=Vector(p.position);n.m_q=Vector(p.previous);n.m_v=Vector(p.velocity);n.m_f=Vector(p.force);
        }
        for(unsigned i=0;i<bodies.size();++i) {
            if(rigids[i].linear[3]!=0) {bodies[i]->setLinearVelocity(Vector(rigids[i].linear));bodies[i]->setAngularVelocity(Vector(rigids[i].angular));}
        }
        for(int i=0;i<body.m_anchors.size();++i)body.m_anchors[i].m_body->activate();
        for(int i=0;i<body.m_clusters.size();++i) {
            auto& c=*body.m_clusters[i];
            if(c.m_ndimpulses)for(int j=0;j<2;++j)c.m_dimpulses[j]/=btScalar(c.m_ndimpulses);
        }
    }
};
State::State():impl(std::make_unique<Impl>()){} State::~State()=default;
bool State::Solve(btSoftBody& body,const processor::Selection& selection) {
    auto& s=*impl;
    if(selection!=s.selection){s.decision=0;s.selection=selection;}
    if(selection.mode==processor::Mode::Cpu)return Cpu(Result::Cpu);
    if(body.m_tetras.size())return Cpu(Result::Unsupported,"tetrahedral body uses Bullet CPU");
    if(!body.m_nodes.size() || body.m_nodes.size()>65535*64 || !std::isfinite(body.m_sst.sdt) || body.m_sst.sdt<=0)return Cpu(Result::Unsupported,"invalid soft-body time step or topology");
    const bool automatic=selection.mode!=processor::Mode::Adapter;
    if(automatic && body.m_nodes.size()<512)return Cpu(Result::Small);
    bool cpuSolved=false;
    try {
        if(!s.Topology(body))return Cpu(Result::Unsupported,"unsupported link graph");
        if(automatic && s.decision<0)return Cpu(Result::MeasuredCpu);
        if(!runtime.Prepare(selection.mode==processor::Mode::Adapter?selection.adapterId:0))return Cpu(Result::Unavailable,runtime.solver->Error());
        status.adapter=runtime.solver->AdapterName();
        const auto packBegin=std::chrono::steady_clock::now();
        if(!s.Pack(body))return Cpu(Result::Unsupported,"unsupported solver sequence or invalid input");
        const double packMilliseconds=Elapsed(packBegin);
        const gpu::ClothParameters parameters{body.m_sst.sdt,body.m_sst.isdt,body.m_cfg.kDP,body.m_cfg.kVCF};
        const auto work=[&]{return runtime.solver->Step(s.nodes,s.links,s.rigids,s.couplings,s.groups,s.passes,parameters)&&s.Valid();};
        if(automatic && !s.decision) {
            const auto initialNodes=s.nodes;const auto initialRigids=s.rigids;
            if(!work()){runtime.failed=true;return Cpu(Result::Unavailable,runtime.solver->Error());}
            s.nodes=initialNodes;s.rigids=initialRigids;
            const auto begin=std::chrono::steady_clock::now();
            if(!work()){runtime.failed=true;return Cpu(Result::Unavailable,runtime.solver->Error());}
            status.gpuMilliseconds=Elapsed(begin)+packMilliseconds;
            std::vector<btVector3> clusterDrift;
            for(int i=0;i<body.m_clusters.size();++i)for(int j=0;j<2;++j)clusterDrift.push_back(body.m_clusters[i]->m_dimpulses[j]);
            const auto cpuBegin=std::chrono::steady_clock::now();body.solveConstraints();cpuSolved=true;status.cpuMilliseconds=Elapsed(cpuBegin);
            s.decision=status.gpuMilliseconds<status.cpuMilliseconds*.95?1:-1;
            if(s.decision<0){Cpu(Result::MeasuredCpu);return true;}
            for(int i=0;i<body.m_clusters.size();++i)for(int j=0;j<2;++j)body.m_clusters[i]->m_dimpulses[j]=clusterDrift[i*2+j];
        } else if(!work()) {runtime.failed=true;return Cpu(Result::Unavailable,runtime.solver->Error());}
        s.Commit(body);status.result=Result::Gpu;status.detail.clear();++status.gpuSteps;return true;
    }catch(const std::exception& e){runtime.failed=true;Cpu(Result::Unavailable,e.what());return cpuSolved;}
}
void State::Measure(plugins::resource_budget::Collector& out)const {
#if !MMD_ORIGINAL_PROFILE
    out.Array(this,1);out.Array(impl.get(),1);const auto& s=*impl;
    out.Vector(s.nodes);out.Vector(s.links);out.Vector(s.rigids);out.Vector(s.couplings);
    out.Vector(s.groups);out.Vector(s.colors);out.Vector(s.passes);out.Vector(s.order);out.Vector(s.bodies);
    out.Vector(s.nodeRefs);
    out.String(s.selection.adapterName);
#else
    (void)out;
#endif
}
}
