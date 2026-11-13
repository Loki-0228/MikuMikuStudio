// Included in the full-profile curve host after the strict TargetChannels reader.
namespace {
std::size_t CopyAuthoringIndex(Kind kind, int slot) {
    switch(kind) {
    case Kind::Camera:case Kind::Light:case Kind::Shadow:case Kind::Gravity:
        if(slot!=0)throw std::invalid_argument("global copy authoring requires slot zero");
        return static_cast<std::size_t>(kind);
    case Kind::Accessory:
        if(slot<0||slot>=255)throw std::invalid_argument("invalid accessory copy authoring slot");
        return std::size_t(slot)+4;
    default:throw std::invalid_argument("unknown global copy authoring kind");
    }
}
void ValidateCopyCallbacks(const std::vector<Channel>& channels) {
    // Inspect the callable without copying it: an opaque closure's copy
    // constructor can itself access external state or have side effects.
    for(const auto& channel:channels)
        if(channel.sample&&!CanonicalCameraTiming(channel))
            throw std::invalid_argument("opaque global copy authoring callback");
}
void ValidateCopyChannels(const std::vector<Channel>& channels, Kind kind, bool baseline) {
    const auto& fields=Fields(kind);
    if(channels.size()!=fields.size())throw std::invalid_argument("invalid global copy authoring lanes");
    std::string detail;
    for(std::size_t c=0;c<channels.size();++c) {
        const auto& channel=channels[c];const auto& track=channel.track;const auto& field=fields[c];
        if(channel.id!=field.id||track.discrete!=field.discrete||!ac::Validate(track,detail)||
           (!baseline&&track.keys.empty())||(!track.keys.empty()&&track.keys.front().frame!=0))
            throw std::invalid_argument("invalid global copy authoring track");
        if(!std::isfinite(channel.base)||!std::isfinite(channel.minimum)||!std::isfinite(channel.maximum)||
           channel.minimum>channel.maximum)
            throw std::invalid_argument("invalid global copy authoring display bounds");
        for(const auto& key:track.keys) {
            if(key.frame>frame_limits::maximum||(!baseline&&(key.value<field.minimum||key.value>field.maximum))||
               (field.discrete&&std::floor(key.value)!=key.value))
                throw std::invalid_argument("invalid global copy authoring key value");
        }
        // Native baseline lanes share their frame domain. Load can deliberately
        // retain an entirely empty baseline while restoring valid authoring.
        if(baseline&&c) {
            const auto& first=channels.front().track.keys;
            if(track.keys.size()!=first.size())throw std::invalid_argument("inconsistent global copy authoring baseline");
            for(std::size_t i=0;i<first.size();++i)if(track.keys[i].frame!=first[i].frame)
                throw std::invalid_argument("inconsistent global copy authoring baseline frames");
        }
        // Metadata may be inactive after scalar editing (including the -1
        // geometry sentinel); never normalize or discard such stored rows.
        // The root's raw bytes are not sampled and may occupy the full byte
        // range. Other controls use native timing, including crossed/endpoint X.
        for(std::size_t i=0;i<channel.nativeControls.size();++i)
            for(double value:channel.nativeControls[i])
                if(!std::isfinite(value)||value<0||value>(i?127:255))
                    throw std::invalid_argument("invalid global copy native controls");
        for(const auto& geometry:channel.controlGeometry)for(double value:geometry)
            if(!std::isfinite(value))throw std::invalid_argument("invalid global copy control geometry");
        if(!channel.preservedCameraIntervals.empty()&&
           (kind!=Kind::Camera||c>=8||!CanonicalCameraTiming(channel)))
            throw std::invalid_argument("invalid global copy exact camera owner");
        int previousEnd=-1;
        for(const auto& interval:channel.preservedCameraIntervals) {
            if(!PreservedCurrent(channel,interval)||interval.first<previousEnd||
               interval.sourceValueFirst<field.minimum||interval.sourceValueFirst>field.maximum||
               interval.sourceValueLast<field.minimum||interval.sourceValueLast>field.maximum||
               SamplePreservedSource(interval,interval.first).value!=interval.geometry[0]||
               SamplePreservedSource(interval,interval.last).value!=interval.geometry[3])
                throw std::invalid_argument("invalid global copy exact camera interval");
            previousEnd=interval.last;
        }
    }
}
template<class Key> std::vector<Channel> CopyNativeChannels(const Key* pool, Kind kind) {
    const auto capacity=mdl::KeyPoolCapacity(pool);
    // TargetChannels checks every later backlink. Check the head too, without
    // weakening or changing that shared reader's existing callers.
    if(!pool||!capacity||pool[0].previous)
        throw std::invalid_argument("invalid global copy native head");
    const NativeKeyView view{pool,capacity,
        [](const void* source,std::uint32_t index) noexcept -> const void* {
            return static_cast<const Key*>(source)+index;
        }};
    return TargetChannels(view,kind);
}
std::vector<Channel> CopyNativeChannels(MMDApp& app, Kind kind, int slot) {
    switch(kind) {
    case Kind::Camera:return CopyNativeChannels(app.CameraKeys(),kind);
    case Kind::Light:return CopyNativeChannels(app.LightKeys(),kind);
    case Kind::Shadow:return CopyNativeChannels(app.ShadowKeys(),kind);
    case Kind::Gravity:return CopyNativeChannels(app.GravityKeys(),kind);
    case Kind::Accessory:return CopyNativeChannels(app.AccessoryKeys(slot),kind);
    default:throw std::invalid_argument("unknown global copy native kind");
    }
}
}

bool CaptureCopyAuthoring(MMDApp& app,const std::vector<CopyAuthoringTarget>& targets,
                          std::vector<CopyAuthoring>& out,std::wstring& error) {
    out.clear();error.clear();
    try {
        // Validate selectors even when there is no registry entry. No author
        // means no native traversal: raw clipboard admission is a later stage.
        for(const auto& target:targets)CopyAuthoringIndex(target.kind,target.slot);
        std::array<bool,259> seen{};
        std::vector<CopyAuthoring> captured;
        for(const auto& target:targets) {
            const auto index=CopyAuthoringIndex(target.kind,target.slot);
            if(seen[index])continue;
            seen[index]=true;
            const auto found=states.find({&app,target.kind,target.slot});
            if(found==states.end()||!StateIdentityCurrent(found->second,app,target.kind,target.slot))continue;
            // A registration may intentionally add a sparse authored key even
            // when the native row equals its old baseline. Reconcile alone
            // cannot infer that intent, so leave the pending batch untouched.
            for(auto* batch=LegacyRegistrationBatch::active_;batch;batch=batch->previous_)
                if(batch->app_==&app&&!batch->frames_[index].empty())
                    throw std::invalid_argument("unfinished global copy authoring registration");
            const State& stored=found->second;
            ValidateCopyCallbacks(stored.channels);
            ValidateCopyCallbacks(stored.baseline);
            ValidateCopyChannels(stored.channels,target.kind,false);
            ValidateCopyChannels(stored.baseline,target.kind,true);
            // Copy construction preserves revision/author without advancing
            // NextAuthoringRevision. Never default-construct a temporary State.
            State detached=stored;
            const auto native=CopyNativeChannels(app,target.kind,target.slot);
            Reconcile(detached,native);
            ValidateCopyChannels(detached.channels,target.kind,false);
            captured.push_back({target.kind,target.slot,std::move(detached.channels)});
        }
        out.swap(captured);return true;
    }catch(const std::bad_alloc&){throw;}
    catch(const std::exception& failure) {
        const char* message=failure.what();
        error.assign(message,message+std::strlen(message));return false;
    }
}
