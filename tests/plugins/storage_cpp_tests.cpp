#include <mms/cpp/storage.hpp>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <type_traits>
#include <vector>

using namespace mms::sdk;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (false)
struct Fake {
    struct Record { std::vector<unsigned char> bytes; uint32_t schema; };
    std::map<MmsHandle,Record> held;
    Record current{{1,2,3},7};
    MmsResult queryResult = MMS_OK, readResult = MMS_OK;
    unsigned queries = 0, reads = 0, releases = 0;
    unsigned shortHeader = 0, missing = 0, queryThrow = 0, readThrow = 0, releaseThrow = 0;
    bool wrongMajor = false, zeroHandle = false;
    MmsHandle next = 41, lastRelease = 0;
    SavedBytesLease* reenterLease = nullptr;
    StorageServices* reenterServices = nullptr;
    Fake* replacement = nullptr;
    MmsResult nestedRead = MMS_OK, nestedRelease = MMS_OK, nestedTransfer = MMS_OK;
    MmsHandle nestedDetach = 99;
    bool movedDuringCall = true;
    static void Throw(unsigned kind) {
        if (kind == 1) throw std::bad_alloc{};
        if (kind == 2) throw std::length_error("fixture");
        if (kind == 3) throw std::runtime_error("fixture");
    }
    static MmsResult MMS_CALL Read(void* context, MmsBytes* bytes, uint32_t* schema, MmsHandle* lease) {
        auto& f = *static_cast<Fake*>(context); ++f.reads;
        Throw(f.readThrow);
        if (f.reenterLease) {
            f.nestedRead = f.reenterServices->ReadSaved(*f.reenterLease);
            f.nestedRelease = f.reenterLease->Release();
            f.nestedDetach = f.reenterLease->Detach();
            SavedBytesLease other;
            f.nestedTransfer = other.TransferFrom(*f.reenterLease);
            auto moved = std::move(*f.reenterLease); f.movedDuringCall = moved.valid();
        }
        if (f.replacement) CHECK(StorageServices::Query(f.replacement->Host(),*f.reenterServices) == MMS_OK);
        if (f.readResult != MMS_OK) {
            // Failure scribbles deliberately: wrapper candidates must isolate it.
            *bytes = {"bad",3}; *schema = 999; *lease = 999;
            return f.readResult;
        }
        if (f.zeroHandle) { *bytes = {}; *schema = 0; *lease = 0; return MMS_OK; }
        const auto handle = f.next++;
        auto& record = f.held.emplace(handle,f.current).first->second;
        *bytes = {record.bytes.data(),record.bytes.size()}; *schema = record.schema; *lease = handle;
        return MMS_OK;
    }
    static void MMS_CALL Release(void* context, MmsHandle handle) {
        auto& f = *static_cast<Fake*>(context); ++f.releases; f.lastRelease = handle;
        Throw(f.releaseThrow); // Failure is before consumption, so retry is testable.
        if (f.reenterLease) {
            f.nestedRelease = f.reenterLease->Release();
            f.nestedDetach = f.reenterLease->Detach();
            SavedBytesLease other;
            f.nestedTransfer = other.TransferFrom(*f.reenterLease);
            auto moved = std::move(*f.reenterLease); f.movedDuringCall = moved.valid();
        }
        CHECK(f.held.erase(handle) == 1);
    }
    static MmsResult MMS_CALL Query(void* context, MmsString name, uint32_t major, uint32_t minor, uint32_t size, void* out) {
        auto& f = *static_cast<Fake*>(context); ++f.queries;
        CHECK(name.data && name.bytes == 11 && std::memcmp(name.data,"mms.storage",11) == 0);
        CHECK(major == 1 && minor == 0 && size == sizeof(MmsStorageApiV1));
        Throw(f.queryThrow);
        MmsStorageApiV1 api{{f.shortHeader ? 1u : uint32_t(sizeof(MmsStorageApiV1)),uint16_t(f.wrongMajor ? 2 : 1),0},context,Read,Release};
        if (f.missing == 1) api.readSaved = nullptr;
        if (f.missing == 2) api.releaseSaved = nullptr;
        *static_cast<MmsStorageApiV1*>(out) = api;
        return f.queryResult;
    }
    MmsHostV1 Host() { MmsHostV1 host{}; host.h = {sizeof(host),1,0}; host.context = this; host.queryService = Query; return host; }
};
static_assert(!std::is_copy_constructible_v<SavedBytesLease>);
static_assert(!std::is_copy_assignable_v<SavedBytesLease>);
static_assert(!std::is_move_assignable_v<SavedBytesLease>);
static_assert(std::is_nothrow_move_constructible_v<SavedBytesLease>);

void QueryAndFailure() {
    Fake f; StorageServices service; SavedBytesLease lease;
    CHECK(service.ReadSaved(lease) == MMS_E_NOT_READY && !lease);
    MmsHostV1 bad{};
    CHECK(StorageServices::Query(bad,service) == MMS_E_VERSION && !service.ready());
    bad.h = {sizeof(bad),1,0};
    CHECK(StorageServices::Query(bad,service) == MMS_E_ARGUMENT && !service.ready());
    CHECK(StorageServices::Query(f.Host(),service) == MMS_OK && service.ready());
    f.queryResult = MMS_E_NOT_FOUND;
    CHECK(StorageServices::Query(f.Host(),service) == MMS_E_NOT_FOUND && service.api().context == &f);
    f.queryResult = MMS_OK; f.shortHeader = 1;
    CHECK(StorageServices::Query(f.Host(),service) == MMS_E_VERSION && service.ready());
    f.shortHeader = 0; f.wrongMajor = true;
    CHECK(StorageServices::Query(f.Host(),service) == MMS_E_VERSION && service.ready());
    f.wrongMajor = false;
    for (unsigned missing : {1u,2u}) {
        f.missing = missing;
        CHECK(StorageServices::Query(f.Host(),service) == MMS_E_UNSUPPORTED && service.ready());
    }
    f.missing = 0;
    for (unsigned fault : {1u,2u,3u}) {
        f.queryThrow = fault;
        CHECK(StorageServices::Query(f.Host(),service) == (fault == 3 ? MMS_E_FAILED : MMS_E_MEMORY));
    }
    f.queryThrow = 0;
    for (MmsResult failure : {MMS_E_NOT_FOUND,MMS_E_BUSY,MMS_E_NOT_READY,MMS_E_MEMORY}) {
        f.readResult = failure;
        CHECK(service.ReadSaved(lease) == failure && !lease && lease.schema() == 0 && lease.bytes().bytes == 0);
    }
    f.readResult = MMS_OK;
    for (unsigned fault : {1u,2u,3u}) {
        f.readThrow = fault;
        CHECK(service.ReadSaved(lease) == (fault == 3 ? MMS_E_FAILED : MMS_E_MEMORY) && !lease);
    }
    f.readThrow = 0; f.zeroHandle = true;
    CHECK(service.ReadSaved(lease) == MMS_E_FAILED && !lease);
    f.zeroHandle = false;
    CHECK(service.ReadSaved(lease) == MMS_OK && lease.schema() == 7 && lease.bytes().bytes == 3);
    const auto reads = f.reads; const auto bytes = lease.bytes(); const auto handle = lease.get();
    CHECK(service.ReadSaved(lease) == MMS_E_BUSY && f.reads == reads && lease.get() == handle && lease.bytes().data == bytes.data);
    CHECK(lease.Release() == MMS_OK && !lease && f.held.empty());
}

void OwnershipAndThreads() {
    Fake first, second; SavedBytesLease old;
    {
        StorageServices service;
        CHECK(StorageServices::Query(first.Host(),service) == MMS_OK);
        CHECK(service.ReadSaved(old) == MMS_OK);
        CHECK(StorageServices::Query(second.Host(),service) == MMS_OK);
        SavedBytesLease other;
        CHECK(service.ReadSaved(other) == MMS_OK && old.get() == other.get());
        CHECK(other.Release() == MMS_OK && second.releases == 1 && first.releases == 0);
    }
    CHECK(old.api().context == &first && old.schema() == 7 && old.bytes().bytes == 3);
    first.current = {{9,8},12}; // Existing lease remains a prior immutable version.
    CHECK(static_cast<const unsigned char*>(old.bytes().data)[0] == 1);
    auto moved = std::move(old);
    CHECK(!old && old.schema() == 0 && old.bytes().data == nullptr && moved.valid());
    CHECK(old.Release() == MMS_OK && first.releases == 0);
    StorageServices service;
    CHECK(StorageServices::Query(first.Host(),service) == MMS_OK);
    SavedBytesLease current;
    CHECK(service.ReadSaved(current) == MMS_OK && current.schema() == 12 && current.bytes().bytes == 2);
    const auto movedHandle = moved.get(), currentHandle = current.get();
    CHECK(current.TransferFrom(moved) == MMS_E_BUSY && current.get() == currentHandle && moved.get() == movedHandle);
    CHECK(moved.TransferFrom(moved) == MMS_OK && moved.get() == movedHandle);
    MmsResult read = MMS_OK, release = MMS_OK;
    const auto reads = first.reads;
    std::thread worker([&] { SavedBytesLease unused; read = service.ReadSaved(unused); release = moved.Release(); });
    worker.join();
    CHECK(read == MMS_E_THREAD && release == MMS_E_THREAD && first.reads == reads && first.releases == 0 && moved.get() == movedHandle);
    first.releaseThrow = 3;
    CHECK(moved.Release() == MMS_E_FAILED && moved.get() == movedHandle && moved.bytes().bytes == 3);
    first.releaseThrow = 0;
    CHECK(moved.Release() == MMS_OK && !moved && first.releases == 2);
    CHECK(moved.Release() == MMS_OK && first.releases == 2);
    CHECK(moved.TransferFrom(current) == MMS_OK && !current && moved.get() == currentHandle);
    CHECK(moved.Release() == MMS_OK && first.held.empty());
    first.current = {{},23};
    CHECK(service.ReadSaved(current) == MMS_OK && current.valid() && current.bytes().bytes == 0 && current.schema() == 23);
    const auto api = current.api(); const auto detached = current.Detach();
    CHECK(detached != 0 && !current && current.bytes().bytes == 0 && current.schema() == 0);
    api.releaseSaved(api.context,detached);
    MmsHandle abandoned = 0;
    const auto releases = first.releases;
    { SavedBytesLease explicitCleanup; CHECK(service.ReadSaved(explicitCleanup) == MMS_OK); abandoned = explicitCleanup.get(); }
    CHECK(first.releases == releases && first.held.count(abandoned) == 1);
    service.api().releaseSaved(service.api().context,abandoned);
    CHECK(first.held.empty());
}

void ReentrantCalls() {
    Fake first, second; StorageServices service; SavedBytesLease lease;
    CHECK(StorageServices::Query(first.Host(),service) == MMS_OK);
    first.reenterLease = &lease; first.reenterServices = &service; first.replacement = &second;
    CHECK(service.ReadSaved(lease) == MMS_OK && lease.api().context == &first && service.api().context == &second);
    CHECK(first.nestedRead == MMS_E_BUSY && first.nestedRelease == MMS_E_BUSY && first.nestedTransfer == MMS_E_BUSY);
    CHECK(first.nestedDetach == 0 && !first.movedDuringCall && first.reads == 1);
    CHECK(lease.Release() == MMS_OK && !lease && first.releases == 1 && second.releases == 0);
    CHECK(first.nestedRelease == MMS_E_BUSY && first.nestedTransfer == MMS_E_BUSY && first.nestedDetach == 0 && !first.movedDuringCall);
    CHECK(first.held.empty());
}
} // namespace
int main() {
    QueryAndFailure(); OwnershipAndThreads(); ReentrantCalls();
    std::printf("storage_cpp_tests: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
