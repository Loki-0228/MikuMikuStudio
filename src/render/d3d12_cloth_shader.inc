R"CLOTH(
struct Node { float4 position,previous,velocity,force; };
struct Link { uint a,b; float inverseStiffness,restSquared; };
struct Rigid {
    float4 linearVelocity,angular,linearFactor,angularFactor,inertia[3];
    float4 velocityImpulse[2],driftImpulse[2],center;uint kind,velocityCount,driftCount,reserved;
};
struct Coupling { uint node,rigid,type,reserved; float4 response[3],relative,target,parameters; uint4 extra;float4 drift; };
struct Group { uint first,count; };
StructuredBuffer<Link> links:register(t0);
StructuredBuffer<Coupling> couplings:register(t1);
StructuredBuffer<Group> groups:register(t2);
RWStructuredBuffer<Node> nodes:register(u0);
RWStructuredBuffer<Rigid> rigids:register(u1);
cbuffer Params:register(b0) { uint stage,first,count,pad; float dt,inverseDt,damping,velocityCorrection; };
float3 matrixProduct(float4 r0,float4 r1,float4 r2,float3 v) {
    return float3(dot(r0.xyz,v),dot(r1.xyz,v),dot(r2.xyz,v));
}
void impulse(inout Rigid r,float3 p,float3 relative) {
    if(r.linearVelocity.w==0) return;
    float3 scaled=p*r.linearFactor.xyz;
    r.linearVelocity.xyz+=scaled*r.linearVelocity.w;
    r.angular.xyz+=matrixProduct(r.inertia[0],r.inertia[1],r.inertia[2],cross(relative,scaled))*r.angularFactor.xyz;
}
void clusterImpulse(inout Rigid r,float3 p,float3 relative) {
    if(r.kind==0){impulse(r,p,relative);return;}
    float3 lv=p*r.linearVelocity.w,av=matrixProduct(r.inertia[0],r.inertia[1],r.inertia[2],cross(relative,p));
    r.linearVelocity.xyz+=lv;r.angular.xyz+=av;
    r.velocityImpulse[0].xyz+=lv;r.velocityImpulse[1].xyz+=av;++r.velocityCount;
}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=count) return;
    uint i=first+id.x;
    if(stage>=8 && stage<=10) {
        Group group=groups[i];if(group.count==0)return;
        uint nodeIndex=couplings[group.first].node;
        Node n=nodes[nodeIndex];float3 delta=0;float weight=0;
        for(uint j=0;j<group.count;++j) {
            Coupling c=couplings[group.first+j];Rigid r=rigids[c.rigid];
            if(stage==9) {
                float3 v=r.linearVelocity.xyz+cross(r.angular.xyz,n.previous.xyz-r.center.xyz);
                if(n.position.w>0 && r.center.w>0 && dot(v,v)<=dot(n.velocity.xyz,n.velocity.xyz))
                    n.velocity.xyz+=r.center.w*(v-n.velocity.xyz);
            } else {
                uint impulses=stage==8 ? r.velocityCount : r.driftCount;
                if(impulses==0)continue;
                float3 v=stage==8 ? r.velocityImpulse[0].xyz : r.driftImpulse[0].xyz/impulses;
                float3 w=stage==8 ? r.velocityImpulse[1].xyz : r.driftImpulse[1].xyz/impulses;
                delta+=(v*dt+cross(w*dt,n.position.xyz-r.center.xyz))*c.parameters.x;
                weight+=c.parameters.x;
            }
        }
        if(stage!=9 && weight>0)n.position.xyz+=delta/weight;
        nodes[nodeIndex]=n;return;
    }
    if(stage==11) {
        Group group=groups[i];
        for(uint j=0;j<group.count;++j) {
            Coupling c=couplings[group.first+j];Rigid a=rigids[c.node],b=rigids[c.rigid];
            float3 vr=a.linearVelocity.xyz+cross(a.angular.xyz,c.relative.xyz)-b.linearVelocity.xyz-cross(b.angular.xyz,c.target.xyz);
            float3 p=c.drift.xyz;
            if(c.type==3) {
                float dn=dot(vr,c.parameters.xyz);
                if(dn<0){float3 normal=c.parameters.xyz*dn;p+=normal+(vr-normal)*c.drift.w;}
            } else p+=vr*c.parameters.w;
            p=matrixProduct(c.response[0],c.response[1],c.response[2],p);
            if(c.extra.x!=0) {
                if(!all(isfinite(p)) || length(p)<c.target.w)continue;
                p*=c.relative.w;
            }
            clusterImpulse(a,-p,c.relative.xyz);
            if(c.node==c.rigid) {
                clusterImpulse(a,p,c.target.xyz);rigids[c.node]=a;
            } else {
                clusterImpulse(b,p,c.target.xyz);
                if(a.kind!=0 || a.linearVelocity.w!=0)rigids[c.node]=a;
                if(b.kind!=0 || b.linearVelocity.w!=0)rigids[c.rigid]=b;
            }
        }
        return;
    }
    if(stage==0 || stage==4) {
        Link l=links[i];
        if(l.inverseStiffness<=0) return;
        Node a=nodes[l.a],b=nodes[l.b];
        if(stage==0) {
            float3 d=b.previous.xyz-a.previous.xyz;
            float denominator=dot(d,d)*l.inverseStiffness;
            if(denominator<=1e-20) return;
            float j=-dot(d,a.velocity.xyz-b.velocity.xyz)/denominator;
            a.velocity.xyz+=d*(j*a.position.w);b.velocity.xyz-=d*(j*b.position.w);
        } else {
            float3 d=b.position.xyz-a.position.xyz;
            float length=dot(d,d),denominator=l.inverseStiffness*(l.restSquared+length);
            if(denominator<=1e-20) return;
            float k=(l.restSquared-length)/denominator;
            a.position.xyz-=d*(k*a.position.w);b.position.xyz+=d*(k*b.position.w);
        }
        nodes[l.a]=a;nodes[l.b]=b;return;
    }
    if(stage==2) {
        Group group=groups[i];
        for(uint j=0;j<group.count;++j) {
            Coupling c=couplings[group.first+j];
            if(c.type==2) {
                Node n=nodes[c.node],a=nodes[c.rigid],b=nodes[c.extra.x],d=nodes[c.extra.y];
                float3 p=a.position.xyz*c.relative.x+b.position.xyz*c.relative.y+d.position.xyz*c.relative.z;
                float3 q=a.previous.xyz*c.relative.x+b.previous.xyz*c.relative.y+d.previous.xyz*c.relative.z;
                float3 vr=(n.position.xyz-n.previous.xyz)-(p-q);
                float dn=dot(vr,c.target.xyz);
                float3 correction=dn<0 ? c.target.xyz*(c.target.w-dot(c.target.xyz,n.position.xyz)+dot(c.target.xyz,p)) : 0;
                correction-=(vr-c.target.xyz*dn)*c.parameters.z;
                // Update addresses in Bullet order, including aliased nodes.
                nodes[c.node].position.xyz+=correction*c.parameters.x;
                nodes[c.rigid].position.xyz-=correction*(c.parameters.y*c.relative.x);
                nodes[c.extra.x].position.xyz-=correction*(c.parameters.y*c.relative.y);
                nodes[c.extra.y].position.xyz-=correction*(c.parameters.y*c.relative.z);
                continue;
            }
            Node n=nodes[c.node];
            Rigid r=(Rigid)0;
            if(c.rigid!=0xffffffff) r=rigids[c.rigid];
            float3 va=(r.linearVelocity.xyz+cross(r.angular.xyz,c.relative.xyz))*dt;
            float3 vb=n.position.xyz-n.previous.xyz,p=0;
            if(c.type==0) {
                float3 vr=va-vb+(c.target.xyz-n.position.xyz)*c.parameters.x;
                p=matrixProduct(c.response[0],c.response[1],c.response[2],vr);
                n.position.xyz+=p*c.relative.w;
                impulse(r,-p,c.relative.xyz);
            } else {
                float3 vr=vb-va;float dn=dot(vr,c.target.xyz);
                if(dn<=1.192092896e-7) {
                    float dp=min(dot(n.position.xyz,c.target.xyz)+c.target.w,c.parameters.z);
                    float3 fv=vr-c.target.xyz*dn;
                    p=matrixProduct(c.response[0],c.response[1],c.response[2],vr-fv*c.parameters.x+c.target.xyz*(dp*c.parameters.y));
                    n.position.xyz-=p*c.relative.w;
                    impulse(r,p,c.relative.xyz);
                }
            }
            nodes[c.node]=n;
            if(c.rigid!=0xffffffff && r.linearVelocity.w!=0) rigids[c.rigid]=r;
        }
        return;
    }
    Node n=nodes[i];
    if(stage==1) n.position.xyz=n.previous.xyz+n.velocity.xyz*dt;
    if(stage==5) { n.velocity.xyz=(n.position.xyz-n.previous.xyz)*(inverseDt*(1-damping));n.force.xyz=0; }
    if(stage==6) n.previous.xyz=n.position.xyz;
    if(stage==7) n.velocity.xyz+=(n.position.xyz-n.previous.xyz)*(inverseDt*velocityCorrection);
    nodes[i]=n;
}
)CLOTH"
