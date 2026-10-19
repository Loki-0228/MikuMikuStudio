#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/mmv4mms/material_pass.hpp"
#include <cstdio>
#include <memory>
#include <stdexcept>
using namespace mikumikustudio;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
}
int main(){try{
    auto app=std::make_unique<MMDApp>();auto renderer=std::make_unique<D3DRenderer>();
    app->Renderer()=renderer.get();auto model=std::make_unique<mdl::ModelRecord>();model->physicsMode=2;
    std::vector<MmvObject> objects(1);objects[0].kind=MMV_MODEL;objects[0].id=reinterpret_cast<uint64_t>(model.get());
    mmv4mms::MaterialPassContext context{app.get(),&objects};MmvMaterial material{};material.diffuse[3]=1;
    const auto query=[&](unsigned effects=0u){uint32_t pass=99;Check(mmv4mms::ReadMaterialPass(&context,0,&material,effects,&pass)==MMV_OK,"host classification succeeds");return pass;};
    Check(query()==MMV_MMS_MATERIAL_OPAQUE,"ordinary material is opaque");
    material.diffuse[3]=.5f;Check(query()==MMV_MMS_MATERIAL_TRANSLUCENT,"diffuse transparency");
    material.diffuse[3]=0;Check(query()==MMV_MMS_MATERIAL_OPAQUE,"native alpha-zero draw remains opaque");
    Check(query(MMV_MMS_MATERIAL_CUSTOM_EFFECT)==MMV_MMS_MATERIAL_TRANSLUCENT,"FX alpha override retains transparent group");
    material.diffuse[3]=1;model->postLoadFlag2=1;Check(query()==MMV_MMS_MATERIAL_TRANSLUCENT,"additive model group");
    model->displayState=1;Check(query()==MMV_MMS_MATERIAL_ORIGINAL,"per-model translucent display preserves original order");
    model->displayState=0;model->postLoadFlag2=0;
    float pixels[8]={1,1,1,1,1,1,1,.5f};material.texture={nullptr,pixels,2,1};
    material.sourceMaterial=-1;Check(query()==MMV_MMS_MATERIAL_COVERAGE,"new evaluated inline alpha texture gets native coverage");
    Check(query(MMV_MMS_MATERIAL_CUSTOM_EFFECT)==MMV_MMS_MATERIAL_TRANSLUCENT,"custom FX has no extra coverage draw");
    pixels[7]=1;Check(query()==MMV_MMS_MATERIAL_OPAQUE,"changed evaluated pixels are classified afresh");
    material.texture={L"native-alpha.png",nullptr,0,0};auto& entry=renderer->resourcePool[0];
    entry.heapBuffer=const_cast<wchar_t*>(L"native-alpha.png");
    reinterpret_cast<unsigned char*>(&entry.tag)[3]=kTextureAlphaKnown|kTextureUsesAlpha;InvalidateTextureLookup();
    Check(query()==MMV_MMS_MATERIAL_COVERAGE,"native cached alpha tag is used without texture readback");
    reinterpret_cast<unsigned char*>(&entry.tag)[3]=kTextureAlphaKnown;
    Check(query()==MMV_MMS_MATERIAL_OPAQUE,"native cached opaque tag");
    reinterpret_cast<unsigned char*>(&entry.tag)[3]=0;
    Check(query()==MMV_MMS_MATERIAL_TRANSLUCENT,"unknown native texture alpha has one transparent draw");
    material.texture={L"plugin-only.png",nullptr,0,0};Check(query()==MMV_MMS_MATERIAL_TRANSLUCENT,"unknown plugin texture is not guessed opaque");
    material.sphere=material.texture;material.texture={};material.sphereMode=0;
    Check(query()==MMV_MMS_MATERIAL_OPAQUE,"PMX disabled sphere does not alter color pass");
    material.sphereMode=1;Check(query()==MMV_MMS_MATERIAL_TRANSLUCENT,"enabled sphere alpha participates");
    material.sphereMode=0;model->physicsMode=0;Check(query()==MMV_MMS_MATERIAL_TRANSLUCENT,"PMD sphere path participates");
    uint32_t output=0;Check(mmv4mms::ReadMaterialPass(&context,1,&material,0,&output)==MMV_INVALID_ARGUMENT,"reject invalid object mapping");
    entry.heapBuffer=nullptr;InvalidateTextureLookup();app->Renderer()=nullptr;
    std::puts("PASS host material classification, evaluated changes, native tags and custom FX");return 0;
}catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}}
