#include "native_vmd_source.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../model/vmd_read_guard.hpp"
#include "../model/vmd_prepare.hpp"
#include "mikumikustudio/accessory_motion_io.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/key_pool.hpp"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
namespace mikumikustudio::plugins {
namespace {
struct VmdDescriptor {int value=-1;~VmdDescriptor(){if(value>=0)_close(value);}};
template<class T> T VmdValue(const std::uint8_t* bytes){T value;std::memcpy(&value,bytes,sizeof(value));return value;}
bool VmdFinite(const std::uint8_t* bytes, unsigned count) noexcept {
    for (unsigned i = 0; i < count; ++i)
        if (!std::isfinite(VmdValue<float>(bytes + 4 * i))) return false;
    return true;
}
bool VmdCurve(const std::uint8_t* bytes, unsigned count) noexcept {
    for (unsigned i = 0; i < count; ++i)
        if (bytes[i] > 127) return false;
    return true;
}
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
        const auto boneStart = std::size_t(30 + nameBytes + 4);
        const auto morphStart = boneStart + std::size_t(input.boneCount) * 111 + 4;
        if (!vmd_read::PrepareRecords(input.boneCount, [&](std::size_t i) noexcept {
            const auto* record = input.Data(boneStart + i * 111);
            return VmdFinite(record + 19, 7) && VmdCurve(record + 47, 64);
        }) || !vmd_read::PrepareRecords(input.morphCount, [&](std::size_t i) noexcept {
            return VmdFinite(input.Data(morphStart + i * 23) + 19, 1);
        })) return MMS_E_ARGUMENT;
        decoded.source.cameras.resize(input.cameraCount);
        const auto cameraStride = input.version == 1 ? 36u : 61u;
        if (!vmd_read::PrepareRecords(decoded.source.cameras.size(), [&](std::size_t i) noexcept {
            const auto* record = input.Data(input.cameraOffset + 4 + i * cameraStride);
            if (!VmdFinite(record + 4, 7)) return false;
            auto& key = decoded.source.cameras[i];
            key.frame = VmdValue<std::uint32_t>(record);
            key.distance = VmdValue<float>(record + 4);
            std::memcpy(key.eye, record + 8, sizeof(key.eye));
            std::memcpy(key.target, record + 20, sizeof(key.target));
            key.parentModel = -1;
            key.parentBone = 0;
            const bool old = input.version == 1;
            if (!VmdCurve(record + 32, old ? 4 : 24)) return false;
            for (unsigned lane = 0; lane < 6; ++lane) {
                const auto* curve = record + 32 + (old ? 0 : 4 * lane);
                key.interpolation[lane] = curve[0];
                key.interpolation[12 + lane] = curve[1];
                key.interpolation[6 + lane] = curve[2];
                key.interpolation[18 + lane] = curve[3];
            }
            key.fov = old ? 45 : VmdValue<std::int32_t>(record + 56);
            key.perspective = old ? 0 : std::uint8_t(record[60] != 0);
            return key.fov >= 1 && key.fov <= 179;
        })) return MMS_E_ARGUMENT;
        decoded.source.lights.resize(input.lightCount);
        if (!vmd_read::PrepareRecords(decoded.source.lights.size(), [&](std::size_t i) noexcept {
            const auto* record = input.Data(input.lightOffset + 4 + i * 28);
            if (!VmdFinite(record + 4, 6)) return false;
            auto& key = decoded.source.lights[i];
            key.frame = VmdValue<std::uint32_t>(record);
            std::memcpy(key.color, record + 4, sizeof(key.color));
            std::memcpy(key.direction, record + 16, sizeof(key.direction));
            return true;
        })) return MMS_E_ARGUMENT;
        decoded.source.shadows.resize(input.shadowCount);
        if (!vmd_read::PrepareRecords(decoded.source.shadows.size(), [&](std::size_t i) noexcept {
            const auto* record = input.Data(input.shadowOffset + 4 + i * 9);
            if (!VmdFinite(record + 5, 1)) return false;
            auto& key = decoded.source.shadows[i];
            key.frame = VmdValue<std::uint32_t>(record);
            key.mode = record[4];
            key.distance = VmdValue<float>(record + 5);
            return key.mode <= 2 && key.distance >= 0 && key.distance <= 1;
        })) return MMS_E_ARGUMENT;
        const auto tailBytes=input.Size()-input.standardEnd;
        if(tailBytes>=sizeof(accessory_motion_io::kMagic)&&!std::memcmp(input.Data(input.standardEnd),accessory_motion_io::kMagic,sizeof(accessory_motion_io::kMagic))){
            accessory_motion_io::Decode(input.Data(input.standardEnd),tailBytes,decoded.accessoryTracks);
        }
        out=std::move(decoded);return MMS_OK;
    });
}
}
#endif
