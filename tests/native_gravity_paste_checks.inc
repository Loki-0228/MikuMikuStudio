// Include after native_global_paste_checks.inc and global registration fixtures.
namespace mikudancestudio { int RegisterGravityKey(MMDApp*,const void*);void ApplyGravityTrack(MMDApp*); }
namespace {
void GravityPasteOnly(GlobalPasteFixture& f) {
    f.app->ClipboardCounts()={};f.app->ClipboardCounts().gravity=unsigned(f.gravity.size());
    f.app->GravityClipboard()=f.gravity.data();
    auto* keys=f.app->GravityKeys();
    for(unsigned i=0;i<3;++i) {
        keys[i].acceleration=1.371f+float(i)*3.171f;
        keys[i].direction[0]=.211f+float(i)*.071f;keys[i].direction[1]=-.317f-float(i)*.113f;
        keys[i].direction[2]=.919f-float(i)*.017f;
        keys[i].noise=i==0?-7:i==1?13:-2;keys[i].noiseEnabled=std::uint8_t(i+2);
        keys[i].reserved1[0]=std::uint8_t(0x71+i);keys[i].reserved1[1]=std::uint8_t(0xb1+i);
    }
    f.app->GravityMagnitude()=123;f.app->GravityX()=1;f.app->GravityY()=-1;f.app->GravityZ()=2;
    f.app->GravityNoise()=42;f.app->GravityNoiseModeWord()=0xaabbccdd;
    for(unsigned i=0;i<f.gravity.size();++i) {
        auto& source=f.gravity[i];source.frame=i*10;
        source.acceleration=2.139f+float(i)*1.371f;
        source.direction[0]=.91f-.17f*float(i);source.direction[1]=-.391f+.031f*float(i);
        source.direction[2]=.137f+.051f*float(i);source.noise=i==0?-9:i==1?17:3;
        source.noiseEnabled=std::uint8_t(5+i);
        source.reserved[0]=0xe1;source.reserved[1]=0xe2;source.reserved[2]=0xe3;
    }
}
bool GravityPastePoseEqual(MMDApp& a,MMDApp& b) {
    return MorphSameFloat(a.GravityMagnitude(),b.GravityMagnitude())&&MorphSameFloat(a.GravityX(),b.GravityX())&&
        MorphSameFloat(a.GravityY(),b.GravityY())&&MorphSameFloat(a.GravityZ(),b.GravityZ())&&
        a.GravityNoise()==b.GravityNoise()&&a.GravityNoiseModeWord()==b.GravityNoiseModeWord();
}
void NativeGravityPasteChecks() {
    std::puts("  gravity paste: actual registrar sampler and four-byte mode parity");
    for(int offset:{7,12,20,60}) {
        GlobalPasteFixture f,oracle;GravityPasteOnly(f);GravityPasteOnly(oracle);
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=offset;
        const GlobalPasteKeys before(*f.app);const auto initial=f.Digest();
        auto change=GlobalPastePlan(f);Check(change&&f.Digest()==initial,"gravity-only plan leaves pose keys and reserved bytes untouched");
        GlobalPasteCommit(f,change);ClearGlobalFrameSelection(oracle.app.get());
        for(const auto& source:oracle.gravity)Check(RegisterGravityKey(oracle.app.get(),&source)==1,"real gravity registrar accepts finite source record");
        ApplyGravityTrack(oracle.app.get());
        Check(GlobalRegistrationSame(f.app->GravityKeys(),GlobalRegistrationKeys(oracle.app->GravityKeys()))&&
            GravityPastePoseEqual(*f.app,*oracle.app)&&f.app->LastRegisteredFrame()==oracle.app->LastRegisteredFrame(),
            "owned gravity paste matches real registrar payload marks watermark and copied four-byte noise mode");
        const auto after=GlobalRegistrationKeys(f.app->GravityKeys());
        f.ClearClipboard();
        for(int playhead:{0,5,11,12,17,25,31,47,120}) {
            f.app->CurrentFrame()=oracle.app->CurrentFrame()=playhead;
            Check(f.history.History(false)==MMS_OK&&before.Same(*f.app),"gravity undo restores every global selection snapshot");
            std::memcpy(oracle.app->GravityKeys(),before.gravity.data(),before.gravity.size()*sizeof(mdl::GravityKey));
            ApplyGravityTrack(oracle.app.get());
            Check(GravityPastePoseEqual(*f.app,*oracle.app),"gravity undo matches float interpolation signed integer truncation and upper-key mode word");
            Check(f.history.History(true)==MMS_OK&&GlobalRegistrationSame(f.app->GravityKeys(),after),"gravity redo owns copied source payload and mark bytes");
            std::memcpy(oracle.app->GravityKeys(),after.data(),after.size()*sizeof(mdl::GravityKey));ApplyGravityTrack(oracle.app.get());
            Check(GravityPastePoseEqual(*f.app,*oracle.app),"gravity redo matches real ordinary sampler at exact between and tail times");
        }
    }
    std::puts("  gravity paste: frame-zero head and nonsemantic clipboard padding");
    {
        GlobalPasteFixture f;GravityPasteOnly(f);f.app->CurrentFrame()=0;
        const auto before=GlobalRegistrationKeys(f.app->GravityKeys());const auto identity=mdl::KeyPoolIdentity(f.app->GravityKeys());
        GlobalPasteCommit(f,GlobalPastePlan(f));
        Check(f.app->GravityKeys()[0].frame==0&&f.app->GravityKeys()[0].noise==f.gravity[0].noise&&
            f.app->GravityKeys()[0].selected&&f.app->GravityKeys()[0].reserved1[0]==0x71&&f.app->GravityKeys()[0].reserved1[1]==0xb1,
            "frame-zero gravity paste updates the head and keeps key reserved bytes instead of copying clipboard padding");
        const auto grown=mdl::KeyPoolCapacity(f.app->GravityKeys());
        Check(grown>before.size()&&mdl::KeyPoolIdentity(f.app->GravityKeys())==identity,"full gravity pool grows privately with stable ownership");
        const auto digest=f.Digest();const auto count=f.history.UndoCount();auto noop=GlobalPastePlan(f);
        Check(!noop&&f.Digest()==digest&&f.history.UndoCount()==count,"identical gravity payload preserves history pose and all marks");
        Check(f.history.History(false)==MMS_OK&&GlobalRegistrationSame(f.app->GravityKeys(),before),"frame-zero undo restores head payload and original marks");
        for(std::size_t i=before.size();i<grown;++i)Check(!f.app->GravityKeys()[i].selected,"gravity undo clears newly grown tail selections");
    }
    std::puts("  gravity paste: unsafe integer interpolation rejection");
    {
        GlobalPasteFixture f;GravityPasteOnly(f);f.gravity.resize(1);f.gravity[0].frame=0;
        f.app->CurrentFrame()=12;f.app->ClipboardCounts().gravity=1;f.app->GravityClipboard()=f.gravity.data();
        f.app->GravityKeys()[0].noise=INT32_MIN;f.gravity[0].noise=INT32_MAX;
        plugins::GlobalPasteSource source;source.gravity=f.gravity;source.frameOffset=12;
        plugins::GlobalPasteFamilyResult part;
        Check(plugins::PlanGravityKeyPastePart(*f.app,source,part)==MMS_OK&&part.part,"extreme exact gravity keys can be privately planned");
        std::unique_ptr<plugins::PreparedGlobalPastePart> candidate;
        Check(part.part->Prepare(true,candidate)==MMS_OK,"exact gravity endpoint does not require unsafe integer subtraction");candidate.reset();
        f.app->CurrentFrame()=6;const auto digest=f.Digest();
        Check(part.part->Prepare(true,candidate)==MMS_E_ARGUMENT&&!candidate&&f.Digest()==digest&&!f.history.UndoCount(),
            "gravity noise subtraction overflow rejects entire candidate without keys pose marks or history writes");
    }
    std::puts("  gravity paste: independent weighted authoring");
    {
        GlobalPasteFixture f,oracle;GravityPasteOnly(f);GravityPasteOnly(oracle);
        for(auto* fixture:{&f,&oracle}) {
            // The raw-format parity cases above intentionally exercise mode
            // bytes 2/3/4. The authoring API's boolean channel requires 0/1;
            // preserve the independently tested high mode-word bytes here.
            for(unsigned i=0;i<3;++i) {
                fixture->app->GravityKeys()[i].noise=10+int(i)*3;
                fixture->app->GravityKeys()[i].noiseEnabled=std::uint8_t(i%2);
            }
            for(auto& source:fixture->gravity){source.noise=15;source.noiseEnabled=1;}
            std::vector<curve_editor::Channel> channels;std::wstring error;
            Check(curve_host::Read(*fixture->app,curve_host::Kind::Gravity,0,channels,error),"read gravity authoring channels");
            channels[0].track.keys[1].interpolation=animation_curve::Interpolation::Bezier;
            channels[0].track.keys[0].outWeight=.2;channels[0].track.keys[0].outSlope=.13;
            const bool written=curve_host::Write(*fixture->app,curve_host::Kind::Gravity,0,channels,error);
            if(!written)std::fwprintf(stderr,L"  weighted gravity curve write error: %ls\n",error.c_str());
            Check(written,"write weighted gravity curve handles");
        }
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=7;
        const auto before=GlobalRegistrationKeys(f.app->GravityKeys());
        GlobalPasteCommit(f,GlobalPastePlan(f));ClearGlobalFrameSelection(oracle.app.get());
        for(const auto& source:oracle.gravity)Check(RegisterGravityKey(oracle.app.get(),&source)==1,"native gravity oracle writes into authored track");
        ApplyGravityTrack(oracle.app.get());
        Check(GravityPastePoseEqual(*f.app,*oracle.app),"gravity paste reconciles owned authoring and matches real curve-aware apply");
        std::vector<curve_editor::Channel> observed;std::wstring error;
        Check(curve_host::Read(*f.app,curve_host::Kind::Gravity,0,observed,error),"ordinary gravity reader reconciles native paste");
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=5;
        Check(f.history.History(false)==MMS_OK&&GlobalRegistrationSame(f.app->GravityKeys(),before),"authored gravity undo restores key bytes and marks");
        Check(curve_host::Read(*f.app,curve_host::Kind::Gravity,0,observed,error)&&observed[0].track.keys[0].outWeight==.2&&
            observed[0].track.keys[0].outSlope==.13,"gravity paste undo restores original weighted handles after ordinary reconciliation");
    }
}
}
