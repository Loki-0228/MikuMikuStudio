#include <mms/cpp/layout.hpp>
#include <mms/cpp/scene.hpp>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>

static_assert(!std::is_move_assignable_v<mms::sdk::SceneSnapshot>, "transfer must report errors");
static_assert(!std::is_move_assignable_v<mms::sdk::LayoutSnapshotLease>, "transfer must report errors");

namespace {
int checks;
void Check(bool ok) { if (!ok) throw ++checks; ++checks; }
struct Host {
    MmsHostV1 host{};
    MmsSceneApiV1 scene{};
    MmsUiLayoutApiV1 layout{};
    unsigned sceneAcquires=0, sceneReleases=0, layoutAcquires=0, layoutReleases=0;
    unsigned sceneObjectReads=0, sceneChannelReads=0;
    MmsHandle lastSceneRelease=0;
    MmsObjectV1 sceneObject{};
    MmsBoneV1 sceneBone{};
    Host() {
        host.h = {sizeof(host),1,0}; host.context=this;
        host.queryService=[](void* p,MmsString name,uint32_t major,uint32_t minor,uint32_t size,void* out)->MmsResult {
            auto& h=*static_cast<Host*>(p);
            const auto n=std::string_view(name.data,static_cast<size_t>(name.bytes));
            if (major!=1 || minor!=0 || !out) return MMS_E_ARGUMENT;
            if(n=="mms.scene" && size>=sizeof(h.scene)) {std::memcpy(out,&h.scene,sizeof(h.scene));return MMS_OK;}
            if(n=="mms.ui.layout" && size>=sizeof(h.layout)) {std::memcpy(out,&h.layout,sizeof(h.layout));return MMS_OK;}
            return MMS_E_NOT_FOUND;
        };
        scene.h={sizeof(scene),1,0}; scene.context=this;
        scene.acquire=[](void* p,const MmsSceneQueryV1*,MmsSnapshotV1* out)->MmsResult {
            auto& h=*static_cast<Host*>(p); out->handle=100+(++h.sceneAcquires); return MMS_OK;
        };
        scene.release=[](void* p,MmsHandle handle) {
            auto& h=*static_cast<Host*>(p);
            ++h.sceneReleases;
            h.lastSceneRelease=handle;
        };
        scene.objects=[](void* p,MmsHandle handle,MmsSpan* out)->MmsResult {
            auto& h=*static_cast<Host*>(p);
            ++h.sceneObjectReads;
            if (!out || handle!=100+h.sceneAcquires) return MMS_E_ARGUMENT;
            *out={&h.sceneObject,1,sizeof(h.sceneObject),1};
            return MMS_OK;
        };
        scene.readChannel=[](void* p,MmsHandle handle,MmsObjectRef,uint32_t channel,MmsSpan* out)->MmsResult {
            auto& h=*static_cast<Host*>(p);
            ++h.sceneChannelReads;
            if (!out || handle!=100+h.sceneAcquires || channel!=MMS_CHANNEL_BONES) return MMS_E_ARGUMENT;
            *out={&h.sceneBone,1,sizeof(h.sceneBone),1};
            return MMS_OK;
        };
        layout.h={sizeof(layout),1,0};layout.context=this;
        layout.setDefaultHints=[](void*,MmsString,const MmsUiLayoutHintsV1*)->MmsResult {return MMS_OK;};
        layout.stageHints=[](void*,MmsHandle,MmsString,uint64_t,const MmsUiLayoutHintsV1*)->MmsResult {return MMS_OK;};
        layout.acquireSnapshot=[](void* p,MmsUiLayoutSnapshotV1* out)->MmsResult {
            auto& h=*static_cast<Host*>(p);out->handle=200+(++h.layoutAcquires);return MMS_OK;
        };
        layout.releaseSnapshot=[](void* p,MmsHandle) {++static_cast<Host*>(p)->layoutReleases;};
    }
};

bool SameSpan(MmsSpan a, MmsSpan b) {
    return a.data==b.data && a.count==b.count && a.stride==b.stride && a.recordVersion==b.recordVersion;
}

void SceneOwnershipChecks() {
    using namespace mms::sdk;
    Host a, b;
    SceneServices serviceA, requeriedA, serviceB, notReady;
    const auto copiedHostA = a.host;
    Check(SceneServices::Query(a.host, serviceA)==MMS_OK);
    Check(SceneServices::Query(copiedHostA, requeriedA)==MMS_OK);
    Check(SceneServices::Query(b.host, serviceB)==MMS_OK);
    const auto copiedA = serviceA;
    SceneSnapshot snapshotA, snapshotB;
    const auto acquire = [](const SceneServices& services, SceneSnapshot& snapshot) {
        return services.Acquire(MMS_STAGE_BIND,0,MMS_CHANNEL_BONES,MmsTime{0,1},MmsId{},MmsSpan{},snapshot);
    };
    Check(acquire(serviceA, snapshotA)==MMS_OK);
    Check(acquire(serviceB, snapshotB)==MMS_OK);
    Check(snapshotA.valid() && snapshotB.valid() && snapshotA.get()==snapshotB.get());

    const auto verifyOwner = [](const SceneServices& services, const SceneSnapshot& snapshot, const Host& host) {
        const auto objectReads = host.sceneObjectReads, channelReads = host.sceneChannelReads;
        MmsSpan objects{}, bones{};
        Check(services.Objects(snapshot, objects)==MMS_OK);
        Check(services.ReadChannel(snapshot, MmsObjectRef{}, MMS_CHANNEL_BONES, bones)==MMS_OK);
        Check(SameSpan(objects, MmsSpan{&host.sceneObject,1,sizeof(host.sceneObject),1}));
        Check(SameSpan(bones, MmsSpan{&host.sceneBone,1,sizeof(host.sceneBone),1}));
        Check(host.sceneObjectReads==objectReads+1 && host.sceneChannelReads==channelReads+1);
    };
    const auto verifyRejected = [&](const SceneServices& services, const SceneSnapshot& snapshot, MmsResult expected) {
        const auto aObjects = a.sceneObjectReads, aChannels = a.sceneChannelReads;
        const auto bObjects = b.sceneObjectReads, bChannels = b.sceneChannelReads;
        const MmsSpan sentinel{&snapshot,47,59,71};
        auto objects = sentinel, bones = sentinel;
        const auto objectsResult = services.Objects(snapshot, objects);
        const auto channelResult = services.ReadChannel(snapshot, MmsObjectRef{}, MMS_CHANNEL_BONES, bones);
        if (objectsResult!=expected || channelResult!=expected)
            std::fprintf(stderr, "scene read rejection: Objects=%d, ReadChannel=%d, expected=%d\n",
                         objectsResult, channelResult, expected);
        Check(objectsResult==expected && channelResult==expected);
        Check(SameSpan(objects, sentinel) && SameSpan(bones, sentinel));
        Check(a.sceneObjectReads==aObjects && a.sceneChannelReads==aChannels &&
              b.sceneObjectReads==bObjects && b.sceneChannelReads==bChannels);
    };

    verifyRejected(notReady, snapshotA, MMS_E_NOT_READY);
    verifyOwner(requeriedA, snapshotA, a);
    verifyOwner(copiedA, snapshotA, a);
    verifyOwner(serviceB, snapshotB, b);
    verifyRejected(serviceB, snapshotA, MMS_E_STALE);
    verifyRejected(serviceA, snapshotB, MMS_E_STALE);

    // A distinct issuing entry point can reuse the same opaque context.
    const auto issuingTable = a.scene;
    a.scene.acquire=[](void*,const MmsSceneQueryV1*,MmsSnapshotV1* out)->MmsResult {
        out->handle=101;
        return MMS_OK;
    };
    SceneServices otherIssuer;
    Check(SceneServices::Query(a.host, otherIssuer)==MMS_OK);
    a.scene=issuingTable;
    verifyRejected(otherIssuer, snapshotA, MMS_E_STALE);

    const auto handle = snapshotA.get();
    SceneSnapshot moved(std::move(snapshotA));
    Check(!snapshotA.valid() && moved.get()==handle);
    verifyRejected(serviceB, snapshotA, MMS_E_NOT_READY);
    verifyOwner(serviceA, moved, a);
    Check(snapshotB.TransferFrom(moved)==MMS_OK && !moved.valid() && snapshotB.get()==handle);
    Check(a.sceneReleases==0 && b.sceneReleases==1 && b.lastSceneRelease==handle);
    verifyRejected(serviceB, snapshotB, MMS_E_STALE);
    verifyOwner(requeriedA, snapshotB, a);
    Check(snapshotB.Release()==MMS_OK && !snapshotB.valid());
    Check(snapshotB.Release()==MMS_OK && a.sceneReleases==1 && a.lastSceneRelease==handle && b.sceneReleases==1);
    verifyRejected(serviceA, snapshotB, MMS_E_NOT_READY);
}
}
int main() {
    try {
        Host h; mms::sdk::SceneServices scene; mms::sdk::LayoutServices layout;
        Check(mms::sdk::SceneServices::Query(h.host,scene)==MMS_OK);
        Check(mms::sdk::LayoutServices::Query(h.host,layout)==MMS_OK);
        mms::sdk::SceneSnapshot s;
        const auto acquire=[&] {return scene.Acquire(MMS_STAGE_BIND,0,0,MmsTime{0,1},MmsId{},MmsSpan{},s);};
        Check(acquire()==MMS_OK && s.get()==101);
        Check(acquire()==MMS_E_BUSY && s.get()==101 && h.sceneAcquires==1 && h.sceneReleases==0);
        Check(s.Release()==MMS_OK && h.sceneReleases==1);
        Check(acquire()==MMS_OK && s.get()==102 && h.sceneAcquires==2);
        mms::sdk::SceneSnapshot sceneTarget;
        Check(sceneTarget.TransferFrom(s)==MMS_OK && !s.valid() && sceneTarget.get()==102);
        Check(sceneTarget.TransferFrom(sceneTarget)==MMS_OK && sceneTarget.get()==102);
        Check(sceneTarget.Release()==MMS_OK && h.sceneReleases==2);
        mms::sdk::SceneSnapshot previousS(h.scene,104), replacementS(h.scene,105);
        Check(previousS.TransferFrom(replacementS)==MMS_OK && previousS.get()==105 && !replacementS.valid() && h.sceneReleases==3);
        Check(previousS.Release()==MMS_OK && h.sceneReleases==4);
        mms::sdk::LayoutSnapshotLease l;
        Check(layout.AcquireSnapshot(l)==MMS_OK && l.get()==201);
        Check(layout.AcquireSnapshot(l)==MMS_E_BUSY && l.get()==201 && h.layoutAcquires==1 && h.layoutReleases==0);
        Check(l.Release()==MMS_OK && h.layoutReleases==1);
        Check(layout.AcquireSnapshot(l)==MMS_OK && l.get()==202 && h.layoutAcquires==2);
        mms::sdk::LayoutSnapshotLease layoutTarget;
        Check(layoutTarget.TransferFrom(l)==MMS_OK && !l.valid() && layoutTarget.get()==202);
        Check(layoutTarget.TransferFrom(layoutTarget)==MMS_OK && layoutTarget.get()==202);
        Check(layoutTarget.Release()==MMS_OK && h.layoutReleases==2);
        mms::sdk::LayoutSnapshotLease previousL(h.layout,204), replacementL(h.layout,205);
        Check(previousL.TransferFrom(replacementL)==MMS_OK && previousL.get()==205 && !replacementL.valid() && h.layoutReleases==3);
        Check(previousL.Release()==MMS_OK && h.layoutReleases==4);
        auto brokenScene=h.scene; brokenScene.release=nullptr;
        mms::sdk::SceneSnapshot brokenS(brokenScene,99);
        mms::sdk::SceneSnapshot sceneSource(h.scene,103);
        Check(brokenS.TransferFrom(sceneSource)==MMS_E_UNSUPPORTED && brokenS.get()==99 && sceneSource.get()==103);
        Check(sceneSource.Release()==MMS_OK);
        Check(brokenS.Release()==MMS_E_UNSUPPORTED && brokenS.get()==99 && brokenS.Detach()==99);
        auto brokenLayout=h.layout;brokenLayout.releaseSnapshot=nullptr;
        mms::sdk::LayoutSnapshotLease brokenL(brokenLayout,98);
        mms::sdk::LayoutSnapshotLease layoutSource(h.layout,203);
        Check(brokenL.TransferFrom(layoutSource)==MMS_E_UNSUPPORTED && brokenL.get()==98 && layoutSource.get()==203);
        Check(layoutSource.Release()==MMS_OK);
        Check(brokenL.Release()==MMS_E_UNSUPPORTED && brokenL.get()==98 && brokenL.Detach()==98);
        SceneOwnershipChecks();
        std::printf("snapshot_cpp_safety_tests: %d checks passed\n",checks);return 0;
    } catch(int n) {std::fprintf(stderr,"snapshot check %d failed\n",n);return 1;}
}
