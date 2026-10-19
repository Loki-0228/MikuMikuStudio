#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace mikumikustudio::interpolation_link {
// Mirror the edited handle about the join in the 0..127 easing panel. Keep
// the other handle intact; shorten the linked arm if it would cross it.
template<class T> bool Mirror(std::array<T,4>& adjacent,
                             const std::array<T,4>& edited, bool editedStart) {
    const int target = editedStart ? 2 : 0;
    const double dx = editedStart ? double(edited[0]) : 127.0-double(edited[2]);
    const double dy = editedStart ? double(edited[1]) : 127.0-double(edited[3]);
    const double low = editedStart ? double(adjacent[0])+1.0 : 1.0;
    const double high = editedStart ? 126.0 : double(adjacent[2])-1.0;
    if(low>high)return false;
    const double x=std::clamp(editedStart?127.0-dx:dx,low,high);
    const double length=editedStart?127.0-x:x;
    const double y=std::clamp(dx>0.0?dy*length/dx:dy,0.0,127.0);
    adjacent[target]=static_cast<T>(std::round(x));
    adjacent[target+1]=static_cast<T>(std::round(editedStart?127.0-y:y));
    return true;
}
}
