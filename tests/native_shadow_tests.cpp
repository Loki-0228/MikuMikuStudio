// MAINTENANCE: regression-keep; hidden D3D9 native 9.32 parity and coverage checks.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/native_shadow_projection.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/shadow_depth_filter.hpp"

// Match the application on hybrid systems so technique validation exercises
// the same high-performance adapter when the driver honors these hints.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
}

using namespace mikumikustudio::d3dx;
using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned kSize=128;
void CheckFixedModelProjection() {
    using namespace mikumikustudio;
    const float direction[]{-.5f,-1.f,.5f},focus[]{0,0,0};
    NativeShadowBounds model;const float low[]{-3,0,-1},high[]{3,16,1};model.Include(low);model.Include(high);
    for(unsigned base:{2048u,4096u})for(int mode:{1,2})for(float range:{.00001f,.001f,.01125f,.05f,.1f}) {
        const float wanted=NativeShadowSamplingDensity(direction,range,mode,base);
        std::array<float,16> first{};
        for(float stage:{100.f,1000.f,10000.f}) {
            NativeShadowBounds scene;const float a[]{-stage,-stage,-stage},b[]{stage,stage,stage};scene.Include(a);scene.Include(b);
            const auto coverage=NativeShadowProjection(focus,direction,.1f,mode,&scene);
            auto projection=FixedModelShadowProjection(coverage,model);
            const float width=2.f/MinimumShadowScale(projection,model);
            const unsigned size=ShadowTextureSizeForDensity(width,wanted,8192);
            projection=FitShadowSamplingDensity(projection,wanted,size);
            const float density=MinimumShadowScale(projection,model)*size*.5f;
            if(std::fabs(density/wanted-1)>1e-4f)throw std::runtime_error("fixed model loses original reference density");
            for(unsigned row=0;row<4;++row) {
                if(projection[row*4+2]!=coverage[row*4+2])throw std::runtime_error("fixed model drops offscreen caster depth");
                for(unsigned axis=0;axis<2;++axis)if(stage!=100.f && std::fabs(projection[row*4+axis]-first[row*4+axis])>1e-5f)
                    throw std::runtime_error("fixed model precision changes with stage size");
            }
            for(unsigned corner=0;corner<8;++corner) {
                const auto p=model.Corner(corner);
                for(unsigned axis=0;axis<2;++axis) {
                    const float q=projection[12+axis]+p[0]*projection[axis]+p[1]*projection[4+axis]+p[2]*projection[8+axis];
                    if(std::fabs(q)>.86f)throw std::runtime_error("fixed model enters the detail fade rim");
                }
            }
            first=projection;
        }
    }
    std::puts("PASS fixed model: native density, full receiver core, stage-size independence, offscreen caster Z");
}
void CheckVertexBounds() {
    using namespace mikumikustudio;
    unsigned checks = 0;
    const auto compare = [&](const void* data, std::size_t stride, std::size_t count) {
        NativeShadowBounds reference;
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i=0; i<count; ++i) {
            float point[3]; std::memcpy(point,bytes+i*stride,sizeof(point));
            reference.Include(point);
        }
        const auto actual=NativeShadowVertexBounds(data,stride,count);
        if (std::memcmp(reference.minimum.data(),actual.minimum.data(),12) ||
            std::memcmp(reference.maximum.data(),actual.maximum.data(),12))
            throw std::runtime_error("vertex bounds differ from full scalar scan");
        ++checks;
    };
    for (std::size_t stride : {12u,16u,20u,32u,48u,96u}) for (unsigned offset : {0u,1u,3u}) {
        std::vector<unsigned char> data(stride*1025+offset);
        unsigned random=12345;
        for (unsigned i=0; i<1025; ++i) {
            float point[3];
            for (float& value : point) {random=random*1664525u+1013904223u;value=float(int(random%20001)-10000)/17;}
            if(i%13==0)point[i%3]=std::numeric_limits<float>::quiet_NaN();
            if(i%17==0)point[i%3]=INFINITY;
            if(i%19==0)point[i%3]=-INFINITY;
            std::memcpy(data.data()+offset+i*stride,point,12);
        }
        for(std::size_t count : {0u,1u,2u,7u,1024u,1025u})compare(data.data()+offset,stride,count);
        const float negative[4]={-0.f,-0.f,-0.f,INFINITY},positive[4]={0.f,0.f,0.f,NAN};
        std::memcpy(data.data()+offset,negative,(std::min)(stride,std::size_t(16)));
        std::memcpy(data.data()+offset+stride,positive,(std::min)(stride,std::size_t(16)));
        compare(data.data()+offset,stride,2);
        std::memcpy(data.data()+offset,positive,(std::min)(stride,std::size_t(16)));
        std::memcpy(data.data()+offset+stride,negative,(std::min)(stride,std::size_t(16)));
        compare(data.data()+offset,stride,2);
    }
    if(NativeShadowVertexBounds(nullptr,32,1).Valid() ||
       NativeShadowVertexBounds("",4,1).Valid() ||
       NativeShadowVertexBounds("",32,(std::numeric_limits<std::size_t>::max)()).Valid())
        throw std::runtime_error("invalid vertex span produced bounds");
    std::printf("PASS full vertex bounds: %u scalar parity checks\n",checks);
}
void CheckReferenceFit() {
    // Compatibility projection helper; this fit does not select the current
    // production density or its texture allocation.
    using namespace mikumikustudio;
    NativeShadowBounds bounds;
    const float low[3]={-6,0,-3}, high[3]={6,20,8};
    bounds.Include(low);bounds.Include(high);
    std::array<float,16> inside{};
    inside[0]=inside[5]=inside[10]=.01f;inside[14]=.25f;inside[15]=1;
    if (FitReferenceShadowProjection(inside,bounds)!=inside)
        throw std::runtime_error("unclipped native sampling grid changed");
    unsigned checked=0;
    for (float scale : {.01f,.1f,1.f,10.f}) for (float warp : {-1.f,0.f,.5f,1.f}) {
        auto matrix=inside;matrix[0]=matrix[5]=scale;matrix[10]=scale*.15f;
        matrix[7]=warp;matrix[12]=2;matrix[13]=-1;matrix[14]=-.2f;
        const auto fitted=FitReferenceShadowProjection(matrix,bounds);
        for (unsigned corner=0;corner<8;++corner) {
            const auto p=bounds.Corner(corner);double q[4]{};
            for (int a=0;a<4;++a)
                q[a]=fitted[12+a]+p[0]*double(fitted[a])+p[1]*double(fitted[4+a])+p[2]*double(fitted[8+a]);
            if (!(q[3]>0) || std::abs(q[0]/q[3])>=1 || std::abs(q[1]/q[3])>=1 ||
                !(q[2]/q[3]>0 && q[2]/q[3]<1))
                throw std::runtime_error("native projective coverage or positive W failed");
            ++checked;
        }
        if (!std::isfinite(MinimumShadowScale(fitted,bounds)))
            throw std::runtime_error("native projective sampling density is not finite");
    }
    std::printf("PASS native grid preservation and %u projective coverage corners\n",checked);
}
void CheckVisibleDepthCoverage() {
    using namespace mikumikustudio;
    NativeShadowBounds scene;const float low[3]{-300,-80,1},high[3]{300,80,300};
    scene.Include(low);scene.Include(high);
    std::array<float,16> identity{};for(int i=0;i<4;++i)identity[i*5]=1;
    const float focus[3]{0,0,10},light[3]{-.5f,-1,.5f};
    for(bool orthographic:{false,true}) {
        const float scale=orthographic?.01f:2.f;
        const auto receivers=VisibleShadowReceivers(scene,identity.data(),identity.data(),scale,scale,orthographic);
        if(!receivers.Valid() || receivers.maximum[2]<300)throw std::runtime_error("visible receiver depth was cut at the focus region");
        for(int mode:{1,2})for(float range:{.00001f,.01125f,.05f,.1f}) {
            const auto reference=NativeShadowProjection(focus,light,range,mode);
            const auto map=FitFocusedShadowProjection(reference,scene,focus,light,range,mode,3,&receivers);
            for(unsigned corner=0;corner<8;++corner) {
                const auto p=receivers.Corner(corner);float q[4]{};
                for(int axis=0;axis<4;++axis)q[axis]=map[12+axis]+p[0]*map[axis]+p[1]*map[4+axis]+p[2]*map[8+axis];
                if(q[3]<=0 || std::abs(q[0]/q[3])>=1 || std::abs(q[1]/q[3])>=1 || q[2]/q[3]<=0 || q[2]/q[3]>=1)
                    throw std::runtime_error("raising precision clipped a visible receiver");
            }
        }
    }
    // A down-looking camera sees only a narrow piece of a huge thin floor.
    // The former frustum-AABB intersection included most of the distant floor.
    NativeShadowBounds floor;const float floorLow[3]{-1000,-.01f,-1000},floorHigh[3]{1000,.01f,1000};
    floor.Include(floorLow);floor.Include(floorHigh);
    std::array<float,16> view{};view[0]=1;view[6]=-1;view[9]=1;view[14]=10;view[15]=1;
    const auto visible=VisibleShadowReceivers(floor,view.data(),identity.data(),2,2,false);
    if(!visible.Valid() || visible.maximum[0]-visible.minimum[0]>10.1f ||
        visible.maximum[2]-visible.minimum[2]>10.1f)
        throw std::runtime_error("off-screen stage extents dilute close-up shadow density");
    for(unsigned base:{1024u,2048u})for(float ratio:{1.f,2.f,4.f}) {
        const auto size=FittedShadowResolutionLimit(base,.2f,.2f/ratio,16384);
        if(float(size)*(.2f/ratio)<float(base)*.2f)
            throw std::runtime_error("expanded coverage failed to retain the original texels per world unit");
    }
    std::puts("PASS tight floor receiver intersection and unchanged texel density through 4x coverage");
    std::puts("PASS visible receiver depth retained across both modes, precision extremes and perspective/orthographic views");
}
void CheckCameraStableShadowDepth() {
    using namespace mikumikustudio;
    NativeShadowBounds scene;const float low[3]{-1000,-20,-1000},high[3]{1000,60,1000};
    scene.Include(low);scene.Include(high);
    const float focus[3]{0,14,0},light[3]{-.5f,-1,.5f};
    for(int mode:{1,2}) {
        const auto depth=NativeShadowProjection(focus,light,.1f,mode,&scene);
        for(int angle=-90;angle<=90;++angle) {
            const float a=angle*.01745329252f;
            NativeShadowBounds receivers;
            const float lo[3]{-10+8*std::sin(a),0,-10+8*std::cos(a)},hi[3]{lo[0]+20,25,lo[2]+20};
            receivers.Include(lo);receivers.Include(hi);
            auto reference=NativeShadowProjection(focus,light,.1f,mode);
            reference[3]=.01f*std::sin(a); // view-dependent native projective warp
            const auto map=FitFocusedShadowProjection(reference,scene,focus,light,.1f,mode,12,&receivers);
            for(int row=0;row<4;++row)if(map[row*4+2]!=depth[row*4+2])
                throw std::runtime_error("camera rotation changed the shadow depth/bias mapping");
            if(map[3]!=0 || map[7]!=0 || map[11]!=0 || map[15]!=1)
                throw std::runtime_error("camera rotation switched shadow projection types");
        }
    }
    std::puts("PASS 362 camera rotations preserve shadow depth and projection type in both modes");
}
void CheckFocusedStageCoverage() {
    // Retained focused-fit helper regression, not the current camera-slab
    // production sampling policy.
    using namespace mikumikustudio;
    NativeShadowBounds compactBounds, stage;
    const float low[3]{-6,0,-3}, high[3]{6,20,8};
    const float stageLow[3]{-1000,-1000,-1000}, stageHigh[3]{1000,1000,1000};
    compactBounds.Include(low); compactBounds.Include(high); stage.Include(stageLow); stage.Include(stageHigh);
    const float focus[3]{0,10,0};
    unsigned checked = 0;
    for (const auto& light : std::vector<std::array<float,3>>{
            {-.5f,-1,.5f}, {0,-1,0}, {0,0,1}, {1,0,0}, {0,0,0}})
        for (int mode : {1,2}) for (float range : {.1f,.01125f}) {
            const auto reference = NativeShadowProjection(focus,light.data(),range,mode);
            const auto compact = FitFocusedShadowProjection(reference,compactBounds,focus,light.data(),range,mode,30);
            if (compact != FitReferenceShadowProjection(reference,compactBounds))
                throw std::runtime_error("compactBounds scene lost its native fitted grid");
            const auto complete = FitReferenceShadowProjection(reference,stage);
            const auto nearMap = FitFocusedShadowProjection(reference,stage,focus,light.data(),range,mode,30);
            const auto farMap = FitFocusedShadowProjection(reference,stage,focus,light.data(),range,mode,600);
            if (MinimumShadowScale(nearMap,compactBounds) < MinimumShadowScale(complete,compactBounds)*15 ||
                MinimumShadowScale(nearMap,compactBounds) <= MinimumShadowScale(farMap,compactBounds))
                throw std::runtime_error("large stage diluted nearby sampling or zoom-out coverage did not grow");
            const auto project = [&](const std::array<float,3>& p) {
                std::array<float,3> q{};
                for (int a=0;a<3;++a) q[a]=nearMap[12+a]+p[0]*nearMap[a]+p[1]*nearMap[4+a]+p[2]*nearMap[8+a];
                return q;
            };
            for (unsigned corner=0;corner<8;++corner) {
                const auto p=project(compactBounds.Corner(corner));
                const auto depth=project(stage.Corner(corner))[2];
                if (std::abs(p[0])>=1 || std::abs(p[1])>=1 || p[2]<=0 || p[2]>=1 || depth<=0 || depth>=1)
                    throw std::runtime_error("focused receiver or remote caster depth clipped");
            }
            if (light[0] || light[1] || light[2]) {
                const auto receiver=project({0,10,0});
                const auto caster=project({-300*light[0],10-300*light[1],-300*light[2]});
                if (std::abs(receiver[0]-caster[0])>1e-5 || std::abs(receiver[1]-caster[1])>1e-5 ||
                    !(caster[2]<receiver[2] && caster[2]>0))
                    throw std::runtime_error("remote caster and receiver disagree along light ray");
            }
            ++checked;
        }
    std::printf("PASS %u focused large-stage cases: native compact grid, density, zoom, receiver coverage, remote casters\n",checked);
}
void CheckQualityOrdering() {
    using namespace mikumikustudio;
    // Historical tier helper, retained for compatibility coverage. Production
    // sampling is checked separately against the original centre Jacobian.
    for(unsigned maximum:{2048u,4096u,8192u}) {
        unsigned previous=maximum;
        for(int position=0;position<=9999;++position) {
            const auto size=ShadowQualityMapSize((10000.f-position)/100000.f,maximum);
            if(size>previous)throw std::runtime_error("shadow precision ordering inverted");
            previous=size;
        }
        if(ShadowQualityMapSize(.1f,maximum)<=ShadowQualityMapSize(.0344f,maximum))
            throw std::runtime_error("0 must be finer than 6560");
    }
    std::puts("PASS all 10000 precision positions at three hardware budgets");
}
void CheckDetailGrid() {
    using namespace mikumikustudio;
    const float focus[]{0,10,0}, direction[]{-.5f,-1,.5f};
    NativeShadowBounds scene;
    const float low[]{-300,-30,-300}, high[]{300,100,300};
    scene.Include(low);scene.Include(high);
    for (int mode : {1,2}) {
        const auto coverage=NativeShadowProjection(focus,direction,.1f,mode,&scene);
        const auto detail=DetailShadowProjection(focus,direction,.1f,mode,4.f,8192,coverage);
        if (MinimumShadowScale(detail,scene) < NativeShadowLateralScale(.1f,mode)*.49f)
            throw std::runtime_error("nearby shadow density diluted by stage");
        for(int row=0;row<4;++row) if(detail[row*4+2]!=coverage[row*4+2])
            throw std::runtime_error("detail and coverage must share depth");
        for(int column=0;column<2;++column) {
            const float texel=detail[12+column]*4096.f;
            if(std::abs(texel-std::round(texel))>1.e-4f)
                throw std::runtime_error("detail grid translation not snapped");
        }
        for(unsigned size:{512u,1024u,2048u,4096u,8192u}) {
            const auto resized=DetailShadowProjection(focus,direction,.1f,mode,4.f,size,coverage);
            for(int row=0;row<3;++row)for(int column=0;column<4;++column)
                if(resized[row*4+column]!=detail[row*4+column])
                    throw std::runtime_error("dynamic resolution changed detail coverage or depth");
        }
        // Orbiting changes the coverage XY fit, but must not change the
        // detailed light grid or drop distant caster depths.
        for(int angle=0;angle<360;++angle) {
            auto orbitCoverage=coverage;
            orbitCoverage[0]*=1.f+float(angle)/360.f;
            orbitCoverage[13]+=float(angle)/360.f;
            const auto rotated=DetailShadowProjection(focus,direction,.1f,mode,4.f,8192,orbitCoverage);
            if(rotated!=detail)throw std::runtime_error("detail grid changed on orbit");
        }
    }
    std::puts("detail grid: density, shared depth, texel snapping and 720 orbit cases passed");
}
void CheckUnifiedVisibleShadowBox() {
    using namespace mikumikustudio;
    NativeShadowBounds scene;
    const float low[]{-100,-100,-100},high[]{100,100,100};
    scene.Include(low);scene.Include(high);
    // This off-centre orthographic picture sees x=[-1.5,2.5], y=[-1.2,.8],
    // z=[2,6]. All six clip planes, including the finite far plane, matter.
    std::array<float,16> clip{};
    clip[0]=.5f;clip[5]=1;clip[10]=.25f;clip[15]=1;
    clip[12]=-.25f;clip[13]=.2f;clip[14]=-.5f;
    NativeShadowBounds expectedWorld;
    const float pictureLow[]{-1.5f,-1.2f,2},pictureHigh[]{2.5f,.8f,6};
    expectedWorld.Include(pictureLow);expectedWorld.Include(pictureHigh);
    const auto project=[](const std::array<float,16>& matrix,const std::array<float,3>& p) {
        std::array<float,3> q{};
        for(int axis=0;axis<3;++axis)
            q[axis]=matrix[12+axis]+p[0]*matrix[axis]+p[1]*matrix[4+axis]+p[2]*matrix[8+axis];
        return q;
    };
    const float focus[]{0,0,4};
    for(const auto& light:std::vector<std::array<float,3>>{{-.5f,-1,.5f},{0,-1,0},{1,0,0}}) {
        const auto full=NativeShadowProjection(focus,light.data(),.1f,1,&scene);
        const auto visible=VisibleShadowLightBounds(scene,clip.data(),full.data());
        NativeShadowBounds expected;
        for(unsigned corner=0;corner<8;++corner)expected.Include(project(full,expectedWorld.Corner(corner)).data());
        if(!visible.Valid())throw std::runtime_error("off-centre picture has no visible shadow bounds");
        for(int axis=0;axis<3;++axis)
            if(std::abs(visible.minimum[axis]-expected.minimum[axis])>1e-6f ||
               std::abs(visible.maximum[axis]-expected.maximum[axis])>1e-6f)
                throw std::runtime_error("visible box ignored a final camera clip plane or projection offset");
        const auto fitted=FitVisibleShadowBox(full,visible);
        for(int row=0;row<4;++row)
            if(fitted[row*4+2]!=full[row*4+2])
                throw std::runtime_error("visible box discarded off-screen caster depth");
        for(unsigned corner=0;corner<8;++corner) {
            const auto q=project(fitted,expectedWorld.Corner(corner));
            if(std::abs(q[0])>=1 || std::abs(q[1])>=1 || q[2]<=0 || q[2]>=1)
                throw std::runtime_error("unified box cropped a visible receiver corner");
        }
        const auto receiver=project(fitted,{0,0,4});
        const auto caster=project(fitted,{-40*light[0],-40*light[1],4-40*light[2]});
        if(std::abs(receiver[0]-caster[0])>1e-5f || std::abs(receiver[1]-caster[1])>1e-5f ||
           !(caster[2]>0 && caster[2]<receiver[2]))
            throw std::runtime_error("off-screen caster no longer projects onto its visible receiver");
        const auto basisLength=[&](int column) {
            return std::sqrt(fitted[column]*fitted[column]+fitted[4+column]*fitted[4+column]+fitted[8+column]*fitted[8+column]);
        };
        if(std::abs(basisLength(0)-basisLength(1))>1e-5f)
            throw std::runtime_error("unified coverage box is not square in light space");
        // An object entirely beyond any one camera boundary must not enlarge
        // the receiver box, though it still remains in the caster pass.
        for(int face=0;face<6;++face) {
            NativeShadowBounds outside=expectedWorld;
            const int axis=face/2;
            if(face&1){outside.minimum[axis]=expectedWorld.maximum[axis]+1;outside.maximum[axis]=outside.minimum[axis]+1;}
            else {outside.maximum[axis]=expectedWorld.minimum[axis]-1;outside.minimum[axis]=outside.maximum[axis]-1;}
            if(VisibleShadowLightBounds(outside,clip.data(),full.data()).Valid())
                throw std::runtime_error("an off-screen object enlarged visible receiver coverage");
        }
    }
    std::puts("PASS unified visible box: six clip planes, projection offsets, square fit and off-screen casters");
}
void CheckCameraPictureShadowRegion() {
    using namespace mikumikustudio;
    using Matrix=std::array<float,16>;
    const auto identity=[](){Matrix m{};for(int i=0;i<4;++i)m[i*5]=1;return m;};
    const auto multiply=[](const Matrix& a,const Matrix& b){
        Matrix out{};
        for(int row=0;row<4;++row)for(int column=0;column<4;++column) {
            double sum=0;for(int k=0;k<4;++k)sum+=double(a[row*4+k])*double(b[k*4+column]);
            out[row*4+column]=float(sum);
        }
        return out;
    };
    const auto invert=[](const Matrix& in){
        double a[4][8]{};
        for(int row=0;row<4;++row)for(int column=0;column<4;++column)a[row][column]=in[row*4+column];
        for(int row=0;row<4;++row)a[row][4+row]=1;
        for(int column=0;column<4;++column) {
            int pivot=column;
            for(int row=column+1;row<4;++row)
                if(std::abs(a[row][column])>std::abs(a[pivot][column]))pivot=row;
            if(std::abs(a[pivot][column])<1e-12)throw std::runtime_error("singular camera projection fixture");
            if(pivot!=column)for(int k=0;k<8;++k)std::swap(a[column][k],a[pivot][k]);
            const double divisor=a[column][column];
            for(int k=0;k<8;++k)a[column][k]/=divisor;
            for(int row=0;row<4;++row)if(row!=column) {
                const double factor=a[row][column];
                for(int k=0;k<8;++k)a[row][k]-=factor*a[column][k];
            }
        }
        Matrix out{};
        for(int row=0;row<4;++row)for(int column=0;column<4;++column)out[row*4+column]=float(a[row][4+column]);
        return out;
    };
    const auto perspective=[](float fovDegrees,float aspect,float nearPlane,float farPlane){
        Matrix m{};
        const double y=1.0/std::tan(double(fovDegrees)*3.14159265358979323846/360.0);
        m[0]=float(y/aspect);m[5]=float(y);
        m[10]=float(farPlane/(farPlane-nearPlane));m[11]=1;
        m[14]=float(-nearPlane*farPlane/(farPlane-nearPlane));
        return m;
    };
    const auto rotation=[&](float yaw,float pitch){
        const auto rotationX=[&](float angle){Matrix m{};const float c=std::cos(angle),s=std::sin(angle);
            m[0]=m[15]=1;m[5]=c;m[6]=s;m[9]=-s;m[10]=c;return m;};
        const auto rotationY=[&](float angle){Matrix m{};const float c=std::cos(angle),s=std::sin(angle);
            m[5]=m[15]=1;m[0]=c;m[2]=-s;m[8]=s;m[10]=c;return m;};
        return multiply(rotationY(yaw),rotationX(pitch));
    };
    const auto transform=[](const Matrix& m,const std::array<float,3>& p){
        std::array<float,4> q{};
        for(int a=0;a<4;++a)q[a]=m[12+a]+p[0]*m[a]+p[1]*m[4+a]+p[2]*m[8+a];
        std::array<float,3> result{};
        if(!(std::abs(q[3])>1e-12f))return result;
        for(int a=0;a<3;++a)result[a]=q[a]/q[3];
        return result;
    };
    constexpr float aspect=4.f/3.f;
    const Matrix inverseView=identity();
    const Matrix inverseProjection30=invert(perspective(30.f,aspect,1.f,100000.f));
    // The detail region shrinks in world space with the picture's zoom factor.
    const auto region30=[&](float focusDepth){
        return CameraDepthShadowRegion(inverseView.data(),inverseProjection30.data(),focusDepth);
    };
    const auto closeRegion=region30(5.f),middleRegion=region30(22.5f),farRegion=region30(45.f);
    if(!closeRegion.valid || !middleRegion.valid || !farRegion.valid)
        throw std::runtime_error("camera picture region rejected a valid focus depth");
    for(const auto& sample:std::vector<std::pair<float,CameraShadowRegion>>{{5.f,closeRegion},{22.5f,middleRegion},{45.f,farRegion}}) {
        const auto& region=sample.second;
        if(!(region.farDepth>0) || !std::isfinite(region.radius) || region.radius<=0)
            throw std::runtime_error("camera picture region is not finite");
        if(std::abs(region.centre[2]-sample.first)>sample.first*.02f)
            throw std::runtime_error("camera picture region did not centre on the focus depth");
    }
    const float halfRate=farRegion.radius/middleRegion.radius;
    if(halfRate<1.7f || halfRate>2.3f)
        throw std::runtime_error("halving the camera distance did not halve the detail footprint");
    const float fullRate=farRegion.radius/closeRegion.radius;
    if(fullRate<7.5f)
        throw std::runtime_error("detail footprint does not track camera distance zoom");
    // A narrower lens shows less world, so the detail footprint must shrink too.
    const auto narrow=CameraDepthShadowRegion(inverseView.data(),
        invert(perspective(10.f,aspect,1.f,100000.f)).data(),45.f);
    if(!narrow.valid || narrow.radius>farRegion.radius*.6f)
        throw std::runtime_error("detail footprint does not track field of view");
    // Far depth is the blend end of the detail layer; it must follow the focus.
    if(!(farRegion.farDepth>farRegion.radius) || !(narrow.farDepth<farRegion.farDepth))
        throw std::runtime_error("detail depth cut did not follow the camera picture");
    // Rotating the camera around the focus must not change the footprint.
    for(int angle=-180;angle<180;angle+=30) {
        const Matrix rotated=rotation(float(angle)*.01745329252f,float(angle%45)*.01745329252f);
        const auto turned=CameraDepthShadowRegion(rotated.data(),inverseProjection30.data(),45.f);
        if(!turned.valid || std::abs(turned.radius-farRegion.radius)>farRegion.radius*.001f ||
            std::abs(turned.farDepth-farRegion.farDepth)>1e-3f)
            throw std::runtime_error("detail footprint changed with camera rotation");
    }
    // The picture at the focus plane must stay inside the detail map.
    std::array<std::array<float,3>,4> plane{};
    for(int i=0;i<4;++i) {
        const float x=i&1?1.f:-1.f,y=i&2?1.f:-1.f;
        const auto nearPoint=transform(inverseProjection30,{x,y,0});
        const auto farPoint=transform(inverseProjection30,{x,y,1});
        const float t=(45.f-nearPoint[2])/(farPoint[2]-nearPoint[2]);
        for(int a=0;a<3;++a)plane[i][a]=nearPoint[a]+t*(farPoint[a]-nearPoint[a]);
    }
    NativeShadowBounds scene;
    const float sceneLow[3]{farRegion.centre[0]-farRegion.radius,farRegion.centre[1]-farRegion.radius,farRegion.centre[2]-farRegion.radius};
    const float sceneHigh[3]{farRegion.centre[0]+farRegion.radius,farRegion.centre[1]+farRegion.radius,farRegion.centre[2]+farRegion.radius};
    scene.Include(sceneLow);scene.Include(sceneHigh);
    const float lightDirection[3]{-.5f,-1,.5f};
    const auto coverage=NativeShadowProjection(farRegion.centre.data(),lightDirection,.1f,1,&scene);
    const auto detail=CameraDetailShadowProjection(farRegion,lightDirection,coverage);
    unsigned covered=0;
    for(const auto& point:plane) {
        double q[4]{};
        for(int a=0;a<4;++a)q[a]=detail[12+a]+point[0]*detail[a]+point[1]*detail[4+a]+point[2]*detail[8+a];
        if(!(q[3]>0) || std::abs(q[0]/q[3])>=1 || std::abs(q[1]/q[3])>=1 ||
            q[2]/q[3]<=0 || q[2]/q[3]>=1)
            throw std::runtime_error("detail map misses the picture at the focus plane");
        ++covered;
    }
    // The same check at a distance-zoom and a lens-zoom pose.
    for(const auto& pose:std::vector<std::pair<float,Matrix>>{
            {5.f,invert(perspective(30.f,aspect,1.f,100000.f))},
            {45.f,invert(perspective(10.f,aspect,1.f,100000.f))}}) {
        const auto region=CameraDepthShadowRegion(inverseView.data(),pose.second.data(),pose.first);
        if(!region.valid)throw std::runtime_error("zoomed camera picture region rejected a valid pose");
        NativeShadowBounds bounds;
        const float low[3]{region.centre[0]-region.radius,region.centre[1]-region.radius,region.centre[2]-region.radius};
        const float high[3]{region.centre[0]+region.radius,region.centre[1]+region.radius,region.centre[2]+region.radius};
        bounds.Include(low);bounds.Include(high);
        const auto map=CameraDetailShadowProjection(region,lightDirection,
            NativeShadowProjection(region.centre.data(),lightDirection,.1f,1,&bounds));
        for(int i=0;i<4;++i) {
            const float x=i&1?1.f:-1.f,y=i&2?1.f:-1.f;
            const auto nearPoint=transform(pose.second,{x,y,0});
            const auto farPoint=transform(pose.second,{x,y,1});
            const float t=(pose.first-nearPoint[2])/(farPoint[2]-nearPoint[2]);
            std::array<float,3> point{};
            for(int a=0;a<3;++a)point[a]=nearPoint[a]+t*(farPoint[a]-nearPoint[a]);
            double q[4]{};
            for(int a=0;a<4;++a)q[a]=map[12+a]+point[0]*map[a]+point[1]*map[4+a]+point[2]*map[8+a];
            if(!(q[3]>0) || std::abs(q[0]/q[3])>=1 || std::abs(q[1]/q[3])>=1 || q[2]/q[3]<=0 || q[2]/q[3]>=1)
                throw std::runtime_error("zoomed detail map misses the picture at the focus plane");
        }
    }
    // Orthographic pictures keep their world size with depth; the footprint must
    // follow the camera distance that MMD uses for the orthographic zoom.
    const auto orthographic=[](float scale,float aspectRatio){
        Matrix m{};m[0]=scale;m[5]=aspectRatio*scale;m[10]=.0013f;m[15]=1;return m;
    };
    const auto orthographicRegion=[&](float distance){
        return CameraDepthShadowRegion(inverseView.data(),
            invert(orthographic(2.f/distance,aspect)).data(),distance);
    };
    const auto nearOrtho=orthographicRegion(5.f),wideOrtho=orthographicRegion(45.f);
    if(!nearOrtho.valid || !wideOrtho.valid)
        throw std::runtime_error("orthographic camera picture region rejected a valid pose");
    if(wideOrtho.radius<nearOrtho.radius*2.8f)
        throw std::runtime_error("orthographic detail footprint does not track camera distance");
    // The closest layer starts at the actual camera clipping plane, even
    // when the orbit target is far beyond its sampling budget. Its fixed
    // geometry must retain the original mode's centre sampling density, without
    // deriving a new precision floor from an isolated character's bounds.
    constexpr unsigned maximum=8192;
    constexpr float radiusBudget=5.f;
    NativeShadowBounds completeScene;
    const float completeLow[]{-10000,-10000,-10000},completeHigh[]{10000,10000,10000};
    completeScene.Include(completeLow);completeScene.Include(completeHigh);
    unsigned nearCases=0;
    for(float fov:{10.f,30.f,70.f})for(float shape:{.75f,4.f/3.f,2.4f})for(float nearPlane:{.1f,1.f}) {
        Matrix camera=rotation(.7f,-.3f);
        camera[12]=13;camera[13]=-4;camera[14]=27;
        auto lens=perspective(fov,shape,nearPlane,10000.f);
        lens[8]=.15f;lens[9]=-.1f; // Final projection can move the picture centre.
        const auto inverseLens=invert(lens);
        const auto region=CameraNearShadowRegion(camera.data(),inverseLens.data(),radiusBudget);
        if(!region.valid || std::abs(region.nearDepth-nearPlane)>1e-5f ||
           !(region.farDepth>nearPlane) || region.radius>radiusBudget*1.0001f)
            throw std::runtime_error("near layer does not fit its guaranteed sampling budget");
        const auto coverage=NativeShadowProjection(region.centre.data(),lightDirection,.1f,1,&completeScene);
        const auto map=CameraDetailShadowProjection(region,lightDirection,coverage);
        const auto worldToCamera=invert(camera);
        const auto centreInView=transform(worldToCamera,region.centre);
        if(std::abs(centreInView[2]-(nearPlane+region.farDepth)*.5f)>2e-4f)
            throw std::runtime_error("near layer starts at the orbit target instead of the actual clipping plane");
        for(int corner=0;corner<4;++corner) {
            const float x=corner&1?1.f:-1.f,y=corner&2?1.f:-1.f;
            const auto start=transform(inverseLens,{x,y,0});
            const auto middle=transform(inverseLens,{x,y,.5f});
            for(float depth:{nearPlane,region.farDepth}) {
                const float fraction=(depth-start[2])/(middle[2]-start[2]);
                std::array<float,3> point{};
                for(int axis=0;axis<3;++axis)point[axis]=start[axis]+fraction*(middle[axis]-start[axis]);
                const auto mapped=transform(map,transform(camera,point));
                if(std::abs(mapped[0])>.801f || std::abs(mapped[1])>.801f || mapped[2]<=0 || mapped[2]>=1)
                    throw std::runtime_error("near clip face falls outside the unblended high-density region");
            }
        }
        const float width=2.f/MinimumShadowScale(map,completeScene);
        for(int mode:{1,2})for(float range:{.1f,.075f,.05f,.0344f,.00001f}) {
            const float density=NativeShadowSamplingDensity(lightDirection,range,mode,4096);
            const unsigned size=ShadowTextureSizeForDensity(width,density,maximum);
            const auto fitted=FitShadowSamplingDensity(map,density,size);
            const float actual=MinimumShadowScale(fitted,completeScene)*float(size)*.5f;
            if(std::abs(actual-density)>density*1e-4f)
                throw std::runtime_error("near layer does not retain the original mode's centre sampling density");
        }
        ++nearCases;
    }
    // Every fixed-budget layer starts at the camera clipping plane, including
    // the bridge over the large gap to scene-wide coverage. Pan changes its
    // centre, not its footprint, required resolution, or texel alignment.
    unsigned fixedLayerCases=0,nominalGridDifferences=0;
    for(float budget:{5.f,25.f,80.f})for(float fov:{10.f,30.f,70.f})
        for(float nearPlane:{.1f,1.f})for(unsigned actualSize:{1024u,2048u,8192u}) {
            auto lens=perspective(fov,16.f/9.f,nearPlane,10000.f);
            lens[8]=.15f;lens[9]=-.1f;
            const auto inverseLens=invert(lens);
            auto camera=rotation(.7f,-.3f);
            camera[12]=13;camera[13]=-4;camera[14]=27;
            const auto baseline=CameraNearShadowRegion(camera.data(),inverseLens.data(),budget);
            if(!baseline.valid || std::abs(baseline.nearDepth-nearPlane)>1e-5f ||
                baseline.radius>budget*1.0001f)
                throw std::runtime_error("fixed near/middle/bridge layer lost its clipping-plane budget");
            const auto shared=NativeShadowProjection(baseline.centre.data(),lightDirection,.1f,1,&completeScene);
            const auto baselineMap=CameraDetailShadowProjection(baseline,lightDirection,shared,actualSize);
            const auto nominalMap=CameraDetailShadowProjection(baseline,lightDirection,shared,8192);
            for(int axis=0;axis<2;++axis) {
                const float nominalTexel=nominalMap[12+axis]*(actualSize*.5f);
                nominalGridDifferences+=std::abs(nominalTexel-std::round(nominalTexel))>1e-4f;
            }
            const float baselineWidth=2.f/MinimumShadowScale(baselineMap,completeScene);
            // Stay away from a rounding boundary: any changed tier is a real
            // dependence on camera translation, not an equality tolerance.
            const float demand=.6f*actualSize/(2.f*budget);
            const unsigned expectedSize=ShadowSizeForDensity(baselineWidth,demand,512,8192);
            if(expectedSize!=actualSize)throw std::runtime_error("fixed-layer resolution fixture has the wrong tier");
            for(float pan:{0.f,.003f,.17f,3.25f,31.f}) {
                auto moved=camera;moved[12]+=pan;moved[13]-=pan*.3f;moved[14]+=pan*.7f;
                const auto region=CameraNearShadowRegion(moved.data(),inverseLens.data(),budget);
                if(!region.valid || std::abs(region.nearDepth-nearPlane)>1e-5f ||
                    std::abs(region.radius-baseline.radius)>budget*1e-4f ||
                    std::abs(region.farDepth-baseline.farDepth)>(std::max)(1e-4f,baseline.farDepth*1e-4f))
                    throw std::runtime_error("camera pan changed a fixed layer's footprint or depth interval");
                const auto map=CameraDetailShadowProjection(region,lightDirection,shared,actualSize);
                const float width=2.f/MinimumShadowScale(map,completeScene);
                if(std::abs(width-baselineWidth)>baselineWidth*1e-4f ||
                    ShadowSizeForDensity(width,demand,512,8192)!=expectedSize)
                    throw std::runtime_error("camera pan changed fixed-layer density or allocation tier");
                for(int axis=0;axis<2;++axis) {
                    const float coordinate=map[12+axis]*(actualSize*.5f);
                    if(std::abs(coordinate-std::round(coordinate))>1e-4f)
                        throw std::runtime_error("detail layer snaps to a nominal instead of actual texel grid");
                }
                for(int row=0;row<4;++row)if(map[row*4+2]!=shared[row*4+2])
                    throw std::runtime_error("fixed layer changed the shared off-screen-caster depth range");
                for(int corner=0;corner<4;++corner) {
                    const float x=corner&1?1.f:-1.f,y=corner&2?1.f:-1.f;
                    const auto start=transform(inverseLens,{x,y,0});
                    const auto middle=transform(inverseLens,{x,y,.5f});
                    for(float depth:{region.nearDepth,region.farDepth}) {
                        const float fraction=(depth-start[2])/(middle[2]-start[2]);
                        std::array<float,3> point{};
                        for(int axis=0;axis<3;++axis)point[axis]=start[axis]+fraction*(middle[axis]-start[axis]);
                        const auto mapped=transform(map,transform(moved,point));
                        const float guard=.8f+1.f/actualSize+2e-4f;
                        if(std::abs(mapped[0])>guard || std::abs(mapped[1])>guard || mapped[2]<=0 || mapped[2]>=1)
                            throw std::runtime_error("actual-grid snapping clipped a fixed layer's camera slab");
                    }
                }
                ++fixedLayerCases;
            }
        }
    if(!nominalGridDifferences)throw std::runtime_error("actual texel-grid fixture cannot detect nominal-grid sliding");
    std::printf("PASS %u translated near/middle/bridge slabs at actual 1024/2048/8192 texel grids, nominal_grid_negative_controls=%u\n",
        fixedLayerCases,nominalGridDifferences);
    const auto narrowNear=CameraNearShadowRegion(inverseView.data(),
        invert(perspective(1.f,aspect,1.f,10000.f)).data(),.02f);
    if(!narrowNear.valid || std::abs(narrowNear.nearDepth-1.f)>1e-5f ||
       narrowNear.farDepth<=narrowNear.nearDepth || narrowNear.farDepth-narrowNear.nearDepth>=.1f)
        throw std::runtime_error("narrow nearest layer lost its real clipping interval");
    // The middle layer may include the near plane to bridge the distance
    // between its old focus slab and the closest protected layer.
    const auto continuous=CameraDepthShadowRegion(inverseView.data(),inverseProjection30.data(),45.f,true);
    if(!continuous.valid || std::abs(continuous.nearDepth-1.f)>1e-5f ||
       std::abs(continuous.farDepth-farRegion.farDepth)>1e-4f)
        throw std::runtime_error("continuous middle region moved its far end or missed the actual near plane");
    const auto continuousMap=CameraDetailShadowProjection(continuous,lightDirection,
        NativeShadowProjection(continuous.centre.data(),lightDirection,.1f,1,&completeScene));
    for(int corner=0;corner<4;++corner) {
        const auto point=transform(inverseProjection30,{corner&1?1.f:-1.f,corner&2?1.f:-1.f,0});
        const auto mapped=transform(continuousMap,point);
        if(std::abs(mapped[0])>=.86f || std::abs(mapped[1])>=.86f)
            throw std::runtime_error("middle region cannot bridge to the actual near clipping face");
    }
    for(int mode:{1,2})for(float width:{.1f,1.f,10.f,100.f,10000.f}) {
        float previous=INFINITY;
        Matrix raw{};raw[0]=raw[5]=2.f/width;raw[10]=.01f;raw[14]=.5f;raw[15]=1;
        for(int position=0;position<=9999;++position) {
            const float range=(10000.f-position)/100000.f;
            const float density=NativeShadowSamplingDensity(lightDirection,range,mode,4096);
            const unsigned size=ShadowTextureSizeForDensity(width,density,maximum);
            const auto fitted=FitShadowSamplingDensity(raw,density,size);
            const float actual=MinimumShadowScale(fitted,completeScene)*float(size)*.5f;
            if(actual>previous*(1.f+1e-5f) || actual>density*(1.f+1e-5f))
                throw std::runtime_error("physical texture allocation reversed or raised slider sampling precision");
            previous=actual;
        }
    }
    // A wide orthographic near plane can exceed the finite texture budget.
    // Keep its true footprint so callers can detect the unmet floor; claiming
    // that an 8192 texture alone meets it would hide the original regression.
    Matrix wideLens=identity();wideLens[0]=.01f;wideLens[5]=.02f;
    wideLens[10]=1.f/999.f;wideLens[14]=-1.f/999.f;
    const auto wideNear=CameraNearShadowRegion(inverseView.data(),invert(wideLens).data(),radiusBudget);
    if(!wideNear.valid || wideNear.radius<=radiusBudget)
        throw std::runtime_error("oversized orthographic clip face incorrectly claims a guaranteed near budget");
    const auto wideMap=CameraDetailShadowProjection(wideNear,lightDirection,
        NativeShadowProjection(wideNear.centre.data(),lightDirection,.1f,1,&completeScene));
    const float wideWidth=2.f/MinimumShadowScale(wideMap,completeScene);
    const float densityFloor=NativeShadowSamplingDensity(lightDirection,.1f,1,4096);
    const unsigned wideSize=ShadowTextureSizeForDensity(wideWidth,densityFloor,maximum);
    const auto limitedMap=FitShadowSamplingDensity(wideMap,densityFloor,wideSize);
    const float limitedDensity=MinimumShadowScale(limitedMap,completeScene)*float(wideSize)*.5f;
    if(wideSize!=maximum || !(limitedDensity<densityFloor))
        throw std::runtime_error("hardware-limited near density was not exposed to its caller");
    std::printf("PASS %u true-near-plane poses, 100000 original-density ordering checks and orthographic budget limit\n",nearCases);
    // An unusable focus must not produce a region.
    for(float depth:{NAN,0.f,.5f,-5.f})
        if(CameraDepthShadowRegion(inverseView.data(),inverseProjection30.data(),depth).valid)
            throw std::runtime_error("invalid focus depth produced a camera picture region");
    std::printf("PASS camera picture detail region: %u focus corners, zoom %.2fx, rotation, FOV, orthographic\n",
        covered,fullRate);
}

void CheckDynamicPrecision() {
    using namespace mikumikustudio;
    // Demand is inverse in the picture's focus depth: moving twice as close
    // doubles the texels needed to keep one shadow texel per screen pixel.
    const float projectedPixels=3600.f,coverageWidth=24.f;
    const float atTen=DynamicShadowMapDemand(projectedPixels,coverageWidth,10.f,false);
    const float atTwenty=DynamicShadowMapDemand(projectedPixels,coverageWidth,20.f,false);
    if(!(atTen>0) || std::abs(atTen/atTwenty-2.f)>1e-5f)
        throw std::runtime_error("dynamic demand does not follow the inverse focus depth");
    if(std::abs(DynamicShadowMapDemand(projectedPixels,coverageWidth,5.f,true)-
                DynamicShadowMapDemand(projectedPixels,coverageWidth,500.f,true))>1e-3f)
        throw std::runtime_error("orthographic dynamic demand changed with depth");
    if(DynamicShadowMapDemand(projectedPixels,coverageWidth,0.f,false)!=0.f ||
       DynamicShadowMapDemand(projectedPixels,0.f,10.f,false)!=0.f)
        throw std::runtime_error("invalid dynamic demand was not rejected");
    // Historical tier/hysteresis helper, no longer the production density rule.
    // Retain its API regression while checking ShadowLayerDensity separately.
    // The chosen size only raises the selected quality, by at most one tier.
    for(unsigned selected:{512u,1024u,2048u,4096u,8192u}) {
        const unsigned maximum=8192u;
        const unsigned upper=(std::min)(maximum,selected*2);
        unsigned previous=selected;
        for(float required:{1e-3f,.1f,50.f,200.f,400.f,700.f,900.f,1500.f,4000.f,20000.f,1e9f,
                            800.f,300.f,50.f,1e9f,0.f}) {
            const unsigned size=DynamicShadowQualitySize(required,selected,previous,maximum);
            if(size<selected || size>upper)
                throw std::runtime_error("dynamic quality left the selected tier band");
            if(size&(size-1u))
                throw std::runtime_error("dynamic quality is not a power of two");
            if(size!=selected && size!=upper)
                throw std::runtime_error("dynamic quality skipped a band tier");
            previous=size;
        }
    }
    // Hysteresis: a 1024 selection upgrades above 2.5x its coverage demand and
    // keeps the larger map until demand falls to 1.6x; it never goes below the
    // selected size, so the same slider keeps the same floor precision.
    const auto choose=[](float required,unsigned previous){
        return DynamicShadowQualitySize(required,1024,previous,8192);
    };
    if(choose(700.f,1024)!=1024 || choose(1400.f,1024)!=2048)
        throw std::runtime_error("dynamic growth threshold is not centred on the selection");
    if(choose(900.f,2048)!=2048 || choose(700.f,2048)!=1024)
        throw std::runtime_error("dynamic shrink threshold has no hysteresis");
    if(choose(180.f,1024)!=1024 || choose(0.f,1024)!=1024)
        throw std::runtime_error("dynamic precision dropped below the selected size");
    // A camera sweep only ever raises the size and returns to the selected tier
    // when the view moves far away.
    unsigned quality=1024,previousQuality=quality,changes=0;
    for(float depth=400.f;depth>=1.f;depth*=.85f) {
        const float required=DynamicShadowMapDemand(3600.f,24.f,depth,false);
        quality=DynamicShadowQualitySize(required,1024,quality,8192);
        changes+=quality!=previousQuality;
        if(quality<1024 || quality>previousQuality*2 || quality*2<previousQuality)
            throw std::runtime_error("dynamic quality left its floor or jumped two tiers");
        previousQuality=quality;
    }
    const unsigned closest=quality;
    for(float depth=1.f;depth<=400.f;depth*=1.18f) {
        const float required=DynamicShadowMapDemand(3600.f,24.f,depth,false);
        quality=DynamicShadowQualitySize(required,1024,quality,8192);
        changes+=quality!=previousQuality;
        if(quality<1024)throw std::runtime_error("dynamic quality fell below the slider tier");
        previousQuality=quality;
    }
    if(closest!=2048 || quality!=1024)
        throw std::runtime_error("dynamic precision sweep did not return to the slider tier");
    std::printf("PASS dynamic precision: inverse-depth curve, one-tier raise, %u hysteresis changes\n",changes);
}

void CheckShadowRangeBox() {
    using namespace mikumikustudio;
    // Legacy geometry helper only: its smaller character box must no longer
    // determine production sampling density.
    NativeShadowBounds scene,characterBox;
    const float sceneLow[3]{-1000,-5,-1000},sceneHigh[3]{1000,40,1000};
    const float smallLow[3]{-10,0,-10},smallHigh[3]{10,20,10};
    scene.Include(sceneLow);scene.Include(sceneHigh);
    characterBox.Include(smallLow);characterBox.Include(smallHigh);
    const float focus[3]{0,10,0},light[3]{-.5f,-1,.5f};
    const float half=1.f/NativeShadowLateralScale(.01125f,1);
    const auto limited=NativeShadowProjection(focus,light,.1f,1,&scene,half);
    const auto unlimited=NativeShadowProjection(focus,light,.1f,1,&scene);
    const float limitedWidth=2.f/MinimumShadowScale(limited,scene);
    const float unlimitedWidth=2.f/MinimumShadowScale(unlimited,scene);
    // The default box is capped at the original range and cannot grow with the
    // scene; the scene-fitted box is much wider.
    if(!(limitedWidth<=2.f*half*1.001f) || !(limitedWidth<unlimitedWidth*.2f))
        throw std::runtime_error("shadow range box did not cap the scene-fitted coverage");
    // A character-sized scene keeps its own smaller fitted extent.
    const auto characterProjection=NativeShadowProjection(focus,light,.1f,1,&characterBox,half);
    const float characterWidth=2.f/MinimumShadowScale(characterProjection,characterBox);
    if(!(characterWidth<limitedWidth))
        throw std::runtime_error("shadow range box destroyed a character-sized fitted extent");
    std::printf("PASS shadow range box: %.2f world wide, scene fit %.2f, character fit %.2f\n",
        limitedWidth,unlimitedWidth,characterWidth);
}

// Independent evaluation of the minimal build's original projection chain.
// Solve (Y+b)/W=0 at its centre, then transform two light-space depths through
// scale -> Z*2-1 -> projective orientation -> (Z+W)/2 -> divide by W.
double OriginalCentreDepthDifference(const float light[3],int mode,double gap) {
    const double length=std::sqrt(double(light[0])*light[0]+double(light[1])*light[1]+double(light[2])*light[2]);
    const double slope=-double(light[2])/length;
    double a,b;
    if(slope < -.9 || slope > .9) {a=1-std::abs(slope);b=-a;}
    else if(slope < -.8) {a=mode==1?4*slope+3.7:9*slope+8.2;b=-9*slope-8.2;}
    else if(slope > .8) {a=mode==1?3.7-4*slope:8.2-9*slope;b=9*slope-8.2;}
    else {a=mode==1?.5:1;b=-1;}
    const double centreY=-b,centreW=1+a*centreY;
    if(std::abs((centreY+b)/centreW)>1e-12)throw std::runtime_error("original shadow centre fixture is not centred");
    const auto projectDepth=[&](double lightZ) {
        const double beforeOrientation=2*(.01125*.15*lightZ)-1;
        return (beforeOrientation+centreW)*.5/centreW;
    };
    return projectDepth(50+gap)-projectDepth(50);
}
void CheckComparisonCalibration() {
    using namespace mikumikustudio;
    unsigned checked=0;
    for(float slope:{-1.f,-.90001f,-.9f,-.89999f,-.80001f,-.8f,-.79999f,0.f,
                     .79999f,.8f,.80001f,.89999f,.9f,.90001f,1.f}) {
        const float light[]{std::sqrt((std::max)(0.f,1-slope*slope)),0,-slope};
        for(int mode:{1,2}) {
            const auto map=NativeShadowComparisonProjection(light,mode);
            for(float value:map)if(!std::isfinite(value))
                throw std::runtime_error("mode comparison calibration is not finite at an original branch boundary");
            for(int row=0;row<4;++row)
                if(map[row*4+1]!=0 || map[row*4+3]!=(row==3?1.f:0.f))
                    throw std::runtime_error("comparison calibration reintroduced an unbounded native Y or projective W");
            const double length=std::sqrt(double(light[0])*light[0]+double(light[1])*light[1]+double(light[2])*light[2]);
            for(double gap:{0.,.1,.25,.5,1.}) {
                double actual=0;
                for(int axis=0;axis<3;++axis)actual+=map[axis*4+2]*light[axis]*gap/length;
                const double expected=OriginalCentreDepthDifference(light,mode,gap);
                if(std::abs(actual-expected)>2e-9)
                    throw std::runtime_error("stable comparison does not match the original mode's centre depth derivative");
            }
            const float scaled[]{light[0]*7,light[1]*7,light[2]*7};
            const auto normalized=NativeShadowComparisonProjection(scaled,mode);
            for(unsigned i=0;i<map.size();++i)if(std::abs(map[i]-normalized[i])>2e-9f)
                throw std::runtime_error("light vector magnitude changed mode comparison calibration");
            ++checked;
        }
    }
    for(const auto& light:std::vector<std::array<float,3>>{{0,0,0},{1e-30f,0,0},{NAN,0,1},{0,INFINITY,0},{1e30f,1e30f,1e30f}})
        for(int mode:{1,2})for(float value:NativeShadowComparisonProjection(light.data(),mode))
            if(!std::isfinite(value))throw std::runtime_error("degenerate light produced an invalid mode comparison matrix");
    const float commonLight[]{-.5f,-1,.5f};
    const double first=OriginalCentreDepthDifference(commonLight,1,.25)*1500;
    const double second=OriginalCentreDepthDifference(commonLight,2,.25)*4000;
    if(!(first>.3 && first<1.3 && second>first+.3 && second<1.3))
        throw std::runtime_error("same-gap mode contrast fixture does not distinguish the original modes");
    std::printf("PASS %u original mode-centre branch calibrations, normalized and degenerate light vectors\n",checked);
}

// Independently differentiate the minimal build's original XY projection at
// texture centre. Do not use the production shape/density helpers here: their
// shared mode branches are exactly what this reference is meant to check.
std::array<double,2> OriginalCentreSamplingJacobian(const float light[3],float range,int mode,unsigned size) {
    const double length=std::sqrt(double(light[0])*light[0]+double(light[1])*light[1]+double(light[2])*light[2]);
    const double slope=-double(light[2])/length;
    const double base=(std::clamp)(double(range),.00001,.1);
    double scale,a,b;
    if(mode==1) {
        if(slope < -.9 || slope > .9)scale=base;
        else if(slope < -.8)scale=((slope-.1)*10+10)*base+base;
        else if(slope > .8)scale=(10-(slope+.1)*10)*base+base;
        else scale=base+base;
    } else {
        if(slope < -.9 || slope > .9)scale=base;
        else if(slope < -.8)scale=(slope*20+19)*base;
        else if(slope > .8)scale=(19-slope*20)*base;
        else scale=base*3;
    }
    if(slope < -.9 || slope > .9){a=1-std::abs(slope);b=-a;}
    else if(slope < -.8){a=mode==1?4*slope+3.7:9*slope+8.2;b=-9*slope-8.2;}
    else if(slope > .8){a=mode==1?3.7-4*slope:8.2-9*slope;b=9*slope-8.2;}
    else {a=mode==1?.5:1;b=-1;}
    const double centreY=-b/scale,step=1e-4/scale;
    const auto uv=[&](double x,double y){
        const double w=1+a*scale*y;
        return std::array<double,2>{{.5+.5*scale*x/w,.5+.5*(scale*y+b)/w}};
    };
    const auto left=uv(-step,centreY),right=uv(step,centreY);
    const auto top=uv(0,centreY+step),bottom=uv(0,centreY-step);
    return {{size*(right[0]-left[0])/(2*step),size*(top[1]-bottom[1])/(2*step)}};
}

void CheckNativeSamplingDensity() {
    using namespace mikumikustudio;
    unsigned cases=0;
    for(float slope:{-1.f,-.90001f,-.9f,-.89999f,-.80001f,-.8f,-.79999f,0.f,
                     .79999f,.8f,.80001f,.89999f,.9f,.90001f,1.f}) {
        const float light[]{std::sqrt((std::max)(0.f,1-slope*slope)),0,-slope};
        const float scaled[]{light[0]*7,light[1]*7,light[2]*7};
        for(int mode:{1,2})for(unsigned size:{2048u,4096u})
            for(float range:{-.25f,0.f,.00001f,.01125f,.0344f,.05f,.075f,.1f,.25f}) {
                const auto expected=OriginalCentreSamplingJacobian(light,range,mode,size);
                const float actual=NativeShadowSamplingDensity(light,range,mode,size);
                const float normalized=NativeShadowSamplingDensity(scaled,range,mode,size);
                if(!std::isfinite(actual) || actual<=0)
                    throw std::runtime_error("original sampling density is not finite and positive");
                for(double derivative:expected)
                    if(std::abs(actual-derivative)>derivative*5e-6)
                        throw std::runtime_error("sampling density does not match the original mode's centre XY Jacobian");
                if(std::abs(actual-normalized)>actual*5e-6f)
                    throw std::runtime_error("light vector magnitude changed original sampling density");
                ++cases;
            }
    }
    for(const auto& light:std::vector<std::array<float,3>>{{0,0,0},{1e-30f,0,0},{NAN,0,1},{0,INFINITY,0},{1e30f,1e30f,1e30f}})
        for(int mode:{1,2}) {
            const float density=NativeShadowSamplingDensity(light.data(),.01125f,mode,4096);
            if(!std::isfinite(density) || density<=0)
                throw std::runtime_error("degenerate light produced an invalid sampling density");
        }
    std::printf("PASS %u independent original XY Jacobians: two modes, light branches, 2048/4096 and range clamps\n",cases);
}

// Independent double-precision oracle. Close layers retain 80% of the extra
// L4 demand after the real allocation cap; far layers use the reciprocal lower
// envelope and a three-pixel budget.
double ExpectedLayerDensity(double reference,double finest,double pixels,double depth,
                            bool orthographic,bool dynamic,bool nearest,
                            double capacity=std::numeric_limits<double>::infinity()) {
    if(!dynamic)return reference;
    const double screen=1.25*pixels/(2*(orthographic?1.0:depth));
    const double footprint=screen*std::pow((std::min)(1.0,reference/finest),.25)/(nearest?1.0:3.0);
    if(!nearest)return std::pow(std::pow(reference,-4)+std::pow(footprint,-4),-.25);
    const double fixed=(std::min)(reference,capacity);
    const double boosted=(std::min)(std::pow(std::pow(reference,4)+std::pow(footprint,4),.25),capacity);
    return fixed+.8*(boosted-fixed);
}

void CheckLayerSamplingDensity() {
    using namespace mikumikustudio;
    struct LayerCase {float pixels,depth;bool orthographic,dynamic,nearest;};
    const LayerCase samples[]{
        {3600,1,false,true,false},{3600,25,false,true,false},
        {3600,250,false,true,false},{3600,2500,false,true,false},
        {32,1,true,true,false},{32,2500,true,true,false},
        {3600,2500,false,false,false},{32,2500,true,false,false},
        {3600,2500,false,true,true},{32,2500,true,true,true}};
    const float light[]{-.5f,-1,.5f};
    unsigned cases=0;
    for(int mode:{1,2})for(unsigned size:{2048u,4096u}) {
        const float finest=NativeShadowSamplingDensity(light,.1f,mode,size);
        for(const auto& sample:samples) {
            const float screen=1.25f*sample.pixels/(2.f*(sample.orthographic?1.f:sample.depth));
            float previous=INFINITY;
            for(int position=0;position<=9999;++position) {
                const float range=(10000.f-position)/100000.f;
                const float reference=NativeShadowSamplingDensity(light,range,mode,size);
                const float actual=ShadowLayerDensity(reference,finest,sample.pixels,sample.depth,
                    sample.orthographic,sample.dynamic,sample.nearest);
                const double expected=ExpectedLayerDensity(reference,finest,sample.pixels,sample.depth,
                    sample.orthographic,sample.dynamic,sample.nearest);
                if(!std::isfinite(actual) || actual<=0 || std::abs(actual-expected)>expected*5e-6f ||
                    actual>previous*(1.f+5e-6f) || (!sample.dynamic && actual!=reference) ||
                    (sample.dynamic && sample.nearest && actual<reference*(1.f-5e-6f)) ||
                    (sample.dynamic && !sample.nearest && (actual>reference*(1.f+5e-6f) ||
                        actual>screen*std::pow(reference/finest,.25f)/3*(1.f+5e-6f))))
                    throw std::runtime_error("dynamic screen density disagrees with its L4 plan or reversed the slider rule");
                previous=actual;
                ++cases;
            }
        }
        // Screen quality retains ordered slider endpoints. The reduced close
        // boost lies between the fixed linear ratio and the screen's fourth
        // root ratio, rather than collapsing all lower settings to one floor.
        const float coarse=NativeShadowSamplingDensity(light,.00001f,mode,size);
        for(float depth:{.1f,1.f,25.f,250.f,2500.f}) {
            const float high=ShadowLayerDensity(finest,finest,3600,depth,false,true,false);
            const float low=ShadowLayerDensity(coarse,finest,3600,depth,false,true,false);
            if(!(low<high) || low/high<coarse/finest*(1-1e-5f) ||
               low/high>std::pow(coarse/finest,.25f)*(1+1e-5f))
                throw std::runtime_error("screen demand erased or reversed the slider endpoint relationship");
        }
        const float nearHigh=ShadowLayerDensity(finest,finest,3600,.001f,false,true,true);
        const float nearLow=ShadowLayerDensity(coarse,finest,3600,.001f,false,true,true);
        if(nearLow<=coarse || nearHigh<=finest || nearLow>=nearHigh ||
           nearLow/nearHigh<coarse/finest*(1-1e-5f) ||
           nearLow/nearHigh>std::pow(coarse/finest,.25f)*(1+1e-5f))
            throw std::runtime_error("close-up dynamic precision did not exceed the baseline with ordered screen quality");
        for(float bad:{0.f,-1.f,NAN,INFINITY}) {
            const float invalidPixels=ShadowLayerDensity(coarse,finest,bad,25,false,true,false);
            const float invalidDepth=ShadowLayerDensity(coarse,finest,3600,bad,false,true,false);
            if(!std::isfinite(invalidPixels) || !std::isfinite(invalidDepth) ||
                std::abs(invalidPixels-coarse)>coarse*1e-6f || std::abs(invalidDepth-coarse)>coarse*1e-6f)
                throw std::runtime_error("invalid screen demand did not retain the reference density");
        }
    }
    for(float q:{.0001f,.01f,.1125f,.5f,1.f,2.f}) {
        const float actual=ShadowScreenQuality(q*640,640);
        if(std::abs(actual-std::pow((std::min)(1.f,q),.25f))>1e-6f)
            throw std::runtime_error("screen quality does not use the fourth-root slider ratio");
    }
    for(float invalid:{0.f,-1.f,NAN,INFINITY})
        if(ShadowScreenQuality(invalid,640)!=0 || ShadowScreenQuality(80,invalid)!=0)
            throw std::runtime_error("invalid screen quality did not fail closed");
    unsigned extremeCases=0;
    for(float reference:{1e-30f,1e-10f,1.f,1e10f,1e30f})
        for(float pixels:{1e-20f,1.f,1e20f})for(float depth:{1e-12f,1.f,1e12f})
            for(bool nearest:{false,true}) {
                const float finest=reference*2;
                const double expected=ExpectedLayerDensity(reference,finest,pixels,depth,false,true,nearest);
                const float actual=ShadowLayerDensity(reference,finest,pixels,depth,false,true,nearest);
                if(!std::isfinite(actual) || actual<=0 || std::abs(actual-expected)>expected*8e-6)
                    throw std::runtime_error("normalized dynamic density overflowed or lost finite extreme demand");
                ++extremeCases;
            }
    const float huge=std::numeric_limits<float>::max();
    if(ShadowLayerDensity(80,640,huge,.001f,false,true,false)!=80 ||
       ShadowLayerDensity(80,640,NAN,NAN,false,false,true)!=80)
        throw std::runtime_error("unusable screen demand or disabled dynamics changed fixed precision");
    std::printf("PASS %u dynamic slider/depth samples and %u float extremes: ordered screen quality, fixed baseline and near boost\n",cases,extremeCases);
}

void CheckCloseShadowSamplingBudget() {
    using namespace mikumikustudio;
    constexpr float finest=640.f;
    unsigned cases=0,capped=0,sweepSamples=0;
    for(float reference:{.064f,6.4f,64.f,640.f})for(float capacity:{16.f,64.f,256.f,8192.f})
        for(float pixels:{16.f,1024.f,8192.f})for(float depth:{.1f,1.f,16.f,256.f})
            for(bool orthographic:{false,true}) {
                const float actual=ShadowLayerDensity(reference,finest,pixels,depth,orthographic,true,true,capacity);
                const double expected=ExpectedLayerDensity(reference,finest,pixels,depth,orthographic,true,true,capacity);
                const double screen=.625*pixels/(orthographic?1.0:depth)*std::pow(reference/finest,.25);
                const double old=(std::min)(std::pow(std::pow(double(reference),4)+std::pow(screen,4),.25),double(capacity));
                const double fixed=(std::min)(double(reference),double(capacity));
                if(!std::isfinite(actual) || actual<fixed*(1-2e-6) || actual>old*(1+2e-6) ||
                   std::abs(actual-expected)>expected*5e-6)
                    throw std::runtime_error("close sampling reduction exceeded its fixed baseline or real texture capacity");
                if(capacity<=reference && actual!=capacity)
                    throw std::runtime_error("near sampling reduction lowered the already capacity-limited fixed precision");
                if(old>fixed*1.01) {
                    if(std::abs((actual-fixed)/(old-fixed)-.8)>2e-5)
                        throw std::runtime_error("close sampling did not remove twenty percent of actual extra demand");
                    if(old==capacity)++capped;
                }
                if(ShadowLayerDensity(reference,finest,pixels,depth,orthographic,false,true,capacity)!=reference ||
                   ShadowLayerDensity(reference,finest,pixels,depth,orthographic,true,false,capacity)!=
                   ShadowLayerDensity(reference,finest,pixels,depth,orthographic,true,false))
                    throw std::runtime_error("close capacity budget changed fixed precision or the distant curve");
                ++cases;
            }
    if(!capped)throw std::runtime_error("close sampling fixture never exercised the real capacity limit");
    // Continuous camera/lens enlargement must remain ordered through the cap.
    // The former implementation could reduce a requested value yet produce
    // the same saturated allocation; verify attainable world density as well.
    for(float reference:{6.4f,64.f,640.f})for(float capacity:{32.f,128.f,1024.f}) {
        float previous=0;
        for(unsigned step=0;step<=1024;++step) {
            const float pixels=float(std::exp2(-8.0+28.0*step/1024));
            const float actual=ShadowLayerDensity(reference,finest,pixels,8,false,true,true,capacity);
            const float samePicture=ShadowLayerDensity(reference,finest,pixels*2,16,false,true,true,capacity);
            if(actual<previous*(1-2e-6f) || (previous && actual>previous*1.02f) ||
               actual>capacity*(1+2e-6f) || std::abs(actual-samePicture)>actual*2e-6f)
                throw std::runtime_error("near camera or lens scaling jumps or changes at equal projected demand");
            previous=actual;++sweepSamples;
        }
    }
    const float light[]{-.5f,-1,.5f};
    for(int mode:{1,2})for(float capacity:{32.f,128.f,1024.f}) {
        const float finestMode=NativeShadowSamplingDensity(light,.1f,mode,4096);
        float previous=INFINITY;
        for(int position=0;position<=9999;++position) {
            const float reference=NativeShadowSamplingDensity(light,(10000.f-position)/100000.f,mode,4096);
            const float actual=ShadowLayerDensity(reference,finestMode,3600,8,false,true,true,capacity);
            if(actual>previous*(1+3e-6f))
                throw std::runtime_error("capacity-aware close sampling reversed precision slider ordering");
            previous=actual;++sweepSamples;
        }
    }
    for(unsigned maximum:{512u,2048u,8192u})for(float width:{2.f,10.f,100.f}) {
        const float capacity=(maximum-1.f)/width,reference=capacity*.1f;
        const float wanted=ShadowLayerDensity(reference,capacity,4096,.1f,false,true,true,capacity);
        const unsigned size=ShadowTextureSizeForDensity(width,wanted,maximum);
        std::array<float,16> raw{};raw[0]=raw[5]=2.f/width;raw[10]=.01f;raw[14]=.5f;raw[15]=1;
        const auto fitted=FitShadowSamplingDensity(raw,wanted,size);
        const float actual=fitted[0]*size*.5f;
        const double expected=reference+.8*(capacity-reference);
        if(std::abs(actual-expected)>expected*5e-6 || actual>=capacity*.9f)
            throw std::runtime_error("near boost reduction was hidden by a saturated physical allocation");
        ++cases;
    }
    std::printf("PASS close sampling budget: cases=%u capped_reductions=%u continuous_samples=%u fixed_floor=1 actual_extra_gain=0.8\n",
        cases,capped,sweepSamples);
}

void CheckFarShadowDensityCurve() {
    using namespace mikumikustudio;
    // The reciprocal norm has a smooth crossover where the screen budget
    // equals the original reference; no hard min or clamp knee is allowed.
    constexpr float reference=80.f,finest=640.f,projectedPixels=1024.f;
    const auto factor=[=](float depth) {
        return ShadowLayerDensity(reference,finest,projectedPixels,depth,false,true,false)/reference;
    };
    const float joinDepth=.625f*projectedPixels*std::pow(reference/finest,.25f)/(3*reference);
    const float join=factor(joinDepth);
    if(std::abs(join-std::pow(2.f,-.25f))>1e-6f)
        throw std::runtime_error("far screen budget did not use the reciprocal smooth minimum");
    float previousSlopeGap=INFINITY;
    for(float step:{.02f,.01f,.005f,.0025f}) {
        const float leftSlope=(join-factor(joinDepth*(1.f-step)))/(step*join);
        const float rightSlope=(factor(joinDepth*(1.f+step))-join)/(step*join);
        const float gap=std::abs(leftSlope-rightSlope);
        if(leftSlope>=0 || rightSlope>=0 || gap>4*step || gap>previousSlopeGap*.6f+2e-4f)
            throw std::runtime_error("dynamic screen/far composition lost its continuous first derivative");
        previousSlopeGap=gap;
    }
    unsigned samples=0;
    float previous=INFINITY;
    for(unsigned step=0;step<=4096;++step) {
        const float depth=float(std::exp2(-3.0+17.0*step/4096));
        const float actual=factor(depth);
        const double expected=ExpectedLayerDensity(reference,finest,projectedPixels,depth,false,true,false)/reference;
        if(!std::isfinite(actual) || actual<=0 || actual>1.f+1e-6f || actual>joinDepth/depth*(1+3e-6f) ||
            actual>previous+1e-6f || std::abs(actual-expected)>expected*3e-6)
            throw std::runtime_error("far curve exceeded fixed precision, its screen budget or monotone envelope");
        previous=actual;++samples;
    }
    for(float depth:{512.f,2048.f,8192.f,32768.f}) {
        const float ratio=factor(depth*4)/factor(depth);
        if(ratio<.249f || ratio>.251f)
            throw std::runtime_error("four times farther did not follow the far screen-pixel budget");
    }
    for(float depth:{1.f,10.f,100.f,10000.f}) {
        float previousDensity=0,previousFraction=INFINITY;
        for(float requested:{.064f,.64f,6.4f,64.f,640.f}) {
            const float actual=ShadowLayerDensity(requested,finest,projectedPixels,depth,false,true,false);
            if(actual<previousDensity || actual/requested>previousFraction+1e-6f || actual>requested*(1+1e-6f))
                throw std::runtime_error("higher original far precision did not attenuate more relatively while staying ordered");
            previousDensity=actual;previousFraction=actual/requested;
        }
    }
    for(float pixels:{16.f,64.f,1024.f})for(float depth:{.1f,1.f,10.f,1000.f}) {
        const float orthographic=ShadowLayerDensity(reference,finest,pixels,depth,true,true,false);
        const float orthographicReference=ShadowLayerDensity(reference,finest,pixels,1.f,true,true,false);
        const float perspective=ShadowLayerDensity(reference,finest,pixels,depth,false,true,false);
        const float equivalentProjection=ShadowLayerDensity(reference,finest,pixels*2,depth*2,false,true,false);
        if(orthographic!=orthographicReference || std::abs(perspective-equivalentProjection)>reference*1e-6f)
            throw std::runtime_error("far-shadow attenuation depends on depth under orthographic projection or changes at equal projected demand");
    }
    // Lens zoom and viewport scale enter through the final projection. Equal
    // pixels/depth must agree even if panel distance, FOV and viewport differ.
    for(float fov:{10.f,30.f,70.f})for(float aspect:{.75f,4.f/3.f,2.4f}) {
        const float y=1.f/std::tan(fov*3.14159265358979323846f/360.f);
        const float pixels=(std::max)(768.f*aspect*(y/aspect),768.f*y);
        const float demand=ShadowLayerDensity(reference,finest,pixels,20,false,true,false);
        const float sameImage=ShadowLayerDensity(reference,finest,pixels*2,40,false,true,false);
        const float enlarged=ShadowLayerDensity(reference,finest,pixels*2,20,false,true,false);
        if(std::abs(demand-sameImage)>demand*2e-6f || enlarged<=demand)
            throw std::runtime_error("final lens/viewport footprint did not drive dynamic precision");
    }
    // Object bounds enter only the allocation. Small pieces and one merged
    // large box at the same rendered depth must retain the same requested rho;
    // a budgeted case may reduce it but must never raise it.
    unsigned footprints=0;
    for(float depth:{.25f,1.f,4.f,16.f,256.f})for(float width:{.001f,.1f,1.f,10.f,100.f,1000.f}) {
        const float density=ShadowLayerDensity(reference,finest,projectedPixels,depth,false,true,false);
        const unsigned size=ShadowTextureSizeForDensity(width,density,8192);
        std::array<float,16> raw{};raw[0]=raw[5]=2.f/width;raw[10]=.01f;raw[14]=.5f;raw[15]=1;
        const auto fitted=FitShadowSamplingDensity(raw,density,size);
        const float actual=fitted[0]*size*.5f;
        if(actual>density*(1.f+3e-6f) ||
            (double(width)*density+2<8192 && std::abs(actual-density)>density*3e-6f))
            throw std::runtime_error("object footprint or allocation tier changed attainable dynamic density");
        ++footprints;
    }
    std::printf("PASS dynamic density: C1 join, %u bounded far samples, inverse-depth budget, projection equivalence and %u independent footprints\n",
        samples,footprints);
}

void CheckSamplingFootprint() {
    using namespace mikumikustudio;
    using Matrix=std::array<float,16>;
    // Orthogonal world light axes, with nonzero translations on both texture
    // axes. Raw boxes represent small/split models and large/merged models at
    // the same camera depth; they are deliberately unrelated to target density.
    const float axes[3][3]{{.8f,-.6f,0},{.36f,.48f,.8f},{-.48f,-.64f,.6f}};
    const auto rawBox=[&](float width,float centreX,float centreY) {
        Matrix map{};
        for(int row=0;row<3;++row) {
            map[row*4]=axes[0][row]*2.f/width;
            map[row*4+1]=axes[1][row]*2.f/width;
            map[row*4+2]=axes[2][row]*.0013f;
        }
        map[12]=-centreX*2.f/width;map[13]=-centreY*2.f/width;map[14]=.47f;map[15]=1;
        return map;
    };
    const auto actualDensity=[](const Matrix& map,unsigned size) {
        double squared=0;for(int row=0;row<3;++row)squared+=double(map[row*4])*map[row*4];
        return float(std::sqrt(squared)*size*.5);
    };
    const auto texel=[](const Matrix& map,unsigned size,const std::array<float,3>& p,int axis) {
        double clip=map[12+axis];
        for(int row=0;row<3;++row)clip+=double(p[row])*map[row*4+axis];
        return clip*size*.5;
    };
    const auto checkFit=[&](float width,float centreX,float centreY,float density,unsigned size) {
        const auto raw=rawBox(width,centreX,centreY);
        const auto fitted=FitShadowSamplingDensity(raw,density,size);
        for(float value:fitted)if(!std::isfinite(value))
            throw std::runtime_error("sampling footprint is not finite");
        for(int row=0;row<4;++row)for(int axis:{2,3})
            if(fitted[row*4+axis]!=raw[row*4+axis])
                throw std::runtime_error("sampling footprint changed shared caster Z or affine W");
        const float actual=actualDensity(fitted,size),rawDensity=actualDensity(raw,size);
        if(actual<=0 || actual>(std::min)(density,rawDensity)*(1.f+2e-5f))
            throw std::runtime_error("minimum texture size or tier rounding raised sampling density");
        // Leave room for an edge guard when the raw box already exhausts the
        // budget. With genuine expansion room, allocations must hit the rule.
        if(density<rawDensity*.8f && std::abs(actual-density)>density*2e-5f)
            throw std::runtime_error("a small object or physical texture size changed in-budget density");
        for(int axis=0;axis<2;++axis) {
            const double grid=double(fitted[12+axis])*size*.5;
            if(std::abs(grid-std::round(grid))>2e-3)
                throw std::runtime_error("sampling footprint did not snap to the actual world texel grid");
        }
        for(int corner=0;corner<8;++corner) {
            const double local[]{centreX+(corner&1?.5:-.5)*width,
                centreY+(corner&2?.5:-.5)*width,corner&4?7.0:-7.0};
            for(int axis=0;axis<2;++axis) {
                double q=fitted[12+axis];
                for(int row=0;row<3;++row) {
                    double world=0;for(int a=0;a<3;++a)world+=axes[a][row]*local[a];
                    q+=world*fitted[row*4+axis];
                }
                if(std::abs(q)>1.00002)
                    throw std::runtime_error("world-grid snapping clipped the original receiver footprint");
            }
        }
        return fitted;
    };
    unsigned cases=0,equalDensityCases=0;
    const float light[]{-.5f,-1,.5f};
    for(int mode:{1,2})for(unsigned reference:{2048u,4096u})
        for(float range:{.1f,.05f,.0344f,.01125f,.00001f})
            for(float width:{.01f,.1f,1.f,7.3f,31.f,300.f,30000.f})
                for(unsigned maximum:{512u,1024u,2048u,8192u}) {
                    const float density=NativeShadowSamplingDensity(light,range,mode,reference);
                    const unsigned size=ShadowTextureSizeForDensity(width,density,maximum);
                    checkFit(width,.137f,-.371f,density,size);
                    ++cases;
                }
    // A lower allocation (including an allocation-failure fallback to 512)
    // changes padding rather than precision while the requested box still fits.
    for(float density:{.03f,1.f,4.f,15.f})for(float width:{.01f,.1f,1.f,7.3f}) {
        const auto baseline=checkFit(width,.137f,-.371f,density,8192);
        for(unsigned size:{512u,1024u,2048u,4096u,8192u}) {
            const auto fitted=checkFit(width,.137f,-.371f,density,size);
            if(std::abs(actualDensity(fitted,size)-actualDensity(baseline,8192))>density*2e-5f)
                throw std::runtime_error("allocation fallback changed an in-budget density");
            for(const auto& point:std::vector<std::array<float,3>>{{0,0,0},{1.1f,-2.3f,4.7f},{-27,13,5}})
                for(int axis=0;axis<2;++axis) {
                    const double delta=texel(fitted,size,point,axis)-texel(baseline,8192,point,axis);
                    if(std::abs(delta-std::round(delta))>2e-3)
                        throw std::runtime_error("allocation fallback shifted the shared world sampling phase");
                }
            ++equalDensityCases;
        }
    }
    // Same geometry as one box or four split boxes. All are in budget and
    // must have identical light-space gradients and texel phase, independent
    // of each object's size and centre.
    constexpr float splitDensity=4.f;
    const auto merged=checkFit(32,.137f,-.371f,splitDensity,512);
    for(int part=0;part<4;++part) {
        const auto split=checkFit(16,.137f+(part&1?8:-8),-.371f+(part&2?8:-8),splitDensity,512);
        for(int row=0;row<3;++row)for(int axis=0;axis<2;++axis)
            if(std::abs(split[row*4+axis]-merged[row*4+axis])>1e-7f)
                throw std::runtime_error("splitting an object changed its sampling density or light basis");
        for(const auto& point:std::vector<std::array<float,3>>{{0,0,0},{1.1f,-2.3f,4.7f},{-27,13,5}})
            for(int axis=0;axis<2;++axis) {
                const double delta=texel(split,512,point,axis)-texel(merged,512,point,axis);
                if(std::abs(delta-std::round(delta))>2e-3)
                    throw std::runtime_error("splitting an object shifted its world sampling phase");
            }
    }
    // Straddle every physical power-of-two boundary. The next larger map must
    // add padding rather than doubling the requested world-space precision.
    for(unsigned boundary:{512u,1024u,2048u,4096u})for(float offset:{-2.25f,-2.f,-1.75f,-.25f,0.f,.25f}) {
        const float width=(float(boundary)+offset)/splitDensity;
        const unsigned size=ShadowTextureSizeForDensity(width,splitDensity,8192);
        const auto fitted=checkFit(width,.137f,-.371f,splitDensity,size);
        if(std::abs(actualDensity(fitted,size)-splitDensity)>splitDensity*2e-5f)
            throw std::runtime_error("physical power-of-two boundary changed attainable precision");
    }
    std::printf("PASS %u object-size/budget footprints, %u equal-density allocation fallbacks, split/merged grids and power-of-two boundaries\n",
        cases,equalDensityCases);
}

double SmoothTransitionWeight(double value) {
    value=(std::clamp)(value,0.0,1.0);
    return value*value*(3-2*value);
}

void CheckScreenShadowTransitions() {
    using namespace mikumikustudio;
    constexpr float reference=80,finest=640,pixels=1024;
    const double screenAtOne=.625*double(pixels)*std::pow(double(reference)/finest,.25);
    unsigned cases=0;
    for(const auto& interval:std::vector<std::array<float,3>>{{1,8,44},{1,44,142},{1,1.13f,1.2f}}) {
        const float nearDepth=interval[0],fixedStart=interval[1],farDepth=interval[2];
        const double latest=(std::max)(double(fixedStart),double(farDepth)/1.5);
        float previous=INFINITY;
        for(int step=0;step<=1024;++step) {
            const float coarse=float(std::exp2(-8.0+24.0*step/1024));
            const float start=ShadowScreenBlendStart(nearDepth,farDepth,fixedStart,coarse,
                reference,finest,pixels,false);
            const double expected=(std::clamp)(screenAtOne/coarse,double(fixedStart),latest);
            if(!std::isfinite(start) || std::abs(start-expected)>farDepth*3e-6 ||
               start<fixedStart || start>latest*(1+1e-6) || start>previous+farDepth*1e-6 ||
               (fixedStart<=farDepth/1.5f && farDepth/start<1.5f-1e-6f))
                throw std::runtime_error("screen transition lost its actual-density plan, fixed core or minimum log width");
            const float fraction=ShadowTransitionFraction(nearDepth,start,farDepth);
            if(std::abs(farDepth-(farDepth-nearDepth)*fraction-start)>farDepth*3e-6)
                throw std::runtime_error("screen transition was not preserved by the shader blend interval");
            previous=start;++cases;
        }
        // Small proportional zoom steps must not jump when either clamp is
        // crossed; continuity is tested through the fixed/full-core endpoints.
        previous=0;
        for(int step=0;step<=512;++step) {
            const float projected=float(pixels*std::exp2(-4.0+8.0*step/512));
            const float start=ShadowScreenBlendStart(nearDepth,farDepth,fixedStart,32,
                reference,finest,projected,false);
            if(start+1e-6f<previous || (previous>0 && start>previous*1.011f+1e-5f))
                throw std::runtime_error("small projection zoom produced an abrupt shadow hand-off");
            previous=start;++cases;
        }
    }
    const auto actualDensity=[](unsigned cap) {
        constexpr float width=100,wanted=64;
        const unsigned size=ShadowTextureSizeForDensity(width,wanted,cap);
        std::array<float,16> map{};map[0]=map[5]=2/width;map[10]=.01f;map[14]=.5f;map[15]=1;
        const auto fitted=FitShadowSamplingDensity(map,wanted,size);
        return fitted[0]*size*.5f;
    };
    const float full=actualDensity(8192),limited=actualDensity(512);
    const float fullStart=ShadowScreenBlendStart(1,44,8,full,reference,finest,pixels,false);
    const float limitedStart=ShadowScreenBlendStart(1,44,8,limited,reference,finest,pixels,false);
    if(!(limited<full && limitedStart>fullStart) || limitedStart>44.f/1.5f+1e-5f)
        throw std::runtime_error("capped coarse map did not let the finer layer take over earlier on approach");
    // Equal projected demand expressed in a different world scale must retain
    // the same transition. In orthographic views demand has no depth divisor.
    const float perspective=ShadowScreenBlendStart(1,44,8,16,reference,finest,pixels,false);
    const float equivalent=ShadowScreenBlendStart(1,44,8,32,reference,finest,pixels*2,false);
    if(std::abs(perspective-equivalent)>1e-5f ||
       ShadowScreenBlendStart(1,44,8,1000,reference,finest,pixels,true)!=8 ||
       std::abs(ShadowScreenBlendStart(1,44,8,1,reference,finest,pixels,true)-44.f/1.5f)>1e-5f)
        throw std::runtime_error("screen transition disagrees at equal demand or under orthographic projection");
    float previousOrtho=8;
    for(int step=0;step<=512;++step) {
        const float ratio=.875f+.5f*float(step)/512;
        const float start=ShadowScreenBlendStartForDemand(1,44,8,32,32*ratio,true);
        const double t=(std::clamp)((double(ratio)-1)*4,0.0,1.0);
        const double expected=8+(44.0/1.5-8)*t*t*(3-2*t);
        if(std::abs(start-expected)>1e-5 || start<previousOrtho || start-previousOrtho>.126f)
            throw std::runtime_error("orthographic screen demand jumps at its hand-off threshold");
        previousOrtho=start;++cases;
    }
    for(float boundary:{1.f,1.25f}) {
        constexpr float step=.001f;
        const float centre=ShadowScreenBlendStartForDemand(1,44,8,32,32*boundary,true);
        const float left=ShadowScreenBlendStartForDemand(1,44,8,32,32*(boundary-step),true);
        const float right=ShadowScreenBlendStartForDemand(1,44,8,32,32*(boundary+step),true);
        if(std::abs((centre-left)-(right-centre))>.0011f)
            throw std::runtime_error("orthographic screen transition does not join its constant branches smoothly");
    }
    for(float invalid:{0.f,-1.f,NAN,INFINITY})
        if(ShadowScreenBlendStart(1,44,8,invalid,reference,finest,pixels,false)!=8)
            throw std::runtime_error("invalid coarse density changed the fixed transition");
    for(float extreme:{1e-30f,1e30f}) {
        const float start=ShadowScreenBlendStart(1,44,8,extreme,reference,finest,pixels,false);
        if(!std::isfinite(start) || start<8 || start>44.f/1.5f+1e-5f)
            throw std::runtime_error("extreme coarse density produced an invalid transition");
    }
    std::printf("PASS %u screen-aware transitions: continuous zoom, actual capped density, orthographic selection and preserved log width\n",cases);
}

void CheckShadowTransitions() {
    using namespace mikumikustudio;
    unsigned samples=0;
    for(const auto& interval:std::vector<std::array<float,3>>{{1,8,44},{1,44,142},{.1f,.5f,10}}) {
        const double nearDepth=interval[0],startDepth=interval[1],farDepth=interval[2];
        const float fraction=ShadowTransitionFraction(float(nearDepth),float(startDepth),float(farDepth));
        const float ratio=ShadowTransitionLogRatio(float(startDepth),float(farDepth));
        const double width=(farDepth-nearDepth)*fraction;
        if(std::abs(farDepth-width-startDepth)>farDepth*2e-6 || ratio<=0 || ratio>=1)
            throw std::runtime_error("adjacent shadow transition does not span the requested complete depth interval");
        double previous=1,previousOld=1,maximumStep=0,maximumOldStep=0;
        for(int i=0;i<=256;++i) {
            const double depth=startDepth*std::pow(farDepth/startDepth,double(i)/256);
            const double linear=(std::clamp)((farDepth-depth)/width,0.0,1.0);
            const double mapped=std::log(1-ratio*linear)/std::log(1-ratio);
            const double expected=1-double(i)/256;
            if(std::abs(mapped-expected)>3e-6)
                throw std::runtime_error("equal camera zoom ratios do not advance equally through the shadow transition");
            const double weight=SmoothTransitionWeight(mapped);
            const double oldWeight=SmoothTransitionWeight((farDepth-depth)/((farDepth-nearDepth)*.35));
            if(weight>previous+1e-6)
                throw std::runtime_error("log shadow transition reversed with increasing camera depth");
            maximumStep=(std::max)(maximumStep,std::abs(weight-previous));
            maximumOldStep=(std::max)(maximumOldStep,std::abs(oldWeight-previousOld));
            previous=weight;previousOld=oldWeight;++samples;
        }
        if(previous>1e-6 || !(maximumStep<maximumOldStep*.65))
            throw std::runtime_error("complete logarithmic transition did not reduce the old 35-percent zoom step");
    }
    for(const auto& interval:std::vector<std::array<float,3>>{{NAN,1,2},{0,NAN,2},{0,1,INFINITY},{2,1,2},{3,1,2}})
        if(ShadowTransitionFraction(interval[0],interval[1],interval[2])!=.35f)
            throw std::runtime_error("invalid shadow transition did not preserve its linear fallback");
    for(const auto& interval:std::vector<std::array<float,2>>{{0,1},{-1,2},{2,2},{3,2},{NAN,2},{1,INFINITY}})
        if(ShadowTransitionLogRatio(interval[0],interval[1])!=0)
            throw std::runtime_error("invalid log depth interval did not select linear fallback");
    if(ShadowTransitionFraction(1,-10,20)!=1.f || ShadowTransitionFraction(1,30,20)!=.001f ||
        ShadowTransitionLogRatio(.000001f,1000)>.9999f)
        throw std::runtime_error("shadow transition safety bounds are not finite");

    // With a common hardware cap, ordered requested densities and increasing
    // footprints retain near >= middle >= bridge. Verify actual fitted density,
    // including capped allocations, rather than sorting noisy float results.
    unsigned state=0x4d4d5326u,ordered=0;
    const auto unit=[&]() {state=state*1664525u+1013904223u;return float(state>>8)/16777216.f;};
    const float light[]{-.5f,-1,.5f};
    for(int mode:{1,2})for(unsigned reference:{2048u,4096u})for(unsigned cap:{512u,1024u,4096u,8192u})
        for(unsigned sample=0;sample<256;++sample) {
            const float density=NativeShadowSamplingDensity(light,.00001f+.09999f*unit(),mode,reference);
            const float finest=NativeShadowSamplingDensity(light,.1f,mode,reference);
            const float width=.1f+400.f*unit(),depth=.1f+500.f*unit(),pixels=100.f+3900.f*unit();
            float previous=INFINITY;
            for(unsigned layer=0;layer<3;++layer) {
                const float multiplier=layer==0?1.f:layer==1?5.f:24.f;
                const float footprint=width*multiplier;
                const float wanted=ShadowLayerDensity(density,finest,pixels,depth*multiplier,false,true,true,
                    (cap-1.f)/footprint);
                const unsigned size=ShadowTextureSizeForDensity(footprint,wanted,cap);
                std::array<float,16> raw{};raw[0]=raw[5]=2.f/footprint;raw[10]=.01f;raw[14]=.5f;raw[15]=1;
                const auto fitted=FitShadowSamplingDensity(raw,wanted,size);
                const float actual=fitted[0]*size*.5f;
                if(actual>previous*(1.f+3e-6f) || actual>wanted*(1.f+3e-6f))
                    throw std::runtime_error("production slab density order reversed or exceeded its planned density");
                previous=actual;
            }
            ++ordered;
        }
    std::printf("PASS %u complete logarithmic depth samples and %u fixed-order production density plans\n",samples,ordered);
}

void CheckExpandedBridgeSampling() {
    using namespace mikumikustudio;
    using Matrix=std::array<float,16>;
    constexpr unsigned maximum=8192,referenceSize=4096;
    const float light[]{-.5f,-1,.5f};
    Matrix identity{};for(unsigned axis=0;axis<4;++axis)identity[axis*5]=1;
    // A real perspective lens defines all slabs. Pixel demand varies with the
    // viewport while object size, object grouping and orbit target are absent.
    // Integration checks in frame_benchmark separately require the renderer's
    // recorded fades to match these quality-independent geometric intervals.
    constexpr double sy=3.732050807568877,aspect=16.0/9.0,nearClip=.1,farClip=10000;
    const double a=farClip/(farClip-nearClip),b=-nearClip*a;
    Matrix inverseLens{};inverseLens[0]=float(aspect/sy);inverseLens[5]=float(1/sy);
    inverseLens[11]=float(1/b);inverseLens[14]=1;inverseLens[15]=float(-a/b);
    const float finestBudget=(std::max)(NativeShadowSamplingDensity(light,.1f,1,referenceSize),
        NativeShadowSamplingDensity(light,.1f,2,referenceSize));
    const float radius=(std::min)(5.f,(maximum-2.f)/(2*finestBudget));
    const auto nearRegion=CameraNearShadowRegion(identity.data(),inverseLens.data(),radius);
    const auto middle=CameraNearShadowRegion(identity.data(),inverseLens.data(),radius*5);
    const auto oldBridge=CameraNearShadowRegion(identity.data(),inverseLens.data(),radius*16);
    auto bridge=CameraNearShadowRegion(identity.data(),inverseLens.data(),radius*24);
    const float nominalBridgeWidth=2*bridge.radius;
    const auto shared=NativeShadowProjection(bridge.centre.data(),light,.1f,1);
    const auto nominal=CameraDetailShadowProjection(bridge,light,shared,0);
    const auto fittedBox=FitCameraShadowBox(identity.data(),inverseLens.data(),nominal,
        (std::max)(nearRegion.farDepth,middle.farDepth/1.5f));
    if(fittedBox.valid && fittedBox.farDepth>bridge.farDepth) {
        bridge.nearDepth=fittedBox.nearDepth;bridge.farDepth=fittedBox.farDepth;
    }
    if(!nearRegion.valid || !middle.valid || !oldBridge.valid || !bridge.valid ||
       bridge.farDepth<=oldBridge.farDepth*1.49f || middle.farDepth>=oldBridge.farDepth)
        throw std::runtime_error("extended bridge fixture did not cover the old far-map handoff");
    const std::array<float,3> starts{
        nearRegion.farDepth-(nearRegion.farDepth-nearRegion.nearDepth)*.35f,
        (std::max)(nearRegion.farDepth,middle.farDepth/1.5f),
        (std::max)(middle.farDepth,bridge.farDepth/2.5f)};
    const std::array<float,3> ends{nearRegion.farDepth,middle.farDepth,bridge.farDepth};
    const std::array<float,3> widths{2*nearRegion.radius,2*middle.radius,nominalBridgeWidth};
    const std::array<float,3> demandDepth{nearRegion.nearDepth,starts[0],middle.farDepth};
    const auto weights=[&](double depth) {
        std::array<double,4> result{};double remaining=1;
        for(unsigned layer=0;layer<3;++layer) {
            const double t=(std::clamp)(std::log(double(ends[layer])/depth)/
                std::log(double(ends[layer])/starts[layer]),0.0,1.0);
            const double local=t*t*(3-2*t);
            result[layer]=local*remaining;remaining*=1-local;
        }
        result[3]=remaining;return result;
    };
    std::vector<double> depths;
    for(unsigned depth=1;depth<=200;++depth)depths.push_back(depth);
    for(double end:{double(nearRegion.farDepth),double(middle.farDepth),
        double(oldBridge.farDepth),double(bridge.farDepth)})
        for(double ratio:{.999999,1.0,1.000001})depths.push_back(end*ratio);
    std::vector<std::array<double,4>> contributions;
    for(double depth:depths) {
        contributions.push_back(weights(depth));
        const auto& w=contributions.back();
        if(std::abs(w[0]+w[1]+w[2]+w[3]-1)>1e-12 ||
           (depth>=bridge.farDepth && (w[0]!=0 || w[1]!=0 || w[2]!=0 || w[3]!=1)))
            throw std::runtime_error("bridge inherited contribution leaked beyond its geometric far endpoint");
    }
    if(weights(oldBridge.farDepth)[2]<.3 || weights(starts[2])[2]<.999999)
        throw std::runtime_error("bridge takes over after the former coverage handoff rather than before it");
    const auto actualDensity=[&](float width,float wanted) {
        const unsigned size=ShadowTextureSizeForDensity(width,wanted,maximum);
        Matrix raw{};raw[0]=raw[5]=2/width;raw[10]=.001f;raw[14]=.5f;raw[15]=1;
        return FitShadowSamplingDensity(raw,wanted,size)[0]*size*.5f;
    };
    unsigned plans=0,mixtures=0;
    double worstOldFootprintRatio=0;
    for(int mode:{1,2})for(float pixels:{500.f,1500.f,2990.f,4480.f}) {
        const float finest=NativeShadowSamplingDensity(light,.1f,mode,referenceSize);
        std::array<double,4> previousDensity{INFINITY,INFINITY,INFINITY,INFINITY};
        std::vector<double> previousMixed(depths.size(),INFINITY),previousFootprint(depths.size(),0),
            previousProgressive(depths.size(),0);
        for(unsigned position=0;position<=9999;++position) {
            const float reference=NativeShadowSamplingDensity(light,(10000.f-position)/100000.f,mode,referenceSize);
            std::array<double,4> density{};
            for(unsigned layer=0;layer<3;++layer) {
                const float capacity=(maximum-1.f)/widths[layer];
                const float requested=ShadowLayerDensity(reference,finest,pixels,demandDepth[layer],false,true,true,capacity);
                density[layer]=actualDensity(widths[layer],requested);
                const double fixed=actualDensity(widths[layer],reference);
                if(density[layer]<fixed*(1-5e-6) ||
                   (layer && density[layer]>density[layer-1]*(1+5e-6)))
                    throw std::runtime_error("extended bridge reduced fixed precision or reversed layer density order");
                if(ShadowLayerDensity(reference,finest,pixels,demandDepth[layer],false,false,true,capacity)!=reference)
                    throw std::runtime_error("expanded dynamic bridge changed the disabled dynamic path");
            }
            // The full-picture map uses the OLD endpoint. The expanded close
            // bridge must not feed back into the far sampling budget.
            const float farRequest=ShadowLayerDensity(reference,finest,pixels,oldBridge.farDepth,false,true,false);
            density[3]=actualDensity(2000,farRequest);
            const double fixedFar=actualDensity(2000,reference);
            if(density[3]>fixedFar*(1+5e-6))
                throw std::runtime_error("distant coverage exceeded the same fixed-mode physical precision");
            for(unsigned layer=0;layer<4;++layer) {
                if(!std::isfinite(density[layer]) || density[layer]<=0 ||
                    density[layer]>previousDensity[layer]*(1+5e-6))
                    throw std::runtime_error("expanded layer reversed a precision slider step");
                previousDensity[layer]=density[layer];
            }
            for(unsigned probe=0;probe<depths.size();++probe) {
                const double pixelScale=pixels/(2*depths[probe]);
                double mixed=0,footprint=0;
                for(unsigned layer=0;layer<4;++layer) {
                    mixed+=contributions[probe][layer]*density[layer];
                    footprint+=contributions[probe][layer]*pixelScale/density[layer];
                }
                if(mixed>previousMixed[probe]*(1+5e-6) || footprint<previousFootprint[probe]*(1-5e-6))
                    throw std::runtime_error("inherited shadow weights reversed total slider quality or screen texel span");
                previousMixed[probe]=mixed;previousFootprint[probe]=footprint;++mixtures;
                // Independent physical-spacing contract: advance geometrically
                // between adjacent maps, keeping one endpoint interval at each
                // end. This does not reproduce the shader's integer grids or
                // fractional-grid interpolation implementation.
                const auto& c=contributions[probe];
                const double remaining=c[2]+c[3];
                const double bridgeWeight=remaining>0?c[2]/remaining:0;
                const double fineSpacing=1/density[2],coarseSpacing=1/density[3];
                const double desiredSpacing=(std::clamp)(std::exp(
                    (1-bridgeWeight)*std::log(coarseSpacing)+bridgeWeight*std::log(fineSpacing))*
                    std::exp2(1-2*bridgeWeight),fineSpacing,coarseSpacing);
                const double progressive=pixelScale*(c[0]/density[0]+c[1]/density[1]+
                    remaining*desiredSpacing);
                if(!std::isfinite(progressive) || progressive<previousProgressive[probe]*(1-5e-6))
                    throw std::runtime_error("progressive physical-spacing contract reversed a quality slider step");
                previousProgressive[probe]=progressive;
            }
            if(position==8875 || position==9900 || position==9999) {
                const auto w=weights(oldBridge.farDepth);
                const double before=pixels/(2*oldBridge.farDepth*density[3]);
                const double after=pixels/(2*oldBridge.farDepth)*(w[2]/density[2]+w[3]/density[3]);
                if(after>=before*.95)
                    throw std::runtime_error("low-precision close bridge did not improve the former coarse-grid handoff");
                worstOldFootprintRatio=(std::max)(worstOldFootprintRatio,after/before);
            }
            ++plans;
        }
    }
    std::printf("PASS expanded bridge: plans=%u inherited_and_progressive_samples=%u P=500,1500,2990,4480 q=0..9999 old_handoff_worst_pixel_span_ratio=%.6f far_extra=0\n",
        plans,mixtures,worstOldFootprintRatio);
}

void CheckTranslatedBridgeBox() {
    using namespace mikumikustudio;
    using Matrix=std::array<float,16>;
    using Point=std::array<double,3>;
    const auto identity=[] {Matrix result{};for(unsigned i=0;i<4;++i)result[i*5]=1;return result;};
    const auto multiply=[](const Matrix& left,const Matrix& right) {
        Matrix result{};
        for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)
            for(unsigned k=0;k<4;++k)result[row*4+col]+=left[row*4+k]*right[k*4+col];
        return result;
    };
    const auto inverse=[](const Matrix& source) {
        double rows[4][8]{};
        for(unsigned row=0;row<4;++row) {
            for(unsigned col=0;col<4;++col)rows[row][col]=source[row*4+col];
            rows[row][row+4]=1;
        }
        for(unsigned col=0;col<4;++col) {
            unsigned pivot=col;
            for(unsigned row=col+1;row<4;++row)if(std::abs(rows[row][col])>std::abs(rows[pivot][col]))pivot=row;
            if(std::abs(rows[pivot][col])<1e-14)throw std::runtime_error("singular translated-box test lens");
            if(pivot!=col)for(unsigned k=0;k<8;++k)std::swap(rows[pivot][k],rows[col][k]);
            const double scale=rows[col][col];for(double& value:rows[col])value/=scale;
            for(unsigned row=0;row<4;++row)if(row!=col) {
                const double factor=rows[row][col];
                for(unsigned k=0;k<8;++k)rows[row][k]-=rows[col][k]*factor;
            }
        }
        Matrix result{};
        for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)result[row*4+col]=float(rows[row][col+4]);
        return result;
    };
    const auto project=[](const Point& point,const Matrix& matrix) {
        double clip[4]{};
        for(unsigned col=0;col<4;++col)
            clip[col]=matrix[12+col]+point[0]*matrix[col]+point[1]*matrix[4+col]+point[2]*matrix[8+col];
        if(!std::isfinite(clip[3]) || std::abs(clip[3])<1e-12)
            throw std::runtime_error("invalid homogeneous point in translated-box oracle");
        return Point{clip[0]/clip[3],clip[1]/clip[3],clip[2]/clip[3]};
    };
    NativeShadowBounds scene;const float low[]{-5000,-5000,-5000},high[]{5000,5000,5000};
    scene.Include(low);scene.Include(high);
    unsigned valid=0,extended=0,clipLimited=0,cornerChecks=0,resourceChecks=0,invalid=0;
    double largestGain=1;
    Matrix validView{},validLens{},validNominal{};float validStart=0;
    for(bool orthographic:{false,true})for(float lensSize:{10.f,30.f,70.f})
      for(float aspect:{.75f,1.6f,2.4f})for(float offset:{0.f,.3f})
       for(float yaw:{0.f,.7f,1.5f})for(float farClip:{80.f,1000.f}) {
        Matrix lens{};constexpr float nearClip=1;
        if(orthographic) {
            lens[0]=2/lensSize;lens[5]=2*aspect/lensSize;
            lens[10]=1/(farClip-nearClip);lens[14]=-nearClip/(farClip-nearClip);lens[15]=1;
            lens[12]=offset;lens[13]=-offset*.4f;
        } else {
            const float vertical=1/std::tan(lensSize*3.14159265358979323846f/360);
            lens[0]=vertical/aspect;lens[5]=vertical;lens[10]=farClip/(farClip-nearClip);
            lens[11]=1;lens[14]=-nearClip*farClip/(farClip-nearClip);lens[8]=offset;lens[9]=-offset*.4f;
        }
        const auto inverseLens=inverse(lens);
        auto rx=identity(),ry=identity(),rz=identity();const float pitch=.31f,roll=-.2f;
        rx[5]=rx[10]=std::cos(pitch);rx[6]=std::sin(pitch);rx[9]=-rx[6];
        ry[0]=ry[10]=std::cos(yaw);ry[2]=-std::sin(yaw);ry[8]=-ry[2];
        rz[0]=rz[5]=std::cos(roll);rz[1]=std::sin(roll);rz[4]=-rz[1];
        auto view=multiply(multiply(rx,ry),rz);view[12]=-59.55f;view[13]=41.72f;view[14]=-2.99f;
        const auto nearRegion=CameraNearShadowRegion(view.data(),inverseLens.data(),5);
        const auto middle=CameraNearShadowRegion(view.data(),inverseLens.data(),25);
        const auto bridge=CameraNearShadowRegion(view.data(),inverseLens.data(),120);
        if(!nearRegion.valid || !middle.valid || !bridge.valid)throw std::runtime_error("invalid nominal box fixture");
        const float start=(std::max)(nearRegion.farDepth,middle.farDepth/1.5f);
        for(const auto& light:std::vector<std::array<float,3>>{{-.5f,-1,.5f},{0,-1,0},{1,0,0},{0,0,1},{.01f,-.001f,1}}) {
            const auto shared=NativeShadowProjection(bridge.centre.data(),light.data(),.1f,1,&scene);
            const auto nominal=CameraDetailShadowProjection(bridge,light.data(),shared,0);
            const auto box=FitCameraShadowBox(view.data(),inverseLens.data(),nominal,start);
            if(!box.valid) {++invalid;continue;}
            ++valid;
            validView=view;validLens=inverseLens;validNominal=nominal;validStart=start;
            if(!std::isfinite(box.farDepth) || box.nearDepth<start-1e-5f || box.farDepth<=box.nearDepth ||
                box.farDepth>farClip*(1+2e-4f))
                throw std::runtime_error("translated bridge ignored camera clipping or the requested overlap start");
            for(unsigned element=0;element<16;++element)if(element!=12 && element!=13 &&
                std::memcmp(&box.projection[element],&nominal[element],sizeof(float)))
                throw std::runtime_error("translated bridge changed its linear XY scale, homogeneous terms or shared caster Z");
            // Independent acceptance: project actual camera corner rays into
            // the returned light box. No sphere radius or fitter output is
            // used to infer whether the visible surfaces are covered.
            for(unsigned corner=0;corner<4;++corner) {
                const double x=corner&1?1.0:-1.0,y=corner&2?1.0:-1.0;
                const auto first=project({x,y,0},inverseLens),last=project({x,y,1},inverseLens);
                for(double depth:{double(box.nearDepth),double(box.farDepth)}) {
                    const double t=(depth-first[2])/(last[2]-first[2]);Point point{};
                    for(unsigned axis=0;axis<3;++axis)point[axis]=first[axis]+t*(last[axis]-first[axis]);
                    const auto mapped=project(project(point,view),box.projection);
                    if(std::abs(mapped[0])>.8002 || std::abs(mapped[1])>.8002)
                        throw std::runtime_error("translated bridge misses a visible frustum corner inside its promised slab");
                    ++cornerChecks;
                }
            }
            const float oldWidth=2/MinimumShadowScale(nominal,scene),newWidth=2/MinimumShadowScale(box.projection,scene);
            if(oldWidth!=newWidth)throw std::runtime_error("translated box grew the physical shadow footprint");
            for(int mode:{1,2})for(unsigned q:{0u,8875u,9900u,9999u}) {
                const float reference=NativeShadowSamplingDensity(light.data(),(10000.f-q)/100000.f,mode,4096);
                const float finest=NativeShadowSamplingDensity(light.data(),.1f,mode,4096);
                const float wanted=ShadowLayerDensity(reference,finest,2990,middle.farDepth,orthographic,true,true,8191/oldWidth);
                const unsigned oldSize=ShadowTextureSizeForDensity(oldWidth,wanted,8192);
                const unsigned newSize=ShadowTextureSizeForDensity(newWidth,wanted,8192);
                const auto oldMap=FitShadowSamplingDensity(nominal,wanted,oldSize);
                const auto newMap=FitShadowSamplingDensity(box.projection,wanted,newSize);
                if(oldSize!=newSize)throw std::runtime_error("translated bridge increased its allocation bytes");
                for(unsigned element=0;element<16;++element)if(element!=12 && element!=13 && oldMap[element]!=newMap[element])
                    throw std::runtime_error("translated bridge changed fitted density or shared shadow depth");
                ++resourceChecks;
            }
            if(box.farDepth>bridge.farDepth)++extended;
            if(box.farDepth>farClip*.999f)++clipLimited;
            largestGain=(std::max)(largestGain,double(box.farDepth)/bridge.farDepth);
        }
    }
    if(!valid || !extended || !clipLimited || largestGain<1.25)
        throw std::runtime_error("translated bridge tests did not exercise extension and finite camera clipping");
    for(float bad:{NAN,INFINITY})
        if(FitCameraShadowBox(validView.data(),validLens.data(),validNominal,bad).valid)
            throw std::runtime_error("translated bridge accepted invalid requested depth");
    const auto beforeClip=FitCameraShadowBox(validView.data(),validLens.data(),validNominal,-1);
    if(!beforeClip.valid || std::abs(beforeClip.nearDepth-1)>1e-4f)
        throw std::runtime_error("translated bridge did not clamp an early start to the real near clip plane");
    for(unsigned element:{0u,5u,10u,15u}) {
        auto broken=validNominal;broken[element]=NAN;
        if(FitCameraShadowBox(validView.data(),validLens.data(),broken,validStart).valid)
            throw std::runtime_error("translated bridge accepted a nonfinite nominal projection");
    }
    auto perspectiveBox=validNominal;perspectiveBox[3]=.01f;
    if(FitCameraShadowBox(validView.data(),validLens.data(),perspectiveBox,validStart).valid)
        throw std::runtime_error("translated bridge accepted a projective rather than affine light box");
    auto tinyBox=validNominal;
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<2;++col)tinyBox[row*4+col]*=1000000;
    if(FitCameraShadowBox(validView.data(),validLens.data(),tinyBox,validStart).valid)
        throw std::runtime_error("translated bridge accepted a start face larger than its unchanged box");
    std::printf("PASS translated bridge box: valid=%u fallback=%u extended=%u clip_limited=%u corners=%u unchanged_allocation_checks=%u maximum_depth_gain=%.4f\n",
        valid,invalid,extended,clipLimited,cornerChecks,resourceChecks,largestGain);
}

void CheckProjection() {
    CheckComparisonCalibration();
    CheckNativeSamplingDensity();
    CheckLayerSamplingDensity();
    CheckCloseShadowSamplingBudget();
    CheckFarShadowDensityCurve();
    CheckScreenShadowTransitions();
    CheckSamplingFootprint();
    CheckShadowTransitions();
    CheckExpandedBridgeSampling();
    CheckTranslatedBridgeBox();
    CheckQualityOrdering();
    CheckShadowRangeBox();
    CheckDynamicPrecision();
    CheckCameraPictureShadowRegion();
    CheckUnifiedVisibleShadowBox();
    CheckDetailGrid();
    CheckVertexBounds();
    CheckFixedModelProjection();
    CheckReferenceFit();
    CheckFocusedStageCoverage();
    CheckVisibleDepthCoverage();
    CheckCameraStableShadowDepth();
    {
        using namespace mikumikustudio;
        NativeShadowBounds stage,receivers;
        const float low[3]{-1000,-1000,-1000},high[3]{1000,1000,1000};
        const float nearLow[3]{-20,0,-10},nearHigh[3]{5,10,25};
        stage.Include(low);stage.Include(high);receivers.Include(nearLow);receivers.Include(nearHigh);
        const float focus[3]{-14,5,-1},light[3]{-.5f,-1,.5f};
        unsigned restored=0;
        for(int mode:{1,2})for(float range:{.01125f,.1f}) {
            const auto reference=NativeShadowProjection(focus,light,range,mode);
            const auto before=FitFocusedShadowProjection(reference,stage,focus,light,range,mode,3.f);
            const auto after=FitFocusedShadowProjection(reference,stage,focus,light,range,mode,3.f,&receivers);
            for(unsigned i=0;i<8;++i) {
                const auto point=receivers.Corner(i);
                const auto outside=[&](const std::array<float,16>& matrix) {
                    double p[4]{};
                    for(int a=0;a<4;++a)p[a]=matrix[12+a]+point[0]*matrix[a]+point[1]*matrix[4+a]+point[2]*matrix[8+a];
                    return std::abs(p[0]/p[3])>=1 || std::abs(p[1]/p[3])>=1 || p[2]/p[3]<=0 || p[2]/p[3]>=1;
                };
                restored+=outside(before);
                if(outside(after))throw std::runtime_error("close-up visible receiver clipped at shadow-map edge");
            }
        }
        if(!restored)throw std::runtime_error("close-up fixture did not reproduce the former outer-edge clipping");
        std::printf("PASS close-up visible receivers: restored %u clipped corners across both modes\n",restored);
    }
    using namespace mikumikustudio;
    if (ShadowSamplingFactor(.1f) != 4.f || ShadowSamplingFactor(.05f) != 1.f ||
        ShadowSamplingFactor(.00001f) != 1.f)
        throw std::runtime_error("shared finest range lost its 4x sampling demand");
    for (unsigned base : {2048u, 4096u}) {
        if (FittedShadowResolutionLimit(base, .2f, .2f, 16384) != base ||
            FittedShadowResolutionLimit(base, .2f, .1f, 16384) != base * 2 ||
            FittedShadowResolutionLimit(base, .2f, .001f, 16384) != 8192 ||
            FittedShadowResolutionLimit(base, .2f, .001f, 2048) != 2048)
            throw std::runtime_error("fitted coverage density compensation or hardware limit failed");
    }
    std::puts("PASS finest sampling demand, coverage density compensation and hardware limits");
    const float focus[3] = {2, 10, -3};
    for (const auto& direction : std::vector<std::array<float, 3>>{
             {-.5f, -1, .5f}, {0, -1, 0}, {0, 0, 1}, {1, 0, 0}, {0, 0, 0}}) {
        for (int mode : {1, 2}) {
            const auto matrix = mikumikustudio::NativeShadowProjection(focus, direction.data(), .01784f, mode);
            const auto project = [&](const std::array<float, 3>& point) {
                std::array<float, 3> result{};
                for (int axis = 0; axis < 3; ++axis)
                    result[axis] = matrix[12+axis] + point[0]*matrix[axis] +
                        point[1]*matrix[4+axis] + point[2]*matrix[8+axis];
                return result;
            };
            // The centre and all sides of a small object must be covered,
            // including lights parallel to either candidate up direction.
            for (int axis = 0; axis < 3; ++axis) for (float offset : {-4.f, 0.f, 4.f}) {
                std::array<float, 3> point{focus[0], focus[1], focus[2]}; point[axis] += offset;
                const auto p = project(point);
                if (!std::isfinite(p[0]+p[1]+p[2]) || std::abs(p[0]) >= 1 || std::abs(p[1]) >= 1 || p[2] <= 0 || p[2] >= 1)
                    throw std::runtime_error("shadow projection clips the focused object");
            }
            if (direction[0] == 0 && direction[1] == 0 && direction[2] == 0) continue;
            const auto front = project({focus[0], focus[1], focus[2]});
            const auto back = project({focus[0]+direction[0], focus[1]+direction[1], focus[2]+direction[2]});
            if (std::abs(front[0]-back[0]) > 1e-6f || std::abs(front[1]-back[1]) > 1e-6f || front[2] >= back[2])
                throw std::runtime_error("occluder and receiver disagree along a light ray");
        }
    }
    std::puts("PASS focused coverage and directional occluder/receiver projection");
    mikumikustudio::NativeShadowBounds bounds;
    const float lower[3]{-20, -5, -130}, upper[3]{35, 48, 160};
    bounds.Include(lower); bounds.Include(upper);
    const float panned[3]{300, -120, 90};
    unsigned covered = 0;
    for (const auto& direction : std::vector<std::array<float, 3>>{
             {-.5f, -1, .5f}, {0, -1, 0}, {0, 0, 1}, {1, 0, 0}, {0, 0, 0}}) {
        for (int mode : {1, 2}) for (float range : {.1f, .01125f, .00001f}) {
            const auto projection = mikumikustudio::NativeShadowProjection(focus, direction.data(), range, mode, &bounds);
            if (projection != mikumikustudio::NativeShadowProjection(panned, direction.data(), range, mode, &bounds))
                throw std::runtime_error("camera pan moved fitted shadow coverage");
            for (unsigned corner = 0; corner < 8; ++corner) {
                const auto point = bounds.Corner(corner);
                float p[3]{};
                for (int axis = 0; axis < 3; ++axis)
                    p[axis] = projection[12+axis] + point[0]*projection[axis] +
                        point[1]*projection[4+axis] + point[2]*projection[8+axis];
                if (!std::isfinite(p[0]+p[1]+p[2]) || std::abs(p[0]) >= .99f || std::abs(p[1]) >= .99f || p[2] <= .01f || p[2] >= .99f)
                    throw std::runtime_error("fitted shadow projection clips geometry or filter margin");
                ++covered;
            }
        }
    }
    std::printf("PASS %u fitted coverage corners, both modes, depth, extreme precision and camera pan\n", covered);
}
void Check(HRESULT hr,const char* operation) {
    if(FAILED(hr)){std::printf("%s: 0x%08lx\n",operation,static_cast<unsigned long>(hr));throw std::runtime_error(operation);}
}
struct Vertex {float p[3],n[3],uv[2];};
struct CoverageProbe {
    float offset[3];
    bool specular = false;
};
struct AccessoryProbe {
    const char* technique;
    bool transformed = false;
    bool doubleWorld = false;
};
struct BridgeProbe {
    bool enabled = true, middleEnabled = false, nearEnabled = false, outside = false;
    float depth[3]{.4995f,1.f,.2f}; // Middle, nearest, bridge: distinct constant maps.
    float blend[3]{1,1,1}; // Inputs to the shader's smoothstep, before rim coverage.
    float rank[3]{1,2,0};
    float coverageDepth = 1;
    float logBlend[3]{0,0,0};
    unsigned mapSize[3]{1,1,1};
    int texturePattern[3]{0,0,0}; // 0 constant, 1 diagonal, 2 strip, 3 affine plane, 4 corner.
    int coveragePattern = 0;
    float casterGap = .0005f;
    float stripeHalfWidth = .043f;
    bool layerFilter = false, receiverFilter = false, prefilterCoverage = false;
    bool productionPrefilterCoverage = false;
    bool prefilterLayer[3]{false,false,false};
    float coverageHalfWidth = 1, halfWidth[3]{1,1,1}, planeSlope = 0;
    float backgroundDepth=1,planeSlopeY=0,planeZOffset=0;
    float normalBend=0;
    bool reverseDiagonal=false,degenerateUV=false,poisonSharedCoverage=false;
    bool directional=false; // Exact historical-oracle fixtures opt out explicitly.
    bool progressive=false; // Old transition oracles retain the released path.
    const std::vector<float>* coverageValues=nullptr;
    const std::vector<float>* layerValues[3]{nullptr,nullptr,nullptr};
    bool overrideMapping[3]{false,false,false};
    std::array<float,16> layerMapping[3]{};
    float edgeSlope=.35f;
};

float IndependentFilterSideWeight() {
    // Integrate the fixed original-4096 cubic kernel independently. No
    // production constants or paired-hardware reconstruction helper is used.
    const double scale=.75*1.240967365;
    const double x=1/scale,t=2-x;
    const double side=t*t*t/6;
    return float(side/(2*side+2.0/3));
}

float ProbeDepth(unsigned x,unsigned y,unsigned size,float halfWidth,int pattern,float gap,float slope,float stripeHalfWidth,
                 float background=1,float slopeY=0,float edgeSlope=.35f) {
    const float worldX=(2.f*x/size-1.f)*halfWidth,worldY=(1.f-2.f*y/size)*halfWidth;
    const float plane=.5f+slope*worldX+slopeY*worldY;
    if(pattern==3)return plane;
    if(pattern==5)return plane-gap*(.5f+.5f*std::sin(5.f*worldX+3.f*worldY));
    const float edge=worldX+edgeSlope*worldY-.083f;
    const bool occluded=pattern==4?(worldX<.083f && worldY<-.071f):
        pattern==1?edge<0:std::abs(edge)<stripeHalfWidth;
    return occluded?plane-gap:background;
}

void IndependentPrefilter(const D3DLOCKED_RECT& pixels,unsigned size) {
    std::vector<float> input(size*size),filtered(size*size);
    for(unsigned y=0;y<size;++y)
        std::copy_n(reinterpret_cast<const float*>(static_cast<const char*>(pixels.pBits)+y*pixels.Pitch),
            size,input.data()+y*size);
    const double side=IndependentFilterSideWeight(),weights[]{side,1-2*side,side};
    // Independent double-precision nine-tap convolution with the same CLAMP
    // edge policy.
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
        double sum=0;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
            const unsigned sx=unsigned((std::clamp)(int(x)+dx,0,int(size)-1));
            const unsigned sy=unsigned((std::clamp)(int(y)+dy,0,int(size)-1));
            sum+=weights[dx+1]*weights[dy+1]*input[sy*size+sx];
        }
        filtered[y*size+x]=float(sum);
    }
    for(unsigned y=0;y<size;++y)
        std::copy_n(filtered.data()+y*size,size,
            reinterpret_cast<float*>(static_cast<char*>(pixels.pBits)+y*pixels.Pitch));
}

struct Effect {
    void* value=nullptr;
    Effect()=default;
    Effect(const Effect&)=delete;
    Effect(Effect&& other)noexcept:value(other.value){other.value=nullptr;}
    ~Effect(){if(value)static_cast<IUnknown*>(value)->Release();}
};
Effect Load(IDirect3DDevice9* device,const std::filesystem::path& file) {
    std::ifstream stream(file,std::ios::binary);
    if(!stream)throw std::runtime_error("Cannot read production shader");
    const std::string source{std::istreambuf_iterator<char>(stream),{}};
    static HMODULE module=LoadLibraryW(L"d3dx9_43.dll");
    using Create=HRESULT(WINAPI*)(IDirect3DDevice9*,const void*,UINT,const void*,const void*,DWORD,void*,void**,void**);
    const auto create=reinterpret_cast<Create>(GetProcAddress(module,"D3DXCreateEffect"));
    if(!create)throw std::runtime_error("D3DXCreateEffect unavailable");
    Effect fx;void* errors=nullptr;
    const auto hr=create(device,source.data(),UINT(source.size()),nullptr,nullptr,0,nullptr,&fx.value,&errors);
    if(errors){using Data=void*(WINAPI*)(void*);std::printf("%s\n",static_cast<const char*>(EffectMethod<Data,3>(errors)(errors)));static_cast<IUnknown*>(errors)->Release();}
    Check(hr,"Compile production shader");return fx;
}
std::vector<DWORD> IndependentVisibilityImage(unsigned size,int mode,float phase,const BridgeProbe& probe,float slope=0) {
    // Independently convolve per-texel native comparisons with the 3x3 kernel,
    // then bilinearly reconstruct those values (36 scalar contributions). The
    // shader combines them into 16 taps; no averaged-depth oracle is used here.
    const double a=IndependentFilterSideWeight(),weights[]{a,1-2*a,a};
    const double half=probe.coverageHalfWidth;
    std::vector<DWORD> result(kSize*kSize);
    for(unsigned y=0;y<kSize;++y)for(unsigned x=0;x<kSize;++x) {
        const double worldX=2.0*x/kSize-1,worldY=1-2.0*y/kSize;
        const double sampleX=worldX+2*double(phase)*half/size;
        const double gx=(sampleX/half+1)*size*.5,gy=(1-worldY/half)*size*.5;
        const int ix=int(std::floor(gx)),iy=int(std::floor(gy));
        const double fx=gx-ix,fy=gy-iy;
        const double receiver=.5+double(slope)*worldX+double(probe.planeSlopeY)*worldY;
        const double strength=mode==2?8000.0*y/kSize:1500.0;
        double visibility=0;
        for(int by=0;by<2;++by)for(int bx=0;bx<2;++bx)
            for(int ky=-1;ky<=1;++ky)for(int kx=-1;kx<=1;++kx) {
                const int tx=(std::clamp)(ix+bx+kx,0,int(size)-1),ty=(std::clamp)(iy+by+ky,0,int(size)-1);
                const double px=(2.0*tx/size-1)*half,py=(1-2.0*ty/size)*half;
                const double edge=px+double(probe.edgeSlope)*py-.083;
                const bool occluded=probe.coveragePattern==4?(px<.083 && py<-.071):
                    probe.coveragePattern==1?edge<0:std::abs(edge)<probe.stripeHalfWidth;
                double depth=probe.coveragePattern==3 || occluded?
                    .5+double(probe.planeSlope)*px+double(probe.planeSlopeY)*py-
                        (probe.coveragePattern==3?0:probe.casterGap):probe.backgroundDepth;
                depth-=double(slope)*(px-sampleX)+double(probe.planeSlopeY)*(py-worldY);
                const double compared=1-(std::clamp)((std::max)(receiver-depth,0.0)*strength-.3,0.0,1.0);
                visibility+=(bx?fx:1-fx)*(by?fy:1-fy)*weights[kx+1]*weights[ky+1]*compared;
            }
        const DWORD color=DWORD(std::lround((std::clamp)(.2+.8*visibility,0.0,1.0)*255));
        result[y*kSize+x]=0xff000000u|color*0x10101u;
    }
    return result;
}

void CheckAllNativeTechniquesGpu(IDirect3DDevice9* device,void* effect) {
    D3DCAPS9 caps{};Check(device->GetDeviceCaps(&caps),"Read native shader device limits");
    std::printf("NATIVE_SHADER_CAPS ps_version=0x%08lX instruction_slots=%lu executed_instructions=%lu temporaries=%u\n",
        static_cast<unsigned long>(caps.PixelShaderVersion),
        static_cast<unsigned long>(caps.MaxPixelShader30InstructionSlots),
        static_cast<unsigned long>(caps.MaxPShaderInstructionsExecuted),caps.PS20Caps.NumTemps);
    struct EffectDescription {const char* creator;UINT parameters,techniques,functions;};
    struct TechniqueDescription {const char* name;UINT passes,annotations;};
    using GetDescription=HRESULT(WINAPI*)(void*,EffectDescription*);
    using GetTechnique=const char*(WINAPI*)(void*,UINT);
    using GetTechniqueDescription=HRESULT(WINAPI*)(void*,const char*,TechniqueDescription*);
    using ValidateTechnique=HRESULT(WINAPI*)(void*,const char*);
    EffectDescription description{};
    Check(EffectMethod<GetDescription,3>(effect)(effect,&description),"Read native effect techniques");
    if(!description.techniques)throw std::runtime_error("native effect has no executable techniques");
    unsigned failed=0;
    for(UINT index=0;index<description.techniques;++index) {
        const char* handle=EffectMethod<GetTechnique,12>(effect)(effect,index);
        if(!handle)throw std::runtime_error("native effect technique enumeration returned null");
        TechniqueDescription technique{};
        Check(EffectMethod<GetTechniqueDescription,5>(effect)(effect,handle,&technique),"Read native technique description");
        const HRESULT result=EffectMethod<ValidateTechnique,60>(effect)(effect,handle);
        std::printf("NATIVE_TECHNIQUE name=%s passes=%u validation=0x%08lX\n",
            technique.name?technique.name:"(unnamed)",technique.passes,static_cast<unsigned long>(result));
        failed+=FAILED(result);
        if (SUCCEEDED(result)) {
            Check(FxSetTechnique(effect,handle),"Select native technique for binding");
            Check(device->SetPixelShader(nullptr),"Clear pixel shader before technique binding");
            Check(device->BeginScene(),"Begin native technique binding check");
            UINT passes=0;
            const HRESULT began=FxBegin(effect,&passes);
            unsigned missing=0;
            if (SUCCEEDED(began)) {
                for (UINT pass=0;pass<passes;++pass) {
                    const HRESULT passResult=FxBeginPass(effect,pass);
                    IDirect3DPixelShader9* pixel=nullptr;
                    if (SUCCEEDED(passResult)) {
                        Check(device->GetPixelShader(&pixel),"Read bound pixel shader");
                        if (!pixel) ++missing;
                        if (pixel) pixel->Release();
                        Check(FxEndPass(effect),"End native technique binding pass");
                    } else ++missing;
                }
                Check(FxEnd(effect),"End native technique binding");
            } else ++missing;
            Check(device->EndScene(),"End native technique binding check");
            std::printf("NATIVE_TECHNIQUE_BIND name=%s missing_pixel_shader=%u\n",
                technique.name?technique.name:"(unnamed)",missing);
            failed+=missing;
        }
    }
    if(failed)throw std::runtime_error("native effect includes techniques that exceed the actual device capabilities");
}

std::vector<DWORD> Draw(IDirect3DDevice9* device,void* effect,unsigned mapSize,int mode,float slope,bool split,bool legacy,float depthGap=0,int pattern=0,float phase=0,const CoverageProbe* coverage=nullptr,float facing=1,bool fitted=false,int detail=0,bool stable=false,int nearLayer=0,const AccessoryProbe* accessory=nullptr,const BridgeProbe* bridgeLayer=nullptr,const std::array<float,16>* comparison=nullptr,const std::array<float,16>* coverageProjection=nullptr) {
    ComPtr<IDirect3DTexture9> shadow;
    // This fixture owns one D3D9 device for the process. Reuse the production
    // prefilter target so a frame sweep does not recompile its shader per draw.
    static mikumikustudio::shadow_filter::DepthPrefilter filteredCoverage;
    Check(device->CreateTexture(mapSize,mapSize,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,&shadow,nullptr),"Create shadow texture");
    D3DLOCKED_RECT pixels{};Check(shadow->LockRect(0,&pixels,nullptr,0),"Lock shadow texture");
    for(unsigned y=0;y<mapSize;++y)for(unsigned x=0;x<mapSize;++x){
        auto* row=reinterpret_cast<float*>(static_cast<char*>(pixels.pBits)+y*pixels.Pitch);
        row[x]=split?(x<mapSize/2?.2f:.8f):.5f+slope*(2.f*x/mapSize-1.f)-depthGap;
        if(pattern==1)row[x]=float(x)<mapSize*.5f+(float(y)-mapSize*.5f)*.35f?.2f:.8f;
        if(pattern==2)row[x]=(x==mapSize/2)?.2f:.8f;
        if(fitted)row[x]=row[x]*.5f+.2f;
        if(coverage)row[x]=1; // No occluder: only material lighting may shade this receiver.
        // The physical-gap probe uses a plane perpendicular to its local
        // light axis, transformed through a real scene-fitted coverage map.
        if(coverageProjection)row[x]=(*coverageProjection)[14]+(.5f-depthGap)*(*coverageProjection)[10];
        if(bridgeLayer && bridgeLayer->coveragePattern)
            row[x]=ProbeDepth(x,y,mapSize,bridgeLayer->coverageHalfWidth,bridgeLayer->coveragePattern,
                bridgeLayer->casterGap,bridgeLayer->planeSlope,bridgeLayer->stripeHalfWidth,
                bridgeLayer->backgroundDepth,bridgeLayer->planeSlopeY,bridgeLayer->edgeSlope);
        if(bridgeLayer && bridgeLayer->coverageValues) {
            if(bridgeLayer->coverageValues->size()!=size_t(mapSize)*mapSize)
                throw std::runtime_error("independent coverage fixture size mismatch");
            row[x]=(*bridgeLayer->coverageValues)[size_t(y)*mapSize+x];
        }
        if(bridgeLayer)row[x]+=bridgeLayer->planeZOffset;
    }
    if(bridgeLayer && bridgeLayer->prefilterCoverage)IndependentPrefilter(pixels,mapSize);
    Check(shadow->UnlockRect(0),"Unlock shadow texture");
    // Production keeps the compatible prefiltered coverage on stage zero;
    // enhanced native receivers fetch its untouched depth from stage six.
    const bool nativeEnhanced=bridgeLayer && bridgeLayer->receiverFilter && bridgeLayer->layerFilter;
    const bool sharedPrefilter=bridgeLayer && (bridgeLayer->productionPrefilterCoverage || nativeEnhanced);
    if(sharedPrefilter) {
        Check(device->BeginScene(),"Begin production depth prefilter");
        const HRESULT filtered=filteredCoverage.Apply(device,shadow.Get(),mapSize);
        const HRESULT ended=device->EndScene();
        Check(filtered,"Apply production depth prefilter");Check(ended,"End production depth prefilter");
    }
    D3DXMATRIXF identity{};for(unsigned i=0;i<4;++i)identity.m[i][i]=1;
    Check(FxSetMatrix(effect,"matWorldViewProj",&identity),"Set camera projection");
    auto lightProjection=identity;lightProjection.m[3][0]=2*phase/mapSize;
    auto toNative=identity;
    if(bridgeLayer) {
        lightProjection.m[0][0]=lightProjection.m[1][1]=1.f/bridgeLayer->coverageHalfWidth;
        toNative.m[0][0]=toNative.m[1][1]=bridgeLayer->coverageHalfWidth;
        if(bridgeLayer->degenerateUV)lightProjection.m[0][0]=0;
    }
    if(fitted) {
        lightProjection.m[1][1]=.75f;lightProjection.m[3][1]=.1f;
        lightProjection.m[2][2]=.5f;lightProjection.m[3][2]=.2f;
        toNative.m[1][1]=1.f/.75f;toNative.m[3][1]=-.1f/.75f;
        toNative.m[2][2]=2;toNative.m[3][2]=-.4f;
    }
    if(coverageProjection)std::memcpy(&lightProjection,coverageProjection->data(),sizeof(lightProjection));
    if(comparison)std::memcpy(&toNative,comparison->data(),sizeof(toNative));
    FxSetMatrix(effect,"MdsShadowToNative",&toNative);
    FxSetInt(effect,"MdsStableShadowGrid",stable);
    const float derivativeOffset=bridgeLayer && bridgeLayer->receiverFilter ? .5f+bridgeLayer->planeZOffset : 0.f;
    FxSetFloatArray(effect,"MdsShadowDerivativeOffset",&derivativeOffset,1);
    FxSetInt(effect,"MdsLayerFilterEnhanced",bridgeLayer && bridgeLayer->layerFilter);
    FxSetInt(effect,"MdsShadowFilterEnhanced",bridgeLayer && bridgeLayer->receiverFilter);
    FxSetInt(effect,"MdsDirectionalShadowFilter",bridgeLayer && bridgeLayer->directional);
    FxSetInt(effect,"MdsProgressiveBridge",bridgeLayer && bridgeLayer->progressive);
    const float filterSide=IndependentFilterSideWeight();
    FxSetFloatArray(effect,"MdsShadowFilterSideWeight",&filterSide,1);
    const float dimensions[4]{float(mapSize),float(mapSize),0,0},texel[4]{1.f/mapSize,1.f/mapSize,0,0};
    FxSetFloatArray(effect,"MdsShadowMapSize",dimensions,4);
    FxSetFloatArray(effect,"MdsDetailTexelSize",texel,4);
    FxSetInt(effect,"MdsDetailEnabled",detail!=0&&detail!=2);
    FxSetMatrix(effect,"MdsCoverageToDetail",&identity);
    const float detailDepthBlend[4]{0,0,0,detail==3?.5f:detail==4?0.f:1.f};
    FxSetFloatArray(effect,"MdsDetailDepthBlend",detailDepthBlend,4);
    FxSetInt(effect,"MdsNearEnabled",nearLayer!=0);
    FxSetInt(effect,"MdsPreferMiddle",nearLayer==7);
    auto toNear=identity;
    if(nearLayer==4)toNear.m[3][0]=3; // Outside the closest map: keep middle coverage.
    FxSetMatrix(effect,"MdsCoverageToNear",&toNear);
    FxSetFloatArray(effect,"MdsNearTexelSize",texel,4);
    const float nearDepthBlend[4]{0,0,0,nearLayer==3?0.f:nearLayer==5?.5f:1.f};
    FxSetFloatArray(effect,"MdsNearDepthBlend",nearDepthBlend,4);
    FxSetInt(effect,"MdsBridgeEnabled",bridgeLayer && bridgeLayer->enabled);
    auto toBridge=identity;
    if(bridgeLayer && bridgeLayer->outside)toBridge.m[3][0]=3;
    FxSetMatrix(effect,"MdsCoverageToBridge",&toBridge);
    FxSetFloatArray(effect,"MdsBridgeTexelSize",texel,4);
    const float defaultRank[3]{1,2,0},defaultBridgeBlend[4]{0,0,0,1};
    const float linearBlend[3]{0,0,0};
    FxSetFloatArray(effect,"MdsDetailRank",bridgeLayer?bridgeLayer->rank:defaultRank,3);
    // Effects are reused between frames. A default draw must restore linear
    // blending after a logarithmic transition probe, including legacy shaders.
    FxSetFloatArray(effect,"MdsDetailLogBlend",bridgeLayer?bridgeLayer->logBlend:linearBlend,3);
    FxSetFloatArray(effect,"MdsBridgeDepthBlend",defaultBridgeBlend,4);
    if(bridgeLayer) {
        Check(FxSetInt(effect,"MdsDetailEnabled",bridgeLayer->middleEnabled),"Enable constant middle probe");
        Check(FxSetInt(effect,"MdsNearEnabled",bridgeLayer->nearEnabled),"Enable constant nearest probe");
        Check(FxSetInt(effect,"MdsBridgeEnabled",bridgeLayer->enabled),"Enable constant bridge probe");
        Check(FxSetFloatArray(effect,"MdsDetailRank",bridgeLayer->rank,3),"Set three-layer priorities");
        const char* blendNames[]{"MdsDetailDepthBlend","MdsNearDepthBlend","MdsBridgeDepthBlend"};
        const char* matrixNames[]{"MdsCoverageToDetail","MdsCoverageToNear","MdsCoverageToBridge"};
        for(unsigned layer=0;layer<3;++layer) {
            const float blend[4]{0,0,0,bridgeLayer->blend[layer]};
            Check(FxSetFloatArray(effect,blendNames[layer],blend,4),"Set constant layer blend");
            auto mapping=identity;
            mapping.m[0][0]=mapping.m[1][1]=bridgeLayer->coverageHalfWidth/bridgeLayer->halfWidth[layer];
            if(layer==2 && bridgeLayer->outside)mapping.m[3][0]=3;
            if(bridgeLayer->overrideMapping[layer])
                std::memcpy(&mapping,bridgeLayer->layerMapping[layer].data(),sizeof(mapping));
            Check(FxSetMatrix(effect,matrixNames[layer],&mapping),"Set independent padded layer footprint");
        }
    }
    if(coverage)for(unsigned i=0;i<3;++i)lightProjection.m[3][i]+=coverage->offset[i];
    Check(FxSetMatrix(effect,"matLightViewProj",&lightProjection),"Set light projection");
    const float white[4]={1,1,1,.37f},dark[4]={.2f,.2f,.2f,1},light[4]={std::sqrt(1-facing*facing),0,-facing,1},zero[4]={0,0,0,1},eye[4]={0,0,4,1};
    Check(FxSetFloatArray(effect,"EgColor",white,4),"Set material");
    Check(FxSetFloatArray(effect,"ToonColor",dark,4),"Set toon");
    Check(FxSetFloatArray(effect,"LightDir",light,4),"Set light");
    Check(FxSetFloatArray(effect,"SpcColor",zero,4),"Set specular");
    Check(FxSetFloatArray(effect,"Place",eye,4),"Set eye");
    if(coverage){
        const float backlight[4]={0,.9797959f,.2f,1},gloss[4]={.3f,.3f,.3f,1};
        Check(FxSetFloatArray(effect,"LightDir",backlight,4),"Set backlit material");
        if(coverage->specular)Check(FxSetFloatArray(effect,"SpcColor",gloss,4),"Set authored specular");
    }
    Check(FxSetInt(effect,"parthf",mode==2),"Set shadow mode");
    Check(FxSetInt(effect,"transp",0),"Set material opacity");
    Check(FxSetTechnique(effect,accessory?accessory->technique:"BufferShadowTec"),"Select native receiver");
    Vertex vertices[]={{{-1,1,.5f-slope},{0,0,1},{0,0}},{{1,1,.5f+slope},{0,0,1},{1,0}},
                             {{-1,-1,.5f-slope},{0,0,1},{0,1}},{{1,-1,.5f+slope},{0,0,1},{1,1}}};
    if(bridgeLayer) {
        for(auto& vertex:vertices)vertex.p[2]+=bridgeLayer->planeSlopeY*vertex.p[1]+bridgeLayer->planeZOffset;
        for(auto& vertex:vertices) {
            vertex.n[0]=bridgeLayer->normalBend*vertex.p[0];
            vertex.n[1]=bridgeLayer->normalBend*vertex.p[1];
        }
        if(bridgeLayer->reverseDiagonal){std::swap(vertices[0],vertices[1]);std::swap(vertices[2],vertices[3]);}
    }
    ComPtr<IDirect3DTexture9> accessoryTexture;
    if(accessory) {
        const float diffuse[4]{1,1,1,1}; // Saturated lighting isolates shadow-coordinate changes.
        Check(FxSetFloatArray(effect,"EgColor",dark,4),"Set accessory ambient");
        Check(FxSetFloatArray(effect,"DifColor",diffuse,4),"Set accessory diffuse");
        FxSetInt(effect,"spadd",0);
        FxSetMatrix(effect,"matWRotate",&identity);
        auto world=identity,sharedCoverage=identity,coverageInverse=identity;
        if(accessory->transformed) {
            world.m[0][0]=2;world.m[1][1]=3;world.m[2][2]=1.5f;
            world.m[3][0]=.3f;world.m[3][1]=-.2f;world.m[3][2]=.15f;
            // Store local vertices, leaving their final world-space picture
            // identical to the untransformed reference.
            for(auto& vertex:vertices)for(int axis=0;axis<3;++axis)
                vertex.p[axis]=(vertex.p[axis]-world.m[3][axis])/world.m[axis][axis];
            sharedCoverage.m[0][0]=.5f;sharedCoverage.m[1][1]=.75f;
            sharedCoverage.m[3][0]=-.1f;sharedCoverage.m[3][1]=.05f;
            for(int axis=0;axis<2;++axis) {
                coverageInverse.m[axis][axis]=1.f/sharedCoverage.m[axis][axis];
                coverageInverse.m[3][axis]=-sharedCoverage.m[3][axis]/sharedCoverage.m[axis][axis];
            }
        }
        const auto multiply=[](const D3DXMATRIXF& a,const D3DXMATRIXF& b) {
            D3DXMATRIXF result{};
            for(int row=0;row<4;++row)for(int column=0;column<4;++column)
                for(int k=0;k<4;++k)result.m[row][column]+=a.m[row][k]*b.m[k][column];
            return result;
        };
        const auto placedCoverage=multiply(world,sharedCoverage);
        // local * world * coverage * inverse(coverage) * detail, with
        // detail=identity here. A negative control applies world twice.
        const auto coverageToNear=accessory->doubleWorld?multiply(coverageInverse,world):coverageInverse;
        Check(FxSetMatrix(effect,"matWorld",&world),"Set accessory world placement");
        Check(FxSetMatrix(effect,"matWorldViewProj",&world),"Set accessory camera placement");
        Check(FxSetMatrix(effect,"matLightViewProj",&placedCoverage),"Set accessory coverage placement");
        Check(FxSetMatrix(effect,"MdsCoverageToNear",&coverageToNear),"Set shared accessory detail mapping");
        Check(device->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&accessoryTexture,nullptr),"Create accessory albedo");
        D3DLOCKED_RECT whitePixel{};Check(accessoryTexture->LockRect(0,&whitePixel,nullptr,0),"Lock accessory albedo");
        *static_cast<DWORD*>(whitePixel.pBits)=0xffffffff;
        Check(accessoryTexture->UnlockRect(0),"Unlock accessory albedo");
    }
    Check(device->SetTexture(1,accessoryTexture.Get()),"Bind accessory albedo");
    Check(device->SetTexture(2,accessoryTexture.Get()),"Bind accessory sphere map");
    Check(device->SetTexture(0,sharedPrefilter?
        filteredCoverage.Texture():shadow.Get()),"Bind shadow map");
    IDirect3DTexture9* nativeCoverage=shadow.Get();
    ComPtr<IDirect3DTexture9> empty;
    if((detail || nearLayer || bridgeLayer) &&
        !(bridgeLayer && (bridgeLayer->coveragePattern || bridgeLayer->coverageValues))) {
        Check(device->CreateTexture(1,1,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,&empty,nullptr),"Create empty coverage map");
        D3DLOCKED_RECT blank{};Check(empty->LockRect(0,&blank,nullptr,0),"Lock empty coverage");
        *static_cast<float*>(blank.pBits)=bridgeLayer?bridgeLayer->coverageDepth:1;
        Check(empty->UnlockRect(0),"Unlock empty coverage");
        Check(device->SetTexture(0,empty.Get()),"Bind empty coverage");
        nativeCoverage=empty.Get();
    }
    Check(device->SetTexture(6,nativeCoverage),"Bind isolated raw native coverage");
    ComPtr<IDirect3DTexture9> poisonedSharedCoverage;
    if(bridgeLayer && bridgeLayer->poisonSharedCoverage) {
        Check(device->CreateTexture(1,1,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,&poisonedSharedCoverage,nullptr),"Create incompatible stage-zero control");
        D3DLOCKED_RECT control{};Check(poisonedSharedCoverage->LockRect(0,&control,nullptr,0),"Lock stage-zero control");
        *static_cast<float*>(control.pBits)=0;
        Check(poisonedSharedCoverage->UnlockRect(0),"Unlock stage-zero control");
        Check(device->SetTexture(0,poisonedSharedCoverage.Get()),"Bind stage-zero isolation control");
    }
    Check(device->SetTexture(3,detail?(nearLayer==1?empty.Get():shadow.Get()):nullptr),"Bind detail shadow map");
    Check(device->SetTexture(4,nearLayer?(nearLayer==1||nearLayer==6?shadow.Get():empty.Get()):nullptr),"Bind nearest shadow map");
    Check(device->SetTexture(5,nullptr),"Unbind unused bridge sampler");
    ComPtr<IDirect3DTexture9> constantLayer[3];
    if(bridgeLayer)for(unsigned layer=0;layer<3;++layer) {
        const unsigned size=bridgeLayer->mapSize[layer];
        Check(device->CreateTexture(size,size,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,&constantLayer[layer],nullptr),"Create detail probe map");
        D3DLOCKED_RECT value{};Check(constantLayer[layer]->LockRect(0,&value,nullptr,0),"Lock constant detail map");
        for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
            auto* row=reinterpret_cast<float*>(static_cast<char*>(value.pBits)+y*value.Pitch);
            row[x]=bridgeLayer->depth[layer];
            if(bridgeLayer->texturePattern[layer]) {
                // Raster centres, shared world footprint and shared Z. Different
                // map sizes sample the same occluder, not different geometry.
                row[x]=ProbeDepth(x,y,size,bridgeLayer->halfWidth[layer],bridgeLayer->texturePattern[layer],
                    bridgeLayer->casterGap,bridgeLayer->planeSlope,bridgeLayer->stripeHalfWidth,
                    bridgeLayer->backgroundDepth,bridgeLayer->planeSlopeY,bridgeLayer->edgeSlope);
            }
            if(bridgeLayer->layerValues[layer]) {
                if(bridgeLayer->layerValues[layer]->size()!=size_t(size)*size)
                    throw std::runtime_error("independent layer fixture size mismatch");
                row[x]=(*bridgeLayer->layerValues[layer])[size_t(y)*size+x];
            }
        }
        if(bridgeLayer->prefilterLayer[layer])IndependentPrefilter(value,size);
        Check(constantLayer[layer]->UnlockRect(0),"Unlock constant detail map");
        if(bridgeLayer->texturePattern[layer] || bridgeLayer->layerValues[layer]) {
            const char* names[]{"MdsDetailTexelSize","MdsNearTexelSize","MdsBridgeTexelSize"};
            const float actualTexel[]{1.f/size,1.f/size,0,0};
            Check(FxSetFloatArray(effect,names[layer],actualTexel,4),"Set independent detail probe texel size");
        }
        const bool enabled=layer==0?bridgeLayer->middleEnabled:layer==1?bridgeLayer->nearEnabled:bridgeLayer->enabled;
        Check(device->SetTexture(3+layer,enabled?constantLayer[layer].Get():nullptr),"Bind constant detail map");
    }
    Check(device->SetRenderState(D3DRS_ZENABLE,FALSE),"Disable depth testing");
    Check(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Set culling");
    Check(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE),"Disable blending");
    Check(device->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1),"Set vertex format");
    Check(device->BeginScene(),"Begin scene");
    UINT passes=0;Check(FxBegin(effect,&passes),"Begin effect");
    for(UINT p=0;p<passes;++p){Check(FxBeginPass(effect,p),"Begin pass");
        Check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex)),"Draw receiver");
        Check(FxEndPass(effect),"End pass");}
    Check(FxEnd(effect),"End effect");Check(device->EndScene(),"End scene");
    ComPtr<IDirect3DSurface9> target,readback;Check(device->GetRenderTarget(0,&target),"Get render target");
    Check(device->CreateOffscreenPlainSurface(kSize,kSize,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr),"Create readback");
    Check(device->GetRenderTargetData(target.Get(),readback.Get()),"Read GPU pixels");
    Check(readback->LockRect(&pixels,nullptr,D3DLOCK_READONLY),"Lock readback");
    std::vector<DWORD> output(kSize*kSize);
    for(unsigned y=0;y<kSize;++y)std::copy_n(reinterpret_cast<const DWORD*>(static_cast<const char*>(pixels.pBits)+y*pixels.Pitch),kSize,output.data()+y*kSize);
    Check(readback->UnlockRect(),"Unlock readback");Check(device->SetTexture(0,nullptr),"Unbind map");
    for(DWORD stage=3;stage<=6;++stage)Check(device->SetTexture(stage,nullptr),"Unbind native shadow map");
    return output;
}
void CheckMagnifiedShadowPlaneGpu(IDirect3DDevice9* device,void* effect) {
    unsigned cases=0,worst=0;
    // A magnified, unoccluded tilted plane must remain fully lit. Quantized
    // hardware bilinear weights introduce false depth gaps along each texel.
    // This is an analytic oracle, independent of the reconstruction algorithm.
    for(int mode:{1,2})for(unsigned size:{8u,16u,32u})
      for(float extent:{16.f,64.f})for(float slope:{-.02f,.02f}) {
        BridgeProbe probe;probe.enabled=false;probe.coveragePattern=3;
        probe.coverageHalfWidth=extent;probe.planeSlope=slope;probe.planeSlopeY=.013f;
        const auto pixels=Draw(device,effect,size,mode,slope,false,true,0,0,0,
            nullptr,1,false,0,true,0,nullptr,&probe);
        for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x)
            worst=(std::max)(worst,255u-unsigned((pixels[y*kSize+x]>>16)&255));
        ++cases;
    }
    std::printf("magnified coplanar shadow: cases=%u max_false_darkening=%u\n",cases,worst);
    if(worst>2)throw std::runtime_error("magnified shadow grid creates bands on an unoccluded plane");
    cases=worst=0;
    // Very coarse full-scene maps put neighbouring screen pixels much closer
    // than one float UV step. Adding the 0.5 texture offset before ddx/ddy
    // destroys gradient precision and invents occlusion during plane correction.
    for(int mode:{1,2})for(float extent:{9375.f,13571.f})
      for(float slope:{-.0005f,.0005f})for(bool directional:{false,true})for(float offset:{0.f,-.35f,.35f}) {
        BridgeProbe probe;probe.enabled=false;probe.coveragePattern=3;
        probe.coverageHalfWidth=extent;probe.planeSlope=slope;probe.planeSlopeY=.00031f;
        probe.layerFilter=probe.receiverFilter=true;probe.directional=directional;
        probe.planeZOffset=offset;
        const auto pixels=Draw(device,effect,512,mode,slope,false,true,0,0,0,
            nullptr,1,false,0,true,0,nullptr,&probe);
        for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x)
            worst=(std::max)(worst,255u-unsigned((pixels[y*kSize+x]>>16)&255));
        ++cases;
    }
    std::printf("enhanced magnified coplanar shadow: cases=%u max_false_darkening=%u\n",cases,worst);
    if(worst>2)throw std::runtime_error("enhanced plane correction creates false shadows at coarse precision");
    // At coarse footprints a curved normal field must reject long tangent
    // extrapolation, while the same flat field remains on the planar path.
    unsigned curvedWorst=0,negative=0;
    for(int mode:{1,2})for(float extent:{16.f,64.f})for(unsigned size:{8u,16u}) {
        BridgeProbe probe;probe.enabled=false;probe.coverageDepth=.5f;
        probe.coverageHalfWidth=extent;probe.planeSlope=.02f;probe.normalBend=.6f;
        const auto draw=[&] {return Draw(device,effect,size,mode,probe.planeSlope,false,true,0,0,0,
            nullptr,1,false,0,true,0,nullptr,&probe);};
        const auto reference=draw();
        probe.layerFilter=probe.receiverFilter=true;probe.directional=true;
        const auto curved=draw();
        for(unsigned y=32;y<96;++y)for(unsigned x=32;x<96;++x)
            curvedWorst=(std::max)(curvedWorst,unsigned(std::abs(int((curved[y*kSize+x]>>16)&255)-
                int((reference[y*kSize+x]>>16)&255))));
        probe.normalBend=0;
        const auto flat=draw();probe.layerFilter=probe.receiverFilter=false;
        const auto flatReference=draw();
        for(unsigned y=32;y<96;++y)for(unsigned x=32;x<96;++x)
            negative+=std::abs(int((flat[y*kSize+x]>>16)&255)-int((flatReference[y*kSize+x]>>16)&255))>2;
    }
    std::printf("curved coarse receiver: max_difference=%u flat_negative_pixels=%u\n",curvedWorst,negative);
    if(curvedWorst>1||negative<100)throw std::runtime_error("coarse receiver curvature guard or planar control failed");
}

void CheckBridgeLayersGpu(IDirect3DDevice9* device,void* effect) {
    const std::array<std::array<float,3>,6> orders{{{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}}};
    unsigned worst=0,cases=0,rankDifferences=0,negativePixels=0;
    const auto differences=[](const std::vector<DWORD>& a,const std::vector<DWORD>& b) {
        unsigned count=0;
        for(unsigned y=32;y<96;++y)for(unsigned x=24;x<104;++x) {
            unsigned error=0;
            for(unsigned shift:{0u,8u,16u})error=(std::max)(error,unsigned(std::abs(
                int((a[y*kSize+x]>>shift)&255)-int((b[y*kSize+x]>>shift)&255))));
            count+=error>2;
        }
        return count;
    };
    const auto compare=[&](const std::vector<DWORD>& actual,const std::vector<DWORD>& expected) {
        for(unsigned y=32;y<96;++y)for(unsigned x=24;x<104;++x)for(unsigned shift:{0u,8u,16u})
            worst=(std::max)(worst,unsigned(std::abs(int((actual[y*kSize+x]>>shift)&255)-
                int((expected[y*kSize+x]>>shift)&255))));
        ++cases;
    };
    for(int mode:{1,2}) {
        BridgeProbe probe;
        probe.depth[0]=.5f-(mode==1?.0005f:.00015f);
        const auto draw=[&](const BridgeProbe& sample) {
            return Draw(device,effect,128,mode,0,false,true,0,0,0,nullptr,1,false,0,false,0,nullptr,&sample);
        };
        std::array<std::vector<DWORD>,3> reference;
        for(unsigned layer=0;layer<3;++layer)
            reference[layer]=Draw(device,effect,128,mode,0,false,true,.5f-probe.depth[layer]);
        const auto lit=Draw(device,effect,128,mode,0,false,true,-.5f);
        for(unsigned a=0;a<3;++a)for(unsigned b=a+1;b<3;++b)
            if(differences(reference[a],reference[b])<100)
                throw std::runtime_error("constant GPU layers cannot distinguish a wrong priority or missing sampler");

        const auto isolated=draw(probe);
        compare(isolated,reference[2]);
        auto disabled=probe;disabled.enabled=false;
        const auto withoutBridge=draw(disabled);compare(withoutBridge,lit);
        negativePixels+=differences(isolated,withoutBridge);
        auto outside=probe;outside.outside=true;compare(draw(outside),lit);
        auto noWeight=probe;noWeight.blend[2]=0;compare(draw(noWeight),lit);
        auto half=probe;half.blend[2]=.5f;
        const auto halfImage=draw(half);
        for(unsigned y=32;y<96;++y)for(unsigned x=24;x<104;++x)for(unsigned shift:{0u,8u,16u}) {
            const int expected=(int((lit[y*kSize+x]>>shift)&255)+int((reference[2][y*kSize+x]>>shift)&255))/2;
            worst=(std::max)(worst,unsigned(std::abs(expected-int((halfImage[y*kSize+x]>>shift)&255))));
        }
        ++cases;

        probe.middleEnabled=probe.nearEnabled=true;
        std::vector<DWORD> firstPartial;
        for(const auto& rank:orders) {
            std::copy(rank.begin(),rank.end(),probe.rank);
            std::fill_n(probe.blend,3,1.f);
            const auto full=draw(probe);
            const unsigned strongest=unsigned(std::max_element(rank.begin(),rank.end())-rank.begin());
            compare(full,reference[strongest]);
            if(differences(full,reference[(strongest+1)%3])<100)
                throw std::runtime_error("full-weight three-layer negative control did not change GPU pixels");
            probe.blend[0]=.25f;probe.blend[1]=.5f;probe.blend[2]=.75f;
            const auto partial=draw(probe);
            if(firstPartial.empty())firstPartial=partial;
            else rankDifferences+=differences(partial,firstPartial);
            // Independently composite the separately rendered constant maps in
            // priority order. This is not the shader's product-weight formula.
            for(unsigned y=32;y<96;++y)for(unsigned x=24;x<104;++x)for(unsigned shift:{0u,8u,16u}) {
                float expected=float((lit[y*kSize+x]>>shift)&255);
                for(unsigned priority=0;priority<3;++priority)for(unsigned layer=0;layer<3;++layer)
                    if(rank[layer]==float(priority)) {
                        const float t=probe.blend[layer],weight=t*t*(3-2*t);
                        expected=expected*(1-weight)+float((reference[layer][y*kSize+x]>>shift)&255)*weight;
                    }
                worst=(std::max)(worst,unsigned(std::abs(int(std::lround(expected))-
                    int((partial[y*kSize+x]>>shift)&255))));
            }
            ++cases;
            // Identical partially shadowed inputs expose missing/extra total
            // weight without saturation concealing an incorrect sum.
            auto unity=probe;
            unity.coverageDepth=probe.depth[0];
            std::fill_n(unity.depth,3,probe.depth[0]);
            for(const auto& weights:std::array<std::array<float,3>,3>{{{0,0,0},{.2f,.5f,.8f},{1,1,1}}}) {
                std::copy(weights.begin(),weights.end(),unity.blend);
                compare(draw(unity),reference[0]);
            }
        }
        // Leave a bridge-enabled draw immediately before the default call so
        // stale s5/uniform state has a visibly different negative control.
        const auto bridgeLast=draw(BridgeProbe{});
        const auto defaultImage=Draw(device,effect,128,mode,0,false,true,-.5f);
        compare(defaultImage,lit);
        if(differences(bridgeLast,defaultImage)<100)
            throw std::runtime_error("bridge reset probe has no different shadow pixels");
        using GetBool=HRESULT(WINAPI*)(void*,const char*,BOOL*);
        BOOL enabled=TRUE;
        Check(EffectMethod<GetBool,25>(effect)(effect,"MdsBridgeEnabled",&enabled),"Read reset bridge uniform");
        ComPtr<IDirect3DBaseTexture9> bound;
        Check(device->GetTexture(5,&bound),"Read released bridge sampler");
        if(enabled || bound)throw std::runtime_error("bridge remains enabled or bound after a default draw");
    }
    if(worst>2 || negativePixels<100 || rankDifferences<100)
        throw std::runtime_error("GPU bridge sampling, ranked composition, or unity-weight regression");
    std::printf("PASS bridge GPU: cases=%u max_difference=%u negative_pixels=%u priority_differences=%u\n",
        cases,worst,negativePixels,rankDifferences);
}

void CheckLogTransitionsGpu(IDirect3DDevice9* device,void* effect,bool screenAware=false) {
    using namespace mikumikustudio;
    struct Pair {
        unsigned fine;int coarse;float nearDepth,startDepth,farDepth;unsigned coarseSize;
        float coarseDensity=0;bool interiorStart=false,screenBlend=true;
    };
    // The nearest layer retains its protected core. The two farther layers
    // hand off across the complete gap between adjacent layer endpoints.
    const std::vector<Pair> pairs=screenAware?std::vector<Pair>{
        // Measured 1280x800, 30-degree view: 8875 precision. These coarse
        // densities include capped allocations, not their requested budgets.
        {1,0,1,4.313479f,6.09766f,8,163.82f},
        {0,2,1,6.09766f,28.6173f,16,51.1937f},
        {2,-1,1,28.6173f,90.5004f,32,13.9584f,false,false},
        // The same geometry with attainable intermediate coarse densities
        // exercises the continuous branch between fixed and latest starts.
        {0,2,1,6.09766f,28.6173f,16,100.f,true},
        {0,2,1,6.09766f,28.6173f,16,150.f,true}
    }:std::vector<Pair>{{1,0,1,3.6f,5,8},{0,2,1,5,25,16},{2,-1,1,25,80,32}};
    constexpr unsigned steps=64;
    unsigned frames=0,worst=0,largestStep=0,largestOldStep=0,modePixels=0;
    double largestWeightStep=0;
    const auto enabled=[](BridgeProbe& probe,unsigned layer,bool value) {
        if(layer==0)probe.middleEnabled=value;
        else if(layer==1)probe.nearEnabled=value;
        else probe.enabled=value;
    };
    const auto pixelDifference=[](const std::vector<DWORD>& a,const std::vector<DWORD>& b) {
        unsigned maximum=0,count=0;
        for(unsigned y=32;y<96;++y)for(unsigned x=24;x<104;++x) {
            unsigned difference=0;
            for(unsigned shift:{0u,8u,16u})difference=(std::max)(difference,unsigned(std::abs(
                int((a[y*kSize+x]>>shift)&255)-int((b[y*kSize+x]>>shift)&255))));
            maximum=(std::max)(maximum,difference);count+=difference>8;
        }
        return std::array<unsigned,2>{{maximum,count}};
    };
    const auto linearBaseline=Draw(device,effect,128,1,0,true,true,0,0,0,nullptr,1,false,3);
    // The new interval run uses a diagonal edge; both edge/strip patterns are
    // still covered by the unchanged historical interval run below.
    const std::vector<int> patterns=screenAware?std::vector<int>{1}:std::vector<int>{1,2};
    for(int pattern:patterns)for(const auto& fixture:pairs)for(bool filtered:{false,true}) {
        std::vector<DWORD> modeOne;
        for(int mode:{1,2}) {
            auto pair=fixture;
            if(screenAware && pair.screenBlend) {
                constexpr float width=1280,height=800;
                const float lensY=1.f/std::tan(30.f*3.14159265358979323846f/360.f);
                const float projectedPixels=(std::max)(width*(lensY/(width/height)),height*lensY);
                const float light[]{-.5f,-1,.5f};
                const float reference=NativeShadowSamplingDensity(light,.01125f,mode,4096);
                const float finest=NativeShadowSamplingDensity(light,.1f,mode,4096);
                pair.startDepth=ShadowScreenBlendStart(pair.nearDepth,pair.farDepth,fixture.startDepth,
                    fixture.coarseDensity,reference,finest,projectedPixels,false);
                const double latest=(std::max)(double(fixture.startDepth),double(pair.farDepth)/1.5);
                const double expectedStart=(std::clamp)(.625*projectedPixels*
                    std::pow(double(reference)/finest,.25)/fixture.coarseDensity,
                    double(fixture.startDepth),latest);
                if(std::abs(pair.startDepth-expectedStart)>pair.farDepth*3e-6 ||
                   (fixture.interiorStart && !(pair.startDepth>fixture.startDepth && pair.startDepth<latest)))
                    throw std::runtime_error("screen-aware GPU interval does not match its measured coarse density");
            }
            BridgeProbe probe;
            probe.enabled=probe.middleEnabled=probe.nearEnabled=false;
            probe.layerFilter=probe.receiverFilter=filtered;
            if(filtered && pattern==2)probe.stripeHalfWidth=.14f;
            enabled(probe,pair.fine,true);
            probe.mapSize[pair.fine]=pair.coarseSize*4;
            probe.texturePattern[pair.fine]=pattern;
            probe.casterGap=.00032f; // Same physical depth gap in both modes.
            if(pair.coarse>=0) {
                enabled(probe,unsigned(pair.coarse),true);
                probe.mapSize[pair.coarse]=pair.coarseSize;
                probe.texturePattern[pair.coarse]=pattern;
            } else probe.coveragePattern=pattern;
            const auto draw=[&](const BridgeProbe& sample) {
                return Draw(device,effect,pair.coarseSize,mode,0,false,true,0,0,0,
                    nullptr,1,false,0,true,0,nullptr,&sample);
            };
            auto fineOnly=probe;
            if(pair.coarse>=0)enabled(fineOnly,unsigned(pair.coarse),false);
            else fineOnly.coveragePattern=0;
            const auto fine=draw(fineOnly);
            auto coarseOnly=probe;enabled(coarseOnly,pair.fine,false);
            const auto coarse=draw(coarseOnly);
            const auto contrast=pixelDifference(fine,coarse);
            if(contrast[1]<16)
                throw std::runtime_error("low-density transition textures cannot distinguish coarse and fine sampling");
            if(mode==1)modeOne=fine;
            else {
                const auto contrastBetweenModes=pixelDifference(modeOne,fine);
                if(contrastBetweenModes[1]<8)
                    throw std::runtime_error("log transition endpoint lost the original shadow mode difference");
                modePixels+=contrastBetweenModes[1];
            }
            const float fraction=ShadowTransitionFraction(pair.nearDepth,pair.startDepth,pair.farDepth);
            const double width=(pair.farDepth-pair.nearDepth)*fraction;
            const float ratio=ShadowTransitionLogRatio(pair.startDepth,pair.farDepth);
            std::vector<DWORD> previous,previousOld;
            unsigned maximumStep=0,maximumOldStep=0,caseWorst=0;
            double previousWeight=1,maximumWeightStep=0;
            std::vector<double> depths;
            if(screenAware) {
                // Cover the protected core, the complete actual fade and its
                // far fallback. Exact endpoints are added independently of the
                // logarithmic sweep, which uses equal camera zoom increments.
                for(unsigned step=0;step<=steps*2;++step)
                    depths.push_back(pair.nearDepth*std::pow(double(pair.farDepth)*1.1/pair.nearDepth,
                        double(step)/(steps*2)));
                depths.push_back(pair.startDepth);depths.push_back(pair.farDepth);
                std::sort(depths.begin(),depths.end());
            } else for(unsigned step=0;step<=steps;++step)
                depths.push_back(pair.startDepth*std::pow(double(pair.farDepth)/pair.startDepth,double(step)/steps));
            for(unsigned step=0;step<depths.size();++step) {
                // A continuous camera-depth sweep at equal zoom ratios. Keep
                // receiver positions fixed to isolate the hand-off from image
                // motion; each layer still samples its own real depth texture.
                const double depth=depths[step];
                probe.blend[pair.fine]=float((std::clamp)((pair.farDepth-depth)/width,0.0,1.0));
                probe.logBlend[pair.fine]=ratio;
                const auto current=draw(probe);
                // Independent endpoint/log oracle, without using either the
                // production transition fraction or log-ratio helper.
                const double expectedWeight=SmoothTransitionWeight(
                    std::log(double(pair.farDepth)/depth)/std::log(double(pair.farDepth)/pair.startDepth));
                for(unsigned y=32;y<96;++y)for(unsigned x=24;x<104;++x)for(unsigned shift:{0u,8u,16u}) {
                    const int a=(fine[y*kSize+x]>>shift)&255,b=(coarse[y*kSize+x]>>shift)&255;
                    const int expected=int(std::lround(a*expectedWeight+b*(1-expectedWeight)));
                    const int actual=(current[y*kSize+x]>>shift)&255;
                    worst=(std::max)(worst,unsigned(std::abs(actual-expected)));
                    caseWorst=(std::max)(caseWorst,unsigned(std::abs(actual-expected)));
                    if(actual<(std::min)(a,b)-2 || actual>(std::max)(a,b)+2)
                        throw std::runtime_error("log transition overshot adjacent maps or briefly uncovered the shadow");
                }
                if(depth<=pair.startDepth && pixelDifference(current,fine)[0]>2)
                    throw std::runtime_error("log transition changed its near endpoint");
                if((depth>=pair.farDepth || (!screenAware && step+1==depths.size())) &&
                   pixelDifference(current,coarse)[0]>2)
                    throw std::runtime_error("log transition changed its far endpoint");
                if(!previous.empty())maximumStep=(std::max)(maximumStep,pixelDifference(current,previous)[0]);
                maximumWeightStep=(std::max)(maximumWeightStep,std::abs(expectedWeight-previousWeight));
                previousWeight=expectedWeight;
                previous=current;
                ++frames;
                if(screenAware)continue;
                auto old=probe;old.logBlend[pair.fine]=0;
                old.blend[pair.fine]=float((std::clamp)((pair.farDepth-depth)/
                    ((pair.farDepth-pair.nearDepth)*.35),0.0,1.0));
                const auto oldImage=draw(old);
                if(!previousOld.empty())maximumOldStep=(std::max)(maximumOldStep,pixelDifference(oldImage,previousOld)[0]);
                previousOld=oldImage;
                ++frames;
            }
            // Smoothstep's maximum derivative is 1.5. Quantized readback adds
            // at most two channel values to this independent continuity bound.
            const unsigned limit=unsigned(std::ceil(contrast[0]*1.5/steps))+2;
            if(!screenAware && (maximumStep>limit || (pair.fine!=1 && maximumStep>maximumOldStep*.8+2))) {
                std::printf("log transition failure: mode=%d pattern=%d fine_layer=%u step=%u old_step=%u limit=%u\n",
                    mode,pattern,pair.fine,maximumStep,maximumOldStep,limit);
                throw std::runtime_error("log depth hand-off has a jump or did not improve the old 35-percent zoom transition");
            }
            largestStep=(std::max)(largestStep,maximumStep);
            largestOldStep=(std::max)(largestOldStep,maximumOldStep);
            largestWeightStep=(std::max)(largestWeightStep,maximumWeightStep);
            if(screenAware)std::printf("SCREEN_GPU mode=%d filtered=%u fine=%u coarse_density=%.6f start=%.6f far=%.6f interior=%u frames=%zu max_difference=%u max_step=%u max_weight_step=%.9f\n",
                mode,filtered,pair.fine,fixture.coarseDensity,pair.startDepth,pair.farDepth,fixture.interiorStart,
                depths.size(),caseWorst,maximumStep,maximumWeightStep);
        }
    }
    BridgeProbe logLast;logLast.middleEnabled=true;logLast.enabled=false;
    logLast.blend[0]=.5f;logLast.logBlend[0]=.8f;
    Draw(device,effect,128,1,0,false,true,0,0,0,nullptr,1,false,0,false,0,nullptr,&logLast);
    const auto reset=Draw(device,effect,128,1,0,true,true,0,0,0,nullptr,1,false,3);
    if(pixelDifference(reset,linearBaseline)[0]>1)
        throw std::runtime_error("a default draw retained the previous logarithmic layer blend");
    if(worst>2)throw std::runtime_error("GPU log blend differs from the independent equal-zoom visibility interpolation");
    if(screenAware)
        std::printf("PASS screen-aware GPU transitions: frames=%u max_difference=%u max_step=%u max_weight_step=%.9f mode_pixels=%u\n",
            frames,worst,largestStep,largestWeightStep,modePixels);
    else std::printf("PASS logarithmic GPU transitions: frames=%u max_difference=%u max_step=%u old_35_percent_max_step=%u mode_pixels=%u\n",
            frames,worst,largestStep,largestOldStep,modePixels);
}

struct IndependentShadowGrid {
    unsigned size=0;
    double halfWidth=1;
    std::vector<float> pixels;
    double Spacing()const {return 2*halfWidth/size;}
    double Sample(double worldX,double worldY)const {
        // Work in world units, then interpolate the four stored raster nodes.
        // These are real CPU textures, not shader-local virtual-grid formulas.
        const double px=(worldX/halfWidth+1)*size*.5;
        const double py=(1-worldY/halfWidth)*size*.5;
        const int ix=int(std::floor(px)),iy=int(std::floor(py));
        const double fx=px-ix,fy=py-iy;
        const auto at=[&](int x,int y) {
            return double(pixels[size_t((std::clamp)(y,0,int(size)-1))*size+
                unsigned((std::clamp)(x,0,int(size)-1))]);
        };
        return (1-fy)*((1-fx)*at(ix,iy)+fx*at(ix+1,iy))+
            fy*((1-fx)*at(ix,iy+1)+fx*at(ix+1,iy+1));
    }
};

IndependentShadowGrid IndependentProbeGrid(unsigned size,double halfWidth,int pattern,const BridgeProbe& probe) {
    IndependentShadowGrid grid{size,halfWidth,std::vector<float>(size_t(size)*size)};
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)
        grid.pixels[size_t(y)*size+x]=ProbeDepth(x,y,size,float(halfWidth),pattern,probe.casterGap,
            probe.planeSlope,probe.stripeHalfWidth,probe.backgroundDepth,probe.planeSlopeY,probe.edgeSlope);
    return grid;
}

IndependentShadowGrid IndependentResampleGrid(const IndependentShadowGrid& source,unsigned size,double halfWidth) {
    IndependentShadowGrid grid{size,halfWidth,std::vector<float>(size_t(size)*size)};
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)
        grid.pixels[size_t(y)*size+x]=float(source.Sample((2.0*x/size-1)*halfWidth,(1-2.0*y/size)*halfWidth));
    return grid;
}

double IndependentSmooth(double value) {
    value=(std::clamp)(value,0.0,1.0);
    return value*value*(3-2*value);
}

float IndependentInverseSmooth(double wanted) {
    double low=0,high=1;
    for(unsigned step=0;step<48;++step) {
        const double middle=(low+high)*.5;
        if(IndependentSmooth(middle)<wanted)low=middle;else high=middle;
    }
    return float((low+high)*.5);
}

unsigned ProgressiveImageDifference(const std::vector<DWORD>& a,const std::vector<DWORD>& b,
                                    unsigned left=24,unsigned top=24,unsigned right=104,unsigned bottom=104) {
    unsigned maximum=0;
    for(unsigned y=top;y<bottom;++y)for(unsigned x=left;x<right;++x)
        maximum=(std::max)(maximum,unsigned(std::abs(int((a[y*kSize+x]>>16)&255)-int((b[y*kSize+x]>>16)&255))));
    return maximum;
}

void CheckProgressiveBridgeGpu(IDirect3DDevice9* device,void* effect,unsigned fineLayer=2) {
    using GetBool=HRESULT(WINAPI*)(void*,const char*,BOOL*);
    BOOL defaultValue=TRUE;
    Check(EffectMethod<GetBool,25>(effect)(effect,"MdsProgressiveBridge",&defaultValue),"Read progressive bridge default");
    if(defaultValue)throw std::runtime_error("progressive bridge must default to disabled");
    constexpr unsigned coarseSize=16;
    const unsigned coarseLayer=fineLayer==0?2:0;
    const auto enable=[](BridgeProbe& probe,unsigned layer,bool value) {
        if(layer==0)probe.middleEnabled=value;
        else if(layer==1)probe.nearEnabled=value;
        else probe.enabled=value;
    };
    const auto draw=[&](const BridgeProbe& probe,int mode,float phase=0.f,bool stable=true,unsigned size=16) {
        return Draw(device,effect,size,mode,probe.planeSlope,false,true,0,0,phase,
            nullptr,1,false,0,stable,0,nullptr,&probe);
    };
    const auto color=[](double depth,double receiver,unsigned y,int mode) {
        const double strength=mode==2?8000.0*y/kSize:1500.0;
        return int(std::lround(255-204*(std::clamp)((std::max)(receiver-depth,0.0)*strength-.3,0.0,1.0)));
    };
    struct Configuration {unsigned fineSize;float halfWidth;};
    // Include two exact hierarchies and a non-integral density ratio. The latter
    // makes resampled bridge nodes differ from direct coarse rasterization.
    const Configuration configurations[]{{64,1},{128,128.f/(16*8.25f)},{256,1}};
    unsigned frames=0,worst=0,largestStep=0,lastDifferences=0,changed=0,modeDifference=0;
    for(const auto& configuration:configurations)for(int pattern:{1,2})for(float texturePhase:{0.f,.37f}) {
        if(configuration.fineSize==256 && (pattern==2 || texturePhase!=0))continue;
        std::vector<DWORD> modeOne;
        for(int mode:{1,2}) {
            BridgeProbe probe;probe.progressive=true;
            enable(probe,fineLayer,true);
            probe.mapSize[fineLayer]=configuration.fineSize;probe.halfWidth[fineLayer]=configuration.halfWidth;
            probe.coveragePattern=probe.texturePattern[fineLayer]=pattern;
            if(fineLayer!=2) {
                enable(probe,coarseLayer,true);
                probe.mapSize[coarseLayer]=coarseSize;probe.texturePattern[coarseLayer]=pattern;
                // The adjacent coarse map is not the coverage coordinate frame.
                probe.coverageHalfWidth=2;
            }
            const double coarseHalfWidth=fineLayer==2?probe.coverageHalfWidth:probe.halfWidth[coarseLayer];
            probe.casterGap=.00032f;probe.backgroundDepth=.5f;
            probe.stripeHalfWidth=3*configuration.halfWidth/configuration.fineSize;
            const auto fine=IndependentProbeGrid(configuration.fineSize,configuration.halfWidth,pattern,probe);
            const auto coarse=IndependentProbeGrid(coarseSize,coarseHalfWidth,pattern,probe);
            const double ratios=coarse.Spacing()/fine.Spacing(),octaves=std::log2(ratios),duration=octaves+2;
            std::vector<IndependentShadowGrid> grids;
            for(unsigned size=coarseSize;;size*=2) {
                grids.push_back(IndependentResampleGrid(fine,size,coarseHalfWidth));
                if(grids.back().Spacing()<=fine.Spacing())break;
            }
            auto legacy=probe;legacy.progressive=false;
            const auto fineEndpoint=draw(legacy,mode,texturePhase);
            auto coverageOnly=legacy;enable(coverageOnly,fineLayer,false);
            const auto coarseEndpoint=draw(coverageOnly,mode,texturePhase);
            if(mode==1)modeOne=fineEndpoint;
            else modeDifference=(std::max)(modeDifference,ProgressiveImageDifference(modeOne,fineEndpoint));
            // Independently render the last intermediate as an ordinary real
            // coverage texture, with progressive disabled. This validates the
            // final handoff against a data source distinct from actual coverage.
            auto virtualOnly=coverageOnly;virtualOnly.enabled=virtualOnly.middleEnabled=virtualOnly.nearEnabled=false;
            virtualOnly.coverageValues=&grids.front().pixels;
            virtualOnly.coverageHalfWidth=float(coarseHalfWidth);
            const auto virtualEndpoint=draw(virtualOnly,mode,float(texturePhase*probe.coverageHalfWidth/coarseHalfWidth));
            const unsigned lastDifference=ProgressiveImageDifference(virtualEndpoint,coarseEndpoint);
            if(configuration.halfWidth!=1)lastDifferences+=(lastDifference>2);
            std::vector<double> times;
            const unsigned steps=unsigned(std::ceil(duration*24));
            for(unsigned step=0;step<=steps;++step)times.push_back(duration*step/steps);
            times.push_back(1);times.push_back(duration-1);times.push_back(duration);
            for(unsigned exponent=0;exponent<=unsigned(std::ceil(octaves));++exponent) {
                const double boundary=1+octaves-exponent;
                if(boundary>1 && boundary<duration-1)
                    for(double offset:{-1e-5,0.0,1e-5})times.push_back(boundary+offset);
            }
            std::sort(times.begin(),times.end());
            std::vector<DWORD> previous;
            double previousTime=0,minimumArea=INFINITY,maximumArea=0;
            unsigned caseWorst=0,caseStep=0;
            for(double time:times) {
                probe.blend[fineLayer]=IndependentInverseSmooth(1-time/duration);
                const auto current=draw(probe,mode,texturePhase);
                // Specify desired physical spacing, then find its two stored
                // reference grids by ordered search. No floor(log2) grid lookup
                // or shader fractional interpolation formula is copied here.
                const double spacing=(std::clamp)(fine.Spacing()*std::exp2(time-1),fine.Spacing(),coarse.Spacing());
                unsigned lo=0;
                while(lo+2<grids.size() && grids[lo+1].Spacing()>spacing)++lo;
                const unsigned hi=(std::min)(lo+1,unsigned(grids.size()-1));
                const double fraction=lo==hi?0:(grids[lo].Spacing()-spacing)/
                    (grids[lo].Spacing()-grids[hi].Spacing());
                const double enter=IndependentSmooth(time),leave=IndependentSmooth(time-duration+1);
                double area=0;
                for(unsigned y=24;y<104;++y)for(unsigned x=24;x<104;++x) {
                    const double wx=2.0*x/kSize-1+2.0*texturePhase*probe.coverageHalfWidth/coarseSize;
                    const double wy=1-2.0*y/kSize;
                    const double intermediate=(1-fraction)*grids[lo].Sample(wx,wy)+fraction*grids[hi].Sample(wx,wy);
                    const double entering=(1-enter)*fine.Sample(wx,wy)+enter*intermediate;
                    const double expectedDepth=(1-leave)*entering+leave*coarse.Sample(wx,wy);
                    const int expected=color(expectedDepth,.5,y,mode),actual=int((current[y*kSize+x]>>16)&255);
                    caseWorst=(std::max)(caseWorst,unsigned(std::abs(expected-actual)));
                    area+=(255-actual)/204.0;
                }
                minimumArea=(std::min)(minimumArea,area);maximumArea=(std::max)(maximumArea,area);
                if(!previous.empty()) {
                    const unsigned difference=ProgressiveImageDifference(previous,current);
                    caseStep=(std::max)(caseStep,difference);
                    // All stored depths lie in a casterGap interval. Geometric
                    // spacing interpolation has derivative <= 2*ln(2), endpoint
                    // easing <= 1.5. Apply the native ramp's Lipschitz constant;
                    // allow four levels for raster/float/readback rounding.
                    const double strength=mode==2?8000.0*103/kSize:1500.0;
                    const unsigned bound=unsigned(std::ceil(204*strength*probe.casterGap*1.5*(time-previousTime)))+4;
                    if(difference>bound)throw std::runtime_error("progressive grid or final coverage handoff has a discontinuous step");
                }
                if(time==0 && ProgressiveImageDifference(current,fineEndpoint)>1)
                    throw std::runtime_error("progressive bridge changed the fine endpoint");
                if(time==duration && ProgressiveImageDifference(current,coarseEndpoint)>1)
                    throw std::runtime_error("progressive bridge changed the actual coverage endpoint");
                if(std::abs(time-duration*.5)<duration/steps) {
                    legacy.blend[fineLayer]=probe.blend[fineLayer];
                    changed+=(ProgressiveImageDifference(current,draw(legacy,mode,texturePhase))>2);
                }
                previous=current;previousTime=time;++frames;
            }
            std::printf("PROGRESSIVE_GPU fine_layer=%u mode=%d pattern=%d ratio=%.7f phase=%.3f frames=%zu max_oracle_error=%u max_step=%u last_virtual_actual_difference=%u area_min=%.6f area_max=%.6f\n",
                fineLayer,mode,pattern,ratios,texturePhase,times.size(),caseWorst,caseStep,lastDifference,minimumArea,maximumArea);
            worst=(std::max)(worst,caseWorst);largestStep=(std::max)(largestStep,caseStep);
        }
    }
    if(worst>5 || !changed || !lastDifferences || modeDifference<8)
        throw std::runtime_error("progressive shadow spacing/depth oracle, final mismatch control or mode contrast failed");
    unsigned isolated=0;
    for(int mode:{1,2}) {
        BridgeProbe probe;probe.mapSize[2]=128;probe.coveragePattern=probe.texturePattern[2]=1;
        probe.casterGap=.00032f;probe.backgroundDepth=.5f;probe.blend[2]=.5f;
        for(unsigned control=0;control<8;++control) {
            auto sample=probe;
            if(control==0)sample.layerFilter=sample.receiverFilter=true;
            if(control==1)sample.layerFilter=true;
            if(control==2)sample.receiverFilter=true;
            if(control==3)sample.middleEnabled=true;
            if(control==4)sample.nearEnabled=true;
            if(control==5)sample.enabled=false;
            if(control==6)sample.outside=true;
            const bool stable=control!=7;
            sample.progressive=false;const auto expected=draw(sample,mode,.17f,stable);
            sample.progressive=true;const auto actual=draw(sample,mode,.17f,stable);
            if(ProgressiveImageDifference(expected,actual)>1)
                throw std::runtime_error("progressive path affected enhanced, unstable, disabled, rim or fully covered receivers");
            ++isolated;
        }
        // A smooth affine plane must remain lit at every intermediate scale.
        probe.coveragePattern=probe.texturePattern[2]=3;probe.planeSlope=.15f;
        probe.progressive=true;
        for(float blend:{.02f,.13f,.31f,.53f,.77f,.98f}) {
            probe.blend[2]=blend;const auto image=draw(probe,mode);
            for(unsigned y=24;y<104;++y)for(unsigned x=24;x<104;++x)
                if(((image[y*kSize+x]>>16)&255)<254)
                    throw std::runtime_error("progressive virtual grid invented shadow on a coplanar receiver");
        }
        // Cross the former hard full-rim gate with a shadow contour present.
        // Very small camera motion cannot switch the whole reconstruction.
        probe.planeSlope=0;probe.coveragePattern=probe.texturePattern[2]=1;
        probe.edgeSlope=4;probe.blend[2]=.5f;
        for(int side:{-1,1}) {
            const unsigned x=side<0?10:118;
            const double world=2.0*x/kSize-1;
            const float centre=float((side*.86-world)*coarseSize*.5);
            const auto a=draw(probe,mode,centre-1e-5f),b=draw(probe,mode,centre+1e-5f);
            if(ProgressiveImageDifference(a,b,x-1,24,x+2,104)>3)
                throw std::runtime_error("progressive bridge has a hard spatial-rim activation line");
        }
        // A continuous test field avoids rasterizing a new geometric hit when
        // changing the physical width infinitesimally across a power of two.
        probe.coveragePattern=probe.texturePattern[2]=5;probe.edgeSlope=.35f;
        for(float blend:{.2f,.5f,.8f}) {
            probe.blend[2]=blend;std::vector<DWORD> previous;
            for(float ratio:{7.9999f,8.f,8.0001f}) {
                probe.halfWidth[2]=8.f/ratio;
                const auto current=draw(probe,mode,.29f);
                if(!previous.empty() && ProgressiveImageDifference(previous,current)>2)
                    throw std::runtime_error("progressive level count jumped across a density-ratio power of two");
                previous=current;
            }
        }
    }
    // A normal draw must reset the runtime-only switch, including after an
    // enabled draw that exits through a different shadow branch.
    Draw(device,effect,32,1,0,true,true);
    BOOL retained=TRUE;
    Check(EffectMethod<GetBool,25>(effect)(effect,"MdsProgressiveBridge",&retained),"Read restored progressive state");
    if(retained)throw std::runtime_error("default draw retained progressive bridge state");
    std::printf("PASS progressive GPU: frames=%u max_oracle_error=%u max_step=%u changed_controls=%u last_mismatch_controls=%u mode_contrast=%u isolated_paths=%u\n",
        frames,worst,largestStep,changed,lastDifferences,modeDifference,isolated);
}

void CheckProgressiveCompositionGpu(IDirect3DDevice9* device,void* effect) {
    const auto enable=[](BridgeProbe& probe,unsigned layer,bool value) {
        if(layer==0)probe.middleEnabled=value;
        else if(layer==1)probe.nearEnabled=value;
        else probe.enabled=value;
    };
    const auto draw=[&](const BridgeProbe& probe,int mode,unsigned size=8) {
        return Draw(device,effect,size,mode,probe.planeSlope,false,true,0,0,0,
            nullptr,1,false,0,true,0,nullptr,&probe);
    };
    unsigned cases=0,worstFallback=0,worstEndpoint=0,worstPlane=0,largestStep=0;
    for(unsigned fineLayer:{0u,1u})for(int mode:{1,2}) {
        const unsigned coarseLayer=fineLayer==0?2:0;
        BridgeProbe probe;probe.progressive=true;
        enable(probe,fineLayer,true);enable(probe,coarseLayer,true);
        probe.mapSize[fineLayer]=128;probe.mapSize[coarseLayer]=16;
        probe.texturePattern[fineLayer]=probe.texturePattern[coarseLayer]=1;
        probe.coveragePattern=2;probe.casterGap=.00032f;probe.backgroundDepth=.5f;
        probe.stripeHalfWidth=.12f;
        // Near->middle also observes a genuinely blended lower bridge result.
        if(fineLayer==1) {
            probe.texturePattern[2]=2;probe.mapSize[2]=16;probe.blend[2]=.43f;
        }
        for(bool missing:{false,true})for(double contribution:{0.,.01,.2,.8,.99999,1.}) {
            auto sample=probe;enable(sample,coarseLayer,!missing);
            sample.blend[coarseLayer]=IndependentInverseSmooth(contribution);
            auto lower=sample;enable(lower,fineLayer,false);
            const auto lowerImage=draw(lower,mode);
            auto full=sample;full.blend[fineLayer]=1;
            const auto fullImage=draw(full,mode);
            for(float fade:{0.f,.0001f,.02f,.15f,.5f,.85f,.98f,.9999f,1.f}) {
                sample.blend[fineLayer]=fade;const auto current=draw(sample,mode);
                const double weight=IndependentSmooth(fade);
                for(unsigned y=24;y<104;++y)for(unsigned x=24;x<104;++x) {
                    const int actual=(current[y*kSize+x]>>16)&255;
                    if(actual<50)throw std::runtime_error("progressive composite lost nonnegative unity visibility weights");
                    if(missing || contribution==0) {
                        const double expected=(1-weight)*int((lowerImage[y*kSize+x]>>16)&255)+
                            weight*int((fullImage[y*kSize+x]>>16)&255);
                        worstFallback=(std::max)(worstFallback,unsigned(std::abs(actual-int(std::lround(expected)))));
                    }
                }
                if(fade<=.0001f)worstEndpoint=(std::max)(worstEndpoint,ProgressiveImageDifference(current,lowerImage));
                if(fade>=.9999f)worstEndpoint=(std::max)(worstEndpoint,ProgressiveImageDifference(current,fullImage));
                ++cases;
            }
        }
        // Changing the coarse layer from almost full to full, or from empty
        // to almost empty, must not replace the whole fine-layer algorithm.
        probe.blend[fineLayer]=.5f;
        for(double centre:{0.,1.}) {
            probe.blend[coarseLayer]=IndependentInverseSmooth(centre);
            const auto at=draw(probe,mode);
            probe.blend[coarseLayer]=IndependentInverseSmooth(centre==0?1e-6:1-1e-6);
            const auto beside=draw(probe,mode);
            if(ProgressiveImageDifference(at,beside)>2)
                throw std::runtime_error("adjacent progressive layer hard-switched at coarse confidence zero or one");
        }
        // An independent Gaussian solve maps raster nodes through translated,
        // sheared light XY frames. An affine shared-Z plane must remain exact.
        const auto identity=[] {std::array<float,16> m{};for(unsigned i=0;i<4;++i)m[i*5]=1;return m;};
        const auto planeMap=[](unsigned size,const std::array<float,16>& m,float sx,float sy,float gap) {
            std::vector<float> values(size_t(size)*size);
            for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
                double rows[2][3]{{m[0],m[4],2.0*x/size-1-m[12]},
                    {m[1],m[5],1-2.0*y/size-m[13]}};
                if(std::abs(rows[1][0])>std::abs(rows[0][0]))
                    for(unsigned k=0;k<3;++k)std::swap(rows[0][k],rows[1][k]);
                const double factor=rows[1][0]/rows[0][0];
                for(unsigned k=0;k<3;++k)rows[1][k]-=factor*rows[0][k];
                const double wy=rows[1][2]/rows[1][1],wx=(rows[0][2]-rows[0][1]*wy)/rows[0][0];
                values[size_t(y)*size+x]=float(.5+sx*wx+sy*wy-gap);
            }
            return values;
        };
        for(float gap:{0.f,.00032f}) {
            auto affine=probe;affine.blend[coarseLayer]=1;
            affine.coveragePattern=affine.texturePattern[fineLayer]=affine.texturePattern[coarseLayer]=3;
            // D3D9 LINEAR weights may have only eight fractional bits. A 16^2
            // coarse map with slopes .13/-.07 introduces about 5.1e-5 depth
            // error, which the unchanged mode-2 ramp amplifies to 33 levels.
            // Bound that sampler error before testing the affine invariant;
            // retain the two-level assertion rather than tolerating the error.
            affine.mapSize[coarseLayer]=64;affine.mapSize[fineLayer]=512;
            affine.planeSlope=.13f/16;affine.planeSlopeY=-.07f/16;
            affine.overrideMapping[fineLayer]=affine.overrideMapping[coarseLayer]=true;
            auto a=identity(),b=identity();
            a[0]=.97f;a[1]=-.08f;a[4]=.06f;a[5]=.94f;a[12]=-.013f;a[13]=.011f;
            b[0]=.91f;b[1]=.09f;b[4]=-.07f;b[5]=.93f;b[12]=.021f;b[13]=-.017f;
            affine.layerMapping[fineLayer]=a;affine.layerMapping[coarseLayer]=b;
            {
                // Preserve the original sparse/steep fixture as an explicit
                // hardware-precision control. Both paths must show the same
                // coarse-endpoint quantization, rather than blaming the new
                // matrix inverse for native LINEAR sampler rounding.
                auto steep=affine;steep.mapSize[fineLayer]=128;steep.mapSize[coarseLayer]=16;
                steep.planeSlope=.13f;steep.planeSlopeY=-.07f;steep.blend[fineLayer]=.02f;
                const auto sf=planeMap(128,a,steep.planeSlope,steep.planeSlopeY,gap);
                const auto sc=planeMap(16,b,steep.planeSlope,steep.planeSlopeY,gap);
                steep.layerValues[fineLayer]=&sf;steep.layerValues[coarseLayer]=&sc;
                const auto current=draw(steep,mode);
                steep.progressive=false;const auto legacy=draw(steep,mode);
                unsigned nativeError=0,currentError=0;
                for(unsigned y=24;y<104;++y)for(unsigned x=24;x<104;++x) {
                    const double strength=mode==2?8000.0*y/kSize:1500.0;
                    const int expected=int(std::lround(255-204*(std::clamp)(gap*strength-.3,0.0,1.0)));
                    nativeError=(std::max)(nativeError,unsigned(std::abs(expected-int((legacy[y*kSize+x]>>16)&255))));
                    currentError=(std::max)(currentError,unsigned(std::abs(expected-int((current[y*kSize+x]>>16)&255))));
                }
                const unsigned difference=ProgressiveImageDifference(current,legacy);
                std::printf("AFFINE_LINEAR_CONTROL fine_layer=%u mode=%d gap=%.8f native_quantization=%u progressive_quantization=%u native_difference=%u\n",
                    fineLayer,mode,gap,nativeError,currentError,difference);
                if(difference>2)throw std::runtime_error("progressive coarse endpoint changed native sparse-plane sampler rounding");
            }
            const auto fineValues=planeMap(affine.mapSize[fineLayer],a,affine.planeSlope,affine.planeSlopeY,gap);
            const auto coarseValues=planeMap(affine.mapSize[coarseLayer],b,affine.planeSlope,affine.planeSlopeY,gap);
            affine.layerValues[fineLayer]=&fineValues;affine.layerValues[coarseLayer]=&coarseValues;
            for(float fade:{.02f,.19f,.43f,.71f,.98f}) {
                affine.blend[fineLayer]=fade;const auto current=draw(affine,mode);
                for(unsigned y=24;y<104;++y)for(unsigned x=24;x<104;++x) {
                    const double strength=mode==2?8000.0*y/kSize:1500.0;
                    const int expected=int(std::lround(255-204*(std::clamp)(gap*strength-.3,0.0,1.0)));
                    worstPlane=(std::max)(worstPlane,unsigned(std::abs(expected-int((current[y*kSize+x]>>16)&255))));
                }
                ++cases;
            }
        }
        {
            // A plane alone cannot detect an incorrect relative XY basis:
            // averaging affine values cancels corner displacement. Use an
            // actual occluder edge and independent prebuilt virtual textures.
            auto mapped=probe;mapped.blend[coarseLayer]=1;
            mapped.planeSlope=mapped.planeSlopeY=0;
            mapped.mapSize[fineLayer]=128;mapped.mapSize[coarseLayer]=16;
            mapped.overrideMapping[fineLayer]=mapped.overrideMapping[coarseLayer]=true;
            auto a=identity(),b=identity();
            a[0]=.97f;a[1]=-.08f;a[4]=.06f;a[5]=.94f;a[12]=-.013f;a[13]=.011f;
            b[0]=.91f;b[1]=.09f;b[4]=-.07f;b[5]=.93f;b[12]=.021f;b[13]=-.017f;
            mapped.layerMapping[fineLayer]=a;mapped.layerMapping[coarseLayer]=b;
            const auto solve=[](const std::array<float,16>& m,double x,double y) {
                double rows[2][3]{{m[0],m[4],x-m[12]},{m[1],m[5],y-m[13]}};
                if(std::abs(rows[1][0])>std::abs(rows[0][0]))
                    for(unsigned k=0;k<3;++k)std::swap(rows[0][k],rows[1][k]);
                const double factor=rows[1][0]/rows[0][0];
                for(unsigned k=0;k<3;++k)rows[1][k]-=factor*rows[0][k];
                const double wy=rows[1][2]/rows[1][1];
                return std::array<double,2>{(rows[0][2]-rows[0][1]*wy)/rows[0][0],wy};
            };
            const auto project=[](const std::array<float,16>& m,const std::array<double,2>& p) {
                return std::array<double,2>{p[0]*m[0]+p[1]*m[4]+m[12],p[0]*m[1]+p[1]*m[5]+m[13]};
            };
            const auto make=[&](unsigned size,const std::array<float,16>& matrix) {
                IndependentShadowGrid grid{size,1,std::vector<float>(size_t(size)*size)};
                for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
                    const auto world=solve(matrix,2.0*x/size-1,1-2.0*y/size);
                    grid.pixels[size_t(y)*size+x]=world[0]+.35*world[1]<.083?.5f-mapped.casterGap:.5f;
                }
                return grid;
            };
            const auto fine=make(128,a),coarse=make(16,b);
            mapped.layerValues[fineLayer]=&fine.pixels;mapped.layerValues[coarseLayer]=&coarse.pixels;
            const auto origin=project(a,solve(b,0,0)),axisX=project(a,solve(b,1,0)),axisY=project(a,solve(b,0,1));
            const double ratio=8*std::hypot(axisX[0]-origin[0],axisY[0]-origin[0]);
            const double fineSpacing=coarse.Spacing()/ratio,duration=std::log2(ratio)+2;
            std::vector<IndependentShadowGrid> grids,wrongGrids;
            for(unsigned size=16;;size*=2) {
                IndependentShadowGrid grid{size,1,std::vector<float>(size_t(size)*size)},wrong=grid;
                for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
                    const double u=2.0*x/size-1,v=1-2.0*y/size;
                    const auto correct=project(a,solve(b,u,v));
                    const auto diagonal=project(a,{(u-b[12])/b[0],(v-b[13])/b[5]});
                    grid.pixels[size_t(y)*size+x]=float(fine.Sample(correct[0],correct[1]));
                    wrong.pixels[size_t(y)*size+x]=float(fine.Sample(diagonal[0],diagonal[1]));
                }
                grids.push_back(std::move(grid));wrongGrids.push_back(std::move(wrong));
                if(grids.back().Spacing()<=fineSpacing)break;
            }
            unsigned error=0,diagonalControl=0;
            for(float fade:{.02f,.19f,.43f,.71f,.98f}) {
                mapped.blend[fineLayer]=fade;const auto current=draw(mapped,mode);
                const double time=(1-IndependentSmooth(fade))*duration;
                const double spacing=(std::clamp)(fineSpacing*std::exp2(time-1),fineSpacing,coarse.Spacing());
                unsigned lo=0;while(lo+2<grids.size() && grids[lo+1].Spacing()>spacing)++lo;
                const unsigned hi=lo+1;
                const double mix=(grids[lo].Spacing()-spacing)/(grids[lo].Spacing()-grids[hi].Spacing());
                const double enter=IndependentSmooth(time),leave=IndependentSmooth(time-duration+1);
                for(unsigned y=24;y<104;++y)for(unsigned x=24;x<104;++x) {
                    const std::array<double,2> world{2.0*x/kSize-1,1-2.0*y/kSize};
                    const auto f=project(a,world),c=project(b,world);
                    const auto expectedColor=[&](const std::vector<IndependentShadowGrid>& reference) {
                        const double intermediate=(1-mix)*reference[lo].Sample(c[0],c[1])+mix*reference[hi].Sample(c[0],c[1]);
                        const double entering=(1-enter)*fine.Sample(f[0],f[1])+enter*intermediate;
                        const double depth=(1-leave)*entering+leave*coarse.Sample(c[0],c[1]);
                        const double strength=mode==2?8000.0*y/kSize:1500.0;
                        return int(std::lround(255-204*(std::clamp)((std::max)(.5-depth,0.0)*strength-.3,0.0,1.0)));
                    };
                    const int expected=expectedColor(grids);
                    error=(std::max)(error,unsigned(std::abs(expected-int((current[y*kSize+x]>>16)&255))));
                    diagonalControl=(std::max)(diagonalControl,unsigned(std::abs(expected-expectedColor(wrongGrids))));
                }
                ++cases;
            }
            std::printf("AFFINE_EDGE_ORACLE fine_layer=%u mode=%d error=%u ignored_offdiagonal_control=%u\n",
                fineLayer,mode,error,diagonalControl);
            if(error>5 || diagonalControl<8)
                throw std::runtime_error("adjacent progressive affine edge mapping or its negative control failed");
        }
        auto singular=probe;singular.blend[coarseLayer]=1;
        singular.overrideMapping[coarseLayer]=true;singular.layerMapping[coarseLayer]=identity();
        singular.layerMapping[coarseLayer][0]=singular.layerMapping[coarseLayer][5]=0;
        singular.progressive=false;const auto fallback=draw(singular,mode);
        singular.progressive=true;
        if(ProgressiveImageDifference(fallback,draw(singular,mode))>1)
            throw std::runtime_error("singular adjacent coarse mapping did not retain its visibility fallback");
        // Enhanced filtering is unchanged for each of the two new pairs too.
        probe.blend[coarseLayer]=1;probe.layerFilter=probe.receiverFilter=true;
        probe.progressive=false;const auto filtered=draw(probe,mode);
        probe.progressive=true;
        if(ProgressiveImageDifference(filtered,draw(probe,mode))>1)
            throw std::runtime_error("adjacent progressive pair changed enhanced filtering");
    }
    // Traverse all three transitions consecutively, with all four textures
    // live. The far endpoint of one stage is the next stage's fine endpoint.
    unsigned joinedFrames=0;
    for(int mode:{1,2}) {
        BridgeProbe probe;probe.progressive=probe.middleEnabled=probe.nearEnabled=true;
        probe.coveragePattern=1;probe.casterGap=.00032f;probe.backgroundDepth=.5f;
        probe.mapSize[0]=32;probe.mapSize[1]=128;probe.mapSize[2]=8;
        for(unsigned layer=0;layer<3;++layer)probe.texturePattern[layer]=1;
        std::vector<DWORD> previous;
        for(unsigned layer:{1u,0u,2u}) {
            probe.blend[1]=layer==1?1:0;probe.blend[0]=layer==2?0:1;probe.blend[2]=1;
            const double duration=layer==2?3:4;
            for(unsigned step=0;step<=128;++step) {
                probe.blend[layer]=IndependentInverseSmooth(1-double(step)/128);
                const auto current=draw(probe,mode,4);
                if(!previous.empty()) {
                    const unsigned difference=ProgressiveImageDifference(previous,current);
                    if(step==0 && difference>1)
                        throw std::runtime_error("consecutive progressive pairs disagree at their shared physical map");
                    const double strength=mode==2?8000.0*103/kSize:1500.0;
                    const unsigned limit=unsigned(std::ceil(204*strength*probe.casterGap*1.5*duration/128))+4;
                    if(step && difference>limit)
                        throw std::runtime_error("complete four-map progressive traversal has a grid handoff jump");
                    largestStep=(std::max)(largestStep,difference);
                }
                previous=current;++joinedFrames;
            }
        }
    }
    Draw(device,effect,32,1,0,true,true);
    std::printf("PROGRESSIVE_COMPOSITE cases=%u joined_frames=%u fallback_error=%u endpoint_error=%u affine_plane_error=%u max_joined_step=%u\n",
        cases,joinedFrames,worstFallback,worstEndpoint,worstPlane,largestStep);
    if(worstFallback>2 || worstEndpoint>2 || worstPlane>2)
        throw std::runtime_error("adjacent progressive composite, partial-coarse endpoint or shared-Z plane regression");
}

void CheckLayerFilterGpu(IDirect3DDevice9* device,void* effect) {
    struct FilterDiagnostic {
        unsigned maximum=0,x=0,y=0,shift=0,actual=0,expected=0,calls=0;
        int mode=0,pattern=0,layer=-1,step=-1;
        unsigned base=0,padding=0;
        float phase=0;
        unsigned long long pixelsOverTwo=0,channels=0,errorSum=0;
        std::array<unsigned long long,256> histogram{};
    };
    enum { Crossfade,Prefilter,Fallback,FilterOff,Plane,Reset,DiagnosticCount };
    FilterDiagnostic diagnostics[DiagnosticCount];
    const char* diagnosticNames[]{"crossfade","production_prefilter","shader_fallback","filter_off","affine_plane","state_reset"};
    std::vector<std::string> caseEvidence;
    const auto enabled=[](BridgeProbe& probe,unsigned layer,bool value) {
        if(layer==0)probe.middleEnabled=value;
        else if(layer==1)probe.nearEnabled=value;
        else probe.enabled=value;
    };
    const auto difference=[](const std::vector<DWORD>& a,const std::vector<DWORD>& b) {
        unsigned worst=0,changed=0;
        for(unsigned y=32;y<96;++y)for(unsigned x=24;x<104;++x) {
            unsigned error=0;
            for(unsigned shift:{0u,8u,16u})error=(std::max)(error,unsigned(std::abs(
                int((a[y*kSize+x]>>shift)&255)-int((b[y*kSize+x]>>shift)&255))));
            worst=(std::max)(worst,error);changed+=error>3;
        }
        return std::array<unsigned,2>{{worst,changed}};
    };
    const auto record=[&](unsigned path,const std::vector<DWORD>& actual,const std::vector<DWORD>& expected,
            int mode,int pattern,unsigned base,int layer,unsigned padding,float phase,int step) {
        auto& result=diagnostics[path];++result.calls;
        FilterDiagnostic local;
        for(unsigned y=32;y<96;++y)for(unsigned x=24;x<104;++x) {
            unsigned pixelError=0;
            for(unsigned shift:{0u,8u,16u}) {
                const unsigned a=(actual[y*kSize+x]>>shift)&255,b=(expected[y*kSize+x]>>shift)&255;
                const unsigned error=unsigned(std::abs(int(a)-int(b)));
                ++result.histogram[error];++result.channels;result.errorSum+=error;
                pixelError=(std::max)(pixelError,error);
                if(error>local.maximum) {
                    local.maximum=error;local.x=x;local.y=y;local.shift=shift;local.actual=a;local.expected=b;
                }
                if(error>result.maximum) {
                    result.maximum=error;result.x=x;result.y=y;result.shift=shift;result.actual=a;result.expected=b;
                    result.mode=mode;result.pattern=pattern;result.base=base;result.layer=layer;
                    result.padding=padding;result.phase=phase;result.step=step;
                }
            }
            result.pixelsOverTwo+=pixelError>2;local.pixelsOverTwo+=pixelError>2;
        }
        return local;
    };
    if(std::abs(IndependentFilterSideWeight()-mikumikustudio::shadow_filter::kDepthFilterSideWeight)>1e-7f)
        throw std::runtime_error("shared filter changed the original 4096-reference kernel");
    const auto rawBefore=Draw(device,effect,32,1,0,true,true,0,1,0,nullptr,1,false,0,true);
    unsigned cases=0,frames=0,worst=0,offWorst=0,fallbackWorst=0,prefilterWorst=0,rawNegative=0,doubleNegative=0;
    for(int mode:{1,2})for(int pattern:{1,2})for(unsigned baseSize:{16u,32u})for(float phase:{0.f,.37f}) {
        BridgeProbe reference;
        reference.enabled=reference.middleEnabled=reference.nearEnabled=false;
        reference.coveragePattern=pattern;reference.prefilterCoverage=true;
        reference.casterGap=.00032f;
        const auto draw=[&](const BridgeProbe& probe,float slope=0.f) {
            return Draw(device,effect,baseSize,mode,slope,false,true,0,0,phase,
                nullptr,1,false,0,true,0,nullptr,&probe);
        };
        // MME retains the independent CPU depth-convolution oracle. Native
        // receivers use a separate comparison-before-filtering oracle below.
        const auto expectedDepth=draw(reference);
        auto rawReference=reference;rawReference.prefilterCoverage=false;
        const auto expectedRaw=draw(rawReference);
        auto shared=rawReference;shared.productionPrefilterCoverage=true;
        prefilterWorst=(std::max)(prefilterWorst,record(Prefilter,draw(shared),expectedDepth,mode,pattern,baseSize,-1,1,phase,-1).maximum);
        // Resizing the shared allocation adds padding, not texels/world-unit.
        // Its production prefilter must retain the same physical kernel too.
        for(unsigned multiplier:{2u,4u}) {
            auto padded=shared;padded.coverageHalfWidth=float(multiplier);
            const auto enlarged=Draw(device,effect,baseSize*multiplier,mode,0,false,true,0,0,phase,
                nullptr,1,false,0,true,0,nullptr,&padded);
            prefilterWorst=(std::max)(prefilterWorst,record(Prefilter,enlarged,expectedDepth,mode,pattern,baseSize,-1,multiplier,phase,-1).maximum);
        }
        auto fallback=rawReference;fallback.receiverFilter=fallback.layerFilter=true;
        const auto expected=IndependentVisibilityImage(baseSize,mode,phase,fallback);
        fallbackWorst=(std::max)(fallbackWorst,record(Fallback,draw(fallback),expected,mode,pattern,baseSize,-1,1,phase,-1).maximum);
        for(unsigned layer=0;layer<3;++layer)for(unsigned multiplier:{1u,2u,4u}) {
            auto probe=fallback;enabled(probe,layer,true);
            probe.mapSize[layer]=baseSize*multiplier;
            probe.halfWidth[layer]=float(multiplier);
            probe.texturePattern[layer]=pattern;
            if(float(probe.mapSize[layer])/(2*probe.halfWidth[layer])!=float(baseSize)/2)
                throw std::runtime_error("filter fixture changed sampling density while padding a larger texture");
            // A raw, unfiltered layer must be distinguishable from the native
            // comparison reconstruction at the same rho and world phase.
            auto raw=probe;raw.layerFilter=raw.receiverFilter=false;
            const auto unfiltered=draw(raw);
            rawNegative+=difference(unfiltered,expected)[1];
            offWorst=(std::max)(offWorst,record(FilterOff,unfiltered,expectedRaw,mode,pattern,baseSize,int(layer),multiplier,phase,-1).maximum);
            auto twice=probe;twice.prefilterLayer[layer]=true;
            doubleNegative+=difference(draw(twice),expected)[1];
            FilterDiagnostic caseWorst;
            unsigned long long casePixelsOverTwo=0;
            for(unsigned step=0;step<=8;++step) {
                probe.blend[layer]=float(step)/8;
                probe.logBlend[layer]=.8f;
                const auto actual=draw(probe);
                const auto diagnostic=record(Crossfade,actual,expected,mode,pattern,baseSize,int(layer),multiplier,phase,int(step));
                const unsigned error=diagnostic.maximum;
                casePixelsOverTwo+=diagnostic.pixelsOverTwo;
                if(error>caseWorst.maximum){caseWorst=diagnostic;caseWorst.step=int(step);}
                worst=(std::max)(worst,error);
                ++frames;
            }
            char evidence[384]{};
            std::snprintf(evidence,sizeof evidence,"FILTER_CASE mode=%d pattern=%d base=%u layer=%u padding=%u phase=%.3f max=%u step=%d x=%u y=%u shift=%u actual=%u expected=%u pixels_over_2=%llu",
                mode,pattern,baseSize,layer,multiplier,phase,caseWorst.maximum,caseWorst.step,
                caseWorst.x,caseWorst.y,caseWorst.shift,caseWorst.actual,caseWorst.expected,casePixelsOverTwo);
            caseEvidence.emplace_back(evidence);
            ++cases;
        }
    }
    // Receiver-plane compensation must preserve a coplanar receiver across raw
    // native coverage and detail, including independent padded texture sizes.
    unsigned planeCases=0,planeWorst=0;
    const std::vector<DWORD> fullyLit(kSize*kSize,0xffffffffu);
    for(int mode:{1,2})for(unsigned baseSize:{16u,32u})for(float slope:{-.15f,.15f})
        for(unsigned layer=0;layer<3;++layer)for(unsigned multiplier:{1u,2u,4u}) {
            BridgeProbe probe;probe.enabled=probe.middleEnabled=probe.nearEnabled=false;
            enabled(probe,layer,true);probe.layerFilter=probe.receiverFilter=true;
            probe.coveragePattern=probe.texturePattern[layer]=3;probe.planeSlope=slope;
            probe.mapSize[layer]=baseSize*multiplier;probe.halfWidth[layer]=float(multiplier);
            for(float blend:{0.f,.5f,1.f}) {
                probe.blend[layer]=blend;
                const auto actual=Draw(device,effect,baseSize,mode,slope,false,true,0,0,0,
                    nullptr,1,false,0,true,0,nullptr,&probe);
                planeWorst=(std::max)(planeWorst,record(Plane,actual,fullyLit,mode,3,baseSize,int(layer),multiplier,slope,int(blend*2)).maximum);
                ++planeCases;
            }
        }
    // Leave enhancement enabled, then use the ordinary fixture. Both feature
    // switches must reset so the older filter-off parity tests remain meaningful.
    BridgeProbe last;last.enabled=false;last.middleEnabled=true;
    last.layerFilter=last.receiverFilter=true;
    last.coveragePattern=last.texturePattern[0]=1;last.mapSize[0]=32;
    Draw(device,effect,32,1,0,false,true,0,0,0,nullptr,1,false,0,true,0,nullptr,&last);
    const auto rawAfter=Draw(device,effect,32,1,0,true,true,0,1,0,nullptr,1,false,0,true);
    const unsigned resetWorst=record(Reset,rawAfter,rawBefore,1,1,32,-1,1,0,-1).maximum;
    // Diagnostic collection deliberately continues after a mismatch, but the
    // original acceptance limits are retained below. Do not turn this evidence
    // mode into a relaxed pass threshold.
    for(const auto& evidence:caseEvidence)std::puts(evidence.c_str());
    for(unsigned path=0;path<DiagnosticCount;++path) {
        const auto& d=diagnostics[path];
        unsigned long long fourToSeven=0,eightOrMore=0;
        for(unsigned error=4;error<256;++error)(error<8?fourToSeven:eightOrMore)+=d.histogram[error];
        std::printf("FILTER_DIAG path=%s calls=%u max=%u mean=%.9f pixels_over_2=%llu channel_hist_0_1_2_3_4to7_8plus=%llu,%llu,%llu,%llu,%llu,%llu mode=%d pattern=%d base=%u layer=%d padding=%u phase=%.6f step=%d x=%u y=%u shift=%u actual=%u expected=%u\n",
            diagnosticNames[path],d.calls,d.maximum,d.channels?double(d.errorSum)/double(d.channels):0,
            d.pixelsOverTwo,d.histogram[0],d.histogram[1],d.histogram[2],d.histogram[3],fourToSeven,eightOrMore,
            d.mode,d.pattern,d.base,d.layer,d.padding,d.phase,d.step,d.x,d.y,d.shift,d.actual,d.expected);
    }
    std::printf("FILTER_TOTAL cases=%u frames=%u planes=%u crossfade=%u prefilter=%u fallback=%u off=%u plane=%u reset=%u raw_negative=%u double_negative=%u acceptance_limits_unchanged=1\n",
        cases,frames,planeCases,worst,prefilterWorst,fallbackWorst,offWorst,planeWorst,resetWorst,rawNegative,doubleNegative);
    if(worst>2)throw std::runtime_error("filtered layer hand-off changed an equal-density world-space shadow");
    if(planeWorst>1)throw std::runtime_error("uniform layer filtering invented self-shadow on a sloping receiver");
    if(resetWorst>1 || offWorst>2 || fallbackWorst>2 || prefilterWorst>2)
        throw std::runtime_error("filter-off parity, default state reset or shared-filter fallback changed pixels");
    if(rawNegative<100 || doubleNegative<100)
        throw std::runtime_error("filter consistency probes cannot detect raw-detail or double-filter regressions");
    std::printf("PASS uniform layer filter: cases=%u crossfade_frames=%u planes=%u max_difference=%u off_difference=%u fallback_difference=%u prefilter_difference=%u raw_negative_pixels=%u double_negative_pixels=%u\n",
        cases,frames,planeCases,worst,offWorst,fallbackWorst,prefilterWorst,rawNegative,doubleNegative);
}

void CheckNativeVisibilityGeometryGpu(IDirect3DDevice9* device,void* effect) {
    unsigned cases=0,worst=0,backgroundWorst=0,largestStep=0,planeWorst=0,triangleWorst=0,sharedIsolationWorst=0;
    double recoveredArea=0;
    const auto compare=[](const std::vector<DWORD>& a,const std::vector<DWORD>& b,unsigned inset=24) {
        unsigned maximum=0;
        for(unsigned y=inset;y<kSize-inset;++y)for(unsigned x=inset;x<kSize-inset;++x)
            for(unsigned shift:{0u,8u,16u})maximum=(std::max)(maximum,unsigned(std::abs(
                int((a[y*kSize+x]>>shift)&255)-int((b[y*kSize+x]>>shift)&255))));
        return maximum;
    };
    const auto area=[](const std::vector<DWORD>& image) {
        double sum=0;
        for(unsigned y=24;y<104;++y)for(unsigned x=24;x<104;++x)sum+=(255-int(image[y*kSize+x]&255))/204.0;
        return sum;
    };
    // An MME-compatible stage-zero binding must not alter enhanced native
    // coverage. Deliberately replace it with impossible zero depth while the
    // native raw source remains unchanged. Filter-off must still observe it,
    // proving that this fixture can detect accidental stage-zero reads.
    for(int mode:{1,2})for(int pattern:{1,2}) {
        BridgeProbe probe;probe.enabled=probe.middleEnabled=probe.nearEnabled=false;
        probe.layerFilter=probe.receiverFilter=true;probe.coveragePattern=pattern;
        const auto draw=[&](const BridgeProbe& value) {
            return Draw(device,effect,32,mode,0,false,true,0,0,.37f,nullptr,1,false,0,true,0,nullptr,&value);
        };
        const auto native=draw(probe);
        probe.poisonSharedCoverage=true;
        sharedIsolationWorst=(std::max)(sharedIsolationWorst,compare(native,draw(probe)));
        probe.layerFilter=probe.receiverFilter=false;
        const auto poisonedFixed=draw(probe);
        probe.poisonSharedCoverage=false;
        if(compare(poisonedFixed,draw(probe))<32)
            throw std::runtime_error("shared coverage control cannot detect an accidental stage-zero dependency");
        ++cases;
    }
    for(int mode:{1,2})for(unsigned size:{16u,32u})for(int pattern:{1,2}) {
        std::vector<DWORD> previous;
        for(unsigned step=0;step<=8;++step) {
            const float phase=float(step)/8;
            BridgeProbe probe;probe.enabled=probe.middleEnabled=probe.nearEnabled=false;
            probe.layerFilter=probe.receiverFilter=true;probe.coveragePattern=pattern;
            probe.casterGap=.00032f;probe.stripeHalfWidth=.043f;
            const auto draw=[&](const BridgeProbe& value) {
                return Draw(device,effect,size,mode,0,false,true,0,0,phase,nullptr,1,false,0,true,0,nullptr,&value);
            };
            std::vector<DWORD> backgroundReference,current;
            for(float background:{.501f,.75f,1.f}) {
                probe.backgroundDepth=background;current=draw(probe);
                const auto expected=IndependentVisibilityImage(size,mode,phase,probe);
                worst=(std::max)(worst,compare(current,expected));
                if(backgroundReference.empty())backgroundReference=current;
                else backgroundWorst=(std::max)(backgroundWorst,compare(current,backgroundReference));
                ++cases;
            }
            if(!previous.empty())largestStep=(std::max)(largestStep,compare(current,previous));
            previous=current;
            if(pattern==2) {
                auto old=probe;old.layerFilter=old.receiverFilter=false;old.productionPrefilterCoverage=true;
                const double gain=area(current)-area(draw(old));
                if(gain<=2)throw std::runtime_error("native comparison filtering still erases narrow cast shadows like depth averaging");
                recoveredArea+=gain;
            }
        }
    }
    // Independently compare oblique receivers with an actual caster above the
    // same plane, and change the receiver's triangle diagonal without changing
    // its geometry. This detects missing/wrong per-texel plane compensation.
    for(int mode:{1,2})for(unsigned size:{16u,32u})for(int pattern:{1,2}) {
        BridgeProbe probe;probe.enabled=probe.middleEnabled=probe.nearEnabled=false;
        probe.layerFilter=probe.receiverFilter=true;probe.coveragePattern=pattern;
        probe.casterGap=.00032f;probe.planeSlope=.05f;probe.planeSlopeY=.03f;
        const auto expected=IndependentVisibilityImage(size,mode,0,probe,probe.planeSlope);
        std::vector<DWORD> original;
        for(bool reverse:{false,true}) {
            probe.reverseDiagonal=reverse;
            const auto current=Draw(device,effect,size,mode,probe.planeSlope,false,true,0,0,0,
                nullptr,1,false,0,true,0,nullptr,&probe);
            worst=(std::max)(worst,compare(current,expected));
            if(!reverse)original=current;
            else triangleWorst=(std::max)(triangleWorst,compare(current,original));
            ++cases;
        }
    }
    const std::vector<DWORD> lit(kSize*kSize,0xffffffffu);
    for(int mode:{1,2})for(unsigned size:{4u,16u,64u})for(float slope:{-.4f,.15f})
        for(float slopeY:{-.07f,.11f})for(bool reverse:{false,true}) {
            BridgeProbe probe;probe.enabled=probe.middleEnabled=probe.nearEnabled=false;
            probe.layerFilter=probe.receiverFilter=true;probe.coveragePattern=3;
            probe.planeSlope=slope;probe.planeSlopeY=slopeY;probe.reverseDiagonal=reverse;
            const auto current=Draw(device,effect,size,mode,slope,false,true,0,0,0,
                nullptr,1,false,0,true,0,nullptr,&probe);
            planeWorst=(std::max)(planeWorst,compare(current,lit,size==4?48:24));
            ++cases;
        }
    for(int mode:{1,2}) {
        BridgeProbe probe;probe.enabled=probe.middleEnabled=probe.nearEnabled=false;
        probe.layerFilter=probe.receiverFilter=true;probe.coveragePattern=3;probe.degenerateUV=true;
        const auto current=Draw(device,effect,16,mode,0,false,true,0,0,0,nullptr,1,false,0,true,0,nullptr,&probe);
        planeWorst=(std::max)(planeWorst,compare(current,lit));++cases;
    }
    std::printf("VISIBILITY_GEOMETRY cases=%u oracle_max=%u background_max=%u phase_max_step=%u plane_max=%u triangle_max=%u shared_isolation_max=%u recovered_strip_area=%.6f\n",
        cases,worst,backgroundWorst,largestStep,planeWorst,triangleWorst,sharedIsolationWorst,recoveredArea);
    if(worst>2 || backgroundWorst>1 || planeWorst>1 || triangleWorst>1 || sharedIsolationWorst>1)
        throw std::runtime_error("native visibility reconstruction changed geometry, background independence or coplanar triangle shading");
    // A 1/8-texel translation of a convex reconstruction cannot change a
    // channel by more than 1/8 of the full 204-level shadow range, plus rounding.
    if(largestStep>28)throw std::runtime_error("subtexel shadow motion has a discontinuity beyond its reconstruction bound");
    std::puts("PASS native visibility geometry: independent comparisons, background invariance, narrow projections, receiver planes and subtexel motion");
}

double ShadowImageValue(DWORD pixel) {return (double((pixel>>16)&255)-51)/204;}
double ShadowImageValue(double pixel) {return pixel;}
template<class Pixel>
double ShadowImageVisibility(const std::vector<Pixel>& pixels,double x,double y) {
    x=(std::clamp)(x,0.0,double(kSize-1));y=(std::clamp)(y,0.0,double(kSize-1));
    const unsigned ix=unsigned(x),iy=unsigned(y),jx=(std::min)(ix+1,kSize-1),jy=(std::min)(iy+1,kSize-1);
    const double fx=x-ix,fy=y-iy;
    const auto value=[&](unsigned px,unsigned py) {return ShadowImageValue(pixels[py*kSize+px]);};
    return (1-fy)*((1-fx)*value(ix,iy)+fx*value(jx,iy))+fy*((1-fx)*value(ix,jy)+fx*value(jx,jy));
}

template<class Pixel>
double ShadowContourCrossing(const std::vector<Pixel>& pixels,double x,double y,
    double nx,double ny,double level) {
    // Measure the rendered contour along the analytic edge normal. This uses
    // only image geometry; it does not duplicate the directional filter weights.
    constexpr double start=-16,step=.25;
    double previous=ShadowImageVisibility(pixels,x+start*nx,y+start*ny);
    for(unsigned i=1;i<=128;++i) {
        const double distance=start+step*i;
        const double current=ShadowImageVisibility(pixels,x+distance*nx,y+distance*ny);
        if(previous<=level && current>level)
            return distance-step+step*(level-previous)/(current-previous);
        previous=current;
    }
    throw std::runtime_error("directional geometry fixture has no measurable silhouette crossing");
}

struct ShadowContourMetrics {double rms=0,ripple=0,width=0,offset=0;};
template<class Pixel>
ShadowContourMetrics MeasureShadowContour(const std::vector<Pixel>& pixels,float slope,unsigned size,float phase) {
    const double normalLength=std::sqrt(1+double(slope)*slope);
    const double nx=1/normalLength,ny=-slope/normalLength,tx=-ny,ty=nx;
    const double centreX=(1+.083)*kSize*.5-double(phase)*kSize/size,centreY=kSize*.5;
    double sum=0,squared=0,width=0;unsigned count=0;
    for(int along=-24;along<=24;++along) {
        const double x=centreX+along*tx,y=centreY+along*ty;
        const double half=ShadowContourCrossing(pixels,x,y,nx,ny,.5);
        sum+=half;squared+=half*half;
        width+=ShadowContourCrossing(pixels,x,y,nx,ny,.9)-ShadowContourCrossing(pixels,x,y,nx,ny,.1);
        ++count;
    }
    const double mean=sum/count;
    return {std::sqrt(squared/count),std::sqrt((std::max)(0.0,squared/count-mean*mean)),width/count,mean};
}

std::vector<double> IsotropicShadowControl(const std::vector<DWORD>& source,double sigma) {
    // An independent isotropic screen-space control: broadening every edge
    // equally must not be mistaken for better subtexel contour reconstruction.
    const int radius=int(std::ceil(sigma*4));
    std::vector<double> weights(2*radius+1),temporary(kSize*kSize),result(kSize*kSize);
    double sum=0;
    for(int i=-radius;i<=radius;++i)sum+=weights[i+radius]=std::exp(-.5*i*i/(sigma*sigma));
    for(auto& weight:weights)weight/=sum;
    for(unsigned y=0;y<kSize;++y)for(unsigned x=0;x<kSize;++x)
        for(int i=-radius;i<=radius;++i)
            temporary[y*kSize+x]+=weights[i+radius]*ShadowImageValue(source[y*kSize+unsigned((std::clamp)(int(x)+i,0,int(kSize)-1))]);
    for(unsigned y=0;y<kSize;++y)for(unsigned x=0;x<kSize;++x)
        for(int i=-radius;i<=radius;++i)
            result[y*kSize+x]+=weights[i+radius]*temporary[unsigned((std::clamp)(int(y)+i,0,int(kSize)-1))*kSize+x];
    return result;
}

ShadowContourMetrics MatchIsotropicShadowWidth(const std::vector<DWORD>& source,const ShadowContourMetrics& original,
    float slope,unsigned size,float phase,double wantedWidth,double& error) {
    if(wantedWidth<=original.width) {error=0;return original;}
    double low=.01,high=8;
    ShadowContourMetrics result;
    for(unsigned iteration=0;iteration<12;++iteration) {
        const double sigma=(low+high)*.5;
        result=MeasureShadowContour(IsotropicShadowControl(source,sigma),slope,size,phase);
        if(result.width<wantedWidth)low=sigma;else high=sigma;
    }
    error=std::abs(result.width/wantedWidth-1);
    if(error>.01)throw std::runtime_error("isotropic control failed to match the measured normal edge width within one percent");
    return result;
}

std::vector<double> NormalizePartialShadowImage(const std::vector<DWORD>& image,int mode,float gap) {
    // Remove only the analytically known original mode amplitude. This lets
    // soft partial shadows use the same geometric contour measurements as
    // saturated building edges without confusing mode-2 strength with blur.
    std::vector<double> result(image.size());
    for(unsigned y=0;y<kSize;++y)for(unsigned x=0;x<kSize;++x) {
        const double amplitude=(std::clamp)(double(gap)*(mode==2?8000.0*y/kSize:1500.0)-.3,0.0,1.0);
        result[y*kSize+x]=amplitude>.05?1-(1-ShadowImageValue(image[y*kSize+x]))/amplitude:1;
    }
    return result;
}

void CheckDirectionalShadowGeometryGpu(IDirect3DDevice9* device,void* effect) {
    const auto difference=[](const std::vector<DWORD>& a,const std::vector<DWORD>& b) {
        unsigned maximum=0;
        for(unsigned y=24;y<104;++y)for(unsigned x=24;x<104;++x)
            for(unsigned shift:{0u,8u,16u})maximum=(std::max)(maximum,unsigned(std::abs(
                int((a[y*kSize+x]>>shift)&255)-int((b[y*kSize+x]>>shift)&255))));
        return maximum;
    };
    const auto area=[](const std::vector<DWORD>& image,unsigned inset=24) {
        double sum=0,peak=0;
        for(unsigned y=inset;y<kSize-inset;++y)for(unsigned x=inset;x<kSize-inset;++x) {
            const double occlusion=(255-int((image[y*kSize+x]>>16)&255))/204.0;
            sum+=occlusion;peak=(std::max)(peak,occlusion);
        }
        return std::array<double,2>{sum,peak};
    };
    const auto draw=[&](BridgeProbe probe,unsigned size,int mode,float phase) {
        return Draw(device,effect,size,mode,probe.planeSlope,false,true,0,0,phase,
            nullptr,1,false,0,true,0,nullptr,&probe);
    };
    BridgeProbe prototype;prototype.enabled=prototype.middleEnabled=prototype.nearEnabled=false;
    prototype.layerFilter=prototype.receiverFilter=true;prototype.casterGap=.002f;
    unsigned cases=0,qualityFailures=0,changed=0,keyGroups=0,isotropicCases=0;
    double oldRms=0,newRms=0,oldRipple=0,newRipple=0,maxWidthRatio=0,maxRippleGrowth=0,maxIsotropicMatchError=0;
    for(int mode:{1,2})for(unsigned size:{16u,32u})for(float slope:{.125f,.15f,.25f,.35f,.7f,1.f,1.3f,3.f,4.f,8.f}) {
        const bool keySlope=slope==.125f || slope==.25f || slope==4.f || slope==8.f;
        double groupOld=0,groupNew=0,groupWidth=0,groupIsotropic=0;
        for(unsigned step=0;step<8;++step) {
            const float phase=float(step)/8;
            auto probe=prototype;probe.coveragePattern=1;probe.edgeSlope=slope;
            const auto baseline=draw(probe,size,mode,phase);
            probe.directional=true;
            const auto directional=draw(probe,size,mode,phase);
            const auto before=MeasureShadowContour(baseline,slope,size,phase);
            const auto after=MeasureShadowContour(directional,slope,size,phase);
            oldRms+=before.rms;newRms+=after.rms;oldRipple+=before.ripple;newRipple+=after.ripple;
            groupOld+=before.ripple;groupNew+=after.ripple;groupWidth+=after.width/before.width;
            if(keySlope) {
                double matchError=0;
                const auto isotropic=MatchIsotropicShadowWidth(baseline,before,slope,size,phase,after.width,matchError);
                groupIsotropic+=isotropic.ripple;
                maxIsotropicMatchError=(std::max)(maxIsotropicMatchError,matchError);
                ++isotropicCases;
            }
            maxWidthRatio=(std::max)(maxWidthRatio,after.width/before.width);
            maxRippleGrowth=(std::max)(maxRippleGrowth,after.ripple-before.ripple);
            changed+=difference(baseline,directional)>1;
            if(after.rms>before.rms*1.08+.08 || after.ripple>before.ripple*1.08+.02 ||
               after.width>before.width*1.25+.05 ||
               std::abs(after.offset)>std::abs(before.offset)+double(kSize)/size*.15) {
                ++qualityFailures;
                std::printf("DIRECTION_EDGE_FAIL mode=%d size=%u slope=%.3f phase=%.3f rms=%.6f->%.6f ripple=%.6f->%.6f width=%.6f->%.6f offset=%.6f->%.6f\n",
                    mode,size,slope,phase,before.rms,after.rms,before.ripple,after.ripple,before.width,after.width,before.offset,after.offset);
            }
            ++cases;
        }
        if(keySlope) {
            ++keyGroups;
            if(groupNew>groupOld*.7 || groupNew>groupIsotropic*.85)++qualityFailures;
        }
        std::printf("DIRECTION_EDGE mode=%d size=%u slope=%.3f key=%u ripple=%.6f->%.6f isotropic_ripple=%.6f width_ratio=%.6f\n",
            mode,size,slope,keySlope,groupOld/8,groupNew/8,keySlope?groupIsotropic/8:0,groupWidth/8);
    }
    unsigned portraitCases=0,portraitFailures=0,plateauWorst=0;
    double portraitMaxWidth=0,portraitMaxShift=0;
    for(int mode:{1,2})for(unsigned size:{64u,128u})for(float slope:{.35f,3.f})
        for(float gap:{.00032f,.0006f})for(float phase:{0.f,.37f}) {
            auto probe=prototype;probe.coveragePattern=1;probe.edgeSlope=slope;probe.casterGap=gap;
            const auto baseline=draw(probe,size,mode,phase);probe.directional=true;
            const auto directional=draw(probe,size,mode,phase);
            const auto before=MeasureShadowContour(NormalizePartialShadowImage(baseline,mode,gap),slope,size,phase);
            const auto after=MeasureShadowContour(NormalizePartialShadowImage(directional,mode,gap),slope,size,phase);
            portraitMaxWidth=(std::max)(portraitMaxWidth,after.width/before.width);
            portraitMaxShift=(std::max)(portraitMaxShift,std::abs(after.offset-before.offset)/(double(kSize)/size));
            if(after.width>before.width*1.15+.05 ||
               std::abs(after.offset-before.offset)>double(kSize)/size*.15+.05) {
                ++portraitFailures;
                std::printf("CONTOUR_PORTRAIT_FAIL mode=%d size=%u slope=%.3f phase=%.3f gap=%.7f width=%.6f->%.6f offset=%.6f->%.6f\n",
                    mode,size,slope,phase,gap,before.width,after.width,before.offset,after.offset);
            }
            // Deep inside a shadow, reconstruction must not change the
            // original continuous native ramp or its mode-2 screen-Y factor.
            for(unsigned y=24;y<104;++y)for(unsigned x=24;x<104;++x) {
                const double worldX=2.0*x/kSize-1+2*double(phase)/size,worldY=1-2.0*y/kSize;
                if((worldX+slope*worldY-.083)/std::sqrt(1+double(slope)*slope)>-8.0/size)continue;
                const double amplitude=(std::clamp)(double(gap)*(mode==2?8000.0*y/kSize:1500.0)-.3,0.0,1.0);
                const int expected=int(std::lround(255-204*amplitude));
                plateauWorst=(std::max)(plateauWorst,unsigned(std::abs(int((directional[y*kSize+x]>>16)&255)-expected)));
            }
            ++portraitCases;
        }
    unsigned shapeFailures=0,shapeCases=0;
    double minStripArea=1,maxStripArea=1,minStripPeak=1,maxCornerAreaChange=0,maxCornerDrift=0;
    for(int mode:{1,2})for(unsigned size:{16u,32u})for(float slope:{.15f,.7f,3.f})
        for(float texelWidth:{.7f,1.5f})for(float phase:{0.f,.25f,.5f,.75f}) {
            auto probe=prototype;probe.coveragePattern=2;probe.edgeSlope=slope;
            // Width is specified perpendicular to the strip in shadow texels.
            probe.stripeHalfWidth=texelWidth/size*std::sqrt(1+slope*slope);
            const auto baseline=draw(probe,size,mode,phase);probe.directional=true;
            const auto directional=draw(probe,size,mode,phase);
            const auto before=area(baseline),after=area(directional);
            minStripArea=(std::min)(minStripArea,after[0]/before[0]);maxStripArea=(std::max)(maxStripArea,after[0]/before[0]);
            minStripPeak=(std::min)(minStripPeak,after[1]/before[1]);
            if(after[0]<before[0]*.95-1 || after[0]>before[0]*1.05+1 || after[1]<before[1]*.9-.01) {
                ++shapeFailures;
                std::printf("DIRECTION_STRIP_FAIL mode=%d size=%u slope=%.3f phase=%.3f width=%.3f area=%.6f->%.6f peak=%.6f->%.6f\n",
                    mode,size,slope,phase,texelWidth,before[0],after[0],before[1],after[1]);
            }
            ++shapeCases;
        }
    // Character-size, still-resolvable features at one or two screen pixels
    // per shadow texel must not disappear as the coarse building kernel changes.
    for(int mode:{1,2})for(unsigned size:{64u,128u})for(float texelWidth:{1.5f,3.f})for(float phase:{0.f,.5f}) {
        auto probe=prototype;probe.coveragePattern=2;
        probe.stripeHalfWidth=texelWidth/size*std::sqrt(1+probe.edgeSlope*probe.edgeSlope);
        const auto baseline=draw(probe,size,mode,phase);probe.directional=true;
        const auto directional=draw(probe,size,mode,phase);
        const auto before=area(baseline),after=area(directional);
        minStripArea=(std::min)(minStripArea,after[0]/before[0]);maxStripArea=(std::max)(maxStripArea,after[0]/before[0]);
        minStripPeak=(std::min)(minStripPeak,after[1]/before[1]);
        if(after[0]<before[0]*.95-1 || after[0]>before[0]*1.05+1 || after[1]<before[1]*.9-.01) {
            ++shapeFailures;
            std::printf("CONTOUR_PORTRAIT_STRIP_FAIL mode=%d size=%u phase=%.3f width=%.3f area=%.6f->%.6f peak=%.6f->%.6f\n",
                mode,size,phase,texelWidth,before[0],after[0],before[1],after[1]);
        }
        ++shapeCases;
    }
    for(int mode:{1,2})for(unsigned size:{16u,32u})for(float phase:{0.f,.25f,.5f,.75f}) {
        auto probe=prototype;probe.coveragePattern=4;
        const auto baseline=draw(probe,size,mode,phase);probe.directional=true;
        const auto directional=draw(probe,size,mode,phase);
        const auto before=area(baseline),after=area(directional);
        const double cx=(1+.083)*kSize*.5-double(phase)*kSize/size,cy=(1+.071)*kSize*.5;
        const double oldCorner=ShadowContourCrossing(baseline,cx,cy,std::sqrt(.5),-std::sqrt(.5),.5);
        const double newCorner=ShadowContourCrossing(directional,cx,cy,std::sqrt(.5),-std::sqrt(.5),.5);
        maxCornerAreaChange=(std::max)(maxCornerAreaChange,std::abs(after[0]/before[0]-1));
        maxCornerDrift=(std::max)(maxCornerDrift,std::abs(newCorner)-std::abs(oldCorner));
        if(std::abs(after[0]-before[0])>before[0]*.03+1 || std::abs(newCorner)>std::abs(oldCorner)+.25) {
            ++shapeFailures;
            std::printf("DIRECTION_CORNER_FAIL mode=%d size=%u phase=%.3f area=%.6f->%.6f corner=%.6f->%.6f\n",
                mode,size,phase,before[0],after[0],oldCorner,newCorner);
        }
        ++shapeCases;
    }
    unsigned fallbackWorst=0,planeWorst=0,triangleWorst=0,isolationWorst=0,backgroundWorst=0,handoffWorst=0;
    unsigned temporalCases=0,temporalFailures=0,stripMotionCases=0;
    double largestContourStep=0,largestAreaStep=0;
    for(int mode:{1,2})for(int pattern:{1,2,4})for(float phase:{0.f,.37f}) {
        auto probe=prototype;probe.coveragePattern=pattern;
        const auto baseline=draw(probe,256,mode,phase);probe.directional=true;
        fallbackWorst=(std::max)(fallbackWorst,difference(baseline,draw(probe,256,mode,phase)));
    }
    for(int mode:{1,2})for(unsigned size:{16u,32u})for(int pattern:{1,2,4}) {
        auto probe=prototype;probe.coveragePattern=pattern;probe.directional=true;
        const auto reference=draw(probe,size,mode,.37f);
        for(float background:{.501f,.75f}) {
            probe.backgroundDepth=background;
            backgroundWorst=(std::max)(backgroundWorst,difference(reference,draw(probe,size,mode,.37f)));
        }
        probe.backgroundDepth=1;
        for(unsigned layer=0;layer<3;++layer)for(unsigned padding:{1u,2u}) {
            auto detail=probe;
            detail.middleEnabled=layer==0;detail.nearEnabled=layer==1;detail.enabled=layer==2;
            detail.texturePattern[layer]=pattern;detail.mapSize[layer]=size*padding;detail.halfWidth[layer]=float(padding);
            detail.logBlend[layer]=.8f;
            for(float weight:{0.f,.5f,1.f}) {
                detail.blend[layer]=weight;
                handoffWorst=(std::max)(handoffWorst,difference(reference,draw(detail,size,mode,.37f)));
            }
        }
    }
    const std::vector<DWORD> lit(kSize*kSize,0xffffffffu);
    for(int mode:{1,2})for(unsigned size:{16u,32u})for(float slope:{-.15f,.15f}) {
        auto probe=prototype;probe.coveragePattern=3;probe.directional=true;
        probe.planeSlope=slope;probe.planeSlopeY=.05f;
        const auto plane=draw(probe,size,mode,0);
        planeWorst=(std::max)(planeWorst,difference(plane,lit));
        probe.reverseDiagonal=true;
        triangleWorst=(std::max)(triangleWorst,difference(plane,draw(probe,size,mode,0)));
        probe.coveragePattern=1;probe.planeSlope=probe.planeSlopeY=0;
        const auto native=draw(probe,size,mode,.37f);probe.poisonSharedCoverage=true;
        isolationWorst=(std::max)(isolationWorst,difference(native,draw(probe,size,mode,.37f)));
    }
    for(int mode:{1,2})for(unsigned size:{16u,32u}) {
        auto probe=prototype;probe.coveragePattern=1;
        std::vector<DWORD> previousBase,previousDirectional;
        unsigned baseStep=0,directionalStep=0;double previousOffset=0;
        for(unsigned step=0;step<=16;++step) {
            const float phase=float(step)/16;
            probe.directional=false;const auto baseline=draw(probe,size,mode,phase);
            probe.directional=true;const auto directional=draw(probe,size,mode,phase);
            const auto contour=MeasureShadowContour(directional,probe.edgeSlope,size,phase);
            if(!previousBase.empty()) {
                baseStep=(std::max)(baseStep,difference(baseline,previousBase));
                directionalStep=(std::max)(directionalStep,difference(directional,previousDirectional));
                const double jump=std::abs(contour.offset-previousOffset);
                largestContourStep=(std::max)(largestContourStep,jump);
                if(jump>double(kSize)/size*.15)++temporalFailures;
            }
            previousBase=baseline;previousDirectional=directional;previousOffset=contour.offset;++temporalCases;
        }
        // A sharper edge legitimately changes pixel intensity faster. The
        // known translation has already been removed from contour.offset;
        // only geometric discontinuities fail this motion test.
        std::printf("DIRECTION_MOTION mode=%d size=%u pixel_step=%u->%u\n",mode,size,baseStep,directionalStep);
    }
    for(int mode:{1,2})for(unsigned size:{16u,64u})for(float texelWidth:{1.5f,3.f}) {
        auto probe=prototype;probe.coveragePattern=2;probe.directional=true;
        probe.stripeHalfWidth=texelWidth/size*std::sqrt(1+probe.edgeSlope*probe.edgeSlope);
        double previousArea=0;
        for(unsigned step=0;step<=16;++step) {
            const auto image=draw(probe,size,mode,float(step)/16);
            const double currentArea=area(image,16)[0];
            if(step) {
                const double delta=std::abs(currentArea-previousArea);
                largestAreaStep=(std::max)(largestAreaStep,delta/(.5*(currentArea+previousArea)));
                // Translation does not change the strip's geometric area.
                // Allow two readback-pixel equivalents for 8-bit rounding.
                if(delta>.005*.5*(currentArea+previousArea)+2)++temporalFailures;
            }
            previousArea=currentArea;++stripMotionCases;
        }
    }
    std::printf("CONTOUR_TOTAL edges=%u key_groups=%u rms=%.6f->%.6f ripple=%.6f->%.6f max_width_ratio=%.6f max_ripple_growth=%.6f changed=%u edge_failures=%u isotropic_cases=%u isotropic_match_error=%.6f portrait_cases=%u portrait_width_ratio=%.6f portrait_shift_texels=%.6f portrait_failures=%u plateau_error=%u shapes=%u strip_area_ratio=%.6f..%.6f strip_peak_min=%.6f corner_area_change=%.6f corner_extra_drift=%.6f shape_failures=%u fallback=%u plane=%u triangle=%u isolation=%u background=%u handoff=%u motion_frames=%u contour_step=%.6f strip_motion_frames=%u strip_area_step=%.6f motion_failures=%u\n",
        cases,keyGroups,oldRms/cases,newRms/cases,oldRipple/cases,newRipple/cases,maxWidthRatio,maxRippleGrowth,changed,qualityFailures,
        isotropicCases,maxIsotropicMatchError,portraitCases,portraitMaxWidth,portraitMaxShift,portraitFailures,plateauWorst,
        shapeCases,minStripArea,maxStripArea,minStripPeak,maxCornerAreaChange,maxCornerDrift,shapeFailures,
        fallbackWorst,planeWorst,triangleWorst,isolationWorst,backgroundWorst,handoffWorst,
        temporalCases,largestContourStep,stripMotionCases,largestAreaStep,temporalFailures);
    if(!changed || qualityFailures || newRms>oldRms*1.01 || newRipple>oldRipple*.97)
        throw std::runtime_error("directional filtering did not improve geometric edge ripple without shifting or blurring silhouettes");
    if(portraitFailures || plateauWorst>2 || shapeFailures || fallbackWorst>1 || planeWorst>1 || triangleWorst>1 ||
       isolationWorst>1 || backgroundWorst>1 || handoffWorst>2 || temporalFailures)
        throw std::runtime_error("directional filtering damaged thin projections, corners, subtexel continuity or native-source isolation");
    Check(FxSetInt(effect,"MdsDirectionalShadowFilter",TRUE),"Restore production directional filtering");
    std::puts("PASS directional shadow geometry: analytic contours, controlled normal width, thin shapes, corners, minification, planes and continuous translation");
}

void Save(const std::filesystem::path& path,const std::vector<DWORD>& pixels) {
    std::ofstream out(path,std::ios::binary);out<<"P6\n"<<kSize<<" "<<kSize<<"\n255\n";
    for(auto pixel:pixels){const char rgb[]{char(pixel>>16),char(pixel>>8),char(pixel)};out.write(rgb,3);}
    if(!out)throw std::runtime_error("Cannot write GPU readback image");
}
void CheckModeContrastGpu(IDirect3DDevice9* device,void* effect,void* original,const std::filesystem::path& output) {
    using namespace mikumikustudio;
    using Matrix=std::array<float,16>;
    const auto identity=[] {Matrix m{};for(int i=0;i<4;++i)m[5*i]=1;return m;};
    const auto multiply=[](const Matrix& a,const Matrix& b) {
        Matrix out{};
        for(int row=0;row<4;++row)for(int column=0;column<4;++column) {
            double value=0;
            for(int k=0;k<4;++k)value+=double(a[4*row+k])*b[4*k+column];
            out[4*row+column]=float(value);
        }
        return out;
    };
    const auto inverse=[](const Matrix& matrix) {
        double rows[4][8]{};
        for(int row=0;row<4;++row)for(int column=0;column<4;++column)rows[row][column]=matrix[4*row+column];
        for(int i=0;i<4;++i)rows[i][i+4]=1;
        for(int column=0;column<4;++column) {
            int pivot=column;
            for(int row=column+1;row<4;++row)if(std::abs(rows[row][column])>std::abs(rows[pivot][column]))pivot=row;
            if(std::abs(rows[pivot][column])<1e-12)throw std::runtime_error("physical mode fixture has a singular coverage map");
            for(int k=0;k<8;++k)std::swap(rows[pivot][k],rows[column][k]);
            const double divisor=rows[column][column];
            for(double& value:rows[column])value/=divisor;
            for(int row=0;row<4;++row)if(row!=column) {
                const double factor=rows[row][column];
                for(int k=0;k<8;++k)rows[row][k]-=factor*rows[column][k];
            }
        }
        Matrix out{};
        for(int row=0;row<4;++row)for(int column=0;column<4;++column)out[4*row+column]=float(rows[row][column+4]);
        return out;
    };
    auto centre=identity();centre[5]=0; // Original shader samples texture v=.5 everywhere.
    const float focus[]{0,0,0};
    const std::array<std::array<float,3>,3> lights{{{-.5f,-1,.5f},{0,0,1},{.5267827f,0,-.85f}}};
    unsigned cases=0,worst=0,contrastingPairs=0;
    for(unsigned lightIndex=0;lightIndex<lights.size();++lightIndex) {
        const auto& light=lights[lightIndex];
        const auto unbounded=NativeShadowProjection(focus,light.data(),.1f,1);
        auto localToWorld=identity();
        for(int axis=0;axis<3;++axis) {
            const double length=std::sqrt(double(unbounded[axis])*unbounded[axis]+
                double(unbounded[4+axis])*unbounded[4+axis]+double(unbounded[8+axis])*unbounded[8+axis]);
            for(int row=0;row<3;++row)localToWorld[axis*4+row]=float(unbounded[row*4+axis]/length);
        }
        for(float gap:{0.f,.1f,.25f,.5f,1.f}) {
            std::array<std::vector<DWORD>,2> expected;
            for(int mode:{1,2}) {
                const float originalGap=float(OriginalCentreDepthDifference(light.data(),mode,gap));
                expected[mode-1]=Draw(device,original,128,mode,0,false,true,originalGap,0,0,nullptr,1,
                    false,0,false,0,nullptr,nullptr,nullptr,&centre);
            }
            for(float extent:{10.f,100.f,1000.f})for(int fit:{0,1,2}) {
                NativeShadowBounds scene;
                const float low[]{-extent,-extent,-extent},high[]{extent,extent,extent};
                scene.Include(low);scene.Include(high);
                auto coverage=fit?NativeShadowProjection(focus,light.data(),.1f,1,&scene):unbounded;
                if(fit==2) {
                    NativeShadowBounds visible;
                    const float visibleLow[]{-.3f,-.3f,0},visibleHigh[]{.3f,.3f,1};
                    visible.Include(visibleLow);visible.Include(visibleHigh);
                    coverage=FitVisibleShadowBox(coverage,visible);
                }
                const auto placedCoverage=multiply(localToWorld,coverage);
                std::array<std::vector<DWORD>,2> actual;
                for(int mode:{1,2}) {
                    // Exercise the real coverage inverse, not an identity
                    // substitute. The camera and receiver plane use light-
                    // aligned local coordinates, while these matrices stay
                    // in the production world-coordinate convention.
                    const auto toNative=multiply(inverse(coverage),NativeShadowComparisonProjection(light.data(),mode));
                    actual[mode-1]=Draw(device,effect,128,mode,0,false,true,gap,0,0,nullptr,1,
                        false,0,false,0,nullptr,nullptr,&toNative,&placedCoverage);
                    for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x)for(unsigned shift:{0u,8u,16u}) {
                        const auto a=actual[mode-1][y*kSize+x],b=expected[mode-1][y*kSize+x];
                        worst=(std::max)(worst,unsigned(std::abs(int((a>>shift)&255)-int((b>>shift)&255))));
                    }
                    if(!output.empty() && lightIndex==0 && gap==.25f && extent==100 && fit==2)
                        Save(output/("physical-gap-025-mode"+std::to_string(mode)+".ppm"),actual[mode-1]);
                    ++cases;
                }
                const int first=(actual[0][64*kSize+64]>>16)&255,second=(actual[1][64*kSize+64]>>16)&255;
                const int refFirst=(expected[0][64*kSize+64]>>16)&255,refSecond=(expected[1][64*kSize+64]>>16)&255;
                if(std::abs(refFirst-refSecond)>4) {
                    if(std::abs(first-second)<=4 || (first-second)*(refFirst-refSecond)<=0)
                        throw std::runtime_error("same physical depth gap lost the original mode contrast");
                    ++contrastingPairs;
                }
                if(gap==0 && (first<254 || second<254))
                    throw std::runtime_error("mode calibration invented coplanar self-shadow");
            }
        }
    }
    std::printf("mode contrast physical gaps: cases=%u distinct_pairs=%u max_original_difference=%u\n",
        cases,contrastingPairs,worst);
    if(worst>2 || !contrastingPairs)
        throw std::runtime_error("mode comparison no longer matches original centre calibration after coverage fitting");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2||argc>4){std::fputs("usage: native_shadow_tests <production shader> [--legacy|--coverage-only|--ramp-only|--detail-only|--direction-only|--progressive-only|--modes-only|--bands-only] [output-directory]\n",stderr);return 2;}
    const bool legacy=argc>=3&&!wcscmp(argv[2],L"--legacy");
    const bool rampOnly=argc>=3&&!wcscmp(argv[2],L"--ramp-only");
    const bool detailOnly=argc>=3&&!wcscmp(argv[2],L"--detail-only");
    const bool directionOnly=argc>=3&&!wcscmp(argv[2],L"--direction-only");
    const bool progressiveOnly=argc>=3&&!wcscmp(argv[2],L"--progressive-only");
    const bool coverageOnly=argc>=3&&!wcscmp(argv[2],L"--coverage-only");
    const bool modesOnly=argc>=3&&!wcscmp(argv[2],L"--modes-only");
    const bool bandsOnly=argc>=3&&!wcscmp(argv[2],L"--bands-only");
    const int outputIndex=(legacy||coverageOnly||rampOnly||detailOnly||directionOnly||progressiveOnly||modesOnly||bandsOnly)?3:2;
    const std::filesystem::path output=argc>outputIndex?argv[outputIndex]:L"";
    if(!output.empty())std::filesystem::create_directories(output);
    HWND window=CreateWindowExW(0,L"STATIC",L"native shadow test",WS_OVERLAPPED,0,0,kSize,kSize,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    try {
        CheckProjection();
        if(!wcscmp(argv[1],L"--bounds-only")){DestroyWindow(window);return 0;}
        ComPtr<IDirect3D9> d3d;d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        if(!d3d)throw std::runtime_error("D3D9 unavailable");
        D3DADAPTER_IDENTIFIER9 adapter{};
        Check(d3d->GetAdapterIdentifier(0,0,&adapter),"Read native shader adapter");
        std::printf("NATIVE_SHADER_ADAPTER %s\n",adapter.Description);
        D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
        pp.BackBufferWidth=kSize;pp.BackBufferHeight=kSize;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        ComPtr<IDirect3DDevice9> device;
        Check(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device),"Create hidden HAL device");
        auto effect=Load(device.Get(),argv[1]);unsigned failures=0;
        CheckAllNativeTechniquesGpu(device.Get(),effect.value);
        if(bandsOnly) {CheckMagnifiedShadowPlaneGpu(device.Get(),effect.value);DestroyWindow(window);return 0;}
        if(detailOnly || directionOnly) {
            using GetBool=HRESULT(WINAPI*)(void*,const char*,BOOL*);
            BOOL directionalDefault=FALSE;
            Check(EffectMethod<GetBool,25>(effect.value)(effect.value,"MdsDirectionalShadowFilter",&directionalDefault),"Read default directional filter state");
            if(!directionalDefault)throw std::runtime_error("production directional shadow filtering is not enabled by default");
        }
        if(directionOnly) {
            CheckDirectionalShadowGeometryGpu(device.Get(),effect.value);
            DestroyWindow(window);return 0;
        }
        if(progressiveOnly) {
            for(unsigned layer:{2u,0u,1u})CheckProgressiveBridgeGpu(device.Get(),effect.value,layer);
            CheckProgressiveCompositionGpu(device.Get(),effect.value);
            DestroyWindow(window);return 0;
        }
        if(modesOnly) {
            auto original=Load(device.Get(),std::filesystem::path(argv[1]).parent_path()/"original"/"skin_effect_sm3.fx");
            CheckModeContrastGpu(device.Get(),effect.value,original.value,output);
            DestroyWindow(window);return 0;
        }
        if(rampOnly) {
            unsigned worst=0;
            for(int mode:{1,2})for(float gap:{.00015f,.0004f,.001f,.002f}) {
                const auto before=Draw(device.Get(),effect.value,128,mode,0,false,true,gap);
                const auto after=Draw(device.Get(),effect.value,128,mode,0,false,true,gap,0,0,nullptr,1,true);
                for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x)for(unsigned shift:{0u,8u,16u})
                    worst=(std::max)(worst,unsigned(std::abs(int((before[y*kSize+x]>>shift)&255)-int((after[y*kSize+x]>>shift)&255))));
            }
            std::printf("native ramp after coverage fit: maximum channel difference=%u\n",worst);
            DestroyWindow(window);return worst>2?1:0;
        }
        if(detailOnly) {
            using GetBool=HRESULT(WINAPI*)(void*,const char*,BOOL*);
            BOOL bridgeDefault=TRUE;
            Check(EffectMethod<GetBool,25>(effect.value)(effect.value,"MdsBridgeEnabled",&bridgeDefault),"Read default bridge state");
            if(bridgeDefault)throw std::runtime_error("bridge sampler must default to disabled");
            unsigned worst=0, recovered=0;
            for(int mode:{1,2}) {
                const auto expected=Draw(device.Get(),effect.value,128,mode,0,true,true);
                const auto detailed=Draw(device.Get(),effect.value,128,mode,0,true,true,0,0,0,nullptr,1,false,1);
                const auto disabled=Draw(device.Get(),effect.value,128,mode,0,true,true,0,0,0,nullptr,1,false,2);
                const auto middle=Draw(device.Get(),effect.value,128,mode,0,true,true,0,0,0,nullptr,1,false,3);
                const auto distant=Draw(device.Get(),effect.value,128,mode,0,true,true,0,0,0,nullptr,1,false,4);
                for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x) {
                    for(unsigned shift:{0u,8u,16u}) {
                        worst=(std::max)(worst,unsigned(std::abs(int((expected[y*kSize+x]>>shift)&255)-int((detailed[y*kSize+x]>>shift)&255))));
                        const int a=(expected[y*kSize+x]>>shift)&255,b=(disabled[y*kSize+x]>>shift)&255;
                        worst=(std::max)(worst,unsigned(std::abs((a+b)/2-int((middle[y*kSize+x]>>shift)&255))));
                        worst=(std::max)(worst,unsigned(std::abs(b-int((distant[y*kSize+x]>>shift)&255))));
                    }
                    recovered+=detailed[y*kSize+x]!=disabled[y*kSize+x];
                }
            }
            for(int mode:{1,2}) {
                const auto expected=Draw(device.Get(),effect.value,128,mode,0,true,true);
                const auto empty=Draw(device.Get(),effect.value,128,mode,0,true,true,0,0,0,nullptr,1,false,2);
                for(int nearLayer:{1,2,3,4,5,6,7}) {
                    const auto image=Draw(device.Get(),effect.value,128,mode,0,true,true,0,0,0,nullptr,1,false,
                        nearLayer==6?0:1,false,nearLayer);
                    for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x)for(unsigned shift:{0u,8u,16u}) {
                        const int shadowed=(expected[y*kSize+x]>>shift)&255,lit=(empty[y*kSize+x]>>shift)&255;
                        // Nearest empty data overrides middle shadow data; a
                        // depth/edge miss falls back to the middle layer.
                        const int target=nearLayer==2?lit:nearLayer==5?(shadowed+lit)/2:shadowed;
                        worst=(std::max)(worst,unsigned(std::abs(target-int((image[y*kSize+x]>>shift)&255))));
                    }
                }
            }
            std::puts("PASS nearest layer precedence, isolated sampling, depth blend and middle-layer fallback in both modes");
            unsigned accessoryRecovered=0,doubleWorldDifferences=0;
            for(int mode:{1,2})for(const char* technique:{"DiffuseBSTextureTec","DiffuseBSSphiaTec","DiffuseBSSphiaTexTec"}) {
                const AccessoryProbe original{technique},placed{technique,true},wrong{technique,true,true};
                const auto reference=Draw(device.Get(),effect.value,128,mode,0,true,true,0,0,0,nullptr,1,false,0,false,0,&original);
                const auto nearest=Draw(device.Get(),effect.value,128,mode,0,true,true,0,0,0,nullptr,1,false,0,false,6,&placed);
                const auto disabled=Draw(device.Get(),effect.value,128,mode,0,true,true,0,0,0,nullptr,1,false,2,false,0,&placed);
                const auto doubled=Draw(device.Get(),effect.value,128,mode,0,true,true,0,0,0,nullptr,1,false,0,false,6,&wrong);
                for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x) {
                    for(unsigned shift:{0u,8u,16u})
                        worst=(std::max)(worst,unsigned(std::abs(int((reference[y*kSize+x]>>shift)&255)-int((nearest[y*kSize+x]>>shift)&255))));
                    accessoryRecovered+=nearest[y*kSize+x]!=disabled[y*kSize+x];
                    doubleWorldDifferences+=nearest[y*kSize+x]!=doubled[y*kSize+x];
                }
            }
            if(!accessoryRecovered || !doubleWorldDifferences)
                throw std::runtime_error("accessory probe cannot detect missing detail sampling or a duplicated world transform");
            std::printf("accessory near sampling: recovered_pixels=%u double_world_control_differences=%u\n",
                accessoryRecovered,doubleWorldDifferences);
            for(int mode:{1,2})for(unsigned resolution:{16u,32u,64u,128u})for(float slope:{-.15f,.15f})for(int detail:{0,1,6}) {
                const auto flat=Draw(device.Get(),effect.value,resolution,mode,slope,false,true,0,0,0,nullptr,1,false,
                    detail==6?0:detail,true,detail==6?6:0);
                for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x)
                    if(((flat[y*kSize+x]>>16)&255)<254)throw std::runtime_error("stable grid invents self-shadow on a sloping receiver");
            }
            std::puts("PASS stable-grid sloping receivers at four resolutions in both modes");
            CheckMagnifiedShadowPlaneGpu(device.Get(),effect.value);
            CheckBridgeLayersGpu(device.Get(),effect.value);
            CheckLogTransitionsGpu(device.Get(),effect.value);
            CheckLogTransitionsGpu(device.Get(),effect.value,true);
            for(unsigned layer:{2u,0u,1u})CheckProgressiveBridgeGpu(device.Get(),effect.value,layer);
            CheckProgressiveCompositionGpu(device.Get(),effect.value);
            CheckLayerFilterGpu(device.Get(),effect.value);
            CheckNativeVisibilityGeometryGpu(device.Get(),effect.value);
            CheckDirectionalShadowGeometryGpu(device.Get(),effect.value);
            std::printf("detail sampling both modes: max_difference=%u recovered_pixels=%u\n",worst,recovered);
            DestroyWindow(window);return worst>1||recovered==0?1:0;
        }
        for(int mode:{1,2})for(bool specular:{false,true}){
            // Camera-dependent shadow coverage must not toggle toon lighting
            // or uncover specular on a backlit surface. Cross all six bounds.
            for(auto offset:std::vector<CoverageProbe>{{{0,0,0},specular},{{-3,0,0},specular},
                    {{3,0,0},specular},{{0,-3,0},specular},{{0,3,0},specular},
                    {{0,0,-1},specular},{{0,0,1},specular}}){
                const auto image=Draw(device.Get(),effect.value,32,mode,0,false,legacy||coverageOnly,0,0,0,&offset);
                unsigned wrong=0;
                for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x){
                    const auto pixel=image[y*kSize+x];
                    wrong+=std::abs(int((pixel>>16)&255)-51)>1||std::abs(int(pixel>>24)-94)>1;
                }
                std::printf("material_coverage mode=%d specular=%d offset=%.0f,%.0f,%.0f wrong=%u\n",
                    mode,specular,offset.offset[0],offset.offset[1],offset.offset[2],wrong);
                failures+=wrong!=0;
            }
        }
        if(coverageOnly){DestroyWindow(window);std::printf("material_coverage failures=%u\n",failures);return failures?1:0;}
        auto reference=Load(device.Get(),legacy ? std::filesystem::path(argv[1]) :
            std::filesystem::path(argv[1]).parent_path()/"original"/"skin_effect_sm3.fx");
        unsigned comparisons=0, partialDepths=0;
        const auto compare=[&](unsigned resolution,int mode,float slope,float gap,int pattern,float phase,float facing=1.f){
            const auto actual=Draw(device.Get(),effect.value,resolution,mode,slope,false,false,gap,pattern,phase,nullptr,facing);
            const auto original=Draw(device.Get(),reference.value,resolution,mode,slope,false,true,gap,pattern,phase,nullptr,facing);
            unsigned difference=0, different=0;
            for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x){
                const DWORD a=actual[y*kSize+x], b=original[y*kSize+x];
                unsigned error=0;
                for(unsigned shift:{0u,8u,16u,24u})
                    error=(std::max)(error,unsigned(std::abs(int((a>>shift)&255)-int((b>>shift)&255))));
                difference=(std::max)(difference,error);different+=error>1;
            }
            const unsigned centre=(actual[64*kSize+64]>>16)&255;
            if(gap>0&&slope==0&&pattern==0&&facing==1)partialDepths+=centre>55&&centre<250;
            std::printf("native_parity size=%u mode=%d slope=%.3f gap=%.5f pattern=%d phase=%.3f facing=%.2f max_difference=%u different=%u\n",
                resolution,mode,slope,gap,pattern,phase,facing,difference,different);
            failures+=different!=0;++comparisons;
            if(pattern==1&&phase==0&&!output.empty()){
                const auto name="diagonal-mode"+std::to_string(mode);
                Save(output/(name+".ppm"),actual);Save(output/(name+"-932.ppm"),original);
            }
        };
        for(int mode:{1,2}){
            // Magnified and minified maps; coplanar slopes; thin and diagonal
            // occluders; fractional map positions. Compare real GPU RGBA with
            // the checked-in 9.32 shader instead of imposing a new PCF filter.
            for(unsigned resolution:{16u,32u,64u,192u}){
                for(float slope:{-.15f,0.f,.15f})compare(resolution,mode,slope,0,0,0);
                for(float gap:{.00015f,.00025f,.0004f,.0006f,.001f})compare(resolution,mode,0,gap,0,0);
                compare(resolution,mode,0,0,2,0);
            }
            for(float phase:{0.f,.09375f,.1875f,.28125f,.375f,.46875f,.5625f,.65625f})
                compare(16,mode,0,0,1,phase);
            // Native toon interpolation is linear, including partially lit
            // normals. A smoothstep here changes the body gradient even if
            // the depth sampler and both mode thresholds are correct.
            for(float facing:{.05f,.1f,.2f,.3f,.4f})compare(32,mode,0,.00025f,0,0,facing);
        }
        failures+=partialDepths==0;
        std::printf("native_parity comparisons=%u partial_depth_cases=%u\n",comparisons,partialDepths);
        DestroyWindow(window);std::printf("native_shadow failures=%u\n",failures);return failures?1:0;
    } catch(const std::exception& e){DestroyWindow(window);std::fprintf(stderr,"%s\n",e.what());return 1;}
}
