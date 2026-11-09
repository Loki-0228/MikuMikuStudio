#pragma once
#include <algorithm>
#include <cstdint>

namespace mikumikustudio::timeline_ruler {
inline int MajorInterval(int playbackFps) {
    return (std::max)(1, (playbackFps + 3) / 6);
}
inline bool IsSecond(std::int32_t frame, int playbackFps) {
    return playbackFps > 0 && frame % playbackFps == 0;
}
inline bool Label(std::int32_t frame, int playbackFps) {
    playbackFps = (std::max)(1, playbackFps);
    if (playbackFps < 4)
        return frame % (playbackFps * ((4 + playbackFps - 1) / playbackFps)) == 0;
    const int interval = MajorInterval(playbackFps);
    if (IsSecond(frame, playbackFps)) return true;
    const int remainder = (frame % playbackFps + playbackFps) % playbackFps;
    const int distance = (std::min)(remainder, playbackFps - remainder);
    return frame % interval == 0 && distance >= (std::min)(3, interval);
}
}
