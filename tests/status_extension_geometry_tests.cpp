#include "mikumikustudio/status_extension_geometry.hpp"
#include <array>
#include <cstdio>
#include <limits>

using namespace mikumikustudio;
namespace {
unsigned checks = 0;
void Check(bool condition) { if (!condition) throw ++checks; ++checks; }
bool Equal(RECT a, RECT b) { return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom; }
}
int main() {
    try {
        // A 1000-unit viewport has the same top-row title boundary as the
        // production layout. Other controls start at TitleRight().
        const RECT available{100,100,1100,600}, scene{100,100,1100,600};
        const auto wide = MakeViewportPanelLayout(available, scene, true, false);
        Check(!wide.topWrapped && wide.TitleRight()==650);
        StatusExtensionGeometryInput in{};
        in.titleInk={110,75,300,100}; in.gap=5; in.minimumWidth=80; in.minimumHeight=20;
        Check(Equal(FindStatusExtensionSlot(wide,in),RECT{}));
        in.titleMeasured=true; in.obstaclesComplete=true;
        Check(Equal(FindStatusExtensionSlot(wide,in),RECT{305,75,650,100}));
        const std::array<RECT,3> obstacles{{{350,75,410,100},{500,75,530,100},{400,200,600,220}}};
        in.nativeObstacles=obstacles.data(); in.obstacleCount=obstacles.size();
        Check(Equal(FindStatusExtensionSlot(wide,in),RECT{535,75,650,100}));
        in.minimumWidth=116; Check(Equal(FindStatusExtensionSlot(wide,in),RECT{}));
        in.minimumWidth=1; in.titleInk.right=1000; Check(Equal(FindStatusExtensionSlot(wide,in),RECT{}));
        in.titleInk.right=300; in.nativeObstacles=nullptr; Check(Equal(FindStatusExtensionSlot(wide,in),RECT{}));
        in.obstacleCount=0; in.minimumHeight=26; Check(Equal(FindStatusExtensionSlot(wide,in),RECT{}));
        in.minimumHeight=1; in.gap=(std::numeric_limits<int>::max)();
        Check(Equal(FindStatusExtensionSlot(wide,in),RECT{}));
        // A wrapped layout keeps the extension in the first row, not in the
        // second row that contains native controls.
        const RECT narrow{0,100,500,600}, sceneWithMargin{0,150,500,600};
        const auto wrapped = MakeViewportPanelLayout(narrow,sceneWithMargin,true,false);
        Check(wrapped.topWrapped && wrapped.TitleRight()==495 && wrapped.topBand.bottom==125);
        in={}; in.titleMeasured=true; in.obstaclesComplete=true; in.titleInk={10,75,100,100};
        in.minimumWidth=10; in.minimumHeight=10;
        const RECT below{110,100,450,125}; in.nativeObstacles=&below; in.obstacleCount=1;
        Check(Equal(FindStatusExtensionSlot(wrapped,in),RECT{100,75,495,100}));
        // Overlap, unsorted obstacles and equal-width segments are deterministic.
        const std::array<RECT,3> overlapping{{{300,75,350,100},{200,75,250,100},{240,75,300,100}}};
        in.nativeObstacles=overlapping.data(); in.obstacleCount=overlapping.size();
        Check(Equal(FindStatusExtensionSlot(wrapped,in),RECT{350,75,495,100}));
        const RECT invalid{20,75,10,100}; in.nativeObstacles=&invalid; in.obstacleCount=1;
        Check(Equal(FindStatusExtensionSlot(wrapped,in),RECT{}));
        in.obstaclesComplete=false; Check(Equal(FindStatusExtensionSlot(wrapped,in),RECT{}));
        std::printf("status_extension_geometry_tests: %u checks passed\n",checks);
        return 0;
    } catch (unsigned n) { std::fprintf(stderr,"geometry check %u failed\n",n); return 1; }
}
