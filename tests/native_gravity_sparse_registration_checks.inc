// Included after the global-paste and gravity fixtures.
namespace {
std::vector<std::uint8_t> GravitySparseBytes(MMDApp& app) {
    std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::Read(app,curve_host::Kind::Gravity,0,channels,error),"read sparse gravity authoring");
    std::vector<animation_curve::NamedTrack> tracks;
    for(const auto& channel:channels)tracks.push_back({channel.id,channel.track});
    std::vector<std::uint8_t> bytes;std::string encodeError;
    Check(animation_curve::Encode(tracks,bytes,encodeError),"encode exact sparse gravity tracks");return bytes;
}
void GravitySparseSeed(GlobalPasteFixture& f,plugins::GlobalPasteSource& source) {
    for(unsigned i=0;i<3;++i)f.app->GravityKeys()[i].noiseEnabled=std::uint8_t(i%2);
    std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::Read(*f.app,curve_host::Kind::Gravity,0,channels,error),"read gravity before deleting one lane key");
    channels[0].track.keys.erase(channels[0].track.keys.begin()+1);
    channels[0].track.keys[0].outWeight=.23;channels[0].track.keys[0].outSlope=.17;
    Check(curve_host::Write(*f.app,curve_host::Kind::Gravity,0,channels,error),"keep shared gravity row while removing authored magnitude key");
    const auto& key=f.app->GravityKeys()[1];Check(key.frame==12,"other gravity lanes retain frame twelve");
    mdl::GravityClipboardRecord record{};record.acceleration=key.acceleration;record.noise=key.noise;record.noiseEnabled=key.noiseEnabled;
    std::memcpy(record.direction,key.direction,sizeof(record.direction));source.gravity={record};source.frameOffset=12;
    f.app->CurrentFrame()=12;
}
void NativeGravitySparseRegistrationChecks() {
    std::puts("  sparse gravity: author-only paste, exact undo and source-independent redo");
    {
        GlobalPasteFixture f;plugins::GlobalPasteSource source;GravitySparseSeed(f,source);
        const auto before=GravitySparseBytes(*f.app);const auto native=GlobalRegistrationKeys(f.app->GravityKeys());
        std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalKeyPaste(*f.app,source,change)==MMS_OK&&change,"same gravity payload plans missing authored frame");
        Check(GravitySparseBytes(*f.app)==before,"gravity authoring plan is read-only");
        GlobalPasteCommit(f,change);const auto after=GravitySparseBytes(*f.app);
        Check(after!=before&&f.history.UndoCount()==1,"author-only gravity edit is one common history step");
        auto payload=GlobalRegistrationKeys(f.app->GravityKeys());
        for(auto& key:payload)key.selected=0;
        auto expected=native;for(auto& key:expected)key.selected=0;
        Check(!std::memcmp(payload.data(),expected.data(),expected.size()*sizeof(mdl::GravityKey)),"sparse gravity registration preserves native payload bytes");
        source.gravity.clear();
        Check(f.history.History(false)==MMS_OK&&GravitySparseBytes(*f.app)==before,"gravity undo restores sparse authored track and weighted handles exactly");
        Check(f.history.History(true)==MMS_OK&&GravitySparseBytes(*f.app)==after,"gravity redo owns its explicit frame intent without clipboard data");
        plugins::GlobalPasteSource same;
        const auto& key=f.app->GravityKeys()[1];mdl::GravityClipboardRecord record{};
        record.acceleration=key.acceleration;record.noise=key.noise;record.noiseEnabled=key.noiseEnabled;
        std::memcpy(record.direction,key.direction,sizeof(record.direction));same.gravity={record};same.frameOffset=12;
        change.reset();Check(plugins::PlanGlobalKeyPaste(*f.app,same,change)==MMS_OK&&!change,"repeat sparse gravity registration becomes no-op");
    }
    std::puts("  sparse gravity: preparation allocation failures preserve both tracks");
    {
        GlobalPasteFixture f;plugins::GlobalPasteSource source;GravitySparseSeed(f,source);
        std::shared_ptr<plugins::HistoryChange> change;Check(plugins::PlanGlobalKeyPaste(*f.app,source,change)==MMS_OK&&change,"plan sparse gravity fault-injection record");
        const auto before=GravitySparseBytes(*f.app);const auto digest=f.Digest();bool completed=false;
        const std::string label="sparse gravity";
        for(long allocation=0;allocation<512;++allocation){
            std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
            failAfter=allocation;const auto status=f.history.PrepareNative(label,change,token);failAfter=-1;
            Check(f.Digest()==digest&&GravitySparseBytes(*f.app)==before&&!f.history.UndoCount(),"failed sparse gravity preparation has no native or authored writes");
            if(status==MMS_OK){completed=true;break;}
            Check(status==MMS_E_MEMORY&&!token,"sparse gravity allocation failure returns memory error");
        }
        Check(completed,"sparse gravity preparation succeeds after allocation sweep");
    }
}
}
