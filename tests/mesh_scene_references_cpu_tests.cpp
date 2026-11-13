#include "mikumikustudio/mesh_scene_references.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace mikumikustudio;
static unsigned checks=0;
static void Check(bool pass) { ++checks; if(!pass) throw checks; }
template<class Fn> static bool Rejects(Fn fn) {
    try { fn(); } catch(const std::invalid_argument&) { return true; }
    return false;
}
int main() {
    try {
        auto material=std::make_unique<mdl::ModelMaterialRecord>();
        auto model=std::make_unique<mdl::ModelRecord>();
        std::fill(std::begin(material->texturePath),std::end(material->texturePath)-1,L'x');
        material->texturePath[279]=L'\0';
        material->spherePath[0]=L'\u9ad8'; material->spherePath[1]=L'\0';
        material->toonPath[0]=L't'; material->toonPath[1]=L'\0';
        material->toonReference=0xfe; material->sphereMode=2; material->flags=0xa5;
        model->modelDirectory[0]=L'D'; model->modelDirectory[1]=L'\0';
        std::fill_n(model->pmdToonFileNames[9],100,char(0x81));
        MeshSceneMaterialReferences refs{}; MeshSceneToonTableReferences toon{};
        CaptureMeshSceneMaterialReferences(*material,2,refs);
        CaptureMeshSceneToonTableReferences(*model,toon);
        Check(refs.texturePath[278]==L'x' && refs.texturePath[279]==0);
        Check(refs.spherePath[0]==L'\u9ad8' && refs.spherePath[1]==0);
        Check(refs.toonPath[0]==L't' && refs.toonPath[1]==0);
        Check(refs.physicsMode==2 && refs.toonReference==0xfe && refs.sphereMode==2 && refs.flags==0xa5);
        Check(toon.modelDirectory[0]==L'D' && toon.modelDirectory[1]==0);
        Check(std::all_of(toon.fileNames[9].begin(),toon.fileNames[9].end(),[](char c){return c==char(0x81);}));
        const auto before=refs; const auto tableBefore=toon;
        std::fill(std::begin(material->toonPath),std::end(material->toonPath),L'z');
        Check(Rejects([&]{CaptureMeshSceneMaterialReferences(*material,0,refs);}));
        Check(std::memcmp(&refs,&before,sizeof(refs))==0);
        material->toonPath[0]=0;
        std::fill(std::begin(material->spherePath),std::end(material->spherePath),L'z');
        Check(Rejects([&]{CaptureMeshSceneMaterialReferences(*material,0,refs);}));
        Check(std::memcmp(&refs,&before,sizeof(refs))==0);
        material->spherePath[0]=0;
        std::fill(std::begin(material->texturePath),std::end(material->texturePath),L'z');
        Check(Rejects([&]{CaptureMeshSceneMaterialReferences(*material,0,refs);}));
        Check(std::memcmp(&refs,&before,sizeof(refs))==0);
        std::fill(std::begin(model->modelDirectory),std::end(model->modelDirectory),L'z');
        Check(Rejects([&]{CaptureMeshSceneToonTableReferences(*model,toon);}));
        Check(std::memcmp(&toon,&tableBefore,sizeof(toon))==0);
        std::fill(std::begin(model->modelDirectory),std::end(model->modelDirectory)-1,L'd');
        model->modelDirectory[255]=0;
        CaptureMeshSceneToonTableReferences(*model,toon);
        Check(toon.modelDirectory[254]==L'd' && toon.modelDirectory[255]==0);
        std::fill(std::begin(material->texturePath),std::end(material->texturePath),0);
        std::fill(std::begin(material->toonPath),std::end(material->toonPath)-1,L't');
        material->toonPath[511]=0;
        CaptureMeshSceneMaterialReferences(*material,0,refs);
        Check(refs.texturePath[0]==0 && refs.texturePath[278]==0 && refs.toonPath[510]==L't' && refs.toonPath[511]==0);
        material.reset(); model.reset();
        Check(refs.toonPath[510]==L't' && toon.fileNames[9][99]==char(0x81));
        std::printf("mesh_scene_references_cpu_tests: %u checks passed\n",checks);
        return 0;
    } catch(unsigned n) { std::fprintf(stderr,"check %u failed\n",n); return 1; }
}
