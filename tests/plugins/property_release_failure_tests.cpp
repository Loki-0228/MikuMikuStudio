#include "../../src/plugins/ui_registry.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace { thread_local bool failNextAllocation=false; }
void* operator new(std::size_t size) {
    if (failNextAllocation) { failNextAllocation=false; throw std::bad_alloc(); }
    if (auto* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio::plugins;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (false)
MmsString S(const char* text) { return {text,std::strlen(text)}; }
struct Fixture {
    UiRegistry registry;
    UiClient owner{&registry,"example.owner"},reader{&registry,"example.reader"};
    MmsUiApiV1 ownerApi=registry.Api(owner),readerApi=registry.Api(reader);
    int mode=0;unsigned reads=0,releases=0;
    bool Register() {
        MmsPropertyDescV1 p{};p.info.h=Header<MmsPropertyDescV1>();p.info.id=S("example.owner.value");
        p.info.ownerPlugin=S("example.owner");p.info.label=S("Value");p.info.flags=MMS_PROPERTY_READ_ONLY;
        p.info.valueType=MMS_VALUE_INT;p.context=this;
        p.read=[](void* context,MmsObjectRef,MmsValue* out,MmsHandle* lease)->MmsResult {
            auto& self=*static_cast<Fixture*>(context);++self.reads;*lease=self.mode==7?0:91;
            out->type=self.mode==2?MMS_VALUE_BOOL:MMS_VALUE_INT;out->value.signedValue=12;
            if (self.mode==3) throw std::runtime_error("read failed after leasing");
            if (self.mode==4) failNextAllocation=true;
            return self.mode==1 || self.mode==6 ? MMS_E_CANCELLED : MMS_OK;
        };
        p.releaseValue=[](void* context,MmsHandle lease) {
            auto& self=*static_cast<Fixture*>(context);++self.releases;CHECK(lease==91);
            if(self.mode==5 || self.mode==6)throw std::runtime_error("release failure");
        };
        MmsHandle h=0;return ownerApi.registerProperty(ownerApi.context,&p,&h)==MMS_OK;
    }
    MmsResult Read(MmsValue& value,MmsHandle& handle) {
        return readerApi.readProperty(readerApi.context,S("example.owner.value"),{},&value,&handle);
    }
};
void Run() {
    for (int mode=0;mode<=7;++mode) {
        Fixture f;CHECK(f.Register());f.mode=mode;
        MmsValue out{};out.type=MMS_VALUE_INT;out.value.signedValue=99;MmsHandle handle=1234;
        const auto result=f.Read(out,handle);
        CHECK(!failNextAllocation);
        const MmsResult expected=mode==0 || mode==7?MMS_OK:mode==1?MMS_E_CANCELLED:mode==4?MMS_E_MEMORY:MMS_E_FAILED;
        CHECK(result==expected && f.reads==1 && f.releases==unsigned(mode==7?0:1));
        CHECK(!f.registry.InCallback());
        if (result==MMS_OK) {
            CHECK(handle && handle!=1234 && out.type==MMS_VALUE_INT && out.value.signedValue==12);
            CHECK(f.registry.HasLeases(f.reader));f.readerApi.releaseValue(f.readerApi.context,handle);
            CHECK(!f.registry.HasLeases(f.reader));
        } else {
            CHECK(handle==1234 && out.type==MMS_VALUE_INT && out.value.signedValue==99);
            CHECK(!f.registry.HasLeases(f.reader));
        }
        CHECK(f.owner.unsafeToUnload==(mode==5 || mode==6));
        CHECK(f.registry.HasLeases(f.owner)==(mode==5 || mode==6));
    }
}
}
int main() {Run();std::printf("property_release_failure_tests: %u checks, %u failures\n",checks,failures);return failures?1:0;}
