struct Vertex {
    float3 position; float edgeScale;
    float3 normal; uint type;
    float2 uv; uint edgeEnabled; uint material;
    int4 bones;
    float4 weights;
    float4 center, r0, r1;
    float4 additionalUv[4];
};
// Shared with include/mikumikustudio/d3d12_skinning.hpp (gpu::Bone).
// flags bit 0 marks the affine extension (stretch = H of the polar
// decomposition A = H * R, normalMatrix = (linear)^-T) valid. A record with
// flags 0 - the default - keeps the legacy rigid path below untouched, so
// older submissions that fill only transformMatrix and quaternion behave
// exactly as before.
struct Bone {
    row_major float4x4 transformMatrix; float4 quaternion;
    float4 stretch[3]; float4 normalMatrix[3];
    uint flags; uint3 reserved;
};
struct Material { float4 edgeColor; float edgeSize, edgeDistance; uint pmd, opaqueEdge; };
struct Output {
    float3 position, normal; float2 uv;
    float4 additionalUv[4];
    float3 edgePosition; uint edgeColor;
};
StructuredBuffer<Vertex> vertices : register(t0);
StructuredBuffer<Bone> bones : register(t1);
StructuredBuffer<Material> materials : register(t2);
RWStructuredBuffer<Output> results : register(u0);
cbuffer Parameters : register(b0) {
    uint vertexCount, boneCount, materialCount;
    float edgeDistance;
    uint opaqueEdge, pmd, perVertex;
};
bool validBone(int index) { return index>=0 && (uint)index<boneCount; }
bool affineBone(int index) {
    // HLSL boolean operators need not short-circuit. Clamp the address even
    // when the logical result is false: PMD uses 65535 for a missing bone,
    // and root SRVs do not provide descriptor bounds checking.
    uint safeIndex=(uint)clamp(index,0,(int)boneCount-1);
    return validBone(index) && (bones[safeIndex].flags&1)!=0;
}
float3 transform(float3 p, int index, bool position) {
    if (!validBone(index)) return 0;
    row_major float4x4 m=bones[index].transformMatrix;
    precise float3 v=p.x*m[0].xyz;
    v+=p.y*m[1].xyz; v+=p.z*m[2].xyz;
    if(position) v+=m[3].xyz;
    return v;
}
// Per-bone normal transform. A rigid bone is its own inverse transpose, so
// the legacy product runs untouched; an affine bone uses the precomputed one.
float3 transformNormal(float3 p, int index) {
    if (!validBone(index)) return 0;
    if ((bones[index].flags&1)==0) return transform(p,index,false);
    float4 r0=bones[index].normalMatrix[0];
    float4 r1=bones[index].normalMatrix[1];
    float4 r2=bones[index].normalMatrix[2];
    precise float3 v=p.x*r0.xyz;
    v+=p.y*r1.xyz; v+=p.z*r2.xyz;
    return v;
}
// Stretch of one bone: identity when the record carries no affine data.
float3x3 boneStretch(int index) {
    if (!affineBone(index)) return float3x3(1,0,0, 0,1,0, 0,0,1);
    return float3x3(bones[index].stretch[0].xyz,
                    bones[index].stretch[1].xyz,
                    bones[index].stretch[2].xyz);
}
// v * adj(h) for symmetric h (adj(h) = det(h) * inverse(h)). The SDEF normal
// divides by det(h) implicitly: det(h) > 0 and the result is normalized right
// after, so only the direction matters and no per-vertex inverse is needed.
float3 adjugateMul(float3 v, float3x3 h) {
    float a=h._11, b=h._22, c=h._33, d=h._12, e=h._13, f=h._23;
    float3 adj0=float3(b*c-f*f, e*f-d*c, d*f-e*b);
    float3 adj1=float3(e*f-d*c, a*c-e*e, d*e-a*f);
    float3 adj2=float3(d*f-e*b, d*e-a*f, a*b-d*d);
    precise float3 result=v.x*adj0;
    result+=v.y*adj1; result+=v.z*adj2;
    return result;
}
float3 normalizeSafe(float3 v) {
    precise float len=sqrt(((v.x*v.x)+(v.y*v.y))+(v.z*v.z));
    return len>0 ? v*(1/len) : v;
}
float4 quaternion(int index) {
    // D3DXQuaternionRotationMatrix(zero matrix) selects X as largest diagonal.
    return index>=0 && (uint)index<boneCount ? bones[index].quaternion : float4(.5,0,0,0);
}
float3 rotate(float3 v, float4 q) {
    precise float xx=q.x*q.x, yy=q.y*q.y, zz=q.z*q.z;
    precise float xy=q.x*q.y, xz=q.x*q.z, yz=q.y*q.z;
    precise float wx=q.w*q.x, wy=q.w*q.y, wz=q.w*q.z;
    float3 r0=float3(1-2*(yy+zz),2*(xy+wz),2*(xz-wy));
    float3 r1=float3(2*(xy-wz),1-2*(xx+zz),2*(yz+wx));
    float3 r2=float3(2*(xz+wy),2*(yz-wx),1-2*(xx+yy));
    precise float3 result=v.x*r0; result+=v.y*r1; result+=v.z*r2;
    return result;
}
uint edgeColor(float4 c, uint opaque) {
    uint4 b=uint4(int4(c*255))&255;
    if(opaque!=0) b.w=255;
    return (b.w<<24)|(b.x<<16)|(b.y<<8)|b.z;
}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    uint i=id.x; if(i>=vertexCount) return;
    Vertex v=vertices[i]; Output o=(Output)0;
    if(v.type==0) {
        o.position=transform(v.position,v.bones.x,true);
        o.normal=transformNormal(v.normal,v.bones.x);
        if(affineBone(v.bones.x)) o.normal=normalizeSafe(o.normal);
    } else if(v.type==1) {
        precise float w1=1-v.weights.x;
        o.position=transform(v.position,v.bones.y,true)*w1+transform(v.position,v.bones.x,true)*v.weights.x;
        o.normal=transformNormal(v.normal,v.bones.y)*w1+transformNormal(v.normal,v.bones.x)*v.weights.x;
        if(affineBone(v.bones.x)||affineBone(v.bones.y)) o.normal=normalizeSafe(o.normal);
    } else if(v.type==2) {
        precise float3 pos=transform(v.position,v.bones.y,true)*v.weights.y;
        pos+=transform(v.position,v.bones.x,true)*v.weights.x;
        pos+=transform(v.position,v.bones.z,true)*v.weights.z;
        pos+=transform(v.position,v.bones.w,true)*v.weights.w;
        o.position=pos;
        precise float3 n=transformNormal(v.normal,v.bones.x)*v.weights.x;
        n+=transformNormal(v.normal,v.bones.y)*v.weights.y;
        n+=transformNormal(v.normal,v.bones.z)*v.weights.z;
        n+=transformNormal(v.normal,v.bones.w)*v.weights.w;
        o.normal=n;
        if(affineBone(v.bones.x)||affineBone(v.bones.y)||affineBone(v.bones.z)||affineBone(v.bones.w)) o.normal=normalizeSafe(o.normal);
    } else if(v.type==3) {
        precise float w1=1-v.weights.x;
        float4 q0=quaternion(v.bones.x),q1=quaternion(v.bones.y);
        precise float signDot=((q1.x*q0.x+q1.w*q0.w)+q1.y*q0.y)+q1.z*q0.z;
        precise float4 q=signDot>=0 ? (q1-q0)*w1+q0 : (q1+q0)*(-w1)+q0;
        precise float len=sqrt(((q.x*q.x+q.y*q.y)+q.z*q.z)+q.w*q.w);
        q=len>0 ? q*(1/len) : float4(0,0,0,1);
        precise float3 pc=transform(v.center.xyz,v.bones.x,true)*v.weights.x+transform(v.center.xyz,v.bones.y,true)*w1;
        precise float3 arms=transform(v.r0.xyz,v.bones.x,false)*v.weights.x+transform(v.r1.xyz,v.bones.y,false)*w1;
        precise float3 pivot=((arms+pc)+pc)*.5;
        if(!affineBone(v.bones.x) && !affineBone(v.bones.y)) {
            // Legacy exact path: rotations only, quaternion straight from the
            // matrices, normal without inverse transpose or normalization.
            o.position=rotate(v.position-v.center.xyz,q)+pivot;
            o.normal=rotate(v.normal,q);
        } else {
            // Affine path: delta = (position-center) * Hblend * Rblend, and
            // the normal follows the inverse transpose of that composite
            // (Hblend symmetric, so (H*R)^-T = H^-1 * R).
            precise float3x3 h=boneStretch(v.bones.x)*v.weights.x+boneStretch(v.bones.y)*w1;
            precise float3 d=mul(v.position-v.center.xyz,h);
            o.position=rotate(d,q)+pivot;
            o.normal=normalizeSafe(rotate(adjugateMul(v.normal,h),q));
        }
    }
    o.uv=v.uv;
    [unroll] for(uint j=0;j<4;++j) o.additionalUv[j]=v.additionalUv[j];
    Material m=materials[min(v.material,materialCount-1)];
    float modelDistance=perVertex!=0 ? m.edgeDistance : edgeDistance;
    uint modelPmd=perVertex!=0 ? m.pmd : pmd;
    uint modelOpaque=perVertex!=0 ? m.opaqueEdge : opaqueEdge;
    o.edgeColor=modelPmd!=0 ? 0xff000000 : edgeColor(m.edgeColor,modelOpaque);
    if(v.edgeEnabled==0) o.edgePosition=o.position-o.normal*.004999999888241291;
    else if(modelPmd!=0) o.edgePosition=o.position+o.normal*modelDistance;
    else {
        precise float3 amount=modelDistance*o.normal;
        amount*=v.edgeScale; amount*=m.edgeSize;
        o.edgePosition=o.position+amount;
    }
    results[i]=o;
}
