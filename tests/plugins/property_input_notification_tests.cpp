#include "../../src/plugins/ui_registry.hpp"
#include <cstdio>
#include <stdexcept>

using namespace mikumikustudio::plugins;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (false)
MmsString S(const char* s) { return {s, std::strlen(s)}; }
MmsValue Integer(int64_t n) { MmsValue v{}; v.type=MMS_VALUE_INT;v.value.signedValue=n;return v; }
struct Fixture {
    UiRegistry registry;
    UiClient owner{&registry,"example.input"};
    MmsUiApiV1 ui=registry.Api(owner);
    std::string propertyId="example.input.property",elementId="example.input.field";
    MmsValue value=Integer(2); uint64_t revision=19; unsigned changes=0,edits=0;
    bool fail=false,throwRevision=false;
    uint64_t notifiedRevision=0;std::string notifiedId;
    bool Setup() {
        MmsPropertyDescV1 d{};d.info.h=Header<MmsPropertyDescV1>();d.info.id=Text(propertyId);d.info.ownerPlugin=Text(owner.id);
        d.info.valueType=MMS_VALUE_INT;d.info.label=S("Input");d.info.flags=MMS_PROPERTY_UNDOABLE;
        d.info.minimum=Integer(0);d.info.maximum=Integer(100);d.context=this;
        d.read=[](void* p,MmsObjectRef,MmsValue* out,MmsHandle* lease)->MmsResult {auto& f=*static_cast<Fixture*>(p);*out=f.value;*lease=55;return MMS_OK;};
        d.releaseValue=[](void*,MmsHandle){};
        d.stage=[](void*,MmsHandle,MmsObjectRef,const MmsValue*)->MmsResult{return MMS_E_NOT_READY;};
        MmsHandle property=0,element=0;
        if (ui.registerProperty(ui.context,&d,&property)!=MMS_OK) return false;
        MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();e.id=Text(elementId);e.ownerPlugin=Text(owner.id);
        e.parentId=S("mms.toolbar.main.extensions");e.label=S("Input");e.propertyId=Text(propertyId);
        e.kind=MMS_UI_INPUT;e.capabilities=MMS_UI_MOVE|MMS_UI_HIDE|MMS_UI_RESIZE|MMS_UI_LAYOUT;
        if (ui.registerElement(ui.context,&e,&element)!=MMS_OK) return false;
        registry.SetPropertyInputBridges([this](UiClient&,uint64_t& out)->MmsResult {
            if(throwRevision) throw std::runtime_error("revision");out=revision;return MMS_OK;
        },[this](UiClient&,const PropertyInputSnapshot&,const MmsValue& v)->MmsResult {
            ++edits;if(fail)return MMS_E_FAILED;value=v;++revision;return MMS_OK;
        });
        registry.SetPropertyChanged([this](const std::string& id,uint64_t r){++changes;notifiedId=id;notifiedRevision=r;});
        return true;
    }
};
void Run() {
    Fixture f;CHECK(f.Setup());PropertyInputSnapshot snapshot;
    CHECK(f.registry.CapturePropertyInput(f.elementId,snapshot)==MMS_OK);
    CHECK(f.registry.CommitPropertyInput(snapshot,Integer(2))==MMS_OK && f.changes==0 && f.edits==0);
    f.fail=true;
    CHECK(f.registry.CommitPropertyInput(snapshot,Integer(3))==MMS_E_FAILED && f.changes==0 && f.edits==1);
    f.fail=false;
    CHECK(f.registry.CommitPropertyInput(snapshot,Integer(3))==MMS_OK && f.changes==1 && f.edits==2 &&
          f.notifiedId==f.propertyId && f.notifiedRevision==20);
    CHECK(f.registry.CommitPropertyInput(snapshot,Integer(4))==MMS_E_STALE && f.changes==1);
    CHECK(f.registry.CapturePropertyInput(f.elementId,snapshot)==MMS_OK);
    CHECK(f.registry.CommitPropertyInput(snapshot,Integer(3))==MMS_OK && f.changes==1 && f.edits==2);
}
}
int main() {Run();std::printf("property_input_notification_tests: %u checks, %u failures\n",checks,failures);return failures?1:0;}
