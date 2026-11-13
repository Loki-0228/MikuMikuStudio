// Included inside curve_host's private namespace. Editing and codec admission
// deliberately use the shared evaluator's legacy positive-domain defaults.
using PreservedCameraInterval = curve_editor::NativeCameraInterval;
std::array<double,6> IntervalGeometry(const ac::Key& a,const ac::Key& b) {
    return native_camera_sampling::detail::IntervalGeometry(a,b);
}
bool ValidPreservedData(const PreservedCameraInterval& value) noexcept {
    return native_camera_sampling::detail::ValidPreservedData(value);
}
bool PreservedCurrent(const Channel& channel,const PreservedCameraInterval& value) {
    return native_camera_sampling::detail::PreservedCurrent(channel,value);
}
void PrunePreserved(Channel& channel) {
    auto& intervals=channel.preservedCameraIntervals;
    intervals.erase(std::remove_if(intervals.begin(),intervals.end(),[&](const auto& v){return !PreservedCurrent(channel,v);}),intervals.end());
    std::sort(intervals.begin(),intervals.end(),[](const auto& a,const auto& b){return a.first<b.first;});
}
ac::Sample SamplePreservedSource(const PreservedCameraInterval& interval,double frame) {
    return native_camera_sampling::SamplePreserved(interval,frame);
}
// Split/retime by the old visible frame range, retaining the original native
// time domain. Deleted keys that would join different native slices are refused
// by retiming; intentional value/handle edits invalidate only their own slice.
void RemapPreserved(const Channel& source,Channel& target,const std::vector<int>& oldFrames,bool strict,bool deleteKeys=false) {
    target.preservedCameraIntervals.clear();
    const auto& keys=target.track.keys;
    for(std::size_t i=1;i<keys.size();++i) {
        const int first=oldFrames[i-1],last=oldFrames[i];
        const auto& intervals=source.preservedCameraIntervals;
        const auto found=std::upper_bound(intervals.begin(),intervals.end(),first,[](int f,const auto& value){return f<value.last;});
        if(found!=intervals.end()) {const auto& original=*found;
            if(!PreservedCurrent(source,original)||last<=original.first||first>=original.last)continue;
            if(first<original.first||last>original.last) {
                if(strict&&!deleteKeys)throw std::invalid_argument("retime would join exact native camera slices; bake before deleting their boundary");
                continue;
            }
            auto next=original;next.first=keys[i-1].frame;next.last=keys[i].frame;
            const auto position=[&](double f){return original.sampleFirst==original.first&&original.sampleLast==original.last?f:
                original.sampleFirst+(f-original.first)/(double(original.last)-double(original.first))*
                (original.sampleLast-original.sampleFirst);};
            next.sampleFirst=position(first);next.sampleLast=position(last);
            next.geometry=IntervalGeometry(keys[i-1],keys[i]);
            // A native value edit deliberately releases its affected slice.
            if(!strict&&(keys[i-1].value!=SamplePreservedSource(original,first).value||
                         keys[i].value!=SamplePreservedSource(original,last).value))continue;
            if(keys[i].interpolation==ac::Interpolation::Bezier)target.preservedCameraIntervals.push_back(next);
        }
    }
    std::sort(target.preservedCameraIntervals.begin(),target.preservedCameraIntervals.end(),[](const auto& a,const auto& b){return a.first<b.first;});
}
