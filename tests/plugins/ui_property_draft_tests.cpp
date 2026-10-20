// Standalone input-draft contract tests. The host connects this pure state
// module to native input/IME messages; no plugin, window or history is used.
#include "../../src/plugins/ui_property_draft.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <initializer_list>
#include <new>
#include <string>
#include <string_view>
#include <vector>

using mikumikustudio::plugins::UiPropertyDraft;
using mikumikustudio::plugins::UiPropertyValue;

namespace {
long long g_failAfter = -1;
unsigned g_checks = 0, g_failures = 0;
void Check(bool accepted, const char* expression, int line) {
    ++g_checks;
    if (!accepted) { ++g_failures; std::fprintf(stderr, "FAIL line %d: %s\n", line, expression); }
}
struct FailAllocation {
    explicit FailAllocation(long long count) { g_failAfter = count; }
    ~FailAllocation() { g_failAfter = -1; }
};
}
void* operator new(std::size_t size) {
    if (g_failAfter == 0) throw std::bad_alloc();
    if (g_failAfter > 0) --g_failAfter;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
#define CHECK(expression) Check((expression), #expression, __LINE__)
MmsValue Integer(int64_t number) { MmsValue value{}; value.type=MMS_VALUE_INT; value.value.signedValue=number; return value; }
MmsValue Number(double number) { MmsValue value{}; value.type=MMS_VALUE_NUMBER; value.value.number=number; return value; }
MmsValue Text(std::string_view text) { MmsValue value{}; value.type=MMS_VALUE_TEXT; value.value.text={text.data(),text.size()}; return value; }
MmsValue Bytes(std::string_view bytes) { MmsValue value{}; value.type=MMS_VALUE_BYTES; value.value.bytes={bytes.data(),bytes.size()}; return value; }
MmsValue Sample(uint32_t type) {
    MmsValue value{}; value.type=type;
    switch(type){
    case MMS_VALUE_BOOL: value.value.unsignedValue=0; break;
    case MMS_VALUE_INT: value.value.signedValue=7; break;
    case MMS_VALUE_UINT: value.value.unsignedValue=7; break;
    case MMS_VALUE_NUMBER: value.value.number=1.0; break;
    case MMS_VALUE_VECTOR: value.components=3;value.value.vector[0]=1;value.value.vector[1]=2;value.value.vector[2]=3;break;
    case MMS_VALUE_TEXT: value.value.text={"baseline",8}; break;
    case MMS_VALUE_ID: value.value.id={2,1}; break;
    case MMS_VALUE_BYTES: value.value.bytes={"\xab\xcd",2}; break;
    }
    return value;
}
bool Same(const UiPropertyValue& value,const MmsValue& expected) {
    UiPropertyValue copy; return UiPropertyValue::Copy(expected,copy)==MMS_OK&&value.Equals(copy);
}
bool OutputIsSentinel(const UiPropertyValue& output) { return Same(output,Integer(-711)); }
void Sentinel(UiPropertyValue& output) { CHECK(UiPropertyValue::Copy(Integer(-711),output)==MMS_OK); }

void EveryTypeChecks() {
    const char* replacements[]={"","1","-9","9","2.5","-12.5","\xe4\xb8\xad\xe6\x96\x87","00000000000000030000000000000004","00FF10"};
    for(uint32_t type=MMS_VALUE_BOOL;type<=MMS_VALUE_BYTES;++type){
        UiPropertyDraft draft;const auto canonical=Sample(type);UiPropertyValue output;Sentinel(output);bool changed=true;
        CHECK(draft.Begin(canonical,17)==MMS_OK);
        CHECK(draft.Active()&&!draft.Composing()&&!draft.Dirty()&&draft.SourceRevision()==17);
        CHECK(draft.Fields().size()==(type==MMS_VALUE_VECTOR?3u:1u));
        CHECK(draft.Prepare(canonical,17,output,changed)==MMS_OK&&!changed&&Same(output,canonical));
        CHECK(draft.SetField(0,replacements[type])==MMS_OK&&draft.Dirty());
        CHECK(draft.Prepare(canonical,17,output,changed)==MMS_OK&&changed&&draft.Active()&&draft.Dirty());
        const auto actual=output.View();CHECK(actual.type==type&&actual.components==canonical.components);
        switch(type){
        case MMS_VALUE_BOOL:CHECK(actual.value.unsignedValue==1);break;
        case MMS_VALUE_INT:CHECK(actual.value.signedValue==-9);break;
        case MMS_VALUE_UINT:CHECK(actual.value.unsignedValue==9);break;
        case MMS_VALUE_NUMBER:CHECK(actual.value.number==2.5);break;
        case MMS_VALUE_VECTOR:CHECK(actual.value.vector[0]==-12.5&&actual.value.vector[1]==2&&actual.value.vector[2]==3);break;
        case MMS_VALUE_TEXT:CHECK(actual.value.text.bytes==6&&std::memcmp(actual.value.text.data,replacements[type],6)==0);break;
        case MMS_VALUE_ID:CHECK(actual.value.id.hi==3&&actual.value.id.lo==4);break;
        case MMS_VALUE_BYTES:CHECK(actual.value.bytes.bytes==3&&std::memcmp(actual.value.bytes.data,"\x00\xff\x10",3)==0);break;
        }
        CHECK(draft.SetField(0,draft.Fields()[0])==MMS_OK); // borrowed source may alias the target field.
    }
    for(uint32_t count=1;count<=4;++count){
        auto value=Sample(MMS_VALUE_VECTOR);value.components=count;for(uint32_t i=0;i<count;++i)value.value.vector[i]=i+1;
        UiPropertyDraft draft;UiPropertyValue output;bool changed=false;
        CHECK(draft.Begin(value,count)==MMS_OK&&draft.Fields().size()==count);
        CHECK(draft.SetField(count-1,"-0.25")==MMS_OK);
        CHECK(draft.Prepare(value,count,output,changed)==MMS_OK&&changed);
        for(uint32_t i=0;i<count;++i)CHECK(output.View().value.vector[i]==(i==count-1?-0.25:double(i+1)));
    }
}

void DirtyAndEquivalentChecks() {
    UiPropertyDraft draft;UiPropertyValue output;bool changed=true;const auto canonical=Number(1.0);
    CHECK(draft.Begin(canonical,2)==MMS_OK&&draft.Fields()[0]=="1"&&!draft.Dirty());
    CHECK(draft.SetField(0,"1.000e0")==MMS_OK&&draft.Dirty());
    CHECK(draft.Prepare(canonical,2,output,changed)==MMS_OK&&!changed&&output.View().value.number==1.0);
    CHECK(draft.Dirty()&&draft.Fields()[0]=="1.000e0");
    CHECK(draft.SetField(0,"1")==MMS_OK&&!draft.Dirty());
    CHECK(draft.SetField(0,"-")==MMS_OK&&draft.Dirty());Sentinel(output);changed=true;
    const auto fields=draft.Fields();
    CHECK(draft.Prepare(canonical,2,output,changed)==MMS_E_ARGUMENT&&OutputIsSentinel(output)&&changed);
    CHECK(draft.Fields()==fields&&draft.Active()&&draft.Dirty());
    CHECK(draft.SetField(0,"2")==MMS_OK);
    CHECK(draft.Prepare(canonical,2,output,changed)==MMS_OK&&changed&&output.View().value.number==2);
    auto vector=Sample(MMS_VALUE_VECTOR);CHECK(draft.Begin(vector,3)==MMS_OK);
    CHECK(draft.SetField(0,"1.0")==MMS_OK&&draft.SetField(1,"2e0")==MMS_OK);
    CHECK(draft.Prepare(vector,3,output,changed)==MMS_OK&&!changed&&draft.Dirty());
    auto bytes=Sample(MMS_VALUE_BYTES);CHECK(draft.Begin(bytes,4)==MMS_OK&&draft.SetField(0,"ABCD")==MMS_OK);
    CHECK(draft.Prepare(bytes,4,output,changed)==MMS_OK&&!changed&&draft.Dirty());
    auto integer=Sample(MMS_VALUE_INT);CHECK(draft.Begin(integer,5)==MMS_OK&&draft.SetField(0,"0007")==MMS_OK);
    CHECK(draft.Prepare(integer,5,output,changed)==MMS_OK&&!changed&&draft.Dirty());
}

void CompositionAndStaleChecks() {
    UiPropertyDraft draft;UiPropertyValue output;Sentinel(output);bool changed=false;
    CHECK(!draft.Active()&&!draft.Composing()&&!draft.Dirty()&&draft.SourceRevision()==0&&draft.Fields().empty());
    draft.SetComposing(true);CHECK(!draft.Composing());
    CHECK(draft.SetField(0,"1")==MMS_E_NOT_READY);
    CHECK(draft.Prepare(Integer(1),1,output,changed)==MMS_E_NOT_READY&&!changed&&OutputIsSentinel(output));
    const auto canonical=Integer(7);CHECK(draft.Begin(canonical,42)==MMS_OK&&draft.SetField(0,"-")==MMS_OK);
    draft.SetComposing(true);const auto fields=draft.Fields();
    CHECK(draft.Composing()&&draft.Prepare(canonical,42,output,changed)==MMS_E_BUSY&&!changed&&OutputIsSentinel(output));
    CHECK(draft.Prepare(Integer(9),43,output,changed)==MMS_E_BUSY&&draft.Fields()==fields);
    CHECK(draft.SetField(0,"12")==MMS_OK&&draft.Composing());draft.SetComposing(false);
    CHECK(draft.Prepare(canonical,41,output,changed)==MMS_E_STALE&&!changed&&OutputIsSentinel(output));
    CHECK(draft.Prepare(canonical,0,output,changed)==MMS_E_STALE);
    CHECK(draft.Prepare(Integer(8),42,output,changed)==MMS_E_STALE);
    CHECK(draft.Prepare(Number(7),42,output,changed)==MMS_E_STALE);
    auto invalid=canonical;invalid.components=1;CHECK(draft.Prepare(invalid,42,output,changed)==MMS_E_STALE);
    CHECK(draft.Prepare(Number(std::numeric_limits<double>::quiet_NaN()),42,output,changed)==MMS_E_STALE);
    CHECK(draft.Fields()[0]=="12"&&draft.SourceRevision()==42&&draft.Active()&&draft.Dirty());
    CHECK(draft.Prepare(canonical,42,output,changed)==MMS_OK&&changed&&output.View().value.signedValue==12);
    CHECK(draft.Active()&&draft.Dirty());draft.SetComposing(true);draft.Cancel();
    CHECK(!draft.Active()&&!draft.Composing()&&!draft.Dirty()&&draft.Fields().empty()&&draft.SourceRevision()==0);
    draft.Cancel();CHECK(draft.Begin(Text("reopened"),51)==MMS_OK&&draft.Fields()[0]=="reopened"&&!draft.Dirty());
    CHECK(draft.Prepare(Text("reopened"),51,output,changed)==MMS_OK&&!changed);
}

void RejectedInputChecks() {
    UiPropertyDraft draft;UiPropertyValue output;Sentinel(output);bool changed=true;
    CHECK(draft.Begin(Text("kept"),8)==MMS_OK&&draft.SetField(0,"unfinished")==MMS_OK);draft.SetComposing(true);
    const auto original=draft.Fields();
    CHECK(draft.Begin(Text("new"),0)==MMS_E_ARGUMENT&&draft.Fields()==original&&draft.SourceRevision()==8&&draft.Composing());
    CHECK(draft.Begin(Text(std::string_view("\xc0\xaf",2)),9)==MMS_E_ARGUMENT&&draft.Fields()==original&&draft.Composing());
    for(uint32_t type=MMS_VALUE_BOOL;type<=MMS_VALUE_BYTES;++type){
        if(type==MMS_VALUE_VECTOR)continue;
        for(uint32_t count:{1u,4u,5u}){auto malformed=Sample(type);malformed.components=count;
            CHECK(draft.Begin(malformed,9)==MMS_E_ARGUMENT&&draft.Fields()==original&&draft.Composing());}
    }
    for(uint32_t count:{0u,5u}){auto malformed=Sample(MMS_VALUE_VECTOR);malformed.components=count;
        CHECK(draft.Begin(malformed,9)==MMS_E_ARGUMENT&&draft.Fields()==original);}
    for(double invalid:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        CHECK(draft.Begin(Number(invalid),9)==MMS_E_ARGUMENT&&draft.Fields()==original);
    CHECK(draft.Begin(MmsValue{},9)==MMS_E_ARGUMENT&&draft.Fields()==original);
    CHECK(draft.Begin(Text("kept"),8)==MMS_OK);
    const std::string invalidText[]={std::string("\xc0\xaf",2),std::string("a\0b",3),std::string("\xed\xa0\x80",3)};
    for(const auto& text:invalidText){
        CHECK(draft.SetField(0,text)==MMS_OK);const auto pending=draft.Fields();
        CHECK(draft.Prepare(Text("kept"),8,output,changed)==MMS_E_ARGUMENT&&OutputIsSentinel(output)&&changed&&draft.Fields()==pending);
    }
    const auto bytes=Sample(MMS_VALUE_BYTES);CHECK(draft.Begin(bytes,9)==MMS_OK);
    for(const char* raw:{"a","gg","00 11","0x11","\xff"}){
        CHECK(draft.SetField(0,raw)==MMS_OK);
        CHECK(draft.Prepare(bytes,9,output,changed)==MMS_E_ARGUMENT&&OutputIsSentinel(output)&&changed);
    }
    const auto value=Number(1);CHECK(draft.Begin(value,10)==MMS_OK);
    for(const char* raw:{"","-",".","1e","1e-","+1","nan","inf"," 1","1 ","1e9999"}){
        CHECK(draft.SetField(0,raw)==MMS_OK);
        CHECK(draft.Prepare(value,10,output,changed)==MMS_E_ARGUMENT&&OutputIsSentinel(output)&&changed);
    }
    const auto before=draft.Fields();CHECK(draft.SetField(1,"7")==MMS_E_ARGUMENT&&draft.Fields()==before);
    CHECK(draft.SetField(std::numeric_limits<size_t>::max(),"7")==MMS_E_ARGUMENT&&draft.Fields()==before);
}

void FieldLimitChecks() {
    UiPropertyDraft draft;UiPropertyValue output;bool changed=false;const auto canonical=Text("a");
    CHECK(draft.Begin(canonical,3)==MMS_OK);const std::string maximum(UiPropertyDraft::kMaxFieldBytes,'b'),tooLong(maximum.size()+1,'c');
    CHECK(draft.SetField(0,maximum)==MMS_OK&&draft.Fields()[0]==maximum);
    CHECK(draft.SetField(0,tooLong)==MMS_E_ARGUMENT&&draft.Fields()[0]==maximum);
    CHECK(draft.Prepare(canonical,3,output,changed)==MMS_OK&&changed&&output.View().value.text.bytes==maximum.size());
    CHECK(draft.SetField(0,std::string_view{})==MMS_OK&&draft.Fields()[0].empty());
    CHECK(draft.Prepare(canonical,3,output,changed)==MMS_OK&&changed&&output.View().value.text.bytes==0);
}

void BeginFieldLimitChecks() {
    constexpr auto limit=UiPropertyDraft::kMaxFieldBytes;
    CHECK(limit==1024u*1024u);
    const std::string exactText(limit,'t'),tooMuchText(limit+1,'t');
    const std::string exactBytes(limit/2,char(0xab)),tooManyBytes(limit/2+1,char(0xab));
    UiPropertyDraft draft;UiPropertyValue output;bool changed=true;
    CHECK(draft.Begin(Text(exactText),4)==MMS_OK&&draft.Fields()[0].size()==limit);
    CHECK(draft.Prepare(Text(exactText),4,output,changed)==MMS_OK&&!changed&&Same(output,Text(exactText)));
    CHECK(draft.Begin(Bytes(exactBytes),5)==MMS_OK&&draft.Fields()[0].size()==limit);
    CHECK(draft.Fields()[0].substr(0,4)=="abab"&&draft.Fields()[0].substr(limit-4)=="abab");
    CHECK(draft.SetField(0,draft.Fields()[0])==MMS_OK&&!draft.Dirty());
    CHECK(draft.Prepare(Bytes(exactBytes),5,output,changed)==MMS_OK&&!changed&&Same(output,Bytes(exactBytes)));
    // The binary codec remains able to own data larger than the default hex
    // field. Only the default input draft rejects this representation.
    CHECK(UiPropertyValue::Copy(Bytes(tooManyBytes),output)==MMS_OK&&output.View().value.bytes.bytes==tooManyBytes.size());
    CHECK(draft.Begin(Integer(7),8)==MMS_OK&&draft.SetField(0,"-")==MMS_OK);draft.SetComposing(true);
    const auto fields=draft.Fields();
    for(const auto value:{Text(tooMuchText),Bytes(tooManyBytes)}){
        CHECK(draft.Begin(value,9)==MMS_E_ARGUMENT);
        CHECK(draft.Active()&&draft.Dirty()&&draft.Composing()&&draft.SourceRevision()==8&&draft.Fields()==fields);
        MmsResult result;
        {FailAllocation failure(0);result=draft.Begin(value,9);}
        CHECK(result==MMS_E_ARGUMENT&&draft.Composing()&&draft.Fields()==fields&&draft.SourceRevision()==8);
    }
    draft.SetComposing(false);CHECK(draft.SetField(0,"9")==MMS_OK);
    CHECK(draft.Prepare(Integer(7),8,output,changed)==MMS_OK&&changed&&output.View().value.signedValue==9);
    std::string utf8;utf8.reserve(limit);
    for(size_t n=0;n<limit/2;++n)utf8.append("\xc2\xa2",2);
    CHECK(draft.Begin(Text(utf8),10)==MMS_OK&&draft.Fields()[0].size()==limit);
    utf8.append("\xc2\xa2",2);
    CHECK(draft.Begin(Text(utf8),11)==MMS_E_ARGUMENT&&draft.SourceRevision()==10&&draft.Fields()[0].size()==limit&&!draft.Dirty());
}

void OwnedSourceAndAliasChecks() {
    char text[]="abc";UiPropertyDraft draft;UiPropertyValue output;bool changed=true;
    CHECK(draft.Begin(Text(std::string_view(text,3)),6)==MMS_OK);text[0]='z';
    CHECK(draft.Fields()[0]=="abc"&&!draft.Dirty());
    CHECK(draft.Prepare(Text("abc"),6,output,changed)==MMS_OK&&!changed&&Same(output,Text("abc")));
    CHECK(draft.Prepare(Text(std::string_view(text,3)),6,output,changed)==MMS_E_STALE);
    CHECK(UiPropertyValue::Copy(Text("source"),output)==MMS_OK&&draft.Begin(output.View(),7)==MMS_OK);
    CHECK(draft.SetField(0,"replacement")==MMS_OK);
    CHECK(draft.Prepare(output.View(),7,output,changed)==MMS_OK&&changed&&Same(output,Text("replacement")));
    char bytes[]={char(0xab),char(0xcd)};CHECK(draft.Begin(Bytes(std::string_view(bytes,2)),8)==MMS_OK);bytes[0]=0;
    CHECK(draft.Fields()[0]=="abcd");
    CHECK(draft.Prepare(Bytes(std::string_view("\xab\xcd",2)),8,output,changed)==MMS_OK&&!changed);
    CHECK(draft.SetField(0,"-")==MMS_OK); // invalid raw hex stays private, even when changed initially false.
    Sentinel(output);changed=false;
    CHECK(draft.Prepare(Bytes(std::string_view("\xab\xcd",2)),8,output,changed)==MMS_E_ARGUMENT&&!changed&&OutputIsSentinel(output));
}

void AllocationFailureChecks() {
    const std::string longText(4096,'n');bool succeeded=false;unsigned failedBegins=0;
    for(long long at=0;at<24;++at){
        UiPropertyDraft draft;CHECK(draft.Begin(Integer(7),8)==MMS_OK&&draft.SetField(0,"-")==MMS_OK);draft.SetComposing(true);
        const auto original=draft.Fields();MmsResult result;
        {FailAllocation fail(at);result=draft.Begin(Text(longText),9);}
        if(result==MMS_E_MEMORY){++failedBegins;CHECK(draft.Fields()==original&&draft.SourceRevision()==8&&draft.Composing()&&draft.Dirty());}
        else{CHECK(result==MMS_OK&&draft.Fields()[0]==longText&&draft.SourceRevision()==9&&!draft.Composing()&&!draft.Dirty());succeeded=true;break;}
    }
    CHECK(succeeded&&failedBegins>=3);
    {
        UiPropertyDraft draft;CHECK(draft.Begin(Integer(7),8)==MMS_OK);const auto fields=draft.Fields();MmsResult result;
        {FailAllocation fail(0);result=draft.SetField(0,longText);}
        CHECK(result==MMS_E_MEMORY&&draft.Fields()==fields&&!draft.Dirty()&&draft.SourceRevision()==8);
    }
    succeeded=false;unsigned failedPrepares=0;
    for(long long at=0;at<24;++at){
        UiPropertyDraft draft;CHECK(draft.Begin(Text(longText),8)==MMS_OK&&draft.SetField(0,"different")==MMS_OK);
        UiPropertyValue output;Sentinel(output);const auto fields=draft.Fields();bool changed=at%2==0;const bool prior=changed;MmsResult result;
        {FailAllocation fail(at);result=draft.Prepare(Text(longText),8,output,changed);}
        if(result==MMS_E_MEMORY){++failedPrepares;CHECK(OutputIsSentinel(output)&&changed==prior&&draft.Fields()==fields&&draft.Active()&&draft.Dirty());}
        else{CHECK(result==MMS_OK&&changed&&Same(output,Text("different"))&&draft.Fields()==fields);succeeded=true;break;}
    }
    CHECK(succeeded&&failedPrepares>=2);
}
}

int main() {
    EveryTypeChecks();DirtyAndEquivalentChecks();CompositionAndStaleChecks();RejectedInputChecks();FieldLimitChecks();BeginFieldLimitChecks();OwnedSourceAndAliasChecks();AllocationFailureChecks();
    std::printf("ui property draft: %u checks, %u failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
