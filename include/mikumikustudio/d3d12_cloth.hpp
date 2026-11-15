#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace mikumikustudio::gpu {
struct ClothNode { float position[4], previous[4], velocity[4], force[4]; };
struct ClothLink { std::uint32_t a,b; float inverseStiffness,restSquared; };
struct ClothRigid {
    float linear[4],angular[4],linearFactor[4],angularFactor[4],inertia[3][4];
    float velocityImpulse[2][4],driftImpulse[2][4];
    float center[4]; // cluster center in xyz, node damping in w
    std::uint32_t kind,velocityCount,driftCount,reserved;
};
struct ClothCoupling {
    std::uint32_t node,rigid,type,reserved;
    float matrix[3][4],relative[4],target[4],parameters[4];
    std::uint32_t extra[4];
    float drift[4];
};
struct ClothGroup { std::uint32_t first,count; };
struct ClothPass { std::uint32_t stage,first,count; };
struct ClothParameters { float dt,inverseDt,damping,velocityCorrection; };
static_assert(sizeof(ClothNode)==64 && sizeof(ClothLink)==16 && sizeof(ClothRigid)==208 && sizeof(ClothCoupling)==144);

// Independent link colors and coupling components have disjoint writable nodes
// and dynamic rigid bodies. UAV barriers order colors, phases and iterations.
class Cloth {
public:
    Cloth(); ~Cloth();
    bool Initialize(std::uint64_t adapter=0);
    bool Step(std::vector<ClothNode>& nodes,const std::vector<ClothLink>& links,
              std::vector<ClothRigid>& rigids,const std::vector<ClothCoupling>& couplings,
              const std::vector<ClothGroup>& groups,const std::vector<ClothPass>& passes,
              ClothParameters parameters);
    const char* Error() const;
    const wchar_t* AdapterName() const;
private:
    struct Impl; std::unique_ptr<Impl> impl;
};
}
