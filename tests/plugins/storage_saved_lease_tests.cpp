#include "../../src/plugins/storage_store.hpp"
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>

namespace mikumikustudio::plugins {
struct StorageStoreTestAccess {
    static void Next(StorageStore& s,MmsHandle h) { s.next_=h; }
    static void Total(StorageStore& s,uint64_t bytes) { s.savedLeaseBytes_=bytes; }
    static uint64_t Total(const StorageStore& s) { return s.savedLeaseBytes_; }
};
}
using namespace mikumikustudio::plugins;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (false)
constexpr uint64_t MiB=1024ull*1024;
StoredRecords Records(unsigned char a,unsigned char b) {
    StoredRecords records;
    records["a"]={1,std::vector<unsigned char>(1024,a)};
    records["b"]={2,std::vector<unsigned char>(1024,b)};
    return records;
}
struct Result { MmsBytes bytes{}; uint32_t schema=0; MmsHandle lease=0; };
MmsResult Read(const MmsStorageApiV1& api,Result& r) {return api.readSaved(api.context,&r.bytes,&r.schema,&r.lease);}
void Run() {
    StorageStore store;StorageClient a,b;a.id="a";b.id="b";
    const auto api=store.Api(a),other=store.Api(b);
    auto first=Records(0x31,0x32); store.Saved(first);
    Result x,y,z;
    CHECK(Read(api,x)==MMS_OK && x.lease && x.schema==1 && x.bytes.bytes==1024);
    CHECK(Read(api,y)==MMS_OK && y.lease!=x.lease && y.bytes.data==x.bytes.data);
    CHECK(a.savedLeaseBytes==1024 && StorageStoreTestAccess::Total(store)==1024);
    CHECK(Read(other,z)==MMS_OK && z.bytes.data!=x.bytes.data && b.savedLeaseBytes==1024);
    other.releaseSaved(other.context,x.lease);
    CHECK(store.HasLeases(a) && a.savedLeaseBytes==1024);
    api.releaseSaved(api.context,x.lease);
    CHECK(a.savedLeaseBytes==1024 && static_cast<const unsigned char*>(y.bytes.data)[0]==0x31);
    auto second=Records(0x41,0x42);store.Saved(second);
    Result newer;CHECK(Read(api,newer)==MMS_OK && newer.bytes.data!=y.bytes.data &&
        static_cast<const unsigned char*>(newer.bytes.data)[0]==0x41 && static_cast<const unsigned char*>(y.bytes.data)[0]==0x31);
    CHECK(a.savedLeaseBytes==2048 && StorageStoreTestAccess::Total(store)==3072);
    api.releaseSaved(api.context,y.lease);CHECK(a.savedLeaseBytes==1024);
    // Restore-only preparation/commit must not change published saved data or
    // invalidate the current snapshot, including when there are no providers.
    CHECK(store.PrepareRestore({})==MMS_OK);
    Result whilePrepared;CHECK(Read(api,whilePrepared)==MMS_OK && whilePrepared.bytes.data==newer.bytes.data);
    CHECK(store.Commit());Result after;CHECK(Read(api,after)==MMS_OK && after.bytes.data==newer.bytes.data);
    api.releaseSaved(api.context,whilePrepared.lease);api.releaseSaved(api.context,after.lease);
    auto replacement=Records(0x61,0x62);
    CHECK(store.Prepare(std::move(replacement))==MMS_OK && store.Commit());
    Result reloaded;CHECK(Read(api,reloaded)==MMS_OK && reloaded.bytes.data!=newer.bytes.data &&
        static_cast<const unsigned char*>(reloaded.bytes.data)[0]==0x61 && static_cast<const unsigned char*>(newer.bytes.data)[0]==0x41);
    api.releaseSaved(api.context,reloaded.lease);
    api.releaseSaved(api.context,newer.lease);other.releaseSaved(other.context,z.lease);
    CHECK(a.savedLeaseBytes==0 && b.savedLeaseBytes==0 && StorageStoreTestAccess::Total(store)==0);
    CHECK(store.Retire(a) && store.Retire(b));

    // Exercise byte-limit arithmetic without allocating hundreds of MiB.
    auto third=Records(0x51,0x52);store.Saved(third);
    a.savedLeaseBytes=128*MiB-1023;
    Result unchanged{{reinterpret_cast<const void*>(static_cast<uintptr_t>(7)),9},11,13};
    CHECK(Read(api,unchanged)==MMS_E_MEMORY && unchanged.bytes.data==reinterpret_cast<const void*>(static_cast<uintptr_t>(7)) &&
        unchanged.bytes.bytes==9 && unchanged.schema==11 && unchanged.lease==13);
    a.savedLeaseBytes=0;StorageStoreTestAccess::Total(store,256*MiB-1023);
    CHECK(Read(api,unchanged)==MMS_E_MEMORY && unchanged.lease==13);
    StorageStoreTestAccess::Total(store,0);
    CHECK(Read(api,unchanged)==MMS_OK);
    api.releaseSaved(api.context,unchanged.lease);
    CHECK(StorageStoreTestAccess::Total(store)==0);
    StorageStoreTestAccess::Next(store,(std::numeric_limits<MmsHandle>::max)()-1);
    Result last;CHECK(Read(api,last)==MMS_OK && last.lease==(std::numeric_limits<MmsHandle>::max)()-1);
    Result exhausted{{reinterpret_cast<const void*>(static_cast<uintptr_t>(7)),9},11,13};
    CHECK(Read(api,exhausted)==MMS_E_BUSY && exhausted.lease==13 && exhausted.schema==11 && exhausted.bytes.bytes==9);
    CHECK(Read(api,exhausted)==MMS_E_BUSY && exhausted.lease==13);
    api.releaseSaved(api.context,last.lease);
}

void LifecycleAndThread() {
    StorageStore store;StorageClient client;client.id="a";const auto api=store.Api(client);
    Result missing{{reinterpret_cast<const void*>(static_cast<uintptr_t>(7)),9},11,13};
    CHECK(Read(api,missing)==MMS_E_NOT_FOUND && missing.bytes.data==reinterpret_cast<const void*>(static_cast<uintptr_t>(7)) &&
          missing.bytes.bytes==9 && missing.schema==11 && missing.lease==13);
    auto records=Records(0x31,0x32);store.Saved(records);
    Result held;CHECK(Read(api,held)==MMS_OK);
    MmsResult wrongThread=MMS_OK;Result wrong{{reinterpret_cast<const void*>(static_cast<uintptr_t>(7)),9},11,13};
    std::thread worker([&] { wrongThread=Read(api,wrong);api.releaseSaved(api.context,held.lease); });worker.join();
    CHECK(wrongThread==MMS_E_THREAD && wrong.bytes.data==reinterpret_cast<const void*>(static_cast<uintptr_t>(7)) &&
          wrong.bytes.bytes==9 && wrong.schema==11 && wrong.lease==13);
    CHECK(store.HasLeases(client) && client.savedLeaseBytes==1024 && !store.Retire(client));
    client.stopping=true;
    Result stopped{{reinterpret_cast<const void*>(static_cast<uintptr_t>(7)),9},11,13};
    CHECK(Read(api,stopped)==MMS_E_NOT_READY && stopped.lease==13 && stopped.schema==11 && stopped.bytes.bytes==9);
    CHECK(!store.Retire(client) && static_cast<const unsigned char*>(held.bytes.data)[0]==0x31);
    api.releaseSaved(api.context,held.lease);
    CHECK(client.savedLeaseBytes==0 && !store.HasLeases(client) && store.Retire(client));
}

void RealByteQuota() {
    StorageStore store;StorageClient client;client.id="a";const auto api=store.Api(client);
    auto publish=[&](std::size_t size,unsigned char value) {
        StoredRecords next;next["a"]={1,std::vector<unsigned char>(size,value)};
        store.Saved(next);
    };
    publish(64*MiB,0x11);
    Result first,alias;CHECK(Read(api,first)==MMS_OK && first.bytes.bytes==64*MiB);
    CHECK(Read(api,alias)==MMS_OK && alias.bytes.data==first.bytes.data && client.savedLeaseBytes==64*MiB);
    publish(64*MiB,0x22);
    Result second;CHECK(Read(api,second)==MMS_OK && second.bytes.bytes==64*MiB && client.savedLeaseBytes==128*MiB);
    CHECK(static_cast<const unsigned char*>(first.bytes.data)[0]==0x11 &&
          static_cast<const unsigned char*>(second.bytes.data)[0]==0x22);
    publish(1,0x33);
    Result blocked{{reinterpret_cast<const void*>(static_cast<uintptr_t>(7)),9},11,13};
    CHECK(Read(api,blocked)==MMS_E_MEMORY && blocked.bytes.data==reinterpret_cast<const void*>(static_cast<uintptr_t>(7)) &&
          blocked.bytes.bytes==9 && blocked.schema==11 && blocked.lease==13);
    api.releaseSaved(api.context,first.lease);
    CHECK(client.savedLeaseBytes==128*MiB && Read(api,blocked)==MMS_E_MEMORY);
    api.releaseSaved(api.context,alias.lease);
    CHECK(client.savedLeaseBytes==64*MiB && Read(api,blocked)==MMS_OK &&
          blocked.bytes.bytes==1 && static_cast<const unsigned char*>(blocked.bytes.data)[0]==0x33);
    api.releaseSaved(api.context,blocked.lease);api.releaseSaved(api.context,second.lease);
    CHECK(client.savedLeaseBytes==0 && StorageStoreTestAccess::Total(store)==0);
}
}
int main() {Run();LifecycleAndThread();RealByteQuota();std::printf("storage_saved_lease_tests: %u checks, %u failures\n",checks,failures);return failures?1:0;}
