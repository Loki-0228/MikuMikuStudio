// Production Manager with two separately loaded fixture DLLs. Device event
// probes use fixed storage so allocation failures test the host dispatch path.
#include "manager.hpp"
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

static thread_local bool failAllocation=false;
static thread_local unsigned allocationHits=0;
void* operator new(std::size_t n) {
    if(failAllocation){++allocationHits;throw std::bad_alloc();}
    if(auto* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}

namespace fs=std::filesystem;
using namespace mikumikustudio::plugins;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #x); } while(false)
using DeviceHook=bool(MMS_CALL*)(void*,const MmsEventV1*);
using SetDeviceHook=void(MMS_CALL*)(DeviceHook,void*);
using OrdinaryHook=void(MMS_CALL*)(void*);
using SetOrdinaryHook=void(MMS_CALL*)(OrdinaryHook,void*);

struct Row {unsigned recipient=0;uint32_t kind=0;uint64_t generation=0,sequence=0;};
struct Shared {
    Manager* manager=nullptr;
    std::array<Row,64> rows{};
    unsigned count=0;
    bool invalid=false,interruptRestore=false,interrupted=false,nestedFlush=true;
    bool throwFirst=false,thrown=false;
};
struct Probe {Shared* shared=nullptr;unsigned index=0;};
static bool MMS_CALL Observe(void* context,const MmsEventV1* event) {
    auto& p=*static_cast<Probe*>(context);auto& s=*p.shared;
    if(!event || event->payloadVersion!=1 || event->payload.bytes!=sizeof(MmsDeviceEventV1) || !event->payload.data){s.invalid=true;return true;}
    const auto& payload=*static_cast<const MmsDeviceEventV1*>(event->payload.data);
    if(payload.h.size!=sizeof(payload)||payload.h.major!=1||payload.h.minor!=0||!payload.deviceGeneration||!event->sequence){s.invalid=true;return true;}
    if(s.count==s.rows.size()){s.invalid=true;return true;}
    s.rows[s.count++]={p.index,event->kind,payload.deviceGeneration,event->sequence};
    if(s.interruptRestore&&!s.interrupted&&event->kind==MMS_EVENT_DEVICE_RESTORED&&payload.deviceGeneration==2){
        s.interrupted=true;s.manager->RecordDeviceLost(2);
        s.nestedFlush=s.manager->FlushDeviceEvents();
    }
    if(s.throwFirst&&!s.thrown){s.thrown=true;throw 7;}
    return true; // Avoid fixture logging and resource creation in no-allocation cases.
}
static unsigned Count(const Shared& s,unsigned who,uint32_t kind,uint64_t generation){
    unsigned n=0;for(unsigned i=0;i<s.count;++i){const auto& row=s.rows[i];if(row.recipient==who&&row.kind==kind&&row.generation==generation)++n;}return n;
}
static void Write(const fs::path& path,const std::string& content){
    std::ofstream f(path,std::ios::binary);f<<content;f.close();CHECK(bool(f));
}
static std::string FixtureManifest(unsigned index){
    return "{\"schemaVersion\":1,\"id\":\"org.example.device-events-"+std::to_string(index)+
        "\",\"name\":\"device events\",\"version\":\"1.0.0\",\"kind\":\"native\","
        "\"abi\":{\"major\":1,\"minMinor\":0},\"entry\":\"lifecycle-"+std::to_string(index)+".dll\",\"architecture\":\"x64\","
        "\"minWindows\":\"6.1.7601\",\"capabilities\":[\"ui.command\"],"
        "\"requiresServices\":[{\"id\":\"mms.ui\",\"major\":1,\"minMinor\":0}],"
        "\"dependencies\":[],\"nativeDependencies\":[],\"reloadPolicy\":\"restart\"}";
}
struct Harness {
    Shared shared;
    std::array<Probe,2> probes{};
    std::array<fs::path,2> files{};
    std::array<SetDeviceHook,2> setDevice{};
    std::array<SetOrdinaryHook,2> setOrdinary{};
    // The manager is destroyed before contexts even after a failed assertion.
    std::unique_ptr<Manager> manager;
    Harness(const fs::path& base,const fs::path& fixture){
        for(unsigned i=0;i<2;++i){
            const auto package=base/"PLUGINS"/std::to_string(i);
            CHECK(fs::create_directories(package));files[i]=package/("lifecycle-"+std::to_string(i)+".dll");
            CHECK(fs::copy_file(fixture,files[i]));Write(package/"plugin.json",FixtureManifest(i));
        }
        manager=std::make_unique<Manager>(base/"data",[](const std::string&){});
        shared.manager=manager.get();manager->Load(base/"PLUGINS");
        const auto state=manager->Status();CHECK(state.size()==2);
        for(const auto& row:state)CHECK(row.stage=="started");
        for(unsigned i=0;i<2;++i){
            auto dll=GetModuleHandleW(files[i].c_str());CHECK(dll!=nullptr);
            setDevice[i]=reinterpret_cast<SetDeviceHook>(GetProcAddress(dll,"MmsTestSetDeviceEventHook"));
            setOrdinary[i]=reinterpret_cast<SetOrdinaryHook>(GetProcAddress(dll,"MmsTestSetRenderReentryHook"));
            CHECK(setDevice[i]&&setOrdinary[i]);probes[i]={&shared,i};setDevice[i](Observe,&probes[i]);
        }
    }
    void Stop(){CHECK(manager->Stop());for(const auto& file:files)CHECK(GetModuleHandleW(file.c_str())==nullptr);}
};
static void CoalescedNoAllocation(const fs::path& root,const fs::path& fixture){
    Harness h(root/"coalesced",fixture);auto& m=*h.manager;auto& s=h.shared;
    CHECK(!m.DeviceEventsPending());m.RecordDeviceLost(0);m.RecordDeviceRestored(0);m.RecordDeviceRestored(2);
    CHECK(!m.DeviceEventsPending()&&m.FlushDeviceEvents()&&s.count==0);
    allocationHits=0;failAllocation=true;
    m.RecordDeviceLost(1);m.RecordDeviceLost(1);m.RecordDeviceRestored(2);
    m.RecordDeviceLost(2);m.RecordDeviceRestored(3);
    const bool pending=m.DeviceEventsPending(),flushed=m.FlushDeviceEvents();
    failAllocation=false;
    CHECK(pending&&flushed&&!m.DeviceEventsPending()&&allocationHits==0);
    CHECK(!s.invalid&&s.count==4);
    CHECK(s.rows[0].kind==MMS_EVENT_DEVICE_LOST&&s.rows[1].kind==MMS_EVENT_DEVICE_LOST);
    CHECK(s.rows[2].kind==MMS_EVENT_DEVICE_RESTORED&&s.rows[3].kind==MMS_EVENT_DEVICE_RESTORED);
    for(unsigned i=0;i<2;++i){CHECK(Count(s,i,MMS_EVENT_DEVICE_LOST,2)==1);CHECK(Count(s,i,MMS_EVENT_DEVICE_RESTORED,3)==1);}
    for(unsigned who=0;who<2;++who){uint64_t previous=0;for(unsigned i=0;i<s.count;++i)if(s.rows[i].recipient==who){CHECK(s.rows[i].sequence>previous);previous=s.rows[i].sequence;}}
    m.RecordDeviceRestored(3);CHECK(m.FlushDeviceEvents()&&!m.DeviceEventsPending()&&s.count==4);
    h.Stop();
}
static void ChangeDuringRestore(const fs::path& root,const fs::path& fixture){
    Harness h(root/"interrupt",fixture);auto& m=*h.manager;auto& s=h.shared;s.interruptRestore=true;
    m.RecordDeviceLost(1);m.RecordDeviceRestored(2);
    CHECK(!m.FlushDeviceEvents()&&m.DeviceEventsPending());
    CHECK(s.interrupted&&!s.nestedFlush&&!s.invalid);
    CHECK(s.count==3&&s.rows[2].kind==MMS_EVENT_DEVICE_RESTORED&&s.rows[2].generation==2);
    const unsigned first=s.rows[2].recipient,other=1-first;
    CHECK(Count(s,first,MMS_EVENT_DEVICE_RESTORED,2)==1&&Count(s,other,MMS_EVENT_DEVICE_RESTORED,2)==0);
    CHECK(m.FlushDeviceEvents()&&!m.DeviceEventsPending());
    CHECK(s.count==5);
    m.RecordDeviceRestored(3);CHECK(m.FlushDeviceEvents()&&!m.DeviceEventsPending());
    CHECK(s.count==7&&!s.invalid);
    for(unsigned i=0;i<2;++i){CHECK(Count(s,i,MMS_EVENT_DEVICE_LOST,1)==1);CHECK(Count(s,i,MMS_EVENT_DEVICE_LOST,2)==1);CHECK(Count(s,i,MMS_EVENT_DEVICE_RESTORED,3)==1);}
    CHECK(Count(s,other,MMS_EVENT_DEVICE_RESTORED,2)==0);
    CHECK(m.FlushDeviceEvents()&&s.count==7);h.Stop();
}
static void ExceptionContinues(const fs::path& root,const fs::path& fixture){
    Harness h(root/"throw",fixture);auto& m=*h.manager;auto& s=h.shared;s.throwFirst=true;
    m.RecordDeviceLost(1);m.RecordDeviceRestored(2);
    allocationHits=0;failAllocation=true;const bool flushed=m.FlushDeviceEvents();failAllocation=false;
    CHECK(flushed&&!m.DeviceEventsPending()&&s.thrown&&!s.invalid&&s.count==4);
    // The callback-error log may allocate and fail; delivery must still finish.
    for(unsigned i=0;i<2;++i){CHECK(Count(s,i,MMS_EVENT_DEVICE_LOST,1)==1);CHECK(Count(s,i,MMS_EVENT_DEVICE_RESTORED,2)==1);}
    CHECK(m.FlushDeviceEvents()&&s.count==4);h.Stop();
}
struct OrdinaryProbe {Manager* manager;Shared* shared;unsigned calls=0;bool flushed=true,inside=false;unsigned seen=0;};
static void MMS_CALL RecordInsideOrdinary(void* context){
    auto& p=*static_cast<OrdinaryProbe*>(context);++p.calls;p.inside=p.manager->InCallback();
    p.manager->RecordDeviceLost(1);p.manager->RecordDeviceRestored(2);
    p.flushed=p.manager->FlushDeviceEvents();p.seen=p.shared->count;
}
static void RejectedBoundaries(const fs::path& root,const fs::path& fixture){
    Harness h(root/"boundaries",fixture);auto& m=*h.manager;auto& s=h.shared;
    OrdinaryProbe probe{&m,&s};h.setOrdinary[0](RecordInsideOrdinary,&probe);
    MmsCodeEventV1 language{Header<MmsCodeEventV1>(),0,0};m.Event(MMS_EVENT_LANGUAGE_CHANGED,{&language,sizeof(language)},1);
    h.setOrdinary[0](nullptr,nullptr);
    CHECK(probe.calls==1&&probe.inside&&!probe.flushed&&probe.seen==0&&m.DeviceEventsPending());
    CHECK(m.FlushDeviceEvents()&&s.count==4);
    m.RecordDeviceLost(2);CHECK(m.Storage().Prepare({})==MMS_OK);
    CHECK(!m.FlushDeviceEvents()&&m.DeviceEventsPending()&&s.count==4);
    m.Storage().Cancel();CHECK(!m.Storage().Prepared());
    m.Render().MarkDeviceLost(2);
    CHECK(!m.FlushDeviceEvents()&&m.DeviceEventsPending()&&s.count==4);
    CHECK(m.Render().FlushDeviceLost());CHECK(m.FlushDeviceEvents()&&!m.DeviceEventsPending()&&s.count==6);
    m.Render().DeviceRestored(3);m.RecordDeviceRestored(3);
    CHECK(m.FlushDeviceEvents()&&s.count==8&&!s.invalid);
    h.Stop();
}
static void RetirementAndFreshManager(const fs::path& root,const fs::path& fixture){
    {
        Harness h(root/"retired",fixture);auto& m=*h.manager;m.RecordDeviceLost(1);m.RecordDeviceRestored(2);
        CHECK(m.DeviceEventsPending());h.Stop();
        CHECK(m.FlushDeviceEvents()&&!m.DeviceEventsPending()&&h.shared.count==0);
    }
    Harness h(root/"fresh",fixture);auto& m=*h.manager;
    CHECK(!m.DeviceEventsPending()&&m.FlushDeviceEvents()&&h.shared.count==0);
    m.RecordDeviceLost(1);m.RecordDeviceRestored(2);CHECK(m.FlushDeviceEvents()&&h.shared.count==4);
    for(unsigned i=0;i<2;++i){CHECK(Count(h.shared,i,MMS_EVENT_DEVICE_LOST,1)==1);CHECK(Count(h.shared,i,MMS_EVENT_DEVICE_RESTORED,2)==1);}
    h.Stop();
}
int wmain(int argc,wchar_t** argv){
    if(argc!=3){std::fputs("usage: device_event_tests <output-parent> <lifecycle.dll>\n",stderr);return 2;}
    try{
        const auto root=fs::absolute(argv[1])/("run-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        CHECK(!fs::exists(root)&&fs::create_directories(root));CHECK(SetEnvironmentVariableW(L"MMS_ABI_PROBE_LOG",nullptr));
        const auto fixture=fs::absolute(argv[2]);CHECK(fs::is_regular_file(fixture));
        CoalescedNoAllocation(root,fixture);ChangeDuringRestore(root,fixture);ExceptionContinues(root,fixture);
        RejectedBoundaries(root,fixture);RetirementAndFreshManager(root,fixture);
        std::printf("device events PASS checks=%u\n",checks);return 0;
    }catch(const std::exception& e){failAllocation=false;std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
