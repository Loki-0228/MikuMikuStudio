// Display FPS counts completed scene submissions, never message-pump work.
#include "mikumikustudio/frame_rate.hpp"
#include "mikumikustudio/timeline_ruler.hpp"
#include <cstdio>
#include <cstdlib>
using mikumikustudio::frame_rate::State;
void Check(bool ok,const char* what) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",what);
    if(!ok)std::exit(2);
}
int main() {
    namespace ruler = mikumikustudio::timeline_ruler;
    Check(ruler::MajorInterval(30) == 5 && ruler::MajorInterval(60) == 10 &&
          ruler::MajorInterval(24) == 4, "ruler intervals follow the playback rate");
    Check(ruler::IsSecond(60, 60) && !ruler::IsSecond(30, 60) &&
          ruler::IsSecond(-24, 24), "second markers use signed timeline frames at the selected rate");
    Check(ruler::Label(59, 59) && !ruler::Label(60, 59) && ruler::Label(70, 59),
          "unusual integer rates label whole seconds without overlapping the next major label");
    Check(ruler::Label(-59, 59) && !ruler::Label(-60, 59),
          "negative ruler labels use the same spacing around whole seconds");
    Check(ruler::Label(4, 1) && !ruler::Label(3, 1) && ruler::Label(6, 3),
          "very slow playback keeps second accents while spacing numeric labels");
    State fps;
    fps.Tick(0);
    for(int i=0;i<60;++i) {
        fps.Tick(double(i)/60);
        fps.Begin();fps.Complete();
        Check(fps.Present(true),"one successful completed scene counts");
    }
    Check(fps.Tick(1)&&fps.rate==60&&fps.presentedFrames==60,"one second of real scenes reports 60 FPS");
    for(int i=0;i<1000;++i) {
        fps.Tick(1+double(i)/1000);
        fps.Begin(); // The performance-saving scene gate returns here.
        Check(!fps.Present(true),"an idle pump cannot count a stale successful Present");
    }
    Check(fps.Tick(2)&&fps.rate==0&&fps.presentedFrames==60,"one thousand idle loops report zero rendered FPS");
    for(int pass=0;pass<8;++pass){fps.Begin();fps.Complete();}
    Check(fps.Present(true)&&!fps.Present(true),"eight convergence samples and repeated HUD presents count once");
    fps.Begin();fps.Complete();
    Check(!fps.Present(false)&&!fps.Present(true),"failed display discards its completion token");
    fps.Begin();
    Check(!fps.Present(true),"failed scene rendering cannot count a successful stale display");
    Check(fps.Tick(3)&&fps.rate==1&&fps.presentedFrames==61,"only the successfully displayed converged frame counts");
    fps.Begin();fps.Complete();
    Check(fps.Present(true),"native DX12 counts its own successful swap-chain frame");
    Check(!fps.Present(true),"host DX9 present cannot double-count the native DX12 frame");
    for(int i=0;i<29;++i){fps.Begin();fps.Complete();fps.Present(true);}
    Check(fps.Tick(4)&&fps.rate==30,"native or MME or MMV frames share the same counter");
    // Dialog scene refreshes enter the same Begin/Complete/Present sequence.
    for(int i=0;i<30;++i){fps.Begin();fps.Complete();fps.Present(true);}
    Check(fps.Tick(5)&&fps.rate==30,"modal preview FPS has the same definition as normal playback");
    Check(fps.Tick(7)&&fps.rate==0,"long idle time never preserves a stale nonzero rate");
    fps={};fps.Tick(8);fps.Begin();
    Check(!fps.Present(true)&&fps.presentedFrames==0,"recording reset excludes offline/prewarm frames");
    Check(!fps.Tick(7.5)&&fps.epoch==7.5&&fps.frames==0,"clock restart cannot produce a negative or huge rate");
}
