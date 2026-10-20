#pragma once
// Independent byte builder for main's committed MMSN/MMVCURV2 wire layout.
// The production motion encoder is deliberately not used for the input file.
#include "mikumikustudio/animation_curve_motion.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>
namespace curve_v2_fixture {
namespace ac=mikumikustudio::animation_curve;
namespace cm=mikumikustudio::curve_motion;
struct Wire {
    std::vector<std::uint8_t> bytes;
    void U8(unsigned value){bytes.push_back(std::uint8_t(value));}
    void U32(std::uint32_t value){for(unsigned i=0;i<4;++i)U8(value>>(8*i));}
    void U64(std::uint64_t value){U32(std::uint32_t(value));U32(std::uint32_t(value>>32));}
    void F32(float value){std::uint32_t bits=0;std::memcpy(&bits,&value,4);U32(bits);}
    void F64(double value){std::uint64_t bits=0;std::memcpy(&bits,&value,8);U64(bits);}
    void Raw(const void* data,std::size_t size){if(!size)return;const auto* first=static_cast<const std::uint8_t*>(data);bytes.insert(bytes.end(),first,first+size);}
    void Text(const char* text,std::size_t size){const auto count=(std::min)(std::strlen(text),size);Raw(text,count);bytes.resize(bytes.size()+size-count);}
};
inline cm::Motion Source(){
    cm::Motion motion;motion.kind=cm::Kind::Camera;
    ac::Track x;for(const auto point:{std::pair<int,double>{0,0},{1,10},{13,-7}}){ac::Key key;key.frame=point.first;key.value=point.second;
        key.tangent=ac::Tangent::Free;key.interpolation=ac::Interpolation::Bezier;x.keys.push_back(key);}
    x.keys[0].outWeight=.2;x.keys[1].inWeight=.3;x.keys[1].outWeight=.35;x.keys[2].inWeight=.15;
    x.keys[0].outSlope=1.25;x.keys[1].inSlope=-.75;x.keys[1].outSlope=.625;x.keys[2].inSlope=-.5;
    ac::Track y=x;y.keys.erase(y.keys.begin()+1);y.keys[0].value=2;y.keys[1].value=-3;
    y.keys[0].outSlope=2.5;y.keys[1].inSlope=-1.25;
    motion.tracks={{"x",x},{"y",y}};
    motion.nativeTiming={{"x",1,{20,20+5e-13,107,107+5e-13},false},{"x",13,{100.25,10.5,20.75,115.125},true}};
    return motion;
}
inline float Ease(const std::array<double,4>& control,float time){
    if(std::abs(control[0]-control[1])<=1e-12&&std::abs(control[2]-control[3])<=1e-12)return time;
    const float x1=float(control[0])/127.f,x2=float(control[2])/127.f;float parameter=.5f,step=.25f;
    for(int i=0;i<12;++i){const float opposite=1-parameter,three=opposite*3;
        const float value=(three*opposite*parameter)*x1+(three*parameter*parameter)*x2+parameter*parameter*parameter;
        if(value==time)break;parameter=value>=time?parameter-step:parameter+step;step*=.5f;}
    const float opposite=1-parameter,three=opposite*3;
    return (three*opposite*parameter)*float(control[1])/127.f+(three*parameter*parameter)*float(control[3])/127.f+parameter*parameter*parameter;
}
inline double ExpectedX(const cm::Motion& source,double frame){
    const auto& keys=source.tracks[0].track.keys;
    if(frame<=0)return keys[0].value;if(frame>=13)return keys[2].value;
    const unsigned end=frame<1?1u:2u;const auto& a=keys[end-1];const auto& b=keys[end];
    if(frame==a.frame)return a.value;
    return a.value+(b.value-a.value)*Ease(source.nativeTiming[end-1].controls,float((frame-a.frame)/(b.frame-a.frame)));
}
inline std::uint32_t Crc(const std::uint8_t* data,std::size_t size){std::uint32_t result=~0u;
    for(std::size_t i=0;i<size;++i){result^=data[i];for(unsigned bit=0;bit<8;++bit)result=(result>>1)^(0xedb88320u&(0u-(result&1u)));}return ~result;}
inline std::uint64_t U64(const std::vector<std::uint8_t>& bytes,std::size_t at){std::uint64_t value=0;
    for(unsigned i=0;i<8;++i)value|=std::uint64_t(bytes.at(at+i))<<(i*8);return value;}
inline void Set32(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes.at(at+i)=std::uint8_t(value>>(i*8));}
inline void PayloadCrc(std::vector<std::uint8_t>& bytes){const auto footer=bytes.size()-40,prefix=std::size_t(U64(bytes,footer+8));
    Set32(bytes,footer+28,Crc(bytes.data()+prefix,footer-prefix));}
inline std::vector<std::uint8_t> Bytes(const cm::Motion& source=Source()){
    Wire standard;standard.Text("Vocaloid Motion Data 0002",30);standard.Text("MMS timing fixture",20);
    standard.U32(0);standard.U32(0);standard.U32(14);
    for(unsigned frame=0;frame<14;++frame){standard.U32(frame);standard.F32(-45);standard.F32(float(ExpectedX(source,frame)));
        standard.F32(float(ac::Evaluate(source.tracks[1].track,frame).value));standard.F32(0);
        for(unsigned axis=0;axis<3;++axis)standard.F32(0);
        for(unsigned lane=0;lane<6;++lane){standard.U8(20);standard.U8(107);standard.U8(20);standard.U8(107);}
        standard.U32(45);standard.U8(0);}
    standard.U32(0);standard.U32(0);standard.U32(0);
    std::vector<std::uint8_t> scalar;std::string error;if(!ac::Encode(source.tracks,scalar,error))throw std::runtime_error(error);
    Wire payload;payload.Raw("MMSN",4);payload.U64(scalar.size());payload.Raw(scalar.data(),scalar.size());payload.U32(std::uint32_t(source.nativeTiming.size()));
    for(const auto& timing:source.nativeTiming){payload.U32(std::uint32_t(timing.id.size()));payload.Raw(timing.id.data(),timing.id.size());
        payload.U32(std::uint32_t(timing.endFrame));payload.U32(timing.oneFrameHold?1u:0u);for(double control:timing.controls)payload.F64(control);}
    const auto prefix=standard.bytes.size();const auto prefixCrc=Crc(standard.bytes.data(),prefix),payloadCrc=Crc(payload.bytes.data(),payload.bytes.size());
    standard.Raw(payload.bytes.data(),payload.bytes.size());standard.Raw("MMVCURV2",8);standard.U64(prefix);standard.U64(payload.bytes.size());
    standard.U32(prefixCrc);standard.U32(payloadCrc);standard.U32(0);standard.U32(0);return standard.bytes;
}
}
