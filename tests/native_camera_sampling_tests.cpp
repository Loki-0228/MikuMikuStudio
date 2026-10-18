// MAINTENANCE: regression-keep; ordinary camera export and native channel bake.
// CPU-only: real ReloadModels, curve_host, VMD encoder and bake Prepare/Commit.
#define NOMINMAX
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/animation_curve_motion.hpp"
#include "mikudancestudio/channel_bake.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/native_frame_lines.hpp"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace allocation_fault {
thread_local long remaining=-1;
void* Allocate(std::size_t bytes){
    if(remaining==0)throw std::bad_alloc();
    if(remaining>0)--remaining;
    if(auto* p=std::malloc(bytes?bytes:1))return p;
    throw std::bad_alloc();
}
}
void* operator new(std::size_t bytes){return allocation_fault::Allocate(bytes);}
void* operator new[](std::size_t bytes){return allocation_fault::Allocate(bytes);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mikudancestudio;
namespace ac=animation_curve;
namespace {
constexpr double pi=3.14159265358979323846;
unsigned checks=0;
void Check(bool value,const char* why){++checks;if(!value){std::printf("FAIL %s\n",why);std::exit(1);}}
using Channels=std::vector<curve_editor::Channel>;
double Value(const curve_editor::Channel& channel,double frame){return channel.sample?channel.sample(channel,frame).value:ac::Evaluate(channel.track,frame).value;}
std::array<float,9> Pose(MMDApp& app){return {app.CameraPositionX(),app.CameraPositionY(),app.CameraPositionZ(),
    app.CameraRotation()[0],app.CameraRotation()[1],app.CameraRotation()[2],app.CameraDistance(),app.CameraFov(),float(app.CameraPerspective())};}
std::uint32_t Bits(float value){std::uint32_t bits=0;std::memcpy(&bits,&value,4);return bits;}
std::uint64_t Ulps(float a,float b){
    const auto ordered=[](float value){const auto raw=Bits(value);return raw&0x80000000u?~raw:raw|0x80000000u;};
    const auto x=ordered(a),y=ordered(b);return x>y?std::uint64_t(x)-y:std::uint64_t(y)-x;
}
struct Fixture {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    mdl::CameraKey* keys=mdl::KeyPoolCreate<mdl::CameraKey>(128);
    Fixture(const std::array<unsigned char,4>& controls){
        Check(keys!=nullptr,"allocate managed ordinary camera keys");
        app->state=MMDAppState{};app->CameraKeys()=keys;app->CameraMode()=1;app->EnglishUI()=1;
        app->LastRegisteredFrame()=80;
        for(unsigned i=0;i<3;++i){auto& key=keys[i];key.frame=40*i;key.previous=i?i-1:0;key.next=i<2?i+1:0;
            key.eye[0]=i==0?0.f:i==1?10.f:-4.f;key.eye[1]=float(i)-2;key.eye[2]=float(i)*.25f;
            key.target[0]=float(i==1?4*pi:i==2?-2*pi:0);key.target[1]=float(i)*.2f;key.target[2]=float(i)*-.3f;
            key.distance=-12.f+float(i)*1.5f;key.fov=i==0?30:i==1?75:45;key.perspective=i==1?1:0;
            key.parentModel=-1;key.parentBone=0;key.selected=static_cast<unsigned char>(i+1);
            for(unsigned lane=0;lane<6;++lane)for(unsigned c=0;c<4;++c)key.interpolation[c][lane]=controls[c];
        }
    }
    ~Fixture(){curve_host::Reset(app.get());mdl::KeyPoolDestroy(app->CameraKeys());app->CameraKeys()=nullptr;}
    Channels Read(){Channels result;std::wstring error;Check(curve_host::Read(*app,curve_host::Kind::Camera,0,result,error),"read ordinary unmodified native camera");return result;}
    std::vector<unsigned char> Raw()const{const auto* p=reinterpret_cast<const unsigned char*>(app->CameraKeys());
        return {p,p+mdl::KeyPoolCapacity(app->CameraKeys())*sizeof(mdl::CameraKey)};}
};
std::uint32_t U32(const std::vector<std::uint8_t>& bytes,std::size_t offset){Check(offset+4<=bytes.size(),"standard VMD field is in bounds");
    return std::uint32_t(bytes[offset])|std::uint32_t(bytes[offset+1])<<8|std::uint32_t(bytes[offset+2])<<16|std::uint32_t(bytes[offset+3])<<24;}
float F32(const std::vector<std::uint8_t>& bytes,std::size_t offset){const auto bits=U32(bytes,offset);float result=0;std::memcpy(&result,&bits,4);return result;}
void NativeExport(const std::array<unsigned char,4>& controls){
    Fixture f(controls);const auto input=f.Read(),saved=input;const auto raw=f.Raw();std::wstring error;
    Check(!curve_host::Has(f.app.get(),curve_host::Kind::Camera),"ordinary source has no authored or preserved-insertion layer");
    for(const auto& channel:input)Check(channel.preservedCameraIntervals.empty(),"source uses ordinary raw timing without exact slices");
    Channels prepared;
    Check(curve_host::PrepareMotionChannels(input,prepared,error),"prepare ordinary raw camera export");
    Check(prepared[0].track.keys.size()==3&&prepared[0].nativeControls==input[0].nativeControls&&prepared[0].sample,
        "ordinary camera stays sparse and retains its raw timing sampler");
    auto motion=curve_motion::CaptureChannels(curve_motion::Kind::Camera,prepared);
    std::vector<std::uint8_t> bytes;std::string detail;
    Check(curve_motion::Encode(motion,bytes,detail),"encode ordinary native camera through production VMD writer");
    Check(U32(bytes,50)==0&&U32(bytes,54)==0&&U32(bytes,58)==3,"standard camera section remains sparse with unchanged preceding sections");
    for(unsigned index=0;index<3;++index){const auto row=62+61*index;const auto& key=f.keys[index];
        Check(U32(bytes,row)==key.frame&&Bits(F32(bytes,row+4))==Bits(key.distance),"native export retains camera frame and distance bits");
        for(unsigned axis=0;axis<3;++axis){
            Check(Bits(F32(bytes,row+8+4*axis))==Bits(key.eye[axis]),"native export retains raw position floats");
            Check(Bits(F32(bytes,row+20+4*axis))==Bits(key.target[axis]),"native export retains Euler winding and raw rotation floats");
        }
        for(unsigned lane=0;lane<6;++lane)for(unsigned component=0;component<4;++component){
            const unsigned order[]={0,2,1,3};
            Check(bytes[row+32+4*lane+component]==key.interpolation[order[component]][lane],"native export retains every standard control byte including frame zero");
        }
        Check(U32(bytes,row+56)==std::uint32_t(key.fov)&&bytes[row+60]==key.perspective,"native export retains integer FOV and perspective bytes");
    }
    Check(f.Raw()==raw&&!curve_host::Has(f.app.get(),curve_host::Kind::Camera),"export preparation and encoding never publish authoring or native changes");
    Check(saved[0].track.keys.size()==input[0].track.keys.size(),"old camera input remains sparse after export");
}
void MixedExport(){
    Fixture f({100,10,20,115});auto source=f.Read();std::wstring error;
    source[1].sample={};source[1].nativeControls.clear();source[1].controlGeometry.clear();
    source[1].track.keys[0].outSlope=1.25;
    Channels output;Check(curve_host::PrepareMotionChannels(source,output,error),"prepare mixed native and authored scalar camera export");
    Check(output[0].track.keys.size()==81&&!output[0].sample,"mixed export samples actual native timing instead of scalar canvas handles");
    Check(output[1].track.keys.size()==source[1].track.keys.size()&&output[1].track.keys[0].outSlope==1.25,
        "mixed export retains the authored Free curve rather than replacing it with native timing");
    for(int frame=0;frame<=80;++frame)Check(ac::Evaluate(output[0].track,frame).value==Value(source[0],frame),"mixed export integer proxy matches canonical native sampling exactly");
    auto opaque=source;opaque[0].sample=[](const curve_editor::Channel&,double frame)->ac::Sample{return {frame*.125+3.0,.125};};
    Check(curve_host::PrepareMotionChannels(opaque,output,error),"prepare a camera with an opaque non-native sampler");
    for(int frame=0;frame<=80;++frame)Check(ac::Evaluate(output[0].track,frame).value==3+frame*.125,"opaque callback is sampled instead of being mistaken for raw native metadata");
}
void ReconciledRoot(){
    Fixture f({100,10,20,115});auto source=f.Read();std::wstring error;
    Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,source,error),"seed an authored camera without changing native controls");
    for(unsigned lane=0;lane<6;++lane)for(unsigned component=0;component<4;++component)
        Check(f.app->CameraKeys()[0].interpolation[component][lane]==static_cast<unsigned char>(source[lane<4?lane:lane==4?6:7].nativeControls[0][component]),
            "writing an unchanged camera retains its original unused root control bytes");
    f.app->CameraKeys()[1].eye[0]+=.125f;
    const auto reconciled=f.Read();
    for(unsigned c=0;c<8;++c){
        if(reconciled[c].nativeControls[0]!=source[c].nativeControls[0])std::printf("ROOT_RECONCILE channel=%u before=%.0f/%.0f/%.0f/%.0f after=%.0f/%.0f/%.0f/%.0f\n",c,
            source[c].nativeControls[0][0],source[c].nativeControls[0][1],source[c].nativeControls[0][2],source[c].nativeControls[0][3],
            reconciled[c].nativeControls[0][0],reconciled[c].nativeControls[0][1],reconciled[c].nativeControls[0][2],reconciled[c].nativeControls[0][3]);
        Check(reconciled[c].nativeControls[0]==source[c].nativeControls[0],"native post-root edits reconcile authoring without clearing the root's raw controls");
    }
    Channels prepared;Check(curve_host::PrepareMotionChannels(reconciled,prepared,error),"prepare reconciled ordinary native timing");
    auto motion=curve_motion::CaptureChannels(curve_motion::Kind::Camera,prepared);std::vector<std::uint8_t> bytes;std::string detail;
    Check(curve_motion::Encode(motion,bytes,detail)&&U32(bytes,58)==3,"reconciled native camera remains eligible for sparse export");
    for(unsigned lane=0;lane<6;++lane)for(unsigned component=0;component<4;++component){const unsigned order[]={0,2,1,3};
        Check(bytes[62+32+4*lane+component]==f.app->CameraKeys()[0].interpolation[order[component]][lane],
            "reconciled raw export retains every unused root control byte");}
    f.app->CameraKeys()[0].interpolation[0][0]=255;
    const auto unused=f.Read();
    Check(unused[0].nativeControls[0][0]==255&&curve_host::Write(*f.app,curve_host::Kind::Camera,0,unused,error)&&
        f.app->CameraKeys()[0].interpolation[0][0]==255,"unused root bytes retain their full byte domain without changing active controls");
    const auto authored=f.Read();const auto native=f.Raw();
    const auto encodeTracks=[](const Channels& channels){
        std::vector<ac::NamedTrack> tracks;for(const auto& channel:channels)tracks.push_back({channel.id,channel.track});
        std::vector<std::uint8_t> result;std::string message;
        Check(ac::Encode(tracks,result,message),"encode camera tracks for strict PMM restoration comparison");return result;
    };
    const auto authoredBytes=encodeTracks(authored);pmm_extension::Document document;
    curve_host::Capture(*f.app,document);curve_host::Reset(f.app.get());curve_host::Load(*f.app,document);
    Check(curve_host::Has(f.app.get(),curve_host::Kind::Camera),"PMM restores the authoring layer with unused root byte 255");
    const auto restored=f.Read();
    Check(f.Raw()==native&&encodeTracks(restored)==authoredBytes,
        "PMM authoring restoration preserves the entire native pool and exact authored tracks with root byte 255");
    for(unsigned c=0;c<8;++c)Check(restored[c].nativeControls[0]==authored[c].nativeControls[0],
        "PMM reconstructs every unused root control from the unchanged standard native record");
}
#if !MMD_ORIGINAL_PROFILE
void DeletedRootExport(){
    Fixture f({100,10,20,115});f.app->CameraParentModel()=-1;f.keys[1].frame=1;
    for(unsigned lane=0;lane<6;++lane)for(unsigned component=0;component<4;++component){
        f.keys[0].interpolation[component][lane]=static_cast<unsigned char>(200+4*lane+component);
        f.keys[1].interpolation[component][lane]=static_cast<unsigned char>(10+17*lane+component);
    }
    const auto source=f.Read();std::wstring error;
    Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,source,error),"seed native camera authoring before deleting its zero frame");
    const auto winner=f.app->CameraKeys()[1];const auto before=f.Raw();
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanCameraFrameLine(*f.app,0,false,change)==MMS_OK&&change&&f.Raw()==before,
        "plan actual zero-frame deletion without changing the native winner or authoring");
    const auto publish=[&](bool forward){
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(forward,candidate)==MMS_OK&&candidate&&candidate->IsCurrent(),
            "prepare root-collision history against its current native and authored source");
        allocation_fault::remaining=0;candidate->Publish();allocation_fault::remaining=-1;
    };
    const auto exportedWinner=[&](){
        Check(f.app->CameraKeys()[0].frame==0&&f.app->CameraKeys()[0].next==2&&f.app->CameraKeys()[2].frame==79,
            "actual zero-frame deletion promotes frame one without changing the native winner rule");
        Check(std::memcmp(f.app->CameraKeys()[0].interpolation,winner.interpolation,sizeof(winner.interpolation))==0,
            "native root collision preserves all 24 control bytes from the winning frame one");
        const auto channels=f.Read();
        for(unsigned lane=0;lane<6;++lane)for(unsigned component=0;component<4;++component){
            const unsigned channel=lane<4?lane:lane==4?6:7;
            Check(channels[channel].nativeControls[0][component]==winner.interpolation[component][lane],
                "retimed authoring root metadata follows the actual native winner after read");
        }
        Channels prepared;Check(curve_host::PrepareMotionChannels(channels,prepared,error),"prepare sparse native export after root collision");
        auto motion=curve_motion::CaptureChannels(curve_motion::Kind::Camera,prepared);
        std::vector<std::uint8_t> bytes;std::string detail;
        Check(curve_motion::Encode(motion,bytes,detail)&&U32(bytes,58)==2,"root-collision camera remains sparse through capture and VMD encoding");
        for(unsigned lane=0;lane<6;++lane)for(unsigned component=0;component<4;++component){const unsigned order[]={0,2,1,3};
            Check(bytes[62+32+4*lane+component]==winner.interpolation[order[component]][lane],
                "sparse VMD export preserves all 24 unused control bytes from the promoted root");
        }
    };
    publish(true);exportedWinner();publish(false);
    Check(f.Raw()==before,"root-collision undo restores the original native pool including discarded root controls");
    publish(true);exportedWinner();
}
#endif
void Bake(const std::array<unsigned char,4>& controls,int first,int last,int step){
    Fixture f(controls);const auto source=f.Read();const auto raw=f.Raw();std::wstring error;
    std::array<std::array<float,9>,81> legacy{};
    for(int frame=0;frame<=80;++frame){f.app->CurrentFrame()=frame;ReloadModels(f.app.get());legacy[frame]=Pose(*f.app);}
    f.app->CurrentFrame()=17;const auto live=Pose(*f.app);
    channel_bake::Request request;request.scope=channel_bake::Scope::Scene;request.firstFrame=first;request.lastFrame=last;request.step=step;
    auto& selected=request.selection.Touch(channel_bake::Kind::Camera,0,11);selected.fields.assign(11,false);
    for(const unsigned field:{0u,3u,7u,8u})selected.fields[field]=true;
    channel_bake::Plan plan;
    Check(channel_bake::Prepare(*f.app,request,plan,error)==channel_bake::Error::None,"prepare native camera baking on private copies");
    Check(f.Raw()==raw&&Pose(*f.app)==live&&f.app->CurrentFrame()==17&&!curve_host::Has(f.app.get(),curve_host::Kind::Camera),
        "camera bake preparation leaves native bytes authoring playhead and live pose unchanged");
    Check(channel_bake::Commit(*f.app,plan,error)==channel_bake::Error::None,"commit actual native camera bake");
    const auto baked=f.Read();
    double largest=0;std::uint64_t largestUlp=0;int worstFrame=-1;unsigned worstField=0;
    for(int frame=first;frame<=last;++frame){if((frame-first)%step&&frame!=last)continue;
        for(const unsigned field:{0u,3u,7u,8u}){
            const double expected=Value(source[field],frame),actual=Value(baked[field],frame);
            Check(actual==expected,"baked key uses the unchanged canonical evaluator rather than its scalar proxy");
            const float authored=field>=3&&field<6?float(actual*pi/180):float(actual);
            const double delta=std::abs(double(authored)-legacy[frame][field]);const auto ulp=Ulps(authored,legacy[frame][field]);
            if(delta>largest){largest=delta;worstFrame=frame;worstField=field;}largestUlp=std::max(largestUlp,ulp);
        }
    }
    std::printf("NATIVE_ORACLE first=%d last=%d step=%d maxAbs=%.17g maxULP=%llu worstFrame=%d field=%u\n",
        first,last,step,largest,static_cast<unsigned long long>(largestUlp),worstFrame,worstField);
    // Report the pre-existing canonical/native float-order boundary separately.
    // The legacy channel_bake Near threshold remains 1e-6; it is not widened.
    Check(largest<1.e-6,"canonical camera baking stays within the existing 1e-6 native-value tolerance");
    for(unsigned field=0;field<8;++field)for(int tick=0;tick<=320;++tick){const double frame=tick*.25;
        if(selected.fields[field]&&frame>=first&&frame<=last)continue;
        Check(Value(baked[field],frame)==Value(source[field],frame),"partial bake preserves exact canonical native timing outside range and in unselected peers");
    }
    pmm_extension::Document document;curve_host::Capture(*f.app,document);curve_host::Reset(f.app.get());curve_host::Load(*f.app,document);
    const auto restored=f.Read();
    for(unsigned field=0;field<9;++field)for(int tick=0;tick<=320;++tick)
        Check(Value(restored[field],tick*.25)==Value(baked[field],tick*.25),"PMM reload retains sampled keys and exact outside/peer camera intervals");
}
void PreparationFailures(){
    Fixture f({0,127,127,0});const auto source=f.Read();const auto raw=f.Raw();
    std::vector<bool> fields(source.size());fields[0]=true;Channels output=source;std::wstring error;
    allocation_fault::remaining=0;
    const bool success=curve_host::PrepareCameraBakeSources(source,fields,1,39,output,error);
    allocation_fault::remaining=-1;
    Check(!success&&output[0].track.keys.size()==3&&output[0].preservedCameraIntervals.empty()&&f.Raw()==raw,
        "failed camera boundary and diagnostic allocation leave both input and output unchanged");
    Check(curve_host::PrepareCameraBakeSources(source,fields,1,39,output,error),"prepare representable camera endpoints independently of proxy handle restrictions");
    for(unsigned c=0;c<8;++c)Check(output[c].nativeControls[0]==source[c].nativeControls[0],
        "private boundary row remapping preserves matching root-frame controls");
    auto before=output[0];allocation_fault::remaining=0;
    const bool finished=curve_host::FinishCameraBakeChannel(source[0],source[0].track,output[0],error);
    allocation_fault::remaining=-1;
    Check(!finished&&output[0].track.keys.size()==before.track.keys.size()&&output[0].preservedCameraIntervals==before.preservedCameraIntervals,
        "failed sampled-camera preparation preserves its destination metadata and topology");
}
}
int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    for(const auto controls:{std::array<unsigned char,4>{100,10,20,115},{0,127,127,0},{20,20,107,107},{57,0,44,127}}){
        NativeExport(controls);Bake(controls,0,80,1);Bake(controls,7,29,2);Bake(controls,1,1,1);
    }
    MixedExport();ReconciledRoot();PreparationFailures();
#if !MMD_ORIGINAL_PROFILE
    DeletedRootExport();
#endif
    std::printf("PASS ordinary native camera sampling: %u checks\n",checks);return 0;
}
