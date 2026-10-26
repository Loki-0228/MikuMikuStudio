// Explicit-baseline scale propagation, independent of live pose/cache state.
#define NOMINMAX
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/model.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace { thread_local int failAfter = -1; }
void* operator new(std::size_t bytes) {
    if (failAfter == 0) throw std::bad_alloc();
    if (failAfter > 0) --failAfter;
    if (auto* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio;
namespace bs = bone_scale;
namespace {
unsigned checks = 0;
const char* currentCase = "setup";
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %s line %d: %s\n",currentCase,__LINE__,#x); std::exit(1); } } while (false)
template<class T> bool Bits(const T& a,const T& b) { return !std::memcmp(&a,&b,sizeof(T)); }
template<class T> bool Bits(const std::vector<T>& a,const std::vector<T>& b) {
    return a.size() == b.size() && (a.empty() || !std::memcmp(a.data(),b.data(),a.size()*sizeof(T)));
}
bs::Matrix Diagonal(float x,float y,float z) { return {{x,0,0,0,y,0,0,0,z}}; }
bs::Transform Identity() { bs::Transform value{}; value[0]=value[5]=value[10]=value[15]=1; return value; }
bool Near(double a,double b,double tolerance=1e-5) { return std::abs(a-b)<tolerance; }

// Historical oracle copied from c792dc1:src/model/bone_scale.cpp. These pure
// helpers and the traversal below intentionally do not call production math
// or its shared propagation kernel. Baseline selection is explicit here.
namespace old {
bs::Matrix Lookup(const bs::Values& values,int index) {
    const auto it=values.find(index); return it==values.end()?bs::Matrix{}:it->second;
}
double Determinant(const bs::Matrix& a) {
    const auto& m=a.v;
    return double(m[0])*(double(m[4])*m[8]-double(m[5])*m[7])
        -double(m[1])*(double(m[3])*m[8]-double(m[5])*m[6])
        +double(m[2])*(double(m[3])*m[7]-double(m[4])*m[6]);
}
bool Inverse(const bs::Matrix& a,bs::Matrix& out) {
    const auto& m=a.v; const double d=Determinant(a);
    if(!std::isfinite(d)||std::abs(d)<1.e-18)return false;
    const double cof[9]={double(m[4])*m[8]-double(m[5])*m[7],double(m[2])*m[7]-double(m[1])*m[8],double(m[1])*m[5]-double(m[2])*m[4],
        double(m[5])*m[6]-double(m[3])*m[8],double(m[0])*m[8]-double(m[2])*m[6],double(m[2])*m[3]-double(m[0])*m[5],
        double(m[3])*m[7]-double(m[4])*m[6],double(m[1])*m[6]-double(m[0])*m[7],double(m[0])*m[4]-double(m[1])*m[3]};
    for(int i=0;i<9;++i)out.v[i]=float(cof[i]/d); return true;
}
bs::Matrix Linear(const float* m) {
    bs::Matrix a;for(int i=0;i<3;++i)for(int j=0;j<3;++j)a.v[i*3+j]=m[i*4+j];return a;
}
bs::Matrix Multiply(const bs::Matrix& a,const bs::Matrix& b) {
    bs::Matrix c;for(int i=0;i<3;++i)for(int j=0;j<3;++j){double sum=0;for(int k=0;k<3;++k)sum+=double(a.v[i*3+k])*b.v[k*3+j];c.v[i*3+j]=float(sum);}return c;
}
bs::Transform Product(const bs::Transform& a,const bs::Transform& b) {
    bs::Transform c{};for(int i=0;i<4;++i)for(int j=0;j<4;++j){double x=0;for(int k=0;k<4;++k)x+=double(a[i*4+k])*b[k*4+j];c[i*4+j]=float(x);}return c;
}
bs::Transform Affine(const bs::Matrix& a) {
    bs::Transform t{};t[15]=1;for(int i=0;i<3;++i)for(int j=0;j<3;++j)t[i*4+j]=a.v[i*3+j];return t;
}
bs::Transform InverseTransform(const bs::Transform& t) {
    bs::Matrix inv;if(!old::Inverse(Linear(t.data()),inv))return Affine({});auto result=Affine(inv);
    for(int j=0;j<3;++j)for(int k=0;k<3;++k)result[12+j]-=t[12+k]*inv.v[k*3+j];return result;
}
bs::Transform Pivot(const mdl::BoneRecord& bone,const bs::Matrix& scale) {
    bs::Transform pivot=Affine(scale);
    for(int c=0;c<3;++c){pivot[12+c]=bone.position[c];for(int k=0;k<3;++k)pivot[12+c]-=bone.position[k]*scale.v[k*3+c];}return pivot;
}
bs::Transform WithLinearAtPivot(const mdl::BoneRecord& bone,const bs::Transform& reference,const bs::Matrix& linear) {
    bs::Transform result=reference;
    for(int c=0;c<3;++c)for(int k=0;k<3;++k){result[12+c]+=bone.position[k]*(reference[k*4+c]-linear.v[k*3+c]);result[k*4+c]=linear.v[k*3+c];}return result;
}
bs::Transform InheritTransform(const mdl::BoneRecord& bone,const bs::Transform& rigid,const bs::Matrix& parentScale,const bs::Transform& parentDelta) {
    return WithLinearAtPivot(bone,Product(rigid,parentDelta),old::Multiply(parentScale,Linear(rigid.data())));
}
void Build(const mdl::ModelRecord& model,const bs::Values& pose,const std::vector<bs::Transform>& rigid,std::vector<bs::Transform>& corrected) {
    const int count=int(model.boneCount);const auto* bones=model.boneTable;
    corrected.clear();if(!bones||count<=0)return;
    corrected.resize(count);
    std::vector<bs::Matrix> cumulativeScale(count);
    std::vector<unsigned char> done(count);std::vector<bool> changed(count);std::vector<int> chain;
    for(int i=0;i<count;++i){
        if(done[i]==2)continue;
        int j=i;while(j>=0&&j<count&&done[j]==0){done[j]=1;chain.push_back(j);j=bones[j].parent;}
        while(!chain.empty()){
            const int index=chain.back();chain.pop_back();const auto& bone=bones[index];
            bs::Transform base=rigid[index];
            const bool inherited=bone.parent>=0&&bone.parent<count&&done[bone.parent]==2&&changed[bone.parent];
            if(inherited){const bs::Transform delta=Product(InverseTransform(rigid[bone.parent]),corrected[bone.parent]);base=InheritTransform(bone,base,cumulativeScale[bone.parent],delta);}
            const bs::Matrix scale=Lookup(pose,index);
            if(scale.v!=bs::Matrix{}.v)base=Product(Pivot(bone,scale),base);
            cumulativeScale[index]=inherited?old::Multiply(scale,cumulativeScale[bone.parent]):scale;
            corrected[index]=base;done[index]=2;changed[index]=inherited||scale.v!=bs::Matrix{}.v;
        }
    }
}
}

struct Fixture {
    std::unique_ptr<mdl::ModelRecord> model=std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones;
    std::vector<bs::Transform> rigid;
    bs::Values pose;
    explicit Fixture(unsigned count=6):bones(count),rigid(count,Identity()) {
        model->boneCount=count;model->boneTable=bones.empty()?nullptr:bones.data();
        model->searchCursor=888;model->displayKeyCursor=777;
        for(unsigned i=0;i<count;++i){
            auto& bone=bones[i];bone.parent=-1;bone.slotIndex=-1;bone.position[0]=float(i+1);bone.position[1]=float(i%3)-1;
            std::snprintf(bone.name,20,"scale-%u",i);bone.trans[0]=111;bone.physicsOffset[1]=222;bone.rotQuat[3]=1;
            rigid[i][12]=float(i)*.25f;rigid[i][13]=-.5f;rigid[i][14]=.75f;
            for(unsigned c=0;c<16;++c){bone.matInit[c]=float(500+c+i);bone.matLocal[c]=float(700+c);bone.matWorld[c]=float(900+c);bone.matExtra[c]=float(1100+c);}
        }
    }
    unsigned char* raw() const {return reinterpret_cast<unsigned char*>(model.get());}
    ~Fixture(){bs::Forget(raw());}
    void SetNativeBaseline(){for(unsigned i=0;i<bones.size();++i)std::memcpy(bones[i].matInit,rigid[i].data(),64);}
};
void Complex(Fixture& f) {
    const int parents[]{3,0,-1,2,1,-1};
    for(unsigned i=0;i<6;++i)f.bones[i].parent=parents[i];
    f.rigid[0][0]=.6f;f.rigid[0][1]=.8f;f.rigid[0][4]=-.8f;f.rigid[0][5]=.6f;
    f.rigid[1][5]=f.rigid[1][10]=0;f.rigid[1][6]=1;f.rigid[1][9]=-1;
    f.pose[2]={{2,.25f,0,0,3,.5f,.125f,0,.75f}};
    f.pose[0]={{1,0,.2f,.125f,2,0,0,.25f,1}};
    f.pose[4]=Diagonal(-1,2,.5f); // Explicit full matrices preserve reflections too.
}
struct SourceState {
    std::unique_ptr<mdl::ModelRecord> model;
    std::vector<mdl::BoneRecord> bones;
    std::vector<bs::Transform> rigid;
    bs::Values pose;
    bs::Snapshot scale;
    std::uint64_t revision;
    bool applied;
    explicit SourceState(const Fixture& f):model(std::make_unique<mdl::ModelRecord>(*f.model)),bones(f.bones),rigid(f.rigid),pose(f.pose),
        scale(bs::Capture(f.raw())),revision(bs::Revision(f.raw())),applied(bs::TransformsApplied(f.raw())){}
    void Check(const Fixture& f) const {
        CHECK(Bits(*model,*f.model)&&Bits(bones,f.bones)&&Bits(rigid,f.rigid)&&pose==f.pose);
        const auto now=bs::Capture(f.raw());CHECK(now.pose==scale.pose&&now.keys==scale.keys&&now.curves==scale.curves);
        CHECK(revision==bs::Revision(f.raw())&&applied==bs::TransformsApplied(f.raw()));
    }
};
std::vector<bs::Transform> Parity(const Fixture& f) {
    const SourceState before(f);std::vector<bs::Transform> expected,legacy,actual;
    old::Build(*f.model,f.pose,f.rigid,expected);
    bs::BuildTransforms(f.raw(),f.pose,legacy,&f.rigid);
    bs::BuildTransformsPrivate(*f.model,f.pose,f.rigid,actual);
    CHECK(Bits(actual,expected)&&Bits(actual,legacy));before.Check(f);return actual;
}
std::array<double,3> Point(const mdl::BoneRecord& bone,const bs::Transform& t) {
    std::array<double,3> p{};for(unsigned c=0;c<3;++c){p[c]=t[12+c];for(unsigned k=0;k<3;++k)p[c]+=double(bone.position[k])*t[k*4+c];}return p;
}

void MatrixAndParentParity() {
    currentCase="full matrices and native parent traversal";
    Fixture f;Complex(f);Parity(f);
    f.pose.clear();CHECK(Bits(Parity(f),f.rigid));
    f.pose[2]={};CHECK(Bits(Parity(f),f.rigid));
    Complex(f);f.rigid[2][0]=f.rigid[2][5]=f.rigid[2][10]=0;Parity(f); // Singular rigid inverse falls back to identity.
    f.rigid[2]=Identity();f.bones[2].parent=4;Parity(f); // Feedback cycle uses native visited-state behavior.
    f.bones[2].parent=-1;f.bones[3].parent=3;Parity(f); // Self parent also terminates.
    f.bones[3].parent=INT32_MAX;f.bones[1].parent=-7;Parity(f); // Historical invalid parents behave as roots.
    Fixture empty(0);CHECK(Parity(empty).empty());
}
void ExplicitGeometry() {
    currentCase="hand calculated pivots shear reflection and rotated lengths";
    Fixture f(3);f.bones[0].position[0]=1;f.bones[0].position[1]=0;
    f.bones[1].parent=0;f.bones[1].position[0]=3;f.bones[1].position[1]=0;
    f.bones[2].parent=1;f.bones[2].position[0]=4;f.bones[2].position[1]=0;
    for(auto& t:f.rigid){t=Identity();t[12]=5;}
    f.pose[0]=Diagonal(2,2,2);auto out=Parity(f);
    CHECK(Near(Point(f.bones[0],out[0])[0],6)&&Near(Point(f.bones[1],out[1])[0],10)&&Near(Point(f.bones[2],out[2])[0],12));
    f.pose[1]=Diagonal(3,1,1);out=Parity(f);
    CHECK(Near(Point(f.bones[1],out[1])[0],10)&&Near(Point(f.bones[2],out[2])[0],16));
    // Rotate the child 90 degrees around its rigid pivot at (8,0,0).
    f.pose.clear();f.pose[0]=Diagonal(4,2,.5f);f.rigid[1]=Identity();
    f.rigid[1][0]=f.rigid[1][5]=0;f.rigid[1][1]=1;f.rigid[1][4]=-1;
    f.rigid[1][12]=8;f.rigid[1][13]=-3;
    out=Parity(f);const auto pivot=Point(f.bones[1],out[1]);
    CHECK(Near(pivot[0],14)&&Near(pivot[1],0)&&Near(pivot[2],0));
    const double expectedSquared[]{16,4,.25};
    for(unsigned row=0;row<3;++row){double length=0;for(unsigned col=0;col<3;++col)length+=double(out[1][row*4+col])*out[1][row*4+col];CHECK(Near(length,expectedSquared[row]));}
    // Root-only shear about a nonzero pivot: (1,2,3) must stay fixed.
    Fixture shear(1);shear.rigid[0]=Identity();shear.bones[0].position[0]=1;shear.bones[0].position[1]=2;shear.bones[0].position[2]=3;
    shear.pose[0]={{2,.5f,0,0,3,0,0,0,4}};out=Parity(shear);
    const auto fixed=Point(shear.bones[0],out[0]);CHECK(Near(fixed[0],1)&&Near(fixed[1],2)&&Near(fixed[2],3));
    CHECK(out[0][1]==.5f&&out[0][12]==-1&&out[0][13]==-4.5f&&out[0][14]==-9);
    shear.pose[0]=Diagonal(-2,3,4);out=Parity(shear);
    CHECK(out[0][0]==-2&&out[0][12]==3&&Near(Point(shear.bones[0],out[0])[0],1));
}
void CachedSourceIsolationAndLifetime() {
    currentCase="explicit baseline wins over applied cache and matInit";
    auto source=std::make_unique<Fixture>();Complex(*source);source->SetNativeBaseline();
    CHECK(bs::SetPose(source->raw(),2,Diagonal(1.5f,2,3)));bs::ApplyTransforms(source->raw());
    CHECK(bs::TransformsApplied(source->raw()));
    // Make all three potential baselines different: cached rigid, live posed
    // matInit and this explicit rigid input. Private must use the third one.
    for(auto& t:source->rigid){t[12]+=37;t[14]-=9;}
    std::vector<bs::Transform> cached;bs::BuildTransforms(source->raw(),source->pose,cached);
    auto expected=Parity(*source);CHECK(!Bits(cached,expected));
    for(auto& bone:source->bones)for(float& value:bone.matInit)value=std::numeric_limits<float>::quiet_NaN();
    const SourceState before(*source);std::vector<bs::Transform> actual;
    bs::BuildTransformsPrivate(*source->model,source->pose,source->rigid,actual);CHECK(Bits(actual,expected));before.Check(*source);
    Fixture owned(unsigned(source->bones.size()));owned.bones=source->bones;owned.model->boneTable=owned.bones.data();owned.rigid=source->rigid;owned.pose=source->pose;
    source->bones[0].parent=-1;source->bones[0].position[0]=900;
    CHECK(bs::SetPose(source->raw(),0,Diagonal(7,8,9)));source.reset();
    bs::BuildTransformsPrivate(*owned.model,owned.pose,owned.rigid,actual);CHECK(Bits(actual,expected));
}

void ExtentsAndAlias() {
    currentCase="extent validation and input-output alias";
    Fixture f;Complex(f);const auto expected=Parity(f);
    auto aliased=f.rigid;bs::BuildTransformsPrivate(*f.model,f.pose,aliased,aliased);CHECK(Bits(aliased,expected));
    for(unsigned kind=0;kind<4;++kind){
        auto model=std::make_unique<mdl::ModelRecord>(*f.model);auto rigid=f.rigid;
        if(kind==0)rigid.pop_back();if(kind==1)rigid.push_back(Identity());
        if(kind==2)model->boneTable=nullptr;if(kind==3)model->boneCount=uint32_t(INT32_MAX)+1u;
        for(bool alias:{false,true}){
            std::vector<bs::Transform> output(2,Identity());auto& target=alias?rigid:output;
            const auto before=target;const auto* pointer=target.data();const auto capacity=target.capacity();
            bool rejected=false;try{bs::BuildTransformsPrivate(*model,f.pose,rigid,target);}catch(const std::invalid_argument&){rejected=true;}
            CHECK(rejected&&Bits(target,before)&&target.data()==pointer&&target.capacity()==capacity);
        }
    }
    Fixture empty(0);std::vector<bs::Transform> output(2,Identity());const std::vector<bs::Transform> none;
    bs::BuildTransformsPrivate(*empty.model,empty.pose,none,output);CHECK(output.empty());
    auto aliasEmpty=none;bs::BuildTransformsPrivate(*empty.model,empty.pose,aliasEmpty,aliasEmpty);CHECK(aliasEmpty.empty());
}
void AllocationAtomicity() {
    currentCase="allocation failures retain output and aliased baseline";
    Fixture f;Complex(f);const SourceState sourceBefore(f);const auto expected=Parity(f);
    for(bool alias:{false,true}){
        bool completed=false;unsigned failures=0;
        for(int allowance=0;allowance<128;++allowance){
            auto rigid=f.rigid;std::vector<bs::Transform> output(3,Identity());auto& target=alias?rigid:output;
            const auto before=target;const auto* pointer=target.data();const auto capacity=target.capacity();
            bool failed=false,unexpected=false;failAfter=allowance;
            try{bs::BuildTransformsPrivate(*f.model,f.pose,rigid,target);}catch(const std::bad_alloc&){failed=true;}catch(...){unexpected=true;}
            failAfter=-1;CHECK(!unexpected);sourceBefore.Check(f);
            if(!failed){CHECK(Bits(target,expected));completed=true;break;}
            ++failures;CHECK(Bits(target,before)&&target.data()==pointer&&target.capacity()==capacity);
            if(!alias)CHECK(Bits(rigid,f.rigid));
        }
        CHECK(completed&&failures>0);std::printf("scale private alias=%d allocation failures=%u\n",int(alias),failures);
    }
}
void LongChain() {
    currentCase="deep forward parent chain uses iterative traversal";
    Fixture f(50000);
    for(unsigned i=0;i<f.bones.size();++i){f.bones[i].parent=i+1<f.bones.size()?int(i+1):-1;for(float& p:f.bones[i].position)p=0;f.rigid[i]=Identity();}
    f.pose[int(f.bones.size()-1)]=Diagonal(2,3,4);const auto result=Parity(f);
    CHECK(result.front()[0]==2&&result.front()[5]==3&&result.front()[10]==4&&Bits(result.front(),result.back()));
}
void ConcurrentReadOnlyInputs() {
    currentCase="shared immutable inputs with four independent outputs";
    Fixture f;Complex(f);const SourceState before(f);const auto expected=Parity(f);
    std::atomic<unsigned> failures{0};std::array<std::thread,4> threads;
    try{
        for(auto& worker:threads)worker=std::thread([&]{
            try{for(unsigned repeat=0;repeat<100;++repeat){std::vector<bs::Transform> out;bs::BuildTransformsPrivate(*f.model,f.pose,f.rigid,out);if(!Bits(out,expected))++failures;}}
            catch(...){++failures;}
        });
    }catch(...){for(auto& worker:threads)if(worker.joinable())worker.join();throw;}
    for(auto& worker:threads)worker.join();CHECK(failures==0);before.Check(f);
}
}
int main(){
    try{MatrixAndParentParity();ExplicitGeometry();CachedSourceIsolationAndLifetime();ExtentsAndAlias();AllocationAtomicity();LongChain();ConcurrentReadOnlyInputs();
        std::printf("PASS %u private bone scale checks\n",checks);return 0;
    }catch(const std::exception& error){failAfter=-1;std::fprintf(stderr,"FAIL %s unexpected exception: %s\n",currentCase,error.what());return 1;}
}
