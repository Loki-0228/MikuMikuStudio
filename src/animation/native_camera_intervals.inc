// Included inside curve_host's private namespace, after CameraCurveTime.
using PreservedCameraInterval = curve_editor::NativeCameraInterval;
std::array<double,6> IntervalGeometry(const ac::Key& a,const ac::Key& b) {
    return {a.value,a.outWeight,a.outSlope,b.value,b.inWeight,b.inSlope};
}
bool ValidPreservedData(const PreservedCameraInterval& value) noexcept {
    if(value.first<0||value.last<=value.first||value.last>frame_limits::maximum||
       value.sourceFirst<0||value.sourceLast<=value.sourceFirst||value.sourceLast>frame_limits::maximum||
       !std::isfinite(value.sampleFirst)||!std::isfinite(value.sampleLast)||
       value.sampleFirst<value.sourceFirst||value.sampleLast>value.sourceLast||value.sampleFirst>=value.sampleLast||
       !std::isfinite(value.sourceValueFirst)||!std::isfinite(value.sourceValueLast))return false;
    for(double component:value.controls)if(!std::isfinite(component)||component<0||component>127)return false;
    for(double component:value.geometry)if(!std::isfinite(component))return false;
    return true;
}
bool PreservedCurrent(const Channel& channel,const PreservedCameraInterval& value) {
    if(!ValidPreservedData(value)||channel.track.discrete)return false;
    const auto& keys=channel.track.keys;
    const auto at=std::lower_bound(keys.begin(),keys.end(),value.last,[](const ac::Key& k,int f){return k.frame<f;});
    return at!=keys.end()&&at!=keys.begin()&&at->frame==value.last&&(at-1)->frame==value.first&&
        at->interpolation==ac::Interpolation::Bezier&&IntervalGeometry(*(at-1),*at)==value.geometry;
}
void PrunePreserved(Channel& channel) {
    auto& intervals=channel.preservedCameraIntervals;
    intervals.erase(std::remove_if(intervals.begin(),intervals.end(),[&](const auto& v){return !PreservedCurrent(channel,v);}),intervals.end());
    std::sort(intervals.begin(),intervals.end(),[](const auto& a,const auto& b){return a.first<b.first;});
}
ac::Sample SamplePreservedSource(const PreservedCameraInterval& interval,double frame) {
    const double source=interval.sampleLast-interval.sampleFirst==double(interval.last)-interval.first?
        interval.sampleFirst+(frame-interval.first):interval.sampleFirst+(frame-interval.first)/(interval.last-interval.first)*
        (interval.sampleLast-interval.sampleFirst);
    const double span=double(interval.sourceLast)-interval.sourceFirst;
    const double ratio=(source-interval.sourceFirst)/span;
    double value=interval.sourceValueFirst;
    if(source>=interval.sourceLast)value=interval.sourceValueLast;
    else if(source>interval.sourceFirst&&span!=1.0)value+=(interval.sourceValueLast-interval.sourceValueFirst)*
        CameraCurveTime(interval.controls,float(ratio));
    const double derivative=span==1.0?0:(interval.sourceValueLast-interval.sourceValueFirst)/span*
        native_curve_sample::Evaluate(interval.controls,ratio).slope*
        (interval.sampleLast-interval.sampleFirst)/(interval.last-interval.first);
    return {value,derivative};
}
bool SamplePreservedInterval(const Channel& channel,double frame,ac::Sample& out) {
    if(!std::isfinite(frame)||channel.track.keys.empty())return false;
    const auto& intervals=channel.preservedCameraIntervals;
    auto at=std::upper_bound(intervals.begin(),intervals.end(),frame,[](double f,const auto& value){return f<value.first;});
    if(at==intervals.begin())return false;const auto& interval=*--at;
    if(frame>=interval.first&&frame<=interval.last&&
       (frame<interval.last||interval.last==channel.track.keys.back().frame)&&PreservedCurrent(channel,interval)) {
        out=SamplePreservedSource(interval,frame);return true;
    }
    return false;
}
// Split/retime by the old visible frame range, retaining the original native
// time domain. Deleted keys that would join different native slices are refused
// by retiming; intentional value/handle edits invalidate only their own slice.
void RemapPreserved(const Channel& source,Channel& target,const std::vector<int>& oldFrames,bool strict) {
    target.preservedCameraIntervals.clear();
    const auto& keys=target.track.keys;
    for(std::size_t i=1;i<keys.size();++i) {
        const int first=oldFrames[i-1],last=oldFrames[i];
        const auto& intervals=source.preservedCameraIntervals;
        const auto found=std::upper_bound(intervals.begin(),intervals.end(),first,[](int f,const auto& value){return f<value.last;});
        if(found!=intervals.end()) {const auto& original=*found;
            if(!PreservedCurrent(source,original)||last<=original.first||first>=original.last)continue;
            if(first<original.first||last>original.last) {
                if(strict)throw std::invalid_argument("retime would join exact native camera slices; bake before deleting their boundary");
                continue;
            }
            auto next=original;next.first=keys[i-1].frame;next.last=keys[i].frame;
            const auto position=[&](double f){return original.sampleFirst==original.first&&original.sampleLast==original.last?f:
                original.sampleFirst+(f-original.first)/(original.last-original.first)*
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
