// MAINTENANCE: regression-keep; rate compensation, CFM stability and native settings controls.
#include "../src/window/physics_settings_controls.hpp"
#include "../src/physics/tuned_constraint_solver.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/playback_timing.hpp"
#include <btBulletDynamicsCommon.h>
#if !MMD_ORIGINAL_PROFILE
#include <BulletSoftBody/btSoftBodyRigidBodyCollisionConfiguration.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace mikumikustudio;
namespace ui = physics_settings_controls;
namespace {
int checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr,"FAIL %s\n",message); std::exit(1); }
}
void Parameters() {
    bake::Settings s;
    for (int rate : {15,30,60,75,120,240,480,960}) {
        const auto tuned = bake::SolverSettings(s,rate);
        Check(std::abs(std::pow(1.0-tuned.erp,rate/10.0)-std::pow(.8,6))<1e-6,
              "automatic ERP keeps the same error decay per second");
    }
    s.cfm=.01f;
    for (int rate : {30,60,120,240}) {
        const auto tuned=bake::SolverSettings(s,rate);
        Check(std::abs(tuned.erp*rate/tuned.cfm-1200)<.001,
              "compliant ERP/CFM conversion preserves stiffness");
        Check(std::abs((1-tuned.erp)/tuned.cfm-80)<.001,
              "compliant ERP/CFM conversion preserves damping");
    }
    s.parameterReferenceRateHz=960; s.solverIterations=175; s.baseRateHz=960;
    const auto tuned=bake::SolverSettings(s,960);
    Check(tuned.erp==s.erp && tuned.cfm==s.cfm && tuned.solverIterations==175,
          "parameters at their reference rate are unchanged");
    bake::SettingsError error;
    s.cfm=(std::numeric_limits<float>::max)();
    Check(bake::ValidateSettings(s,error),"finite CFM and values beyond sliders accepted");
    s.substeps=(std::numeric_limits<int>::max)();
    Check(!bake::ValidateSettings(s,error),"rate product overflow rejected");
    s.substeps=1; s.solverIterations=0;
    Check(!bake::ValidateSettings(s,error),"zero iterations rejected");
    s.solverIterations=175;
    const auto json=bake::EncodePreset(s); bake::Settings decoded;
    Check(bake::DecodePreset(json,decoded,error) && decoded.parameterReferenceRateHz==960 && decoded.solverIterations==175 && decoded.baseRateHz==960,
          "v2 preserves advanced settings beyond slider ranges");
    auto bad=json; const std::string field="\"parameterReferenceRateHz\": 960";
    const auto at=bad.find(field);
    Check(at!=std::string::npos,"v2 serializes the parameter reference rate");
    bad.replace(at,field.size(),"\"parameterReferenceRateHz\": 0");
    Check(!bake::DecodePreset(bad,decoded,error),"zero reference rate rejected");
    bad=json; const auto last=bad.find(",\n\t\"parameterReferenceRateHz\"");
    bad.erase(last,bad.find("\n}")-last);
    Check(!bake::DecodePreset(bad,decoded,error) && error==bake::SettingsError::MissingField,
          "v2 requires reference rate");

}

class Joint final : public btGeneric6DofConstraint {
    float cfm_;
public:
    Joint(btRigidBody& a,btRigidBody& b,float cfm)
        :btGeneric6DofConstraint(a,b,btTransform::getIdentity(),btTransform::getIdentity(),true),cfm_(cfm) {
        setLinearLowerLimit(btVector3(0,0,0)); setLinearUpperLimit(btVector3(0,0,0));
    }
    void getInfo2(btConstraintInfo2* info) override {
        btGeneric6DofConstraint::getInfo2(info);
        btConstraintInfo1 rows{}; getInfo1(&rows);
        for(int i=0;i<rows.m_numConstraintRows;++i)info->cfm[i*info->rowskip]=cfm_;
    }
};
float ConstraintRun(float cfm,bool tuned) {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher(&config); btDbvtBroadphase broadphase;
    TunedConstraintSolver solver; btSequentialImpulseConstraintSolver legacy;
    btDiscreteDynamicsWorld world(&dispatcher,&broadphase,tuned?&solver:&legacy,&config);
    world.setGravity(btVector3(0,-10,0)); world.getSolverInfo().m_globalCfm=cfm;
    btSphereShape shape(.1f); btVector3 inertia; shape.calculateLocalInertia(1,inertia);
    btRigidBody fixed(0,nullptr,&shape), body(1,nullptr,&shape,inertia);
    fixed.setWorldTransform(btTransform::getIdentity()); body.setWorldTransform(btTransform::getIdentity());
    body.setActivationState(DISABLE_DEACTIVATION);
    world.addRigidBody(&fixed); world.addRigidBody(&body);
    Joint joint(fixed,body,cfm); world.addConstraint(&joint,true);
    for(int i=0;i<120;++i) {
        world.stepSimulation(1.f/60,0);
        Check(std::isfinite(body.getWorldTransform().getOrigin().y()) && body.getLinearVelocity().length()<30,
              "large joint CFM remains finite and bounded by free fall");
    }
    const float y=body.getWorldTransform().getOrigin().y();
    world.removeConstraint(&joint); world.removeRigidBody(&body); world.removeRigidBody(&fixed);
    return y;
}
#if !MMD_ORIGINAL_PROFILE
float SoftDecay(int rate,bool simple) {
    btSoftBodyRigidBodyCollisionConfiguration config;
    btCollisionDispatcher dispatcher(&config); btDbvtBroadphase broadphase; TunedConstraintSolver solver;
    DynamicsWorld world(&dispatcher,&broadphase,&solver,&config);
    world.setGravity(btVector3(0,0,0)); world.SetRateCompensation(simple?rate:60);
    world.getWorldInfo().m_broadphase=&broadphase; world.getWorldInfo().m_dispatcher=&dispatcher;
    world.getWorldInfo().m_sparsesdf.Initialize();
    const btVector3 point(0,0,0); const btScalar mass=1;
    btSoftBody soft(&world.getWorldInfo(),1,&point,&mass);
    soft.m_cfg.kDP=.1f; soft.m_nodes[0].m_v=btVector3(1,0,0);
    world.addSoftBody(&soft);
    for(int i=0;i<rate/10;++i) {
        world.stepSimulation(1.f/rate,0);
        Check(soft.m_cfg.kDP==.1f,"soft compensation never modifies the authored model damping");
    }
    const float speed=soft.m_nodes[0].m_v.x();
    world.removeSoftBody(&soft); return speed;
}
#endif

INT_PTR CALLBACK DialogProc(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    auto* editor=reinterpret_cast<ui::Editor*>(GetWindowLongPtrW(window,DWLP_USER));
    if(msg==WM_INITDIALOG) {
        editor=reinterpret_cast<ui::Editor*>(lp); SetWindowLongPtrW(window,DWLP_USER,lp);
        editor->Create(window,2,10,10,360,bake::Settings{}); return TRUE;
    }
    return editor && editor->Handle(msg,wp,lp);
}
void Controls() {
    DLGTEMPLATE layout{}; layout.style=WS_POPUP|DS_SETFONT; layout.cx=380; layout.cy=244;
    std::vector<WORD> data((sizeof layout+1)/2); std::memcpy(data.data(),&layout,sizeof layout);
    data.insert(data.end(),{0,0,0,9}); const wchar_t font[]=L"Segoe UI";
    data.insert(data.end(),std::begin(font),std::end(font));
    ui::Editor editor;
    HWND window=CreateDialogIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<DLGTEMPLATE*>(data.data()),nullptr,DialogProc,reinterpret_cast<LPARAM>(&editor));
    Check(window!=nullptr,"create isolated native settings dialog");
    for(int id:{ui::Rate,ui::Erp,ui::Cfm,ui::Iterations}) {
        Check(GetDlgItem(window,id)&&GetDlgItem(window,id+ui::SliderOffset),"every parameter has a slider and exact input");
        Check(IsWindowEnabled(GetDlgItem(window,id)),"all physical parameters remain editable");
    }
    auto maximum=bake::Settings{}; maximum.cfm=(std::numeric_limits<float>::max)();
    editor.Set(maximum); bake::Settings roundtrip; std::wstring maxError;
    Check(editor.Read(roundtrip,maxError)&&roundtrip.cfm==maximum.cfm,"maximum float survives the editor round trip");
    editor.Set(bake::Settings{});
    SetDlgItemTextW(window,ui::Rate,L"960"); SetDlgItemTextW(window,ui::Cfm,L"1000001");
    SetDlgItemTextW(window,ui::Erp,L"0.9"); SetDlgItemTextW(window,ui::Iterations,L"175");
    bake::Settings s; std::wstring error;
    Check(editor.Read(s,error) && s.baseRateHz==960 && s.cfm==1000001 && s.erp==.9f && s.solverIterations==175,
          "exact input exceeding sliders survives edit notifications and Apply");
    SetDlgItemTextW(window,ui::Rate,L"120");
    Check(editor.Read(s,error)&&s.erp==.9f&&s.solverIterations==175,"rate changes preserve authored reference parameters");
    Check(!GetDlgItem(window,3700),"automatic compensation adds no mode switch");
    SetDlgItemTextW(window,ui::Erp,L"1.1"); Check(!editor.Read(s,error),"out-of-domain ERP rejected");
    SetDlgItemTextW(window,ui::Erp,L"0.2"); SetDlgItemTextW(window,ui::Cfm,L"nan");
    Check(!editor.Read(s,error),"nonfinite CFM rejected");
    SetDlgItemTextW(window,ui::Cfm,L"0"); SetDlgItemTextW(window,ui::Rate,L"0");
    Check(!editor.Read(s,error),"zero rate rejected");
    auto slider=GetDlgItem(window,ui::Rate+ui::SliderOffset);
    SendMessageW(slider,TBM_SETPOS,TRUE,1000); SendMessageW(window,WM_HSCROLL,TB_THUMBPOSITION,reinterpret_cast<LPARAM>(slider));
    Check(editor.Read(s,error)&&s.baseRateHz==480,"trackbar changes the paired exact rate input");
    DestroyWindow(window);
}
}
int main() {
    Parameters(); Controls();
    const float legacy=ConstraintRun(0,false), unchanged=ConstraintRun(0,true);
    Check(legacy==unchanged,"zero CFM retains the original solver result exactly");
    float last=0;
    for(float cfm:{.001f,.1f,1.f,100.f,1000001.f,(std::numeric_limits<float>::max)()}) {
        const float y=ConstraintRun(cfm,true);
        Check(y<=last+.001f,"increasing CFM softens the joint without reversing stiffness"); last=y;
    }
#if !MMD_ORIGINAL_PROFILE
    const float reference=SoftDecay(60,true);
    for(int rate:{120,240,480})Check(std::abs(SoftDecay(rate,true)-reference)<.0001f,"soft-body velocity decay matches the 60 Hz reference");
    Check(SoftDecay(240,false)<reference*.5f,"uncompensated control shows the old frame-rate-dependent damping");
#endif
    std::printf("physics_tuning_tests: %d checks passed\n",checks);
}
