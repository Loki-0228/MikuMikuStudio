#include "mikumikustudio/offscreen_glyph_dc.hpp"
#include <array>
#include <cstdio>
#include <initializer_list>
using namespace mikumikustudio;
namespace {
unsigned checks = 0;
void Check(bool value) { ++checks; if (!value) throw checks; }
struct Fake {
    bool select = true, noPrevious = false, restore = true, deleteDc = true, deleteFont = true;
    std::array<int,16> events{}; std::size_t count = 0;
    void Record(int event) noexcept { if (count < events.size()) events[count++] = event; }
    bool Events(std::initializer_list<int> expected) const noexcept {
        if (count != expected.size()) return false;
        std::size_t i=0; for (int value:expected) if(events[i++]!=value)return false;
        return true;
    }
    OffscreenGlyphOps Ops() {
        return {this,
            [](void* p,std::uintptr_t dc,std::uintptr_t font,std::uintptr_t* prev) noexcept {
                auto& f=*static_cast<Fake*>(p); f.Record(1);
                if (!f.select || dc!=10 || font!=20) return false;
                *prev=f.noPrevious ? 0 : 30; return true;
            },
            [](void* p,std::uintptr_t dc,std::uintptr_t prev) noexcept {
                auto& f=*static_cast<Fake*>(p); f.Record(2);
                return f.restore && dc==10 && prev==30;
            },
            [](void* p,std::uintptr_t dc) noexcept {
                auto& f=*static_cast<Fake*>(p); f.Record(3);
                return f.deleteDc && dc==10;
            },
            [](void* p,std::uintptr_t font) noexcept {
                auto& f=*static_cast<Fake*>(p); f.Record(4);
                return f.deleteFont && font==20;
            }};
    }
};
}
int main() {
    try {
        Fake f; OffscreenGlyphDc a(f.Ops());
        Check(!a.AdoptDc(0) && a.AdoptDc(10) && !a.AdoptDc(11));
        Check(a.SelectOwnedFont(20)==OffscreenFontSelection::Selected && a.RestoreFont() && a.font()==0 && a.ready());
        Check(a.Close() && a.dc()==0 && f.Events({1,2,4,3}));
        Check(a.Close());
        Fake r; r.restore=false; OffscreenGlyphDc b(r.Ops());
        Check(b.AdoptDc(10) && b.SelectOwnedFont(20)==OffscreenFontSelection::Selected && !b.RestoreFont());
        Check(!b.ready() && b.dc()==10 && b.font()==20 && b.SelectOwnedFont(21)==OffscreenFontSelection::Rejected);
        r.deleteDc=false; Check(!b.Close() && b.dc()==10 && b.font()==20);
        Check(r.Events({1,2,3}));
        r.deleteDc=true; r.deleteFont=false;
        Check(!b.Close() && b.dc()==0 && b.font()==20);
        Check(r.Events({1,2,3,3,4}));
        r.deleteFont=true; Check(b.Close() && b.dc()==0 && b.font()==0);
        Check(r.Events({1,2,3,3,4,4}));
        Fake s; s.select=false; OffscreenGlyphDc c(s.Ops());
        Check(c.AdoptDc(10) && c.SelectOwnedFont(20)==OffscreenFontSelection::FailedOwned && c.font()==20 && !c.ready());
        Check(c.Close() && s.Events({1,3,4}));
        Fake missing; missing.noPrevious=true; OffscreenGlyphDc missingOwner(missing.Ops());
        Check(missingOwner.AdoptDc(10) && missingOwner.SelectOwnedFont(20)==OffscreenFontSelection::FailedOwned &&
            missingOwner.font()==20 && !missingOwner.ready());
        Check(missingOwner.Close() && missing.Events({1,3,4}));
        Fake occupied; OffscreenGlyphDc existing(occupied.Ops());
        Check(existing.SelectOwnedFont(20)==OffscreenFontSelection::Rejected && existing.font()==0);
        Check(existing.AdoptDc(10) && existing.SelectOwnedFont(20)==OffscreenFontSelection::Selected &&
            existing.SelectOwnedFont(21)==OffscreenFontSelection::Rejected && existing.font()==20);
        Check(existing.Close() && occupied.Events({1,3,4}));
        Fake d; d.deleteFont=false; OffscreenGlyphDc e(d.Ops());
        Check(e.AdoptDc(10) && e.SelectOwnedFont(20)==OffscreenFontSelection::Selected && !e.RestoreFont());
        Check(!e.ready() && e.font()==20 && e.dc()==10);
        d.deleteFont=true; Check(e.Close() && d.Events({1,2,4,3,4}));
        OffscreenGlyphDc invalid({}); Check(!invalid.AdoptDc(10) &&
            invalid.SelectOwnedFont(20)==OffscreenFontSelection::Rejected && invalid.font()==0 && invalid.Close());
        std::printf("checked_glyph_tests: %u checks passed\n",checks); return 0;
    } catch (unsigned n) { std::fprintf(stderr,"checked glyph check %u failed\n",n); return 1; }
}
