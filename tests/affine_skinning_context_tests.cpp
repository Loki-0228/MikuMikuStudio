// Real D3DX CPU exports versus the independent immutable math context.
// Both SDEF adapters compile in this TU with the same floating-point options.
#define NOMINMAX
#include "mikumikustudio/affine_skinning_context.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {thread_local bool rejectAllocations=false;thread_local unsigned allocationAttempts=0;}
void* operator new(std::size_t size) {
    ++allocationAttempts;if(rejectAllocations)throw std::bad_alloc();
    if(void* value=std::malloc(size?size:1))return value;throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete[](void* value)noexcept{std::free(value);}
void operator delete(void* value,std::size_t)noexcept{std::free(value);}
void operator delete[](void* value,std::size_t)noexcept{std::free(value);}

using namespace mikumikustudio;
namespace {
unsigned checks=0;const char* currentCase="setup";
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %s line %d: %s\n",currentCase,__LINE__,#x);std::exit(1);}}while(false)
template<class T>bool Bits(const T& a,const T& b){return std::memcmp(&a,&b,sizeof(T))==0;}
struct NoAllocation {
    bool previous=rejectAllocations;
    NoAllocation(){rejectAllocations=true;}
    ~NoAllocation(){rejectAllocations=previous;}
};
using Matrix=std::array<float,16>;
using Quaternion=std::array<float,4>;
using Vector=std::array<float,3>;

// Assigned once from the actual loaded host table. Workers never call these
// adapters: all native expected results are materialized on the main thread.
d3dx::FnVecQuat nativeQuaternion=nullptr;
d3dx::FnMatrixQuat nativeMatrix=nullptr;
unsigned quaternionCalls=0,matrixCalls=0;
void NativeQuaternion(float out[4],const float matrix[16]) {
    ++quaternionCalls;nativeQuaternion(out,reinterpret_cast<const d3dx::D3DXMATRIXF*>(matrix));
}
void NativeMatrix(float out[16],const float quaternion[4]) {
    ++matrixCalls;nativeMatrix(reinterpret_cast<d3dx::D3DXMATRIXF*>(out),quaternion);
}
const affine::RotationOps nativeOps{NativeQuaternion,NativeMatrix};
Matrix Identity(){Matrix matrix{};matrix[0]=matrix[5]=matrix[10]=matrix[15]=1;return matrix;}
Quaternion Axis(float x,float y,float z,float angle) {
    const float length=std::sqrt(x*x+y*y+z*z);const float sine=std::sin(angle*.5f)/length;
    return {{x*sine,y*sine,z*sine,std::cos(angle*.5f)}};
}
Matrix Rotation(Quaternion quaternion,Vector translation={}) {
    Matrix matrix{};NativeMatrix(matrix.data(),quaternion.data());
    matrix[12]=translation[0];matrix[13]=translation[1];matrix[14]=translation[2];return matrix;
}
Matrix LinearThenRotation(const std::array<float,9>& linear,const Matrix& rotation) {
    Matrix result=rotation;
    for(unsigned row=0;row<3;++row)for(unsigned column=0;column<3;++column)
        result[row*4+column]=(linear[row*3]*rotation[column]+linear[row*3+1]*rotation[4+column])+linear[row*3+2]*rotation[8+column];
    return result;
}
struct Bone {
    Matrix matrix{};
    Quaternion native{};
    affine::BoneAux aux{};
    affine::BoneRef Ref()const{return {matrix.data(),&aux};}
};
Bone MakeBone(const Matrix& matrix) {
    Bone bone;bone.matrix=matrix;NativeQuaternion(bone.native.data(),matrix.data());
    affine::PrecomputeBone(matrix.data(),bone.native.data(),bone.aux);return bone;
}
struct SkinCase {
    Bone first{},second{};
    float weight=0;
    Vector position{{.35f,-1.25f,2.05f}},normal{{.43643578f,-.87287156f,.21821789f}},center{{.2f,.1f,-.3f}};
    Vector arm0{{1.1f,-.4f,.6f}},arm1{{-.7f,1.3f,-.2f}};
    affine::Skinned expected{};
};
void NativeSkin(const SkinCase& input,const affine::RotationOps& ops,affine::Skinned& out) {
    affine::SkinSdef(input.first.Ref(),input.second.Ref(),input.weight,input.position.data(),input.normal.data(),
        input.center.data(),input.arm0.data(),input.arm1.data(),ops,out);
}
void PrivateSkin(const SkinCase& input,const BoneTransformContext& context,affine::Skinned& out) {
    affine::SkinSdefPrivate(input.first.Ref(),input.second.Ref(),input.weight,input.position.data(),input.normal.data(),
        input.center.data(),input.arm0.data(),input.arm1.data(),context,out);
}
SkinCase Case(const Matrix& first,const Matrix& second,float weight,bool rebase=true) {
    SkinCase input;input.first=MakeBone(first);input.second=MakeBone(second);input.weight=weight;
    if(rebase)for(unsigned i=0;i<3;++i){const float pivot=input.arm0[i]*weight+input.arm1[i]*(1.f-weight);input.arm0[i]-=pivot;input.arm1[i]-=pivot;}
    const auto beforeQuat=quaternionCalls,beforeMatrix=matrixCalls;
    NativeSkin(input,nativeOps,input.expected);
    const bool affinePath=((input.first.aux.flags|input.second.aux.flags)&affine::kBoneFlagAffine)!=0;
    CHECK(quaternionCalls-beforeQuat==(affinePath?0u:2u));CHECK(matrixCalls-beforeMatrix==(affinePath?0u:1u));
    return input;
}
struct Conversion {
    Matrix matrix{},expectedMatrix{};
    Quaternion quaternion{},expectedQuaternion{};
};
Conversion ConversionCase(const Matrix& matrix,const Quaternion& quaternion) {
    Conversion input;input.matrix=matrix;input.quaternion=quaternion;
    NativeQuaternion(input.expectedQuaternion.data(),matrix.data());NativeMatrix(input.expectedMatrix.data(),quaternion.data());return input;
}
struct Random {
    uint32_t state=0x127abcd9u;
    float Next(){state=state*1664525u+1013904223u;return float(int32_t(state%20001u)-10000)/8192.f;}
    Quaternion Unit(){Quaternion q{{Next(),Next(),Next(),Next()}};float length=std::sqrt(((q[0]*q[0]+q[1]*q[1])+q[2]*q[2])+q[3]*q[3]);if(!length)return {{0,0,0,1}};for(float& value:q)value/=length;return q;}
};
std::vector<Conversion> Conversions() {
    currentCase="real D3DX conversion fixtures";std::vector<Conversion> result;
    const Quaternion quaternions[]{{{0,0,0,1}},{{0,0,0,0}},{{-0.f,0,-0.f,-1}},{{1,0,0,0}},{{0,1,0,0}},{{0,0,1,0}},
        {{.5f,.5f,.5f,.5f}},{{.3f,-.6f,.1f,1.7f}},{{1e-20f,-1e-20f,0,1}},{{1e10f,-1e10f,1e10f,1e10f}}};
    for(const auto& quaternion:quaternions)result.push_back(ConversionCase(Rotation(quaternion),quaternion));
    Matrix zero{};result.push_back(ConversionCase(zero,{{0,0,0,0}}));
    Matrix negativeZero{};negativeZero.fill(-0.f);result.push_back(ConversionCase(negativeZero,{{-0.f,-0.f,-0.f,-0.f}}));
    Matrix diagonal=Identity();diagonal[0]=diagonal[5]=diagonal[10]=-1;result.push_back(ConversionCase(diagonal,{{1,0,0,0}}));
    for(unsigned axis=0;axis<3;++axis){diagonal=Identity();diagonal[axis*5]=0;result.push_back(ConversionCase(diagonal,{{.2f,-.3f,.4f,.7f}}));}
    Random random;
    for(unsigned i=0;i<512;++i) {
        const auto q=random.Unit();auto matrix=Rotation(q,{{random.Next(),random.Next(),random.Next()}});
        if(i%3==0)for(unsigned row=0;row<3;++row)for(unsigned column=0;column<3;++column)matrix[row*4+column]+=random.Next()*.8f;
        // These exports are raw math helpers; nonunit quaternions are legal
        // inputs too, and must not acquire SDK normalization semantics.
        const Quaternion input=i%2?q:Quaternion{{random.Next(),random.Next(),random.Next(),random.Next()}};
        result.push_back(ConversionCase(matrix,input));
    }
    return result;
}
std::vector<SkinCase> SkinCases() {
    currentCase="real native SDEF fixtures";std::vector<SkinCase> result;
    const auto first=Rotation(Axis(0,1,0,.9f),{{.25f,-1.5f,3.f}});
    const auto second=Rotation(Axis(1,0,1,-1.7f),{{-2.f,.75f,-.5f}});
    const auto scaled=LinearThenRotation({{2,0,0,0,.5f,0,0,0,1.3f}},first);
    const auto sheared=LinearThenRotation({{1.5f,.25f,-.125f,.25f,.9f,.125f,-.125f,.125f,1.2f}},second);
    auto singular=first;for(unsigned column=0;column<3;++column)singular[4+column]=0;
    auto mirrored=second;for(unsigned column=0;column<3;++column)mirrored[column]=-mirrored[column];
    const auto tiny=LinearThenRotation({{.0001f,0,0,0,1,0,0,0,2}},second);
    Matrix missing{};
    CHECK(MakeBone(first).aux.flags==0&&MakeBone(second).aux.flags==0);
    CHECK(MakeBone(scaled).aux.flags==affine::kBoneFlagAffine&&MakeBone(sheared).aux.flags==affine::kBoneFlagAffine);
    CHECK(MakeBone(singular).aux.flags==0&&MakeBone(mirrored).aux.flags==0&&MakeBone(missing).aux.flags==0);
    const float weights[]{0.f,-0.f,1.f,std::nextafter(0.f,1.f),std::nextafter(1.f,0.f),std::nextafter(1.f,2.f),.25f,.375f,.5f,.75f,-.5f,1.5f};
    for(float weight:weights) {
        result.push_back(Case(first,second,weight));result.push_back(Case(scaled,sheared,weight));
        result.push_back(Case(first,scaled,weight));result.push_back(Case(scaled,first,weight));
        result.push_back(Case(singular,second,weight));result.push_back(Case(mirrored,first,weight));
        result.push_back(Case(missing,second,weight));result.push_back(Case(first,missing,weight));
        result.push_back(Case(missing,missing,weight));result.push_back(Case(tiny,sheared,weight));
    }
    // Boundary inputs remain finite. Any overflow output retains the actual
    // native bytes, rather than being silently clamped by the private adapter.
    for(float weight:{(std::numeric_limits<float>::max)(),-(std::numeric_limits<float>::max)()})result.push_back(Case(first,second,weight,false));
    auto zeroNormal=Case(first,second,.375f);zeroNormal.normal={{0,-0.f,0}};NativeSkin(zeroNormal,nativeOps,zeroNormal.expected);result.push_back(zeroNormal);
    auto zeroAffineNormal=Case(scaled,sheared,.375f);zeroAffineNormal.normal={{0,-0.f,0}};NativeSkin(zeroAffineNormal,nativeOps,zeroAffineNormal.expected);result.push_back(zeroAffineNormal);
    Random random;
    for(unsigned i=0;i<512;++i) {
        auto a=Rotation(random.Unit(),{{random.Next()*3,random.Next()*3,random.Next()*3}});
        auto b=Rotation(random.Unit(),{{random.Next()*3,random.Next()*3,random.Next()*3}});
        if(i%3==1)a=LinearThenRotation({{1.5f,.2f,0,.2f,.8f,.1f,0,.1f,1.1f}},a);
        if(i%3==2)b=LinearThenRotation({{.75f,.125f,.1f,.125f,2,.2f,.1f,.2f,1.25f}},b);
        auto input=Case(a,b,random.Next());
        input.position={{random.Next(),random.Next(),random.Next()}};input.normal={{random.Next(),random.Next(),random.Next()}};
        NativeSkin(input,nativeOps,input.expected);result.push_back(input);
    }
    return result;
}
void WrongQuaternion(float out[4],const float[16]){out[0]=out[1]=out[2]=0;out[3]=1;}
void WrongMatrix(float out[16],const float[4]){const auto matrix=Identity();std::memcpy(out,matrix.data(),sizeof(matrix));}
void NondegenerateEvidence(const BoneTransformContext& context) {
    currentCase="nondegenerate native rotation evidence";
    auto input=Case(Rotation(Axis(1,2,3,.8f),{{2,-3,4}}),Rotation(Axis(-2,1,.5f,-1.2f),{{-1,5,.25f}}),.375f);
    const affine::RotationOps wrongOps{WrongQuaternion,WrongMatrix};affine::Skinned wrong{},actual{};
    NativeSkin(input,wrongOps,wrong);PrivateSkin(input,context,actual);
    CHECK(!Bits(wrong.position,input.expected.position)&&!Bits(wrong.normal,input.expected.normal));CHECK(Bits(actual,input.expected));
    CHECK(std::memcmp(actual.position,input.position.data(),sizeof(actual.position))!=0&&
        std::memcmp(actual.normal,input.normal.data(),sizeof(actual.normal))!=0);
    const auto matrix=Rotation({{0,0,.6f,.8f}},{{2,-1,3}});input=Case(matrix,matrix,.25f);
    input.arm0={};input.arm1={};NativeSkin(input,nativeOps,input.expected);PrivateSkin(input,context,actual);CHECK(Bits(actual,input.expected));
    // Independent simple-transform limit: equal bones and zero arm offsets
    // reduce to a single rotated/translated point and rotated normal.
    for(unsigned axis=0;axis<3;++axis) {
        const float position=((input.position[0]*matrix[axis]+input.position[1]*matrix[4+axis])+input.position[2]*matrix[8+axis])+matrix[12+axis];
        const float normal=(input.normal[0]*matrix[axis]+input.normal[1]*matrix[4+axis])+input.normal[2]*matrix[8+axis];
        CHECK(std::abs(actual.position[axis]-position)<2e-6f);CHECK(std::abs(actual.normal[axis]-normal)<2e-6f);
    }
    const auto rigid=Rotation(Axis(1,-2,.5f,1.1f),{{.25f,-1,2}});
    const auto stretched=LinearThenRotation({{2,.2f,0,.2f,.75f,.1f,0,.1f,1.5f}},Rotation(Axis(0,1,1,-.7f)));
    for(bool reverse:{false,true}) {
        const auto mixed=reverse?Case(rigid,stretched,.375f):Case(stretched,rigid,.375f);
        CHECK((reverse?mixed.first.aux.flags:mixed.second.aux.flags)==0);
        CHECK((reverse?mixed.second.aux.flags:mixed.first.aux.flags)==affine::kBoneFlagAffine);
        const auto& rigidBone=reverse?mixed.first:mixed.second;
        CHECK(std::memcmp(rigidBone.native.data(),rigidBone.aux.rotation,sizeof(rigidBone.aux.rotation))==0);
        PrivateSkin(mixed,context,actual);CHECK(Bits(actual,mixed.expected));
        // A deliberately mismatched rigid auxiliary rotation is an incorrect
        // caller/oracle input. Prove the mixed fixture detects that mistake;
        // do not invent validation or repair semantics for the private API.
        auto mismatched=mixed;auto& wrongAux=reverse?mismatched.first.aux:mismatched.second.aux;
        wrongAux.rotation[0]=wrongAux.rotation[1]=wrongAux.rotation[2]=0;wrongAux.rotation[3]=1;
        NativeSkin(mismatched,nativeOps,wrong);
        CHECK(!Bits(wrong.position,mixed.expected.position)&&!Bits(wrong.normal,mixed.expected.normal));
    }
}
bool Matches(const BoneTransformContext& context,const Conversion& input) {
    Quaternion quaternion{};Matrix matrix{};const affine::ContextRotationOps ops{context};
    context.QuaternionFromMatrix(quaternion.data(),input.matrix.data());context.MatrixFromQuaternion(matrix.data(),input.quaternion.data());
    if(!Bits(quaternion,input.expectedQuaternion)||!Bits(matrix,input.expectedMatrix))return false;
    quaternion.fill(123);matrix.fill(456);ops.quatFromMatrix(quaternion.data(),input.matrix.data());ops.matrixFromQuaternion(matrix.data(),input.quaternion.data());
    return Bits(quaternion,input.expectedQuaternion)&&Bits(matrix,input.expectedMatrix);
}
bool Matches(const BoneTransformContext& context,const SkinCase& input) {
    affine::Skinned output{};PrivateSkin(input,context,output);return Bits(output.position,input.expected.position)&&Bits(output.normal,input.expected.normal);
}
template<class T>std::vector<unsigned char> Bytes(const std::vector<T>& source) {
    std::vector<unsigned char> bytes(source.size()*sizeof(T));std::memcpy(bytes.data(),source.data(),bytes.size());return bytes;
}
void SerialNoAllocation(const BoneTransformContext& context,const std::vector<Conversion>& conversions,const std::vector<SkinCase>& skins) {
    currentCase="conversion and SDEF bit parity without allocation";const auto beforeConversions=Bytes(conversions),beforeSkins=Bytes(skins);
    const auto attempts=allocationAttempts;bool good=true;
    {NoAllocation guard;for(const auto& item:conversions)good=Matches(context,item)&&good;for(const auto& item:skins)good=Matches(context,item)&&good;}
    CHECK(good&&attempts==allocationAttempts);CHECK(!std::memcmp(beforeConversions.data(),conversions.data(),beforeConversions.size()));
    CHECK(!std::memcmp(beforeSkins.data(),skins.data(),beforeSkins.size()));
}
void SharedLifetime(std::shared_ptr<const BoneTransformContext> owner,const std::vector<Conversion>& conversions,const std::vector<SkinCase>& skins) {
    currentCase="shared context lifetime and allocation-free workers";
    std::weak_ptr<const BoneTransformContext> weak=owner;std::atomic<bool> go{false};std::atomic<unsigned> errors{0};std::array<std::thread,4> threads;
    const auto beforeConversions=Bytes(conversions),beforeSkins=Bytes(skins);
    try {
        for(unsigned worker=0;worker<threads.size();++worker)threads[worker]=std::thread([&,worker,lease=owner]{
            while(!go.load(std::memory_order_acquire))std::this_thread::yield();
            try {
                const auto attempts=allocationAttempts;bool good=true;
                {NoAllocation guard;for(unsigned repeat=0;repeat<8;++repeat){
                    for(size_t i=worker;i<conversions.size();i+=4)good=Matches(*lease,conversions[i])&&good;
                    for(size_t i=worker;i<skins.size();i+=4)good=Matches(*lease,skins[i])&&good;
                }}
                if(!good||attempts!=allocationAttempts)++errors;
            }catch(...){++errors;}
        });
    }catch(...){go.store(true,std::memory_order_release);for(auto& thread:threads)if(thread.joinable())thread.join();throw;}
    owner.reset();CHECK(!weak.expired());go.store(true,std::memory_order_release);
    for(auto& thread:threads)thread.join();CHECK(errors.load()==0&&weak.expired());
    CHECK(!std::memcmp(beforeConversions.data(),conversions.data(),beforeConversions.size()));CHECK(!std::memcmp(beforeSkins.data(),skins.data(),beforeSkins.size()));
}
static_assert(noexcept(std::declval<const BoneTransformContext&>().QuaternionFromMatrix(static_cast<float*>(nullptr),static_cast<const float*>(nullptr))));
static_assert(noexcept(std::declval<const BoneTransformContext&>().MatrixFromQuaternion(static_cast<float*>(nullptr),static_cast<const float*>(nullptr))));
}
int main() {
    try {
        auto& global=d3dx::Get();CHECK(global.Load()&&global.quatFromMatrix&&global.matrixRotationQuaternion);
        nativeQuaternion=global.quatFromMatrix;nativeMatrix=global.matrixRotationQuaternion;
        std::array<unsigned char,sizeof(d3dx::Api)> globalBefore{};std::memcpy(globalBefore.data(),&global,sizeof(global));
        auto context=BoneTransformContext::Create();CHECK(bool(context));
        const auto conversions=Conversions();const auto skins=SkinCases();NondegenerateEvidence(*context);
        SerialNoAllocation(*context,conversions,skins);
        {auto independent=BoneTransformContext::Create();CHECK(bool(independent));CHECK(Matches(*independent,conversions[7])&&Matches(*independent,skins[6]));}
        CHECK(Matches(*context,conversions[7])&&Matches(*context,skins[6]));
        const auto beforeQuat=quaternionCalls,beforeMatrix=matrixCalls;
        SharedLifetime(std::move(context),conversions,skins);CHECK(!context);
        CHECK(beforeQuat==quaternionCalls&&beforeMatrix==matrixCalls); // No worker touched reference adapters.
        CHECK(std::memcmp(globalBefore.data(),&global,sizeof(global))==0); // No table poisoning, replacement or Load calls after setup.
        auto fresh=BoneTransformContext::Create();CHECK(bool(fresh));CHECK(Matches(*fresh,conversions[7])&&Matches(*fresh,skins[6]));fresh.reset();
        CHECK(std::memcmp(globalBefore.data(),&global,sizeof(global))==0);
        std::printf("PASS %u checks; %zu real D3DX conversion cases and %zu SDEF cases, shared context workers\n",checks,conversions.size(),skins.size());return 0;
    }catch(const std::exception& error){rejectAllocations=false;std::fprintf(stderr,"FAIL %s exception: %s\n",currentCase,error.what());return 1;}
    catch(...){rejectAllocations=false;std::fprintf(stderr,"FAIL %s unexpected exception\n",currentCase);return 1;}
}
