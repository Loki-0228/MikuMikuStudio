#include "mikumikustudio/offscreen_gdi_surface.hpp"
#include <array>
#include <cstdio>
using namespace mikumikustudio;
namespace {
unsigned checks=0;
void Check(bool ok) { if (!ok) throw ++checks; ++checks; }
struct Fake {
    bool createDc=true, createBitmap=true, pixels=true, selectBitmap=true;
    bool selectFont=true, restoreFont=true, deleteDc=true, deleteFont=true, deleteBitmap=true;
    std::array<int,32> events{}; unsigned n=0; int storage=0;
    void Log(int x) noexcept { if(n<events.size())events[n++]=x; }
    OffscreenSurfaceOps Ops() noexcept {
        return {this,
            [](void* c,std::uintptr_t) noexcept -> std::uintptr_t { auto& f=*static_cast<Fake*>(c);f.Log(1);return f.createDc?10:0; },
            [](void* c,std::uintptr_t,int,int,void** p) noexcept -> std::uintptr_t { auto& f=*static_cast<Fake*>(c);f.Log(2);*p=f.pixels?&f.storage:nullptr;return f.createBitmap?20:0; },
            [](void* c,std::uintptr_t,std::uintptr_t) noexcept { auto& f=*static_cast<Fake*>(c);f.Log(3);return f.selectBitmap; },
            [](void* c,std::uintptr_t) noexcept { auto& f=*static_cast<Fake*>(c);f.Log(7);return f.deleteBitmap; },
            {this,
                [](void* c,std::uintptr_t,std::uintptr_t,std::uintptr_t* p) noexcept { auto& f=*static_cast<Fake*>(c);f.Log(4);*p=40;return f.selectFont; },
                [](void* c,std::uintptr_t,std::uintptr_t) noexcept { auto& f=*static_cast<Fake*>(c);f.Log(5);return f.restoreFont; },
                [](void* c,std::uintptr_t) noexcept { auto& f=*static_cast<Fake*>(c);f.Log(6);return f.deleteDc; },
                [](void* c,std::uintptr_t) noexcept { auto& f=*static_cast<Fake*>(c);f.Log(8);return f.deleteFont; }} };
    }
};
}
int main() {
    try {
        Fake f; OffscreenGdiSurfacePool<1,2> pool; OffscreenSurfaceView view{};
        auto slot=pool.Reserve(f.Ops()); Check(slot.generation==1 && pool.occupied()==1 && !pool.Reserve(f.Ops()).generation);
        Check(pool.Initialize(slot,1,0,10)==OffscreenSurfaceInit::Rejected && f.n==0);
        Check(pool.Read(slot,view) && !view.ready && view.width==0 && view.height==0);
        Check(pool.Initialize(slot,1,20,10)==OffscreenSurfaceInit::Ready && pool.Read(slot,view) && view.ready && view.pixels && view.width==20 && view.height==10);
        Check(pool.Initialize(slot,1,12,7)==OffscreenSurfaceInit::Rejected && pool.Read(slot,view) && view.width==20 && view.height==10);
        Check(pool.SelectOwnedFont(slot,30)==OffscreenFontSelection::Selected);
        f.restoreFont=false; Check(!pool.RestoreFont(slot) && pool.Read(slot,view) && !view.ready && view.font==30 && !view.width && !view.height);
        f.deleteDc=false; Check(!pool.Release(slot) && pool.occupied()==1 && pool.Read(slot,view) && view.dc==10 && view.bitmap==20 && !view.width && !view.height);
        f.deleteDc=true; f.deleteFont=false; Check(!pool.Release(slot) && pool.Read(slot,view) && view.dc==0 && view.font==30 && view.bitmap==20);
        f.deleteFont=true; f.deleteBitmap=false; Check(!pool.Release(slot) && pool.Read(slot,view) && view.font==0 && view.bitmap==20);
        f.deleteBitmap=true; Check(pool.Release(slot) && pool.occupied()==0);
        Check(f.events[0]==1 && f.events[1]==2 && f.events[2]==3 && f.events[3]==4 && f.events[4]==5 &&
              f.events[5]==6 && f.events[6]==6 && f.events[7]==8 && f.events[8]==8 && f.events[9]==7 && f.events[10]==7);
        Fake bad; bad.selectBitmap=false; auto second=pool.Reserve(bad.Ops());
        Check(second.index==slot.index && second.generation==2);
        const auto before=view;
        Check(!pool.Read(slot,view) && view.dc==before.dc && view.width==before.width && view.height==before.height && !pool.Release(slot) &&
              pool.Initialize(slot,1,10,10)==OffscreenSurfaceInit::Rejected &&
              pool.SelectOwnedFont(slot,30)==OffscreenFontSelection::Rejected && !pool.RestoreFont(slot));
        Check(pool.Initialize(second,1,10,10)==OffscreenSurfaceInit::FailedOwned && pool.Read(second,view) && view.dc==10 && view.bitmap==20 && !view.ready && !view.width && !view.height);
        Check(pool.Release(second) && bad.events[3]==6 && bad.events[4]==7);
        Check(!pool.Reserve(bad.Ops()).generation && pool.occupied()==0);
        Fake noPixels; noPixels.pixels=false; OffscreenGdiSurfacePool<1> other;
        auto third=other.Reserve(noPixels.Ops());
        Check(other.Initialize(third,1,10,10)==OffscreenSurfaceInit::FailedOwned && other.Read(third,view) && view.bitmap==20 && !view.pixels && !view.width && !view.height);
        Check(other.Release(third));
        std::printf("offscreen_gdi_surface_tests: %u checks passed\n",checks); return 0;
    } catch(unsigned n) { std::fprintf(stderr,"surface check %u failed\n",n);return 1; }
}
