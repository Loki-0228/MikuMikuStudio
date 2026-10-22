#pragma once
// Test-owned wire writer and independent pre-cache PMX field decoder. This
// header does not include the cache implementation or pmx_vertex_prepare.
#include "mikumikustudio/subrecord_layout.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <vector>

namespace mikumikustudio::geometry_test_reference {
using Bytes = std::vector<unsigned char>;
template<class T> inline void Put(Bytes& bytes, T value) {
    const auto* p = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof value);
}
inline void Fixed(Bytes& bytes, const char* value, std::size_t capacity) {
    const auto at = bytes.size(); bytes.resize(at + capacity, 0);
    const auto n = std::strlen(value); std::memcpy(bytes.data() + at, value, n < capacity ? n : capacity);
}
inline void Text(Bytes& bytes, const wchar_t* value) {
    const auto n = std::wcslen(value) * sizeof(wchar_t); Put(bytes, static_cast<std::uint32_t>(n));
    const auto* p = reinterpret_cast<const unsigned char*>(value); bytes.insert(bytes.end(), p, p + n);
}
inline void SignedIndex(Bytes& bytes, std::int32_t value, unsigned width) {
    if (width == 1) Put(bytes, static_cast<std::int8_t>(value));
    else if (width == 2) Put(bytes, static_cast<std::int16_t>(value));
    else Put(bytes, value);
}
struct Fixture {
    Bytes bytes;
    bool pmd = false;
    std::uint32_t vertices = 0, indices = 0;
    unsigned uv = 0, boneWidth = 0, vertexWidth = 0;
    std::int64_t vertexStart = 0, vertexEnd = 0, indexEnd = 0;
};
inline Fixture Pmx(std::uint32_t count, unsigned uv = 4, unsigned boneWidth = 2,
                   unsigned vertexWidth = 2, bool host = false) {
    Fixture f; f.vertices = count; f.indices = count * 3; f.uv = uv;
    f.boneWidth = boneWidth; f.vertexWidth = vertexWidth;
    auto& b = f.bytes; b = {'P','M','X',' '}; Put(b, 2.f);
    for (unsigned char v : std::array<unsigned char, 9>{8,0,static_cast<unsigned char>(uv),
        static_cast<unsigned char>(vertexWidth),1,1,static_cast<unsigned char>(boneWidth),1,1}) Put(b,v);
    Text(b,L"cache-model"); Text(b,L"cache-model"); Text(b,L"cache test"); Text(b,L"cache test");
    Put(b,count); f.vertexStart = static_cast<std::int64_t>(b.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::array<float,8> base{float(i % 3),float((i + 1) % 3),float(i % 2),0,0,1,float(i % 2),float((i + 1) % 2)};
        for (float v : base) Put(b,v);
        for (unsigned j=0;j<uv;++j) for (unsigned c=0;c<4;++c) Put(b,float(i * 17 + j * 4 + c) * .125f);
        const auto type = static_cast<unsigned char>(host ? 0 : i % 7); Put(b,type);
        const auto bone = [&](int slot) { SignedIndex(b,host ? 0 : int(i % 91) - 45 + slot,boneWidth); };
        bone(0);
        if (type == 1 || type == 3) {
            bone(1); Put(b,.25f);
            if (type == 3) for (int j=0;j<9;++j) Put(b,float(j * 3 - 9));
        } else if (type == 2) {
            bone(1);bone(2);bone(3);for(int j=0;j<4;++j)Put(b,.25f);
        }
        // QDEF and unknown values deliberately use the current loader's
        // original single-bone fallback, rather than a new PMX interpretation.
        Put(b,.75f);
    }
    f.vertexEnd=static_cast<std::int64_t>(b.size()); Put(b,f.indices);
    for(std::uint32_t i=0;i<f.indices;++i) {
        const auto value=count ? (i * 7 + 2) % count : 0;
        if(vertexWidth==1)Put(b,static_cast<std::uint8_t>(value));
        else if(vertexWidth==2)Put(b,static_cast<std::uint16_t>(value));else Put(b,value);
    }
    f.indexEnd=static_cast<std::int64_t>(b.size());
    Put(b,0u); // texture paths
    Put(b,host ? 1u : 0u);
    if(host) {
        Text(b,L"material");Text(b,L"material");
        for(float v:{.7f,.6f,.5f,1.f,0.f,0.f,0.f,1.f,.1f,.1f,.1f})Put(b,v);
        Put(b,static_cast<std::uint8_t>(0));
        for(int i=0;i<4;++i)Put(b,0.f);Put(b,1.f);
        Put(b,static_cast<std::int8_t>(-1));Put(b,static_cast<std::int8_t>(-1));
        Put(b,static_cast<std::uint8_t>(0));Put(b,static_cast<std::uint8_t>(1));Put(b,static_cast<std::uint8_t>(0));
        Text(b,L"");Put(b,f.indices);
    }
    Put(b,host ? 1u : 0u);
    if(host) {
        Text(b,L"root");Text(b,L"root");for(int i=0;i<3;++i)Put(b,0.f);
        SignedIndex(b,-1,boneWidth);Put(b,0u);Put(b,static_cast<std::uint16_t>(0));
        for(float v:{0.f,1.f,0.f})Put(b,v);
    }
    for(int section=0;section<4;++section)Put(b,0u); // morph, display, rigid, joint
    return f;
}
inline Fixture Pmd(std::uint32_t count, bool host = false) {
    Fixture f; f.pmd=true;f.vertices=count;f.indices=count*3;f.vertexWidth=2;
    auto& b=f.bytes;b={'P','m','d'};Put(b,1.f);Fixed(b,"cache-model",20);Fixed(b,"cache test",256);
    Put(b,count);f.vertexStart=static_cast<std::int64_t>(b.size());
    for(std::uint32_t i=0;i<count;++i) {
        for(float v:std::array<float,8>{float(i%3),float((i+1)%3),float(i%2),0,0,1,float(i%2),float((i+1)%2)})Put(b,v);
        Put(b,static_cast<std::int16_t>(host?0:int(i%91)-45));Put(b,static_cast<std::int16_t>(host?0:-1));
        Put(b,static_cast<std::int8_t>(host?100:i%101));Put(b,static_cast<std::uint8_t>(i%2));
    }
    f.vertexEnd=static_cast<std::int64_t>(b.size());Put(b,f.indices);
    for(std::uint32_t i=0;i<f.indices;++i)Put(b,static_cast<std::uint16_t>(count?(i*7+2)%count:0));
    f.indexEnd=static_cast<std::int64_t>(b.size());Put(b,host?1u:0u);
    if(host) {
        for(float v:{.7f,.6f,.5f,1.f,1.f,0.f,0.f,0.f,.1f,.1f,.1f})Put(b,v);
        Put(b,static_cast<std::uint8_t>(0));Put(b,static_cast<std::uint8_t>(0));Put(b,f.indices);Fixed(b,"",20);
    }
    Put(b,static_cast<std::uint16_t>(host?1:0));
    if(host) {Fixed(b,"root",20);Put(b,static_cast<std::uint16_t>(65535));Put(b,static_cast<std::uint16_t>(0));
        Put(b,static_cast<std::uint8_t>(1));Put(b,static_cast<std::uint16_t>(0));for(int i=0;i<3;++i)Put(b,0.f);}
    Put(b,static_cast<std::uint16_t>(0));Put(b,static_cast<std::uint16_t>(0)); // IK and morphs
    Put(b,static_cast<std::uint8_t>(0));Put(b,static_cast<std::uint8_t>(0));Put(b,0u); // facial, groups, grouped bones
    Put(b,static_cast<std::uint8_t>(0));
    for(int i=0;i<10;++i) {char name[32]{};std::snprintf(name,sizeof name,"toon%02d.bmp",i+1);Fixed(b,name,100);}
    Put(b,0u);Put(b,0u); // rigid bodies and joints
    return f;
}
template<class Input> bool OriginalVertex(Input& reader,unsigned additionalUv,unsigned boneWidth,mdl::PmxVertex& v) {
    const auto read=[&](void* out,unsigned n){return reader.Read(out,n)==static_cast<int>(n);};
    const auto index=[&](std::int32_t& out){
        if(boneWidth==1){std::int8_t x=0;if(!read(&x,1))return false;out=x;return true;}
        if(boneWidth==2){std::int16_t x=0;if(!read(&x,2))return false;out=x;return true;}
        return read(&out,4);
    };
    if(!read(&v,32))return false;
    for(unsigned j=0;j<additionalUv;++j)for(unsigned c=0;c<4;++c){float surplus=0;
        if(!read(j<4?&v.additionalUvByComponent[c][j]:&surplus,4))return false;}
    if(!read(&v.weightType,1))return false;
    switch(v.weightType) {
    case mdl::PmxWeightType::bdef2:
        if(!index(v.bone[0])||!index(v.bone[1])||!read(v.weight,4))return false;break;
    case mdl::PmxWeightType::bdef4:
        for(auto& bone:v.bone)if(!index(bone))return false;
        if(!read(v.weight,16))return false;break;
    case mdl::PmxWeightType::sdef: {
        if(!index(v.bone[0])||!index(v.bone[1])||!read(v.weight,4)||!read(&v.sdef,36))return false;
        float mid[3];for(int c=0;c<3;++c)mid[c]=v.weight[0]*v.sdef.r0Offset[c]+(1.f-v.weight[0])*v.sdef.r1Offset[c];
        for(int c=0;c<3;++c){v.sdef.r0Offset[c]-=mid[c];v.sdef.r1Offset[c]-=mid[c];}break;
    }
    default:if(!index(v.bone[0]))return false;break;
    }
    if(!read(&v.edgeScale,4))return false;v.hasPmxEdgeData=1;return true;
}
// Compare only fields the original parser actually writes. Runtime material,
// padding and unused weight slots can have different fresh allocation bytes.
inline Bytes ParsedFields(const mdl::PmxVertex& v,unsigned uv) {
    Bytes b;const auto copy=[&](const void* p,std::size_t n){const auto* q=static_cast<const unsigned char*>(p);b.insert(b.end(),q,q+n);};
    copy(&v,32);for(unsigned c=0;c<4;++c)copy(v.additionalUvByComponent[c],uv*4);
    copy(&v.weightType,1);
    switch(v.weightType){
    case mdl::PmxWeightType::bdef2:copy(v.bone,8);copy(v.weight,4);break;
    case mdl::PmxWeightType::bdef4:copy(v.bone,16);copy(v.weight,16);break;
    case mdl::PmxWeightType::sdef:copy(v.bone,8);copy(v.weight,4);copy(&v.sdef,36);break;
    default:copy(v.bone,4);break;}
    copy(&v.edgeScale,4);copy(&v.hasPmxEdgeData,1);return b;
}
}
