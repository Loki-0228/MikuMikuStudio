#include <mms/storage.h>
#include <cstring>

namespace {
template<class T> MmsHeader Header() { return {sizeof(T), 1, 0}; }
MmsString Text(const char* s) { return {s, std::strlen(s)}; }
struct Slot { MmsBytes bytes{}; uint32_t schema=0; MmsHandle handle=0; };
struct State { MmsStorageApiV1 storage{}; Slot slots[3]{}; unsigned stops=0; } state;
MmsResult MMS_CALL Create(const MmsHostV1* host,const MmsCreateInfoV1* info,void** out) {
    if(!host || !info || !out) return MMS_E_ARGUMENT;
    state={}; *out=nullptr;
    auto r=host->queryService(host->context,Text("mms.storage"),1,0,sizeof(state.storage),&state.storage);
    if(r) return r;
    *out=&state; return MMS_OK;
}
MmsResult MMS_CALL Start(void*) {return MMS_OK;}
void MMS_CALL Event(void*,const MmsEventV1*) {}
MmsResult MMS_CALL Stop(void*) {++state.stops;return MMS_OK;}
void MMS_CALL Destroy(void*) {}
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL MmsStorageFixtureRead(unsigned index) {
    if(index>=3 || state.slots[index].handle) return MMS_E_ARGUMENT;
    auto& s=state.slots[index]; return state.storage.readSaved(state.storage.context,&s.bytes,&s.schema,&s.handle);
}
extern "C" __declspec(dllexport) bool MMS_CALL MmsStorageFixtureCheck(unsigned index,uint32_t schema,unsigned char value) {
    if(index>=3) return false; const auto& s=state.slots[index];
    return s.handle && s.schema==schema && s.bytes.bytes==1024 && s.bytes.data &&
        static_cast<const unsigned char*>(s.bytes.data)[0]==value &&
        static_cast<const unsigned char*>(s.bytes.data)[1023]==value;
}
extern "C" __declspec(dllexport) bool MMS_CALL MmsStorageFixtureSame(unsigned a,unsigned b) {
    return a<3 && b<3 && state.slots[a].handle && state.slots[b].handle &&
        state.slots[a].bytes.data==state.slots[b].bytes.data && state.slots[a].handle!=state.slots[b].handle;
}
extern "C" __declspec(dllexport) void MMS_CALL MmsStorageFixtureRelease(unsigned index) {
    if(index>=3) return; auto& s=state.slots[index];
    if(s.handle) state.storage.releaseSaved(state.storage.context,s.handle); s={};
}
extern "C" __declspec(dllexport) unsigned MMS_CALL MmsStorageFixtureStops() {return state.stops;}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major,uint32_t minor,uint32_t bytes,MmsPluginApiV1* out) {
    if(!out || bytes<sizeof(*out)) return MMS_E_ARGUMENT;
    if(major!=1 || minor) return MMS_E_VERSION;
    *out={Header<MmsPluginApiV1>(),Create,Start,Event,Stop,Destroy};return MMS_OK;
}
