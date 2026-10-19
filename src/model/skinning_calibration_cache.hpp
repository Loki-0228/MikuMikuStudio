#pragma once
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/geometry_workers.hpp"
#include <Windows.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace mikumikustudio::skinning::calibration_cache {
// Bump when the benchmark geometry, CPU/GPU algorithm or measurement changes.
constexpr std::uint32_t kVersion=2;
constexpr std::uint64_t kMagic=0x31484341434e4b53ull;
inline std::filesystem::path Path(const wchar_t* product) {
    // Match Data/mmconfig.ini: bind to the executable, never to the current
    // directory (opening a model or project can change it).
    wchar_t executable[32768]{};
    const auto n=GetModuleFileNameW(nullptr,executable,32768);
    if(!n || n>=32768)return {};
    return std::filesystem::path(executable).parent_path()/L"Data"/
        (std::wstring(L"processor-config-")+product+L"-v1.bin");
}
inline std::filesystem::path LegacyPath(const wchar_t* product) {
    wchar_t local[32768]{};
    const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    if(!n || n>=32768)return {};
    return std::filesystem::path(local)/L"MikuDanceStudio"/
        (std::wstring(L"skinning-")+product+L"-v1.bin");
}
inline std::wstring CpuKey() {
    wchar_t identifier[512]{};
    GetEnvironmentVariableW(L"PROCESSOR_IDENTIFIER",identifier,512);
    SYSTEM_INFO info{};GetNativeSystemInfo(&info);
    return std::wstring(identifier)+L"/"+std::to_wstring(info.wProcessorArchitecture)+L"/"+
        std::to_wstring(info.wProcessorLevel)+L"/"+std::to_wstring(info.wProcessorRevision)+L"/"+
        std::to_wstring(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS))+L"/"+
        std::to_wstring(GeometryWorkerCount());
}
template<class T> bool Read(std::istream& in,T& value) {
    return bool(in.read(reinterpret_cast<char*>(&value),sizeof value));
}
template<class T> void Write(std::ostream& out,const T& value) {
    out.write(reinterpret_cast<const char*>(&value),sizeof value);
}
inline bool ReadString(std::istream& in,std::wstring& value) {
    std::uint32_t size=0;if(!Read(in,size)||size>4096)return false;
    value.resize(size);
    return bool(in.read(reinterpret_cast<char*>(value.data()),size*sizeof(wchar_t)));
}
inline void WriteString(std::ostream& out,const std::wstring& value) {
    Write(out,static_cast<std::uint32_t>(value.size()));
    out.write(reinterpret_cast<const char*>(value.data()),value.size()*sizeof(wchar_t));
}
inline std::wstring AdapterKey(const gpu::Adapter& adapter) {
    // LUIDs can change at reboot. Match stable identity and driver instead.
    return adapter.name+L"/"+adapter.hardwareKey+L"/"+std::to_wstring(adapter.integrated);
}
inline bool Load(const std::filesystem::path& path,const std::wstring& cpu,
                 const std::vector<gpu::Adapter>& adapters,std::vector<StartupMeasurement>& result) {
    try {
        std::ifstream in(path,std::ios::binary);
        std::uint64_t magic=0;std::uint32_t version=0,count=0;std::wstring savedCpu;
        if(!Read(in,magic)||magic!=kMagic||!Read(in,version)||version!=kVersion||
           !ReadString(in,savedCpu)||savedCpu!=cpu||!Read(in,count)||
           count!=adapters.size()||count>32)return false;
        std::vector<std::uint64_t> ids{0};std::vector<bool> used(count,false);
        for(unsigned i=0;i<count;++i) {
            std::wstring key;if(!ReadString(in,key))return false;
            unsigned j=0;
            for(;j<count;++j)if(!used[j] && key==AdapterKey(adapters[j]))break;
            if(j==count)return false;
            used[j]=true;ids.push_back(adapters[j].id);
        }
        std::vector<StartupMeasurement> loaded;
        for(bool pmx : {false,true})for(unsigned vertices : {1024u,32768u,262144u}) {
            StartupMeasurement sample;sample.pmx=pmx;sample.vertexCount=vertices;
            for(unsigned i=0;i<ids.size();++i) {
                double ms=0;std::uint32_t valid=0;
                if(!Read(in,ms)||!Read(in,valid)||valid>1||!std::isfinite(ms)||ms<0||
                   (valid && ms<=0)||(!i && !valid))return false;
                sample.report.measurements.push_back({ids[i],ms,valid!=0});
            }
            sample.report.selectedAdapterId=FastestMeasurement(sample.report.measurements);
            loaded.push_back(std::move(sample));
        }
        if(in.peek()!=std::char_traits<char>::eof())return false;
        result=std::move(loaded);return true;
    }catch(...){return false;}
}
inline bool Save(const std::filesystem::path& path,const std::wstring& cpu,
                 const std::vector<gpu::Adapter>& adapters,const std::vector<StartupMeasurement>& samples) {
    if(path.empty()||samples.size()!=6||adapters.size()>32)return false;
    const auto temp=std::filesystem::path(path.wstring()+L".tmp."+std::to_wstring(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);
        Write(out,kMagic);Write(out,kVersion);WriteString(out,cpu);
        Write(out,static_cast<std::uint32_t>(adapters.size()));
        for(const auto& adapter:adapters)WriteString(out,AdapterKey(adapter));
        for(unsigned n=0;n<samples.size();++n) {
            const auto& sample=samples[n];const unsigned sizes[]={1024,32768,262144};
            if(sample.pmx!=(n>=3)||sample.vertexCount!=sizes[n%3]||
               sample.report.measurements.size()!=adapters.size()+1)throw std::runtime_error("profile layout");
            for(unsigned i=0;i<sample.report.measurements.size();++i) {
                const auto& m=sample.report.measurements[i];
                if(m.adapterId!=(i?adapters[i-1].id:0))throw std::runtime_error("adapter layout");
                Write(out,m.milliseconds);Write(out,static_cast<std::uint32_t>(m.valid));
            }
        }
        out.close();if(!out)throw std::runtime_error("cache write");
        if(MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
    }catch(...){}
    DeleteFileW(temp.c_str());return false;
}
}
