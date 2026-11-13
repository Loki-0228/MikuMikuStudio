#include "manager.hpp"
#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs=std::filesystem;
using namespace mikumikustudio::plugins;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #x); } while(false)
static constexpr const char* id="org.example.storage-lease";
static StoredRecords Records(unsigned char value,uint32_t schema) {
    StoredRecords data; data[id]={schema,std::vector<unsigned char>(1024,value)}; return data;
}
struct Fixture {
    using Read=MmsResult(MMS_CALL*)(unsigned);
    using Check=bool(MMS_CALL*)(unsigned,uint32_t,unsigned char);
    using Same=bool(MMS_CALL*)(unsigned,unsigned);
    using Release=void(MMS_CALL*)(unsigned);
    using Stops=unsigned(MMS_CALL*)();
    Manager manager;
    fs::path dll;
    Read read=nullptr; Check check=nullptr; Same same=nullptr; Release release=nullptr; Stops stops=nullptr;
    bool stopped=false;
    Fixture(const fs::path& base,const fs::path& source):manager(base/"data",[](const std::string& message) {
        std::fprintf(stdout,"manager: %s\n",message.c_str());
    }) {
        const auto package=base/"PLUGINS"/"storage";CHECK(fs::create_directories(package));
        dll=package/"storage-fixture.dll";CHECK(fs::copy_file(source,dll));
        std::ofstream out(package/"plugin.json",std::ios::binary);
        out<<"{\"schemaVersion\":1,\"id\":\"org.example.storage-lease\",\"name\":\"Storage lease fixture\","
             "\"version\":\"1.0.0\",\"kind\":\"native\",\"abi\":{\"major\":1,\"minMinor\":0},"
             "\"entry\":\"storage-fixture.dll\",\"architecture\":\"x64\",\"minWindows\":\"6.1.7601\","
             "\"capabilities\":[\"project.storage\"],\"requiresServices\":[{\"id\":\"mms.storage\",\"major\":1,\"minMinor\":0}],"
             "\"dependencies\":[],\"nativeDependencies\":[],\"reloadPolicy\":\"restart\"}";
        out.close();CHECK(bool(out));
        auto initial=Records(0x31,1); CHECK(manager.Storage().Prepare(std::move(initial))==MMS_OK && manager.Storage().Commit());
        manager.Load(base/"PLUGINS"); const auto status=manager.Status();CHECK(status.size()==1 && status[0].stage=="started");
        HMODULE module=GetModuleHandleW(dll.c_str());CHECK(module);
        read=reinterpret_cast<Read>(GetProcAddress(module,"MmsStorageFixtureRead"));
        check=reinterpret_cast<Check>(GetProcAddress(module,"MmsStorageFixtureCheck"));
        same=reinterpret_cast<Same>(GetProcAddress(module,"MmsStorageFixtureSame"));
        release=reinterpret_cast<Release>(GetProcAddress(module,"MmsStorageFixtureRelease"));
        stops=reinterpret_cast<Stops>(GetProcAddress(module,"MmsStorageFixtureStops"));
        CHECK(read && check && same && release && stops);
    }
    ~Fixture() {
        if(!stopped && release && GetModuleHandleW(dll.c_str())) {
            for(unsigned i=0;i<3;++i) release(i);
            manager.Stop();
        }
    }
};
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) {std::fprintf(stderr,"usage: storage_lease_manager_tests <output-parent> <fixture.dll>\n");return 2;}
    try {
        const auto root=fs::absolute(argv[1])/("run-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        CHECK(!fs::exists(root) && fs::create_directories(root));
        const auto source=fs::absolute(argv[2]);CHECK(fs::is_regular_file(source));
        Fixture f(root,source);
        CHECK(f.read(0)==MMS_OK && f.read(1)==MMS_OK && f.same(0,1));
        CHECK(f.check(0,1,0x31) && f.check(1,1,0x31));
        auto next=Records(0x42,2);CHECK(f.manager.Storage().Prepare(std::move(next))==MMS_OK && f.manager.Storage().Commit());
        CHECK(f.read(2)==MMS_OK && !f.same(0,2));
        CHECK(f.check(0,1,0x31) && f.check(1,1,0x31) && f.check(2,2,0x42));
        CHECK(!f.manager.Stop() && GetModuleHandleW(f.dll.c_str()) && f.stops()==1);
        CHECK(f.check(0,1,0x31) && f.check(2,2,0x42));
        f.release(0);CHECK(f.read(0)==MMS_E_NOT_READY);
        f.release(1);
        CHECK(!f.manager.Stop() && GetModuleHandleW(f.dll.c_str()) && f.stops()==1);
        CHECK(f.check(2,2,0x42));
        f.release(2);
        CHECK(f.manager.Stop()); f.stopped=true;
        CHECK(GetModuleHandleW(f.dll.c_str())==nullptr);
        std::printf("storage lease manager PASS: %u checks\n",checks); return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"storage lease manager FAIL: %s\n",e.what());return 1;}
}
