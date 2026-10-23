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
#include <stdexcept>
#include <string>
#include <vector>
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/native_shadow_projection.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"

using namespace mikumikustudio::d3dx;
using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned kSize=128;
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
    // The chosen size remains inside the selected quality's plus/minus one tier.
    for(unsigned selected:{512u,1024u,2048u,4096u,8192u}) {
        const unsigned maximum=8192u;
        const unsigned lower=(std::max)(512u,selected/2);
        const unsigned upper=(std::min)(maximum,selected*2);
        unsigned previous=selected;
        for(float required:{1e-3f,.1f,50.f,200.f,400.f,700.f,900.f,1500.f,4000.f,20000.f,1e9f,
                            800.f,300.f,50.f,1e9f,0.f}) {
            const unsigned size=DynamicShadowQualitySize(required,selected,previous,maximum);
            if(size<lower || size>upper)
                throw std::runtime_error("dynamic quality left the selected tier band");
            if(size&(size-1u))
                throw std::runtime_error("dynamic quality is not a power of two");
            if(size<selected?size!=lower:size>selected?size!=upper:size!=selected)
                throw std::runtime_error("dynamic quality skipped a band tier");
            previous=size;
        }
    }
    // Hysteresis: a 1024 selection upgrades above 2.5x its coverage demand,
    // keeps the larger map until demand falls to 1.6x, and only downgrades
    // below 0.4x.
    const auto choose=[](float required,unsigned previous){
        return DynamicShadowQualitySize(required,1024,previous,8192);
    };
    if(choose(700.f,1024)!=1024 || choose(1400.f,1024)!=2048)
        throw std::runtime_error("dynamic growth threshold is not centred on the selection");
    if(choose(900.f,2048)!=2048 || choose(700.f,2048)!=1024)
        throw std::runtime_error("dynamic shrink threshold has no hysteresis");
    if(choose(180.f,1024)!=512 || choose(300.f,512)!=512 || choose(400.f,512)!=1024)
        throw std::runtime_error("dynamic lower tier band is wrong");
    // A camera sweep settles on the band ends and never jumps more than one tier
    // per frame.
    unsigned quality=1024,previousQuality=quality,changes=0;
    for(float depth=400.f;depth>=1.f;depth*=.85f) {
        const float required=DynamicShadowMapDemand(3600.f,24.f,depth,false);
        quality=DynamicShadowQualitySize(required,1024,quality,8192);
        changes+=quality!=previousQuality;
        if(quality>previousQuality*2 || quality*2<previousQuality)
            throw std::runtime_error("dynamic quality jumped more than one tier in one frame");
        previousQuality=quality;
    }
    const unsigned closest=quality;
    for(float depth=1.f;depth<=400.f;depth*=1.18f) {
        const float required=DynamicShadowMapDemand(3600.f,24.f,depth,false);
        quality=DynamicShadowQualitySize(required,1024,quality,8192);
        changes+=quality!=previousQuality;
        previousQuality=quality;
    }
    if(closest!=2048 || quality!=512)
        throw std::runtime_error("dynamic precision sweep did not settle on both band ends");
    std::printf("PASS dynamic precision: inverse-depth curve, one-tier band, %u hysteresis changes\n",changes);
}

void CheckVisibleCoverageFit() {
    using namespace mikumikustudio;
    NativeShadowBounds scene,visible;
    const float sceneLow[3]{-1000,-5,-1000},sceneHigh[3]{1000,40,1000};
    const float visibleLow[3]{-6,0,-6},visibleHigh[3]{6,12,6};
    scene.Include(sceneLow);scene.Include(sceneHigh);
    visible.Include(visibleLow);visible.Include(visibleHigh);
    const float focus[3]{0,6,0},light[3]{-.5f,-1,.5f};
    const auto full=NativeShadowProjection(focus,light,.1f,1,&scene);
    const auto fitted=NativeShadowProjection(focus,light,.1f,1,&visible,&scene);
    const auto project=[](const std::array<float,16>& matrix,const std::array<float,3>& point){
        double q[4]{};
        for(int a=0;a<4;++a)q[a]=matrix[12+a]+point[0]*matrix[a]+point[1]*matrix[4+a]+point[2]*matrix[8+a];
        return std::array<double,4>{q[0],q[1],q[2],q[3]};
    };
    // Fitting the lateral extent to the visible receivers must raise the texel
    // density enough to matter, even though the huge off-screen scene stays.
    if(!(MinimumShadowScale(fitted,visible)>MinimumShadowScale(full,visible)*3.f))
        throw std::runtime_error("visible receiver fit did not tighten the lateral coverage");
    for(unsigned corner=0;corner<8;++corner) {
        const auto point=visible.Corner(corner);
        const auto q=project(fitted,point);
        if(!(q[3]>0) || std::abs(q[0]/q[3])>=1 || std::abs(q[1]/q[3])>=1)
            throw std::runtime_error("visible receiver fell outside the fitted coverage");
    }
    // Depth must still span the whole scene so an off-screen caster can land
    // on the visible receiver along the light direction.
    for(unsigned corner=0;corner<8;++corner) {
        const auto point=scene.Corner(corner);
        const auto q=project(fitted,point);
        if(!(q[3]>0) || q[2]/q[3]<=0 || q[2]/q[3]>=1)
            throw std::runtime_error("fitted coverage cut a caster out of the light depth range");
    }
    std::printf("PASS visible coverage fit: lateral %.2fx tighter, scene depth retained\n",
        MinimumShadowScale(fitted,visible)/MinimumShadowScale(full,visible));
}

void CheckProjection() {
    CheckQualityOrdering();
    CheckVisibleCoverageFit();
    CheckDynamicPrecision();
    CheckCameraPictureShadowRegion();
    CheckDetailGrid();
    CheckVertexBounds();
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
std::vector<DWORD> Draw(IDirect3DDevice9* device,void* effect,unsigned mapSize,int mode,float slope,bool split,bool legacy,float depthGap=0,int pattern=0,float phase=0,const CoverageProbe* coverage=nullptr,float facing=1,bool fitted=false,int detail=0,bool stable=false) {
    ComPtr<IDirect3DTexture9> shadow;
    Check(device->CreateTexture(mapSize,mapSize,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,&shadow,nullptr),"Create shadow texture");
    D3DLOCKED_RECT pixels{};Check(shadow->LockRect(0,&pixels,nullptr,0),"Lock shadow texture");
    for(unsigned y=0;y<mapSize;++y)for(unsigned x=0;x<mapSize;++x){
        auto* row=reinterpret_cast<float*>(static_cast<char*>(pixels.pBits)+y*pixels.Pitch);
        row[x]=split?(x<mapSize/2?.2f:.8f):.5f+slope*(2.f*x/mapSize-1.f)-depthGap;
        if(pattern==1)row[x]=float(x)<mapSize*.5f+(float(y)-mapSize*.5f)*.35f?.2f:.8f;
        if(pattern==2)row[x]=(x==mapSize/2)?.2f:.8f;
        if(fitted)row[x]=row[x]*.5f+.2f;
        if(coverage)row[x]=1; // No occluder: only material lighting may shade this receiver.
    }
    Check(shadow->UnlockRect(0),"Unlock shadow texture");
    D3DXMATRIXF identity{};for(unsigned i=0;i<4;++i)identity.m[i][i]=1;
    Check(FxSetMatrix(effect,"matWorldViewProj",&identity),"Set camera projection");
    auto lightProjection=identity;lightProjection.m[3][0]=2*phase/mapSize;
    auto toNative=identity;
    if(fitted) {
        lightProjection.m[1][1]=.75f;lightProjection.m[3][1]=.1f;
        lightProjection.m[2][2]=.5f;lightProjection.m[3][2]=.2f;
        toNative.m[1][1]=1.f/.75f;toNative.m[3][1]=-.1f/.75f;
        toNative.m[2][2]=2;toNative.m[3][2]=-.4f;
    }
    FxSetMatrix(effect,"MdsShadowToNative",&toNative);
    FxSetInt(effect,"MdsStableShadowGrid",stable);
    const float dimensions[4]{float(mapSize),float(mapSize),0,0},texel[4]{1.f/mapSize,1.f/mapSize,0,0};
    FxSetFloatArray(effect,"MdsShadowMapSize",dimensions,4);
    FxSetFloatArray(effect,"MdsDetailTexelSize",texel,4);
    FxSetInt(effect,"MdsDetailEnabled",detail!=0&&detail!=2);
    FxSetMatrix(effect,"MdsCoverageToDetail",&identity);
    const float detailDepthBlend[4]{0,0,0,detail==3?.5f:detail==4?0.f:1.f};
    FxSetFloatArray(effect,"MdsDetailDepthBlend",detailDepthBlend,4);
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
    Check(FxSetTechnique(effect,"BufferShadowTec"),"Select native receiver");
    const Vertex vertices[]={{{-1,1,.5f-slope},{0,0,1},{0,0}},{{1,1,.5f+slope},{0,0,1},{1,0}},
                             {{-1,-1,.5f-slope},{0,0,1},{0,1}},{{1,-1,.5f+slope},{0,0,1},{1,1}}};
    Check(device->SetTexture(0,shadow.Get()),"Bind shadow map");
    ComPtr<IDirect3DTexture9> empty;
    if(detail) {
        Check(device->CreateTexture(1,1,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,&empty,nullptr),"Create empty coverage map");
        D3DLOCKED_RECT blank{};Check(empty->LockRect(0,&blank,nullptr,0),"Lock empty coverage");
        *static_cast<float*>(blank.pBits)=1;
        Check(empty->UnlockRect(0),"Unlock empty coverage");
        Check(device->SetTexture(0,empty.Get()),"Bind empty coverage");
    }
    Check(device->SetTexture(3,detail?shadow.Get():nullptr),"Bind detail shadow map");
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
    return output;
}
void Save(const std::filesystem::path& path,const std::vector<DWORD>& pixels) {
    std::ofstream out(path,std::ios::binary);out<<"P6\n"<<kSize<<" "<<kSize<<"\n255\n";
    for(auto pixel:pixels){const char rgb[]{char(pixel>>16),char(pixel>>8),char(pixel)};out.write(rgb,3);}
    if(!out)throw std::runtime_error("Cannot write GPU readback image");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2||argc>4){std::fputs("usage: native_shadow_tests <production shader> [--legacy|--coverage-only] [output-directory]\n",stderr);return 2;}
    const bool legacy=argc>=3&&!wcscmp(argv[2],L"--legacy");
    const bool rampOnly=argc>=3&&!wcscmp(argv[2],L"--ramp-only");
    const bool detailOnly=argc>=3&&!wcscmp(argv[2],L"--detail-only");
    const bool coverageOnly=argc>=3&&!wcscmp(argv[2],L"--coverage-only");
    const std::filesystem::path output=argc>((legacy||coverageOnly||rampOnly||detailOnly)?3:2)?argv[(legacy||coverageOnly||rampOnly||detailOnly)?3:2]:L"";
    if(!output.empty())std::filesystem::create_directories(output);
    HWND window=CreateWindowExW(0,L"STATIC",L"native shadow test",WS_OVERLAPPED,0,0,kSize,kSize,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    try {
        CheckProjection();
        if(!wcscmp(argv[1],L"--bounds-only")){DestroyWindow(window);return 0;}
        ComPtr<IDirect3D9> d3d;d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        if(!d3d)throw std::runtime_error("D3D9 unavailable");
        D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
        pp.BackBufferWidth=kSize;pp.BackBufferHeight=kSize;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        ComPtr<IDirect3DDevice9> device;
        Check(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device),"Create hidden HAL device");
        auto effect=Load(device.Get(),argv[1]);unsigned failures=0;
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
            for(int mode:{1,2})for(unsigned resolution:{16u,32u,64u,128u})for(float slope:{-.15f,.15f})for(int detail:{0,1}) {
                const auto flat=Draw(device.Get(),effect.value,resolution,mode,slope,false,true,0,0,0,nullptr,1,false,detail,true);
                for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x)
                    if(((flat[y*kSize+x]>>16)&255)<254)throw std::runtime_error("stable grid invents self-shadow on a sloping receiver");
            }
            std::puts("PASS stable-grid sloping receivers at four resolutions in both modes");
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
