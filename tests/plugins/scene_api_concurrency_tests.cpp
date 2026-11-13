#include "../../src/plugins/scene_store.hpp"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace mikumikustudio::plugins;
int main() {
    unsigned failures=0;
    auto data=std::make_shared<SceneData>();data->time={0,1};data->supported=MMS_CHANNEL_BONES;
    SceneData::Channels record;record.object.h=Header<MmsObjectV1>();
    record.object.object={{1,2},3};record.object.name=data->Keep("scene");
    MmsBoneV1 bone{};bone.h=Header<MmsBoneV1>();bone.id={4,5};
    record.values[MMS_CHANNEL_BONES]=data->Keep(std::vector<MmsBoneV1>{bone});
    data->objects.push_back(record);
    SceneStore store([data](const MmsSceneQueryV1&,std::shared_ptr<const SceneData>& out){out=data;return MMS_OK;});
    SceneClient client;MmsSceneApiV1 api{};if(store.Api(client,api)!=MMS_OK) return 1;
    MmsSceneQueryV1 query{};query.h=Header<MmsSceneQueryV1>();query.time={0,1};query.channels=MMS_CHANNEL_BONES;
    MmsSnapshotV1 snapshot{};
    if (api.acquire(api.context,&query,&snapshot)!=MMS_OK || !snapshot.handle) return 1;
    std::atomic<bool> go{false},finished{false};std::atomic<unsigned> errors{0};
    std::thread worker([&] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned n=0;n<100000;++n) {
            MmsSpan objects{},bones{};
            if (api.objects(api.context,snapshot.handle,&objects)!=MMS_OK || objects.count!=1 || !objects.data) {++errors;break;}
            const auto* object=static_cast<const MmsObjectV1*>(objects.data);
            if (api.readChannel(api.context,snapshot.handle,object->object,MMS_CHANNEL_BONES,&bones)!=MMS_OK ||
                bones.count!=1 || !bones.data || static_cast<const MmsBoneV1*>(bones.data)->id.lo!=4) {++errors;break;}
        }
        // This is the only reader of this handle, so release cannot invalidate
        // a borrowed span used by another worker.
        api.release(api.context,snapshot.handle);
        finished.store(true,std::memory_order_release);
    });
    go.store(true,std::memory_order_release);
    unsigned queries=0;
    do {
        MmsSceneApiV1 again{};if(store.Api(client,again)!=MMS_OK) ++failures;
        if (again.context!=api.context || again.objects!=api.objects || again.release!=api.release) ++failures;
        ++queries;
    } while (!finished.load(std::memory_order_acquire));
    worker.join();
    if (errors || store.HasLeases(client) || !queries) ++failures;
    MmsSpan untouched{};
    if (api.objects(api.context,snapshot.handle,&untouched)!=MMS_E_STALE) ++failures;
    // Two stores both start handle numbering at one. Reject rebind without
    // altering the caller's output; the original table and lease stay valid.
    SceneStore other([data](const MmsSceneQueryV1&,std::shared_ptr<const SceneData>& out){out=data;return MMS_OK;});
    SceneClient otherClient; MmsSceneApiV1 otherApi{};
    MmsSnapshotV1 oldSnapshot{}, otherSnapshot{}, skipped{};
    if (store.Api(client,api)!=MMS_OK || other.Api(otherClient,otherApi)!=MMS_OK ||
        otherApi.acquire(otherApi.context,&query,&skipped)!=MMS_OK) ++failures;
    otherApi.release(otherApi.context,skipped.handle);
    if (
        api.acquire(api.context,&query,&oldSnapshot)!=MMS_OK ||
        otherApi.acquire(otherApi.context,&query,&otherSnapshot)!=MMS_OK ||
        !oldSnapshot.handle || oldSnapshot.handle!=otherSnapshot.handle) ++failures;
    auto unchanged=api;const auto before=unchanged;
    if (other.Api(client,unchanged)!=MMS_E_CONFLICT || std::memcmp(&unchanged,&before,sizeof(before))!=0) ++failures;
    MmsSpan oldObjects{},otherObjects{};
    if (api.objects(api.context,oldSnapshot.handle,&oldObjects)!=MMS_OK ||
        otherApi.objects(otherApi.context,otherSnapshot.handle,&otherObjects)!=MMS_OK ||
        oldObjects.count!=1 || otherObjects.count!=1) ++failures;
    api.release(api.context,oldSnapshot.handle);
    if (store.HasLeases(client) || !other.HasLeases(otherClient) ||
        otherApi.objects(otherApi.context,otherSnapshot.handle,&otherObjects)!=MMS_OK) ++failures;
    otherApi.release(otherApi.context,otherSnapshot.handle);
    // Competing initial binds share only the atomic client; each thread owns
    // its own result table. The winner must remain usable on the UI thread.
    SceneStore raceA([data](const MmsSceneQueryV1&,std::shared_ptr<const SceneData>& out){out=data;return MMS_OK;});
    SceneStore raceB([data](const MmsSceneQueryV1&,std::shared_ptr<const SceneData>& out){out=data;return MMS_OK;});
    SceneClient raceClient;
    MmsSceneApiV1 resultA{},resultB{};
    std::memset(&resultA,0x5a,sizeof(resultA));std::memset(&resultB,0xa5,sizeof(resultB));
    unsigned char originalA[sizeof(resultA)],originalB[sizeof(resultB)];
    std::memcpy(originalA,&resultA,sizeof(resultA));std::memcpy(originalB,&resultB,sizeof(resultB));
    std::atomic<bool> startRace{false};std::atomic<unsigned> waiting{0};
    MmsResult statusA=MMS_E_FAILED,statusB=MMS_E_FAILED;
    auto bind=[&](SceneStore& target,MmsSceneApiV1& output,MmsResult& status) {
        waiting.fetch_add(1,std::memory_order_release);
        while(!startRace.load(std::memory_order_acquire)) std::this_thread::yield();
        status=target.Api(raceClient,output);
    };
    std::thread first(bind,std::ref(raceA),std::ref(resultA),std::ref(statusA));
    std::thread second(bind,std::ref(raceB),std::ref(resultB),std::ref(statusB));
    while(waiting.load(std::memory_order_acquire)!=2) std::this_thread::yield();
    startRace.store(true,std::memory_order_release);first.join();second.join();
    const bool firstWon=statusA==MMS_OK && statusB==MMS_E_CONFLICT;
    const bool secondWon=statusB==MMS_OK && statusA==MMS_E_CONFLICT;
    if(!firstWon && !secondWon) ++failures;
    if(firstWon || secondWon) {
        const auto& winner=firstWon?resultA:resultB;
        const bool unchangedLoser=firstWon?std::memcmp(&resultB,originalB,sizeof(resultB))==0:
            std::memcmp(&resultA,originalA,sizeof(resultA))==0;
        if(!unchangedLoser) ++failures;
        MmsSnapshotV1 raceSnapshot{};MmsSpan raceObjects{};
        if(!winner.acquire || !winner.objects || !winner.release ||
            winner.acquire(winner.context,&query,&raceSnapshot)!=MMS_OK || !raceSnapshot.handle) ++failures;
        else {
            if(winner.objects(winner.context,raceSnapshot.handle,&raceObjects)!=MMS_OK || raceObjects.count!=1) ++failures;
            winner.release(winner.context,raceSnapshot.handle);
            if((firstWon?raceA:raceB).HasLeases(raceClient)) ++failures;
        }
    }
    std::printf("scene_api_concurrency_tests: %u queries, %u worker errors, %u failures\n",queries,errors.load(),failures);
    return failures ? 1 : 0;
}
