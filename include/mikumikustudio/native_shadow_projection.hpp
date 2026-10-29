#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cstddef>
#include <cstring>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace mikumikustudio {
struct NativeShadowBounds {
    std::array<float, 3> minimum{INFINITY, INFINITY, INFINITY};
    std::array<float, 3> maximum{-INFINITY, -INFINITY, -INFINITY};
    bool Valid() const { return minimum[0] <= maximum[0]; }
    void Include(const float point[3]) {
        if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2])) return;
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = (std::min)(minimum[axis], point[axis]);
            maximum[axis] = (std::max)(maximum[axis], point[axis]);
        }
    }
    std::array<float, 3> Corner(unsigned index) const {
        return {index & 1 ? maximum[0] : minimum[0],
                index & 2 ? maximum[1] : minimum[1],
                index & 4 ? maximum[2] : minimum[2]};
    }
};
// Bound visible receivers through the scene's camera-space depth, independent
// of shadow precision. A focus-radius depth cut loses distant visible surfaces.
inline NativeShadowBounds VisibleShadowReceivers(const NativeShadowBounds& scene,
    const float view[16],const float inverseView[16],float projectionX,float projectionY,bool orthographic) {
    if(!scene.Valid() || !std::isfinite(projectionX) || !std::isfinite(projectionY) ||
        std::fabs(projectionX)<1e-6f || std::fabs(projectionY)<1e-6f)return {};
    // Intersect the six scene planes and five camera planes directly. Taking
    // the world AABB of a deep view frustum first includes off-screen regions
    // and unnecessarily dilutes the shadow map during close-up oblique views.
    (void)inverseView;
    using Plane=std::array<double,4>;
    std::array<Plane,11> planes{};
    for(int axis=0;axis<3;++axis) {
        planes[2*axis][axis]=1;planes[2*axis][3]=-scene.minimum[axis];
        planes[2*axis+1][axis]=-1;planes[2*axis+1][3]=scene.maximum[axis];
    }
    for(int side=0;side<4;++side) {
        const int axis=side/2;const double sign=(side&1)?-1:1;
        const double projection=axis?projectionY:projectionX;
        for(int row=0;row<4;++row)planes[6+side][row]=sign*projection*view[row*4+axis]+
            (orthographic?(row==3?1.0:0.0):view[row*4+2]);
    }
    for(int row=0;row<4;++row)planes[10][row]=view[row*4+2];
    const auto cross=[](const Plane& a,const Plane& b) {
        return std::array<double,3>{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
    };
    NativeShadowBounds receivers;
    for(unsigned i=0;i<planes.size();++i)for(unsigned j=i+1;j<planes.size();++j)for(unsigned k=j+1;k<planes.size();++k) {
        const auto& a=planes[i];const auto& b=planes[j];const auto& c=planes[k];
        const auto bc=cross(b,c),ca=cross(c,a),ab=cross(a,b);
        const double determinant=a[0]*bc[0]+a[1]*bc[1]+a[2]*bc[2];
        if(std::abs(determinant)<1e-12)continue;
        double point[3]{};
        for(int axis=0;axis<3;++axis)point[axis]=(-a[3]*bc[axis]-b[3]*ca[axis]-c[3]*ab[axis])/determinant;
        bool inside=true;
        for(const auto& plane:planes) {
            const double value=plane[3]+plane[0]*point[0]+plane[1]*point[1]+plane[2]*point[2];
            const double tolerance=1e-7*(1+std::abs(plane[3])+std::abs(plane[0]*point[0])+std::abs(plane[1]*point[1])+std::abs(plane[2]*point[2]));
            if(value < -tolerance){inside=false;break;}
        }
        if(inside) {const float p[3]{float(point[0]),float(point[1]),float(point[2])};receivers.Include(p);}
    }
    return receivers;
}
// Exact intersection of a mesh AABB and the final six camera clip planes.
// Return its light-space bounds directly: a world-space AABB of the clipped
// frustum would grow again and include unrelated off-screen stage geometry.
inline NativeShadowBounds VisibleShadowLightBounds(const NativeShadowBounds& scene,
    const float clip[16],const float light[16]) {
    if(!scene.Valid())return {};
    using Plane=std::array<double,4>;
    std::array<Plane,12> planes{};
    for(int axis=0;axis<3;++axis) {
        planes[2*axis][axis]=1;planes[2*axis][3]=-scene.minimum[axis];
        planes[2*axis+1][axis]=-1;planes[2*axis+1][3]=scene.maximum[axis];
    }
    for(int row=0;row<4;++row) {
        for(int axis=0;axis<2;++axis) {
            planes[6+2*axis][row]=clip[row*4+3]+clip[row*4+axis];
            planes[7+2*axis][row]=clip[row*4+3]-clip[row*4+axis];
        }
        planes[10][row]=clip[row*4+2];
        planes[11][row]=clip[row*4+3]-clip[row*4+2];
    }
    const auto cross=[](const Plane& a,const Plane& b) {
        return std::array<double,3>{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
    };
    NativeShadowBounds result;
    for(unsigned i=0;i<planes.size();++i)for(unsigned j=i+1;j<planes.size();++j)for(unsigned k=j+1;k<planes.size();++k) {
        const auto& a=planes[i];const auto& b=planes[j];const auto& c=planes[k];
        const auto bc=cross(b,c),ca=cross(c,a),ab=cross(a,b);
        const double det=a[0]*bc[0]+a[1]*bc[1]+a[2]*bc[2];
        if(std::abs(det)<1e-12)continue;
        double p[3]{};for(int axis=0;axis<3;++axis)p[axis]=(-a[3]*bc[axis]-b[3]*ca[axis]-c[3]*ab[axis])/det;
        bool inside=true;
        for(const auto& plane:planes) {
            const double value=plane[3]+plane[0]*p[0]+plane[1]*p[1]+plane[2]*p[2];
            const double tolerance=1e-7*(1+std::abs(plane[3])+std::abs(plane[0]*p[0])+std::abs(plane[1]*p[1])+std::abs(plane[2]*p[2]));
            if(value < -tolerance){inside=false;break;}
        }
        if(inside) {
            float q[3]{};
            for(int a=0;a<3;++a)q[a]=float(light[12+a]+p[0]*light[a]+p[1]*light[4+a]+p[2]*light[8+a]);
            result.Include(q);
        }
    }
    return result;
}
// All receivers share one square light-space coverage box. Preserve the full
// scene Z mapping so an off-screen caster on their light ray remains present.
inline std::array<float,16> FitVisibleShadowBox(std::array<float,16> coverage,
    const NativeShadowBounds& visible) {
    if(!visible.Valid())return coverage;
    const float half=(std::max)(visible.maximum[0]-visible.minimum[0],
        visible.maximum[1]-visible.minimum[1])*.5f;
    const float extent=(std::max)(1e-6f,half*1.02f);
    for(int axis=0;axis<2;++axis) {
        const float centre=visible.minimum[axis]*.5f+visible.maximum[axis]*.5f;
        for(int row=0;row<4;++row)coverage[row*4+axis]=(coverage[row*4+axis]-(row==3?centre:0.f))/extent;
    }
    return coverage;
}

// Fixed per-model receiver box. Keep the coverage map's light basis and Z so
// every scene caster can still shadow this model. Padding puts all receivers
// inside the detail shader's fully weighted core, with no camera-depth fade.
inline std::array<float,16> FixedModelShadowProjection(
    const std::array<float,16>& coverage, const NativeShadowBounds& model) {
    NativeShadowBounds light;
    if(!model.Valid())return coverage;
    for(unsigned corner=0;corner<8;++corner) {
        const auto p=model.Corner(corner);float q[3]{};
        for(unsigned axis=0;axis<3;++axis)
            q[axis]=coverage[12+axis]+p[0]*coverage[axis]+p[1]*coverage[4+axis]+p[2]*coverage[8+axis];
        light.Include(q);
    }
    auto result=FitVisibleShadowBox(coverage,light);
    for(unsigned row=0;row<4;++row)for(unsigned axis=0;axis<2;++axis)result[row*4+axis]*=.8f;
    return result;
}

// Read every position from the already locked vertex buffer. SIMD compares
// preserve the scalar Include rule: ignore the whole non-finite vertex and
// keep the earlier bit pattern for equal coordinates (including signed zero).
inline NativeShadowBounds NativeShadowVertexBounds(const void* data,
                                                   std::size_t stride,
                                                   std::size_t count) {
    NativeShadowBounds bounds;
    if (!data || stride < 12 || count > (std::numeric_limits<std::size_t>::max)() / stride)
        return bounds;
    const auto* bytes = static_cast<const unsigned char*>(data);
#if defined(_M_X64) || defined(__SSE2__)
    if (stride >= 16) {
        __m128 low = _mm_set1_ps(INFINITY), high = _mm_set1_ps(-INFINITY);
        const __m128 sign = _mm_set1_ps(-0.0f);
        const __m128 largest = _mm_set1_ps((std::numeric_limits<float>::max)());
        for (std::size_t i = 0; i < count; ++i, bytes += stride) {
            const __m128 point = _mm_loadu_ps(reinterpret_cast<const float*>(bytes));
            if ((_mm_movemask_ps(_mm_cmple_ps(_mm_andnot_ps(sign, point), largest)) & 7) != 7)
                continue;
            // MINPS/MAXPS return their second operand on equality.
            low = _mm_min_ps(point, low);
            high = _mm_max_ps(point, high);
        }
        float minimum[4], maximum[4];
        _mm_storeu_ps(minimum, low); _mm_storeu_ps(maximum, high);
        std::memcpy(bounds.minimum.data(), minimum, 12);
        std::memcpy(bounds.maximum.data(), maximum, 12);
        return bounds;
    }
#endif
    for (std::size_t i = 0; i < count; ++i, bytes += stride) {
        float point[3]; std::memcpy(point, bytes, sizeof(point));
        bounds.Include(point);
    }
    return bounds;
}
inline float NativeShadowLateralScale(float range, int mode) {
    const float base = std::isfinite(range) ? (std::max)(range, .00001f) : .01784f;
    return base * (mode == 1 ? 2.f : 3.f);
}

// Preserve the 9.32 sampling grid unless visible geometry needs more coverage.
// An affine transform of homogeneous clip coordinates enlarges the frustum
// without changing the direction of light rays or discarding its mode warp.
inline std::array<float, 16> FitReferenceShadowProjection(
    std::array<float, 16> matrix, const NativeShadowBounds& bounds) {
    if (!bounds.Valid()) return matrix;
    double minimumW = INFINITY;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto p = bounds.Corner(corner);
        minimumW = (std::min)(minimumW, double(matrix[15]) +
            p[0]*double(matrix[3]) + p[1]*double(matrix[7]) + p[2]*double(matrix[11]));
    }
    // Do not let a projective pole cross the model. Reduce only the warp when
    // necessary; move Z with W to preserve its centre plane before fitting.
    if (minimumW < .25) {
        const double factor = .75 / (1.0 - minimumW);
        for (unsigned row = 0; row < 4; ++row) {
            const double oldW = matrix[row*4+3];
            const double newW = oldW*factor + (row == 3 ? 1.0-factor : 0.0);
            matrix[row*4+2] += float(.5*(newW-oldW));
            matrix[row*4+3] = float(newW);
        }
    }
    double lo[3] = {INFINITY, INFINITY, INFINITY};
    double hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto p = bounds.Corner(corner);
        double q[4]{};
        for (int a = 0; a < 4; ++a)
            q[a] = matrix[12+a] + p[0]*double(matrix[a]) +
                p[1]*double(matrix[4+a]) + p[2]*double(matrix[8+a]);
        for (int a = 0; a < 3; ++a) {
            lo[a] = (std::min)(lo[a], q[a]/q[3]);
            hi[a] = (std::max)(hi[a], q[a]/q[3]);
        }
    }
    for (int a = 0; a < 3; ++a) {
        const double clipLo = a == 2 ? 0.0 : -1.0;
        const double margin = (std::max)(a == 2 ? .000001 : .0005, (hi[a]-lo[a])*.05);
        const double expandedLo = (std::min)(clipLo, lo[a]-margin);
        const double expandedHi = (std::max)(1.0, hi[a]+margin);
        if (expandedLo == clipLo && expandedHi == 1.0) continue;
        const double scale = (1.0-clipLo)/(expandedHi-expandedLo);
        const double offset = clipLo-expandedLo*scale;
        for (unsigned row = 0; row < 4; ++row)
            matrix[row*4+a] = float(matrix[row*4+a]*scale + matrix[row*4+3]*offset);
    }
    return matrix;
}

// Smallest local light-plane sampling density, including perspective division.
// The adaptive allocator must not treat the warped matrix as orthographic.
inline float MinimumShadowScale(const std::array<float, 16>& m,
                                const NativeShadowBounds& bounds) {
    if (!bounds.Valid()) return .00001f;
    double result = INFINITY;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto p = bounds.Corner(corner);
        double q[4]{};
        for (int a = 0; a < 4; ++a)
            q[a] = m[12+a] + p[0]*double(m[a]) + p[1]*double(m[4+a]) + p[2]*double(m[8+a]);
        if (q[3] <= .00001) continue;
        double xx=0, yy=0, xy=0;
        for (int r=0; r<3; ++r) {
            const double dx=(m[r*4]*q[3]-q[0]*m[r*4+3])/(q[3]*q[3]);
            const double dy=(m[r*4+1]*q[3]-q[1]*m[r*4+3])/(q[3]*q[3]);
            xx+=dx*dx; yy+=dy*dy; xy+=dx*dy;
        }
        const double disc=std::sqrt((xx-yy)*(xx-yy)+4*xy*xy);
        result=(std::min)(result, std::sqrt((std::max)(0.0, (xx+yy-disc)*.5)));
    }
    return std::isfinite(result) ? float((std::max)(.0000001, result)) : .00001f;
}
// Model vertices and accessory world matrices are in scene coordinates. Build
// the directional-light projection there, independently of the orbit rotation.
// The requested range cannot clip rendered geometry. Fit its world-space
// bounds independently of the camera, with room for filtering at every edge.
inline std::array<float, 16> NativeShadowProjection(
    const float focus[3], const float direction[3], float range, int mode,
    const NativeShadowBounds* bounds = nullptr,
    float halfWidthLimit = INFINITY) {
    float z[3] = {direction[0], direction[1], direction[2]};
    float length = std::sqrt(z[0]*z[0] + z[1]*z[1] + z[2]*z[2]);
    if (!std::isfinite(length) || length < 1e-6f) {
        z[0] = 0; z[1] = -1; z[2] = 0; length = 1;
    }
    for (float& value : z) value /= length;
    const bool pole = std::abs(z[2]) > .95f;
    float x[3] = {pole ? z[2] : -z[1], pole ? 0.f : z[0], pole ? -z[0] : 0.f};
    length = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
    for (float& value : x) value /= length;
    const float y[3] = {z[1]*x[2]-z[2]*x[1], z[2]*x[0]-z[0]*x[2], z[0]*x[1]-z[1]*x[0]};
    const float* axes[] = {x, y, z};
    const float base = std::isfinite(range) ? (std::max)(range, .00001f) : .01784f;
    const float lateral = NativeShadowLateralScale(range, mode);
    float scale[] = {lateral, lateral, base * .15f};
    float centre[] = {
        focus[0]*x[0] + focus[1]*x[1] + focus[2]*x[2],
        focus[0]*y[0] + focus[1]*y[1] + focus[2]*y[2],
        focus[0]*z[0] + focus[1]*z[1] + focus[2]*z[2]};
    if (bounds && bounds->Valid()) {
        NativeShadowBounds lightBounds;
        for (unsigned corner = 0; corner < 8; ++corner) {
            const auto point = bounds->Corner(corner);
            float light[3]{};
            for (int axis = 0; axis < 3; ++axis)
                for (int row = 0; row < 3; ++row) light[axis] += point[row]*axes[axis][row];
            lightBounds.Include(light);
        }
        float half[3]{};
        for (int axis = 0; axis < 3; ++axis) {
            centre[axis] = lightBounds.minimum[axis]*.5f + lightBounds.maximum[axis]*.5f;
            const float extent = lightBounds.maximum[axis]*.5f - lightBounds.minimum[axis]*.5f;
            half[axis] = extent + (std::max)(.25f, extent*.05f);
        }
        // The range-limited default caps the lateral box at the original 9.32
        // shadow range and keeps it centred on the focus: the box covers the
        // picture's centre, not the middle of a scene that may sit far away.
        // A scene smaller than that keeps its own fitted extent and centre, so
        // a character-sized scene keeps the precision it had before.
        const float fitted = (std::max)(half[0], half[1]);
        const bool capped = std::isfinite(halfWidthLimit) && fitted > halfWidthLimit;
        float halfWidth;
        if (capped) {
            halfWidth = halfWidthLimit;
            centre[0] = focus[0]*x[0] + focus[1]*x[1] + focus[2]*x[2];
            centre[1] = focus[0]*y[0] + focus[1]*y[1] + focus[2]*y[2];
        } else {
            halfWidth = std::isfinite(halfWidthLimit)
                ? (std::max)(.25f, fitted)
                : (std::max)(1.f/lateral, fitted);
        }
        const float halfDepth = (std::max)(.5f/scale[2], half[2]);
        scale[0] = scale[1] = 1.f/halfWidth;
        scale[2] = .5f/halfDepth;
    }
    std::array<float, 16> matrix{};
    matrix[15] = 1;
    for (int axis = 0; axis < 3; ++axis) {
        for (int row = 0; row < 3; ++row) matrix[row*4+axis] = axes[axis][row]*scale[axis];
        matrix[12+axis] = -centre[axis]*scale[axis] + (axis == 2 ? .5f : 0.f);
    }
    return matrix;
}

struct NativeShadowShape {
    std::array<float,3> direction{};
    float lateral=1.f, centreW=1.f;
};
// Original minimal-build mode parameters evaluated with the world light.
// Both X/Y derivatives at the original box centre equal lateral/(1-a*b).
inline NativeShadowShape NativeShadowModeShape(const float direction[3], int mode) {
    NativeShadowShape shape;
    auto& z=shape.direction;
    std::copy_n(direction,3,z.begin());
    float length=std::sqrt(z[0]*z[0]+z[1]*z[1]+z[2]*z[2]);
    if(!std::isfinite(length) || length<1e-6f) {
        z[0]=0;z[1]=-1;z[2]=0;length=1;
    }
    for(float& value:z)value/=length;
    const float slope=-z[2];
    float a,b;
    if(slope<-.9f || slope>.9f) {
        a=1.f-std::fabs(slope);b=-a;
    } else if(slope<-.8f) {
        shape.lateral=mode==1 ? 10.f*slope+10.f : 20.f*slope+19.f;
        a=mode==1 ? 4.f*slope+3.7f : 9.f*slope+8.2f;
        b=-9.f*slope-8.2f;
    } else if(slope>.8f) {
        shape.lateral=mode==1 ? 10.f-10.f*slope : 19.f-20.f*slope;
        a=mode==1 ? 3.7f-4.f*slope : 8.2f-9.f*slope;
        b=9.f*slope-8.2f;
    } else {
        shape.lateral=mode==1 ? 2.f : 3.f;
        a=mode==1 ? .5f : 1.f;b=-1.f;
    }
    shape.centreW=1.f-a*b;
    return shape;
}
inline float NativeShadowSamplingDensity(const float direction[3],float range,int mode,unsigned referenceSize) {
    const auto shape=NativeShadowModeShape(direction,mode);
    const float base=(std::clamp)(std::isfinite(range)?range:.01125f,.00001f,.1f);
    return referenceSize*base*shape.lateral/(2.f*shape.centreW);
}

// Calibrate the original comparison at texture v=.5 (Y=-b, W=1-a*b),
// without restoring the old projective pole or camera-dependent direction.
inline std::array<float,16> NativeShadowComparisonProjection(const float direction[3], int mode) {
    const auto shape=NativeShadowModeShape(direction,mode);
    // Original reset/default range (reset_app_state.cpp), deliberately fixed:
    // the full build's precision slider changes sampling, not shadow thickness.
    constexpr float referenceRange=.01125f;
    const float scale=.15f*referenceRange/shape.centreW;
    std::array<float,16> result{};
    for(int row=0;row<3;++row)result[row*4+2]=shape.direction[row]*scale;
    result[14]=.5f;result[15]=1.f; // Y=0 means texture v=.5 in the native shader.
    return result;
}

// Resource size is not quality: absorb the minimum allocation and power-of-two
// rounding by adding empty coverage, so a small box cannot increase texels/unit.
// Keep at least a half-texel margin for snapping without cropping the input box.
inline std::array<float,16> FitShadowSamplingDensity(std::array<float,16> projection,
                                                   float density,unsigned size) {
    if(!size || !std::isfinite(density) || density<=0)return projection;
    float scale=0;
    for(int column=0;column<2;++column) {
        float length=0;for(int row=0;row<3;++row)length+=projection[row*4+column]*projection[row*4+column];
        scale=(std::max)(scale,std::sqrt(length));
    }
    if(!(scale>0) || !std::isfinite(scale))return projection;
    const float ratio=(std::min)((std::max)(.01f,1.f-1.f/size),2.f*density/(size*scale));
    const float grid=size*.5f;
    for(int column=0;column<2;++column) {
        for(int row=0;row<4;++row)projection[row*4+column]*=ratio;
        projection[12+column]=std::round(projection[12+column]*grid)/grid;
    }
    return projection;
}

// A finite shadow map cannot fit an arbitrarily large stage without losing
// nearby detail. Keep the native grid/full-object fit while its sampling loss
// fits the extra 2x resolution tier. Beyond that, cover the view's focus region
// in the light plane; still retain all scene depths for off-screen casters.
inline std::array<float, 16> FitFocusedShadowProjection(
    const std::array<float, 16>& reference, const NativeShadowBounds& scene,
    const float focus[3], const float direction[3], float range, int mode,
    float viewRadius,const NativeShadowBounds* receivers=nullptr) {
    const auto complete = FitReferenceShadowProjection(reference, scene);
    if (!scene.Valid()) return complete;
    const float radius = (std::max)(1.f / NativeShadowLateralScale(range, mode),
        std::isfinite(viewRadius) ? (std::max)(0.f, viewRadius) : 0.f);
    NativeShadowBounds local;
    const bool stableReceivers=receivers!=nullptr;
    bool clipped = false;
    for (int axis = 0; axis < 3; ++axis) {
        local.minimum[axis] = (std::max)(scene.minimum[axis], focus[axis] - radius);
        local.maximum[axis] = (std::min)(scene.maximum[axis], focus[axis] + radius);
        if (local.minimum[axis] > local.maximum[axis]) {
            if(!stableReceivers)return complete;
            local=scene;break;
        }
        clipped |= local.minimum[axis] != scene.minimum[axis] ||
                   local.maximum[axis] != scene.maximum[axis];
    }
    if(receivers && receivers->Valid()) {
        for(int axis=0;axis<3;++axis) {
            local.minimum[axis]=(std::min)(local.minimum[axis],receivers->minimum[axis]);
            local.maximum[axis]=(std::max)(local.maximum[axis],receivers->maximum[axis]);
        }
    }
    // A live receiver fit must not switch between projective and orthographic
    // depth conventions as the camera crosses a density threshold.
    if (!clipped && !stableReceivers) return complete;
    auto focused = NativeShadowProjection(focus, direction, range, mode, &local);
    if (!stableReceivers && MinimumShadowScale(complete, local) * 2.f >= MinimumShadowScale(focused, local))
        return complete;
    if(stableReceivers) {
        // Keep depth/bias independent of camera fitting. Only light or scene
        // geometry changes may change this mapping; retain remote casters.
        const auto depth=NativeShadowProjection(focus,direction,range,mode,&scene);
        for(unsigned row=0;row<4;++row)focused[row*4+2]=depth[row*4+2];
        return focused;
    }
    // The focused map is directional (W=1), so widening only Z cannot alter
    // shadow UV or texels per world unit. A remote caster on the same light ray
    // is still submitted and can shadow a nearby receiver.
    double lo = 0, hi = 1;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto p = scene.Corner(corner);
        const double z = focused[14] + p[0]*double(focused[2]) +
            p[1]*double(focused[6]) + p[2]*double(focused[10]);
        lo = (std::min)(lo, z); hi = (std::max)(hi, z);
    }
    if (lo < 0 || hi > 1) {
        const double margin = (hi-lo)*.05;
        lo -= margin; hi += margin;
        const double scale = 1.0/(hi-lo);
        for (unsigned row = 0; row < 4; ++row)
            focused[row*4+2] = float((focused[row*4+2] - (row == 3 ? lo : 0.0))*scale);
    }
    return focused;
}

// A second map preserves nearby detail while the coverage map retains distant
// receivers. Its light basis and extent do not depend on camera orientation.
// Quantized extent and texel-aligned translation also avoid subpixel crawling.
inline std::array<float, 16> DetailShadowProjection(const float focus[3],
    const float direction[3], float range, int mode, float viewRadius,
    unsigned size, const std::array<float,16>& coverage) {
    auto result = NativeShadowProjection(focus, direction, range, mode);
    const float minimumRadius = 1.f / NativeShadowLateralScale(range, mode);
    float radius = minimumRadius;
    while (radius < viewRadius && radius < 1.e8f) radius *= 2.f;
    const float ratio = minimumRadius / radius;
    for (int column = 0; column < 2; ++column) {
        for (int row = 0; row < 4; ++row) result[row*4+column] *= ratio;
        if (size) result[12+column] = std::round(result[12+column]*size*.5f)*2.f/size;
    }
    // Identical Z mappings let both maps share the native material depth ramp;
    // distant off-screen casters remain in the detail pass too.
    for (int row = 0; row < 4; ++row) result[row*4+2] = coverage[row*4+2];
    return result;
}

struct CameraShadowRegion {
    std::array<float,3> centre{};
    float radius = 0;
    float farDepth = 0;
    bool valid = false;
    float nearDepth = 0;
};

// The detail region follows the camera picture: the visible view volume comes
// from the final view and projection matrices, and the focus depth from the
// frame's focus point through those same matrices. Camera panel xyz values alone
// miss the reference, attachment and preview transforms, and a fixed world
// footprint keeps the shadow density constant while the picture zooms in.
// The covered depth slab is centred on the focus depth and as thick as the
// picture is wide there, so its world size shrinks with both a closer camera and
// a narrower lens. Build the bounding sphere in view space and transform it back
// to scene space; it therefore does not change when the camera merely rotates.
inline CameraShadowRegion CameraDepthShadowRegion(const float inverseView[16],
    const float inverseProjection[16], float focusDepth, bool includeNearPlane = false) {
    using Point = std::array<float,3>;
    const auto transform=[](const Point& p,const float m[16],Point& result) {
        double q[4]{};
        for(int a=0;a<4;++a)q[a]=m[12+a]+p[0]*double(m[a])+p[1]*double(m[4+a])+p[2]*double(m[8+a]);
        if(!std::isfinite(q[3]) || std::abs(q[3])<1e-12)return false;
        for(int a=0;a<3;++a) {
            result[a]=float(q[a]/q[3]);
            if(!std::isfinite(result[a]))return false;
        }
        return true;
    };
    // Windows headers define near and far as macros, so the corners need names
    // that survive a translation unit which includes Windows.h first.
    std::array<Point,4> nearCorner{},farCorner{};
    float nearDepth=0,clipFar=INFINITY,maxSlope=0;
    for(int i=0;i<4;++i) {
        const float x=i&1?1.f:-1.f,y=i&2?1.f:-1.f;
        if(!transform({x,y,0},inverseProjection,nearCorner[i]) ||
           !transform({x,y,1},inverseProjection,farCorner[i]))return {};
        nearDepth=(std::max)(nearDepth,nearCorner[i][2]);
        clipFar=(std::min)(clipFar,farCorner[i][2]);
        const float dz=farCorner[i][2]-nearCorner[i][2];
        if(dz<=1e-5f)return {};
        maxSlope=(std::max)(maxSlope,std::hypot(farCorner[i][0]-nearCorner[i][0],
            farCorner[i][1]-nearCorner[i][1])/dz);
    }
    if(!std::isfinite(focusDepth) || focusDepth<=nearDepth)return {};
    float halfSlab;
    if(maxSlope>1e-5f) {
        // Half the diagonal of the world patch the picture shows on the focus
        // plane. A very wide lens would otherwise reach an unbounded depth
        // range, so the slab never exceeds one focus distance per side.
        halfSlab=(std::min)(focusDepth*maxSlope,focusDepth);
    } else {
        // Orthographic pictures keep their world size at every depth; the
        // visible half-diagonal bounds the slab around the focus.
        halfSlab=std::hypot(std::fabs(nearCorner[0][0]-nearCorner[1][0])*.5f,
            std::fabs(nearCorner[0][1]-nearCorner[2][1])*.5f);
    }
    const float start=includeNearPlane ? nearDepth : (std::max)(nearDepth,focusDepth-halfSlab);
    const float end=(std::min)(clipFar,focusDepth+halfSlab);
    if(!std::isfinite(end) || end<=start)return {};
    std::array<Point,8> corners{};Point centre{};
    for(int i=0;i<8;++i) {
        const int ray=i%4;
        const float depth=i<4?start:end;
        const float t=(depth-nearCorner[ray][2])/(farCorner[ray][2]-nearCorner[ray][2]);
        for(int a=0;a<3;++a) {
            corners[i][a]=nearCorner[ray][a]+t*(farCorner[ray][a]-nearCorner[ray][a]);
            centre[a]+=corners[i][a]/8.f;
        }
    }
    CameraShadowRegion region;
    if(!transform(centre,inverseView,region.centre))return {};
    for(const auto& corner:corners) {
        Point world{};if(!transform(corner,inverseView,world))return {};
        const float radius=std::sqrt((world[0]-region.centre[0])*(world[0]-region.centre[0])+
            (world[1]-region.centre[1])*(world[1]-region.centre[1])+
            (world[2]-region.centre[2])*(world[2]-region.centre[2]));
        region.radius=(std::max)(region.radius,radius);
    }
    // Keep the visible corners inside the detail map's edge blend guard. This
    // floor must stay small: clamping it to a few world units would stop the
    // density from improving once the camera is close to the receiver.
    region.radius=(std::max)(.0001f,region.radius/0.75f);
    region.nearDepth=start;region.farDepth=end;region.valid=std::isfinite(region.radius);
    return region;
}

// A separate close layer must begin at the real near clip plane, even when
// the orbit target is far away. Limit its world extent by the density budget,
// never by a slider-dependent focus slab. Wider views are explicitly reported
// by the returned radius, rather than pretending a capped texture met demand.
inline CameraShadowRegion CameraNearShadowRegion(const float inverseView[16],
    const float inverseProjection[16],float maximumRadius) {
    if(!std::isfinite(maximumRadius) || maximumRadius<=0)return {};
    using Point=std::array<float,3>;
    const auto transform=[](const Point& p,const float m[16]) {
        Point out{};double w=m[15]+p[0]*double(m[3])+p[1]*double(m[7])+p[2]*double(m[11]);
        for(int a=0;a<3;++a)out[a]=float((m[12+a]+p[0]*double(m[a])+p[1]*double(m[4+a])+p[2]*double(m[8+a]))/w);
        return out;
    };
    std::array<Point,4> start{},ray{};float nearZ=0;
    for(int i=0;i<4;++i) {
        const float x=i&1?1.f:-1.f,y=i&2?1.f:-1.f;
        start[i]=transform({x,y,0},inverseProjection);
        const auto middle=transform({x,y,.5f},inverseProjection);
        const float dz=middle[2]-start[i][2];
        if(!(dz>0) || !std::isfinite(dz))return {};
        for(int a=0;a<3;++a)ray[i][a]=(middle[a]-start[i][a])/dz;
        nearZ=(std::max)(nearZ,start[i][2]);
    }
    const auto slab=[&](float end) {
        std::array<Point,8> points{};CameraShadowRegion r;
        for(int i=0;i<8;++i) {
            const int j=i%4;Point p{};
            for(int a=0;a<3;++a)p[a]=start[j][a]+ray[j][a]*((i<4?nearZ:end)-start[j][2]);
            points[i]=transform(p,inverseView);
            for(int a=0;a<3;++a)r.centre[a]+=points[i][a]/8;
        }
        for(const auto& p:points)r.radius=(std::max)(r.radius,std::sqrt(
            (p[0]-r.centre[0])*(p[0]-r.centre[0])+(p[1]-r.centre[1])*(p[1]-r.centre[1])+
            (p[2]-r.centre[2])*(p[2]-r.centre[2])));
        r.radius=(std::max)(1e-4f,r.radius/.80f);r.nearDepth=nearZ;r.farDepth=end;
        r.valid=std::isfinite(r.radius) && std::isfinite(r.centre[0]) &&
            std::isfinite(r.centre[1]) && std::isfinite(r.centre[2]);
        return r;
    };
    float lo=nearZ,hi=nearZ+(std::max)(1.f,maximumRadius*4);
    for(int i=0;i<28;++i) {const float mid=(lo+hi)*.5f;if(slab(mid).radius<=maximumRadius)lo=mid;else hi=mid;}
    return slab((std::max)(lo,nearZ+.001f));
}

struct CameraShadowBox {
    std::array<float,16> projection{};
    float nearDepth = 0, farDepth = 0;
    bool valid = false;
};

// Pack a camera slab into an existing light-plane square by translation only.
// The nearer maps already cover the start of the view. Using the remaining
// square directly avoids wasting its corners around a camera bounding sphere.
// Always pass the nominal projection, before density fitting: texture padding
// differs by quality and must not move the common depth boundaries.
inline CameraShadowBox FitCameraShadowBox(const float inverseView[16],
    const float inverseProjection[16],const std::array<float,16>& nominalProjection,
    float nearDepth) {
    CameraShadowBox result;
    result.projection=nominalProjection;
    if(!inverseView || !inverseProjection || !std::isfinite(nearDepth))return result;
    for(int i=0;i<16;++i)if(!std::isfinite(inverseView[i]) ||
        !std::isfinite(inverseProjection[i]) || !std::isfinite(nominalProjection[i]))return result;
    // World/view and directional-light transforms must be affine. Do not
    // silently approximate a projective transform or divide by a missing W.
    if(inverseView[3]!=0 || inverseView[7]!=0 || inverseView[11]!=0 ||
        std::abs(double(inverseView[15]))<1e-12 || nominalProjection[3]!=0 ||
        nominalProjection[7]!=0 || nominalProjection[11]!=0 ||
        std::abs(double(nominalProjection[15]))<1e-12)return result;
    double xx=0,yy=0,xy=0;
    for(int row=0;row<3;++row) {
        const double x=nominalProjection[row*4],y=nominalProjection[row*4+1];
        xx+=x*x;yy+=y*y;xy+=x*y;
    }
    if(!(xx>0) || !(yy>0) || xx*yy-xy*xy<=1e-12*xx*yy)return result;
    double rowNorm[3]{};
    for(int row=0;row<3;++row)for(int column=0;column<3;++column)
        rowNorm[row]+=double(inverseView[row*4+column])*inverseView[row*4+column];
    const double determinant=double(inverseView[0])*(double(inverseView[5])*inverseView[10]-double(inverseView[6])*inverseView[9])-
        double(inverseView[1])*(double(inverseView[4])*inverseView[10]-double(inverseView[6])*inverseView[8])+
        double(inverseView[2])*(double(inverseView[4])*inverseView[9]-double(inverseView[5])*inverseView[8]);
    if(std::abs(determinant)<=1e-12*std::sqrt(rowNorm[0]*rowNorm[1]*rowNorm[2]))return result;
    using Point=std::array<double,3>;
    const auto unproject=[&](double x,double y,double z,Point& point) {
        double q[4]{};
        for(int a=0;a<4;++a)q[a]=inverseProjection[12+a]+x*inverseProjection[a]+
            y*inverseProjection[4+a]+z*inverseProjection[8+a];
        if(!std::isfinite(q[3]) || std::abs(q[3])<1e-12)return false;
        for(int a=0;a<3;++a) {
            point[a]=q[a]/q[3];
            if(!std::isfinite(point[a]))return false;
        }
        return true;
    };
    std::array<Point,4> nearCorner{},farCorner{};
    double clipNear=-INFINITY,clipFar=INFINITY;
    for(int i=0;i<4;++i) {
        const double x=i&1?1.0:-1.0,y=i&2?1.0:-1.0;
        if(!unproject(x,y,0,nearCorner[i]) || !unproject(x,y,1,farCorner[i]) ||
            !(farCorner[i][2]>nearCorner[i][2]))return result;
        clipNear=(std::max)(clipNear,nearCorner[i][2]);
        clipFar=(std::min)(clipFar,farCorner[i][2]);
    }
    const double requestedStart=(std::max)(double(nearDepth),clipNear);
    float storedStart=float(requestedStart);
    if(double(storedStart)<requestedStart)storedStart=std::nextafter(storedStart,INFINITY);
    const double start=storedStart;
    if(!std::isfinite(start) || !std::isfinite(clipFar) || !(clipFar>start))return result;

    // The four light-space corner rays are affine functions of view depth.
    // This handles shifted lenses and orthographic cameras without assuming
    // that screen centre is on the optical axis or that rays diverge.
    std::array<std::array<double,2>,4> origin{},slope{};
    const double viewW=inverseView[15],lightW=nominalProjection[15];
    for(int i=0;i<4;++i) {
        Point ray{},base{},worldRay{},worldBase{};
        for(int a=0;a<3;++a) {
            ray[a]=(farCorner[i][a]-nearCorner[i][a])/(farCorner[i][2]-nearCorner[i][2]);
            base[a]=nearCorner[i][a]-ray[a]*nearCorner[i][2];
        }
        for(int a=0;a<3;++a) {
            worldBase[a]=inverseView[12+a];
            for(int b=0;b<3;++b) {
                worldBase[a]+=base[b]*inverseView[b*4+a];
                worldRay[a]+=ray[b]*inverseView[b*4+a];
            }
            worldBase[a]/=viewW;worldRay[a]/=viewW;
        }
        for(int axis=0;axis<2;++axis) {
            for(int a=0;a<3;++a) {
                origin[i][axis]+=worldBase[a]*nominalProjection[a*4+axis]/lightW;
                slope[i][axis]+=worldRay[a]*nominalProjection[a*4+axis]/lightW;
            }
            if(!std::isfinite(origin[i][axis]) || !std::isfinite(slope[i][axis]))return result;
        }
    }
    const auto fits=[&](double end,std::array<float,2>* translation=nullptr) {
        double minimum[2]{INFINITY,INFINITY},maximum[2]{-INFINITY,-INFINITY};
        for(int i=0;i<4;++i)for(double depth:{start,end})for(int axis=0;axis<2;++axis) {
            const double value=origin[i][axis]+slope[i][axis]*depth;
            if(!std::isfinite(value))return false;
            minimum[axis]=(std::min)(minimum[axis],value);
            maximum[axis]=(std::max)(maximum[axis],value);
        }
        std::array<float,2> offset{};
        for(int axis=0;axis<2;++axis) {
            offset[axis]=float(-(minimum[axis]+maximum[axis])*.5*lightW);
            if(!std::isfinite(offset[axis]))return false;
            // Include float translation rounding in the fit. The existing
            // .8 guard still leaves room for final texel snapping and fades.
            const double translated=double(offset[axis])/lightW;
            if(minimum[axis]+translated<-.8 || maximum[axis]+translated>.8)return false;
        }
        if(translation)*translation=offset;
        return true;
    };
    if(!fits(start))return result;
    double lo=start,hi=clipFar;
    if(fits(hi))lo=hi;
    else for(int i=0;i<40;++i) {
        const double mid=lo+(hi-lo)*.5;
        if(fits(mid))lo=mid;else hi=mid;
    }
    float end=float(lo);
    if(!std::isfinite(end))return result;
    if(double(end)>lo)end=std::nextafter(end,-INFINITY);
    std::array<float,2> translation{};
    if(!(double(end)>start) || !fits(end,&translation))return result;
    result.projection[12]=translation[0];result.projection[13]=translation[1];
    result.nearDepth=float(start);result.farDepth=end;result.valid=true;
    return result;
}

inline std::array<float,16> CameraDetailShadowProjection(const CameraShadowRegion& region,
    const float direction[3],const std::array<float,16>& coverage,unsigned mapSize=8192) {
    auto result=NativeShadowProjection(region.centre.data(),direction,.1f,1);
    const float ratio=5.f/region.radius;
    for(int column=0;column<2;++column) {
        for(int row=0;row<4;++row)result[row*4+column]*=ratio;
        // Align to the texture actually sampled, including lower quality tiers.
        // Snapping to a nominal 8192 grid lets a 1024 map slide by eighth-texels.
        if(mapSize) {
            const float grid=mapSize*.5f;
            result[12+column]=std::round(result[12+column]*grid)/grid;
        }
    }
    for(int row=0;row<4;++row)result[row*4+2]=coverage[row*4+2];
    return result;
}
}
