#pragma once
// Host-owned mesh data for native capture and Alembic cache playback.
// This is not a plugin ABI and has no dependency on an external SDK.
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <stdexcept>
namespace mikumikustudio::mesh_data {
inline constexpr unsigned kEdgeDisabled=1;
inline void Identity(float* m){std::fill_n(m,16,0.f);m[0]=m[5]=m[10]=m[15]=1;}
struct Vertex {float position[3]{},normal[3]{},uv[2]{},additionalUv[16]{};float edgeScale=1;uint32_t sourceVertex=0,flags=0,reserved=0;};
struct Triangle {uint32_t vertex[3]{},material=0;};
struct TextureView {const wchar_t* path=nullptr;const float* rgba=nullptr;uint32_t width=0,height=0;};
struct MaterialView {const char* key=nullptr;const char* name=nullptr;const char* nameEn=nullptr;
 float diffuse[4]{},ambient[3]{},specular[3]{},power=0,edgeColor[4]{},edgeSize=0;
 uint32_t flags=0,sphereMode=0;int32_t sourceMaterial=-1;TextureView texture,sphere,toon;};
struct MeshView {const Vertex* vertices=nullptr;uint32_t vertexCount=0;
 const Triangle* triangles=nullptr;uint32_t triangleCount=0;const MaterialView* materials=nullptr;uint32_t materialCount=0;
 float boundsMin[3]{},boundsMax[3]{};uint64_t revision=0;uint32_t vertexCapacity=0,triangleCapacity=0;};
struct Texture {std::wstring path;TextureView View()const{return {path.empty()?nullptr:path.c_str()};}};
struct Material {MaterialView value;std::string key,name,nameEn;Texture texture,sphere,toon;
 MaterialView View()const {auto m=value;m.key=key.c_str();m.name=name.c_str();m.nameEn=nameEn.c_str();m.texture=texture.View();m.sphere=sphere.View();m.toon=toon.View();return m;}};
struct Mesh {std::vector<Vertex> vertices;std::vector<Triangle> triangles;std::vector<Material> materials;
 mutable std::vector<MaterialView> views;uint64_t revision=0;
 MeshView View()const {views.clear();for(const auto& m:materials)views.push_back(m.View());MeshView m;
 m.vertices=vertices.data();m.vertexCount=uint32_t(vertices.size());m.triangles=triangles.data();m.triangleCount=uint32_t(triangles.size());
 m.materials=views.data();m.materialCount=uint32_t(views.size());m.revision=revision;m.vertexCapacity=m.vertexCount;m.triangleCapacity=m.triangleCount;return m;}};
struct Object {uint32_t kind=0,visible=0;uint64_t id=0,generation=0;float world[16]{};MeshView mesh;};
struct ObjectOutput {uint64_t objectId=0;MeshView mesh;bool hidden=false;};
inline void Validate(const MeshView& m) {
 if((m.vertexCount&&!m.vertices)||(m.triangleCount&&!m.triangles)||(m.materialCount&&!m.materials))throw std::runtime_error("Invalid native mesh storage");
 for(uint32_t i=0;i<m.vertexCount;++i){const auto& v=m.vertices[i];for(float x:v.position)if(!std::isfinite(x))throw std::runtime_error("Non-finite native vertex");for(float x:v.normal)if(!std::isfinite(x))throw std::runtime_error("Non-finite native normal");}
 for(uint32_t i=0;i<m.triangleCount;++i){const auto& t=m.triangles[i];if(t.material>=m.materialCount)throw std::runtime_error("Invalid native material index");for(auto v:t.vertex)if(v>=m.vertexCount)throw std::runtime_error("Invalid native vertex index");}
}
}
