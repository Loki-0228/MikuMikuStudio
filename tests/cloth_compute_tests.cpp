// MAINTENANCE: keep while the native soft-body solver has a GPU execution path.
#define NOMINMAX
#include "mikumikustudio/soft_body_compute.hpp"
#include "mikumikustudio/d3d12_skinning.hpp"
#include "mikumikustudio/d3d12_cloth.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "btBulletDynamicsCommon.h"
#include "BulletSoftBody/btSoftBodyHelpers.h"
#include "BulletSoftBody/btSoftBodyRigidBodyCollisionConfiguration.h"
#include "BulletSoftBody/btSoftBodyInternals.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace mikumikustudio;
namespace {
int failures=0;
void Check(bool ok,const char* label){if(!ok){++failures;std::printf("FAIL %s\n",label);}}
soft_compute::State* activeState=nullptr;
processor::Selection activeChoice;
bool Solve(btSoftBody& body){return activeState && activeState->Solve(body,activeChoice);}
unsigned observedClusterConstraints=0;
bool SolveClusterWorld(btAlignedObjectArray<btSoftBody*>& bodies){
    for(int i=0;i<bodies.size();++i)observedClusterConstraints+=bodies[i]->m_joints.size();
    return soft_compute::SolveClusters(bodies,activeChoice);
}
struct World {
    btSoftBodyRigidBodyCollisionConfiguration config;
    SoftGroundDispatcher dispatcher{&config};btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world{&dispatcher,&broadphase,&solver,&config};
    btBoxShape box{btVector3(1,.3f,1)};
    btDefaultMotionState motion;
    std::unique_ptr<btRigidBody> rigid;
    std::unique_ptr<btSoftBody> cloth;
    soft_compute::State compute;
    std::vector<btVector3> rest;
    World(int size=0,bool anchor=false,bool contact=false,bool dynamic=false) {
        world.setGravity(btVector3(0,-9.8f,0));
        world.getWorldInfo().m_gravity=world.getGravity();
        if(size) {
            cloth.reset(btSoftBodyHelpers::CreatePatch(world.getWorldInfo(),btVector3(-5,5,-5),btVector3(5,5,-5),
                btVector3(-5,5,5),btVector3(5,5,5),size,size,3,true));
        } else {
            const btVector3 nodes[]={btVector3(-.3f,2,0),btVector3(.3f,2,0)};const btScalar masses[]={1,1};
            cloth=std::make_unique<btSoftBody>(&world.getWorldInfo(),2,nodes,masses);
            cloth->appendLink(0,1);if(!anchor&&!contact)cloth->setMass(0,0);
        }
        cloth->m_cfg.piterations=8;cloth->m_cfg.kDP=.01f;cloth->m_cfg.kDF=.2f;
        if(anchor || contact) {
            btTransform transform;transform.setIdentity();transform.setOrigin(anchor?btVector3(-.3f,2,0):btVector3(0,0,0));
            motion.setWorldTransform(transform);btVector3 inertia(0,0,0);if(dynamic)box.calculateLocalInertia(2,inertia);
            rigid=std::make_unique<btRigidBody>(btRigidBody::btRigidBodyConstructionInfo(dynamic?2.f:0.f,&motion,&box,inertia));
            rigid->setWorldTransform(transform);rigid->setInterpolationWorldTransform(transform);
            rigid->setAngularFactor(btVector3(.8f,.9f,1));rigid->setLinearFactor(btVector3(1,.9f,1));
            world.addRigidBody(rigid.get());
            if(anchor)cloth->appendAnchor(0,rigid.get());
        }
        world.addSoftBody(cloth.get());
        for(int i=0;i<cloth->m_nodes.size();++i)rest.push_back(cloth->m_nodes[i].m_x);
    }
    ~World(){world.removeSoftBody(cloth.get());if(rigid)world.removeRigidBody(rigid.get());world.getWorldInfo().m_sparsesdf.Reset();}
    void Step(const processor::Selection& choice) {
        activeState=&compute;activeChoice=choice;
        world.stepSimulation(1.f/60,1,1.f/60);
        activeState=nullptr;
    }
    void Reset(){
        for(int i=0;i<cloth->m_nodes.size();++i){auto& n=cloth->m_nodes[i];n.m_x=n.m_q=rest[i];n.m_v.setZero();n.m_f.setZero();}
        cloth->updateNormals();cloth->updateBounds();world.ResetLocalTime();
    }
};
void SceneFloor(const processor::Selection& gpu) {
    constexpr auto groundGroup = static_cast<short>(0x8000);
    constexpr auto excludeGroup16 = static_cast<short>(0x7fff);
    const processor::Selection cpu{processor::Mode::Cpu};
    for (const auto& backend : {cpu, gpu}) {
        btStaticPlaneShape plane(btVector3(0, 1, 0), 0);
        btRigidBody floor(btRigidBody::btRigidBodyConstructionInfo(0, nullptr, &plane));
        btSphereShape sphere(.25f);
        btTransform drop = btTransform::getIdentity();
        drop.setOrigin(btVector3(3, 2, 0));
        btDefaultMotionState motion(drop);
        btVector3 inertia;
        sphere.calculateLocalInertia(1, inertia);
        btRigidBody falling(btRigidBody::btRigidBodyConstructionInfo(1, &motion, &sphere, inertia));
        World fixture(0, false, true);
        // The unrelated model box shares group 16 and would catch the cloth
        // before the floor if the soft body's mask were changed.
        fixture.world.removeRigidBody(fixture.rigid.get());
        auto transform = btTransform::getIdentity();
        transform.setOrigin(btVector3(0, 1, 0));
        fixture.rigid->setWorldTransform(transform);
        fixture.rigid->setInterpolationWorldTransform(transform);
        fixture.world.addRigidBody(fixture.rigid.get(), groundGroup, -1);
        fixture.world.removeSoftBody(fixture.cloth.get());
        fixture.world.addSoftBody(fixture.cloth.get(), 1, excludeGroup16);
        fixture.world.addRigidBody(&floor, groundGroup, -1);
        fixture.world.addRigidBody(&falling);
        fixture.world.SetGround(&floor, fixture.dispatcher);

        fixture.cloth->getCollisionShape()->setMargin(.05f);
        bool solvedFloorOnGpu = false;
        for (int frame = 0; frame < 90; ++frame) {
            const auto gpuBefore = soft_compute::GetStatus().gpuSteps;
            fixture.Step(backend);
            solvedFloorOnGpu |= fixture.cloth->m_rcontacts.size() > 0 &&
                soft_compute::GetStatus().gpuSteps > gpuBefore;
        }
        for (int i = 0; i < fixture.cloth->m_nodes.size(); ++i) {
            const auto height = fixture.cloth->m_nodes[i].m_x.y();
            Check(height >= 0 && height < .2f,
                  "soft body rests on the enabled scene floor while excluding model group 16");
        }
        Check(fixture.cloth->getBroadphaseHandle()->m_collisionFilterMask == excludeGroup16,
              "scene floor does not rewrite the model collision mask");
        Check(std::abs(falling.getWorldTransform().getOrigin().y() - .25f) < .08f,
              "ordinary rigid body rests on the same enabled scene floor");
        if (backend.mode == processor::Mode::Adapter)
            Check(solvedFloorOnGpu, "GPU soft solver actually processes scene floor contacts");

        floor.setCollisionFlags(-1);
        for (int frame = 0; frame < 35; ++frame) fixture.Step(backend);
        for (int i = 0; i < fixture.cloth->m_nodes.size(); ++i)
            Check(fixture.cloth->m_nodes[i].m_x.y() < -.5f, "shared floor switch also disables soft contacts");
        Check(falling.getWorldTransform().getOrigin().y() < -.5f, "shared floor switch disables rigid contacts");
        fixture.world.removeRigidBody(&falling);
        fixture.world.removeRigidBody(&floor);
    }
}
void Pair(const processor::Selection& gpu,bool anchor,bool contact,bool dynamic,int velocity,int drift) {
    World reference(0,anchor,contact,dynamic),actual(0,anchor,contact,dynamic);
    reference.cloth->m_cfg.viterations=actual.cloth->m_cfg.viterations=velocity;
    reference.cloth->m_cfg.diterations=actual.cloth->m_cfg.diterations=drift;
    if(velocity || drift){reference.cloth->setSolver(btSoftBody::eSolverPresets::Velocities);actual.cloth->setSolver(btSoftBody::eSolverPresets::Velocities);}
    const auto before=soft_compute::GetStatus().gpuSteps;
    unsigned contacts=0;
    for(int frame=0;frame<75;++frame) {
        reference.Step({processor::Mode::Cpu});actual.Step(gpu);
        contacts+=actual.cloth->m_rcontacts.size();
        for(int i=0;i<2;++i) {
            const auto& a=actual.cloth->m_nodes[i];const auto& b=reference.cloth->m_nodes[i];
            Check((a.m_x-b.m_x).length()<.003f,"GPU position agrees with Bullet single-link reference");
            Check((a.m_v-b.m_v).length()<.02f,"GPU velocity agrees with Bullet reference");
        }
        if(actual.rigid && dynamic)Check((actual.rigid->getLinearVelocity()-reference.rigid->getLinearVelocity()).length()<.03f,"GPU coupling preserves rigid reaction");
    }
    Check(soft_compute::GetStatus().gpuSteps>=before+75,"cloth constraints ran on GPU every substep");
    if(contact&&!dynamic)Check(contacts>0,"static collision exercised GPU contacts");
}
void Grid(const processor::Selection& gpu) {
    World world(24);const auto before=soft_compute::GetStatus().gpuSteps;
    for(int i=0;i<80;++i)world.Step(gpu);
    Check(soft_compute::GetStatus().gpuSteps>=before+80,"grid cloth uses GPU");
    Check((world.cloth->m_nodes[0].m_x-world.rest[0]).length()<1e-6f,"GPU pin is stable");
    double strain=0;
    for(int i=0;i<world.cloth->m_links.size();++i) {
        const auto& l=world.cloth->m_links[i];const double ratio=(l.m_n[1]->m_x-l.m_n[0]->m_x).length()/l.m_rl;
        Check(std::isfinite(ratio)&&ratio<1.8,"GPU cloth remains finite and bounded");strain+=std::abs(ratio-1);
    }
    std::printf("grid nodes=%d links=%d mean_strain=%.6f\n",world.cloth->m_nodes.size(),world.cloth->m_links.size(),strain/world.cloth->m_links.size());
    // PMX builds clusters before applying fixed-node masses.
    world.cloth->setMass(0,1);world.cloth->setMass(23,1);
    world.cloth->generateClusters(4);world.cloth->setMass(0,0);world.cloth->setMass(23,0);world.Step(gpu);
    Check(soft_compute::GetStatus().result==soft_compute::Result::Gpu,"clustered cloth keeps GPU node iterations");
}
void SoftContacts(const processor::Selection& gpu) {
    World reference(4),actual(4),referencePeer(4),actualPeer(4);
    const auto setup=[](World& world,World& peer) {
        auto& body=*world.cloth;body.predictMotion(1.f/60);peer.cloth->predictMotion(1.f/60);
        body.m_cfg.m_psequence.clear();body.m_cfg.m_psequence.push_back(btSoftBody::ePSolver::SContacts);
        for(int i=0;i<3;++i) {
            btSoftBody::SContact c{};c.m_node=&body.m_nodes[5+i];c.m_face=&peer.cloth->m_faces[i];
            c.m_normal=btVector3(0,1,0);c.m_weights=btVector3(.2f,.3f,.5f);
            c.m_margin=.2f;c.m_friction=.25f;c.m_cfm[0]=.4f;c.m_cfm[1]=.6f;
            c.m_node->m_q+=btVector3(.01f,.1f,0);body.m_scontacts.push_back(c);
        }
    };
    setup(reference,referencePeer);setup(actual,actualPeer);
    reference.cloth->solveConstraints();
    Check(actual.compute.Solve(*actual.cloth,gpu),"soft contact constraints run on GPU");
    for(int i=0;i<16;++i) {
        Check((reference.cloth->m_nodes[i].m_x-actual.cloth->m_nodes[i].m_x).length()<2e-4f,"soft contact owner matches Bullet");
        Check((referencePeer.cloth->m_nodes[i].m_x-actualPeer.cloth->m_nodes[i].m_x).length()<2e-4f,"soft contact peer receives GPU reaction");
    }
}
void Clusters(const processor::Selection& gpu) {
    World reference(4,true,false,true),actual(4,true,false,true);
    const auto setup=[](World& world) {
        std::srand(13);auto& body=*world.cloth;body.setMass(0,1);body.setMass(3,1);body.generateClusters(2);body.setMass(0,0);body.setMass(3,0);body.m_cfg.citerations=6;
        body.predictMotion(1.f/60);
        body.m_cfg.m_psequence.clear();body.m_cfg.m_psequence.push_back(btSoftBody::ePSolver::Anchors);
        for(int i=0;i<body.m_clusters.size();++i) {
            auto& cluster=*body.m_clusters[i];cluster.m_ndamping=.2f;
            auto* joint=new(btAlignedAlloc(sizeof(btSoftBody::CJoint),16))btSoftBody::CJoint();
            joint->m_bodies[0]=btSoftBody::Body(&cluster);joint->m_bodies[1]=btSoftBody::Body(world.rigid.get());
            if(i==1)joint->m_bodies[1]=btSoftBody::Body(body.m_clusters[0]);
            if(i==2){joint->m_bodies[1]=btSoftBody::Body(&cluster);cluster.m_maxSelfCollisionImpulse=.0001f;}
            joint->m_life=0;joint->m_maxlife=3;joint->m_erp=.8f;joint->m_split=.3f;joint->m_cfm=1;
            joint->m_rpos[0]=btVector3(.1f,0,0);joint->m_rpos[1]=btVector3(0,.2f,0);
            joint->m_drift=btVector3(0,-.01f,0);joint->m_sdrift.setZero();joint->m_normal=btVector3(0,1,0);joint->m_friction=.2f;
            joint->m_massmatrix=ImpulseMatrix(joint->m_bodies[0].invMass(),joint->m_bodies[0].invWorldInertia(),joint->m_rpos[0],
                joint->m_bodies[1].invMass(),joint->m_bodies[1].invWorldInertia(),joint->m_rpos[1]);
            body.m_joints.push_back(joint);
        }
    };
    setup(reference);setup(actual);const auto before=soft_compute::GetStatus().clusterGpuSteps;
    btSoftBody::solveClusters(reference.world.getSoftBodyArray());
    Check(soft_compute::SolveClusters(actual.world.getSoftBodyArray(),gpu),"cluster world accepts GPU iteration");
    Check(soft_compute::GetStatus().clusterGpuSteps==before+1,"cluster impulses executed on GPU");
    for(int i=0;i<actual.cloth->m_clusters.size();++i) {
        const auto& a=*actual.cloth->m_clusters[i];const auto& b=*reference.cloth->m_clusters[i];
        Check((a.m_lv-b.m_lv).length()<2e-4f&&(a.m_av-b.m_av).length()<2e-4f,"cluster GPU velocities match Bullet");
        Check(a.m_nvimpulses==b.m_nvimpulses&&a.m_ndimpulses==b.m_ndimpulses,"cluster impulse counts preserved");
    }
    Check((actual.rigid->getLinearVelocity()-reference.rigid->getLinearVelocity()).length()<2e-4f,"cluster rigid reaction matches Bullet");
    reference.cloth->solveConstraints();
    Check(actual.compute.Solve(*actual.cloth,gpu),"cluster node distribution and damping run on GPU");
    for(int i=0;i<actual.cloth->m_nodes.size();++i) {
        const auto& a=actual.cloth->m_nodes[i];const auto& b=reference.cloth->m_nodes[i];
        Check((a.m_x-b.m_x).length()<2e-4f&&(a.m_v-b.m_v).length()<.002f,"cluster node GPU result matches Bullet");
    }
}
void FallbackAndAutomatic(const processor::Selection& gpu) {
    World small;small.Step({});Check(soft_compute::GetStatus().result==soft_compute::Result::Small,"automatic small body stays on CPU");
    World reference(24),actual(24);
    reference.Step({processor::Mode::Cpu});actual.Step({});
    const auto result=soft_compute::GetStatus().result;
    Check(result==soft_compute::Result::Gpu || result==soft_compute::Result::MeasuredCpu,"automatic measures a supported soft body");
    if(result==soft_compute::Result::MeasuredCpu)for(int i=0;i<actual.cloth->m_nodes.size();++i)
        Check((actual.cloth->m_nodes[i].m_x-reference.cloth->m_nodes[i].m_x).length()<1e-6f,"automatic CPU choice never double-steps");
    actual.cloth->appendTetra(0,1,4,5);actual.Step(gpu);
    Check(soft_compute::GetStatus().result==soft_compute::Result::Unsupported,"volumetric tetra retains CPU compatibility");
    World unavailable;unavailable.cloth->predictMotion(1.f/60);
    const auto before=unavailable.cloth->m_nodes[1].m_x;
    Check(!unavailable.compute.Solve(*unavailable.cloth,{processor::Mode::Adapter,UINT64_MAX,L"missing"}),"missing GPU requests CPU fallback");
    Check(unavailable.cloth->m_nodes[1].m_x==before,"GPU failure leaves live simulation untouched");
    Check(unavailable.compute.Solve(*unavailable.cloth,gpu),"valid adapter recovers after a missing device");
}
void CollisionWorlds(const processor::Selection& gpu) {
    {
        World world(8,false,true);
        for(int i=0;i<world.cloth->m_nodes.size();++i)world.cloth->setMass(i,1);
        world.cloth->generateClusters(4);world.cloth->m_cfg.collisions=btSoftBody::fCollision::CL_RS|btSoftBody::fCollision::CL_SS;
        world.cloth->m_cfg.citerations=8;
        const auto before=soft_compute::GetStatus().clusterGpuSteps;
        const auto contactsBefore=observedClusterConstraints;
        for(int frame=0;frame<100;++frame) {
            world.Step(gpu);
            Check(soft_compute::GetStatus().result==soft_compute::Result::Gpu,"collision world keeps clustered nodes on GPU");
            for(int i=0;i<world.cloth->m_nodes.size();++i)Check(std::isfinite(world.cloth->m_nodes[i].m_x.length2()),"cluster collision trajectory stays finite");
        }
        const auto contacts=observedClusterConstraints-contactsBefore;
        Check(contacts>0&&soft_compute::GetStatus().clusterGpuSteps>before,"Bullet-generated cluster collisions reach GPU");
        std::printf("cluster collision world contacts=%u GPU_steps=%llu\n",contacts,soft_compute::GetStatus().clusterGpuSteps-before);
    }
    {
        World world(8);
        std::unique_ptr<btSoftBody> peer(btSoftBodyHelpers::CreatePatch(world.world.getWorldInfo(),btVector3(-5,4.8f,-5),btVector3(5,4.8f,-5),
            btVector3(-5,4.8f,5),btVector3(5,4.8f,5),8,8,15,true));
        world.cloth->m_cfg.collisions|=btSoftBody::fCollision::VF_SS;peer->m_cfg.collisions|=btSoftBody::fCollision::VF_SS;
        world.world.addSoftBody(peer.get());unsigned contacts=0;const auto before=soft_compute::GetStatus().gpuSteps;
        for(int frame=0;frame<40;++frame) {
            world.Step(gpu);contacts+=world.cloth->m_scontacts.size()+peer->m_scontacts.size();
            for(auto* body:{world.cloth.get(),peer.get()})for(int i=0;i<body->m_nodes.size();++i)
                Check(std::isfinite(body->m_nodes[i].m_x.length2()),"soft contact trajectory stays finite");
        }
        Check(contacts>0&&soft_compute::GetStatus().gpuSteps>=before+80,"Bullet-generated soft contacts keep both bodies on GPU");
        std::printf("soft collision world contacts=%u GPU_steps=%llu\n",contacts,soft_compute::GetStatus().gpuSteps-before);
        world.world.removeSoftBody(peer.get());
    }
}
void Benchmark(const processor::Selection& gpu) {
    World world(128);world.cloth->m_cfg.piterations=16;
    double samples[4]{};
    for(int block=0;block<4;++block) {
        const auto choice=(block==0||block==3)?processor::Selection{processor::Mode::Cpu}:gpu;
        world.Reset();for(int warm=0;warm<8;++warm)world.Step(choice);
        const auto start=std::chrono::steady_clock::now();
        for(int i=0;i<32;++i)world.Step(choice);
        samples[block]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/32;
        std::printf("ABBA block=%d backend=%s total_step_ms=%.4f\n",block,(block==0||block==3)?"CPU":"GPU",samples[block]);
    }
    std::printf("ABBA nodes=%d links=%d iterations=16 cpu_ms=%.4f gpu_ms=%.4f\n",world.cloth->m_nodes.size(),world.cloth->m_links.size(),(samples[0]+samples[3])*.5,(samples[1]+samples[2])*.5);
}
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto adapters=gpu::EnumerateAdapters();if(adapters.empty()){std::puts("FAIL no GPU adapter");return 1;}
    gpu::Cloth preflight;if(!preflight.Initialize(adapters[0].id)){std::printf("FAIL GPU setup: %s\n",preflight.Error());return 1;}
    const processor::Selection choice{processor::Mode::Adapter,adapters[0].id,adapters[0].name};
    DynamicsWorld::solveSoftBody=Solve;
    DynamicsWorld::solveClusters=SolveClusterWorld;
    Pair(choice,false,false,false,0,0);
    Pair(choice,true,false,false,0,0);
    Pair(choice,true,false,true,0,0);
    Pair(choice,false,true,false,0,0);
    Pair(choice,false,false,false,2,2);
    SceneFloor(choice);
    Grid(choice);
    SoftContacts(choice);Clusters(choice);
    FallbackAndAutomatic(choice);
    CollisionWorlds(choice);
    if(argc>1&&!std::strcmp(argv[1],"--benchmark"))Benchmark(choice);
    const auto& status=soft_compute::GetStatus();
    std::printf("cloth adapter=%ls gpu_steps=%llu cluster_gpu_steps=%llu cpu_steps=%llu detail=%s failures=%d\n",status.adapter.c_str(),status.gpuSteps,status.clusterGpuSteps,status.cpuSteps,status.detail.c_str(),failures);
    DynamicsWorld::solveSoftBody=nullptr;DynamicsWorld::solveClusters=nullptr;return failures?1:0;
}
