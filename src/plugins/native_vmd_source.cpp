#include "native_vmd_source.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../model/vmd_read_guard.hpp"
#include "mikudancestudio/accessory_motion_io.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/global_key_layout.hpp"
#include "mikudancestudio/key_pool.hpp"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <stdexcept>
#include <sys/stat.h>
namespace mikudancestudio::plugins {
namespace {
struct VmdDescriptor {int value=-1;~VmdDescriptor(){if(value>=0)_close(value);}};
template<class T> T VmdValue(const std::uint8_t* bytes){T value;std::memcpy(&value,bytes,sizeof(value));return value;}
void VmdFinite(const std::uint8_t* bytes,unsigned count){for(unsigned i=0;i<count;++i)if(!std::isfinite(VmdValue<float>(bytes+4*i)))throw std::invalid_argument("nonfinite VMD value");}
void VmdCurve(const std::uint8_t* bytes,unsigned count){for(unsigned i=0;i<count;++i)if(bytes[i]>127)throw std::invalid_argument("invalid VMD interpolation");}
}
MmsResult DecodeGlobalVmdSource(const char* ansi,const wchar_t* wide,std::uint32_t offset,GlobalVmdSource& out) noexcept {
    if((!wide&&!ansi)||offset>unsigned(frame_limits::maximum))return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        VmdDescriptor descriptor;
        const auto error=wide?_wsopen_s(&descriptor.value,wide,_O_BINARY|_O_RDONLY,_SH_DENYWR,_S_IREAD):
            _sopen_s(&descriptor.value,ansi,_O_BINARY|_O_RDONLY,_SH_DENYWR,_S_IREAD);
        if(error)return error==ENOENT||error==ENOTDIR?MMS_E_NOT_FOUND:MMS_E_FAILED;
        vmd_read::Input input;if(!input.Load(descriptor.value))return MMS_E_ARGUMENT;
        unsigned last;if(!frame_limits::Add(int(offset),input.maximumFrame,last))return MMS_E_ARGUMENT;
        if(input.cameraCount>mdl::KeyPoolLimit<mdl::CameraKey>()||input.lightCount>mdl::KeyPoolLimit<mdl::LightKey>()||
            input.shadowCount>mdl::KeyPoolLimit<mdl::SelfShadowKey>())return MMS_E_ARGUMENT;
        GlobalVmdSource decoded;decoded.version=input.version;decoded.source.frameOffset=offset;
        const auto nameBytes=input.version==1?10u:20u;const auto* name=reinterpret_cast<const char*>(input.Data(30));
        decoded.modelName.assign(name,strnlen_s(name,nameBytes));
        constexpr char cameraName[]="\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe";
        if(input.version==2&&decoded.modelName!=cameraName)return MMS_E_ARGUMENT;
        // Global import discards these standard tracks, but their numeric data
        // and maximum frames still participate in whole-file admission.
        auto at=std::size_t(30+nameBytes+4);
        for(unsigned i=0;i<input.boneCount;++i,at+=111){const auto* record=input.Data(at);VmdFinite(record+19,7);VmdCurve(record+47,64);}
        at+=4;
        for(unsigned i=0;i<input.morphCount;++i,at+=23)VmdFinite(input.Data(at)+19,1);
        decoded.source.cameras.reserve(input.cameraCount);
        if(input.cameraOffset!=vmd_read::Input::absent){
            at=input.cameraOffset+4;
            for(unsigned i=0;i<input.cameraCount;++i,at+=input.version==1?36:61){
                const auto* record=input.Data(at);VmdFinite(record+4,7);mdl::CameraClipboardRecord key{};
                key.frame=VmdValue<std::uint32_t>(record);key.distance=VmdValue<float>(record+4);
                std::memcpy(key.eye,record+8,sizeof(key.eye));std::memcpy(key.target,record+20,sizeof(key.target));
                key.parentModel=-1;key.parentBone=0;
                if(input.version==1){
                    VmdCurve(record+32,4);for(unsigned lane=0;lane<6;++lane){key.interpolation[lane]=record[32];key.interpolation[12+lane]=record[33];key.interpolation[6+lane]=record[34];key.interpolation[18+lane]=record[35];}
                    key.fov=45;key.perspective=0;
                }else{
                    VmdCurve(record+32,24);for(unsigned lane=0;lane<6;++lane){key.interpolation[lane]=record[32+4*lane];key.interpolation[12+lane]=record[33+4*lane];key.interpolation[6+lane]=record[34+4*lane];key.interpolation[18+lane]=record[35+4*lane];}
                    key.fov=VmdValue<std::int32_t>(record+56);key.perspective=std::uint8_t(record[60]!=0);
                    if(key.fov<1||key.fov>179)return MMS_E_ARGUMENT;
                }
                decoded.source.cameras.push_back(key);
            }
        }
        decoded.source.lights.reserve(input.lightCount);
        if(input.lightOffset!=vmd_read::Input::absent){
            at=input.lightOffset+4;
            for(unsigned i=0;i<input.lightCount;++i,at+=28){const auto* record=input.Data(at);VmdFinite(record+4,6);mdl::LightClipboardRecord key{};
                key.frame=VmdValue<std::uint32_t>(record);std::memcpy(key.color,record+4,sizeof(key.color));std::memcpy(key.direction,record+16,sizeof(key.direction));decoded.source.lights.push_back(key);}
        }
        decoded.source.shadows.reserve(input.shadowCount);
        if(input.shadowOffset!=vmd_read::Input::absent){
            at=input.shadowOffset+4;
            for(unsigned i=0;i<input.shadowCount;++i,at+=9){const auto* record=input.Data(at);VmdFinite(record+5,1);mdl::ShadowClipboardRecord key{};
                key.frame=VmdValue<std::uint32_t>(record);key.mode=record[4];key.distance=VmdValue<float>(record+5);
                if(key.mode>2||key.distance<0||key.distance>1)return MMS_E_ARGUMENT;decoded.source.shadows.push_back(key);}
        }
        const auto tailBytes=input.Size()-input.standardEnd;
        if(tailBytes>=sizeof(accessory_motion_io::kMagic)&&!std::memcmp(input.Data(input.standardEnd),accessory_motion_io::kMagic,sizeof(accessory_motion_io::kMagic))){
            accessory_motion_io::Decode(input.Data(input.standardEnd),tailBytes,decoded.accessoryTracks);
        }
        out=std::move(decoded);return MMS_OK;
    });
}
}
#endif
