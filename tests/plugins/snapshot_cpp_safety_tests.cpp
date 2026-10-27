#include <mms/cpp/layout.hpp>
#include <mms/cpp/scene.hpp>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <type_traits>

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
        scene.release=[](void* p,MmsHandle) {++static_cast<Host*>(p)->sceneReleases;};
        scene.objects=[](void*,MmsHandle,MmsSpan*)->MmsResult {return MMS_OK;};
        scene.readChannel=[](void*,MmsHandle,MmsObjectRef,uint32_t,MmsSpan*)->MmsResult {return MMS_OK;};
        layout.h={sizeof(layout),1,0};layout.context=this;
        layout.setDefaultHints=[](void*,MmsString,const MmsUiLayoutHintsV1*)->MmsResult {return MMS_OK;};
        layout.stageHints=[](void*,MmsHandle,MmsString,uint64_t,const MmsUiLayoutHintsV1*)->MmsResult {return MMS_OK;};
        layout.acquireSnapshot=[](void* p,MmsUiLayoutSnapshotV1* out)->MmsResult {
            auto& h=*static_cast<Host*>(p);out->handle=200+(++h.layoutAcquires);return MMS_OK;
        };
        layout.releaseSnapshot=[](void* p,MmsHandle) {++static_cast<Host*>(p)->layoutReleases;};
    }
};
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
        std::printf("snapshot_cpp_safety_tests: %d checks passed\n",checks);return 0;
    } catch(int n) {std::fprintf(stderr,"snapshot check %d failed\n",n);return 1;}
}
