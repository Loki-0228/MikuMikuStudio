// Include after native_audio_history_checks.inc. Every expected PCM sample is
// independent of the production decoder; real DS and native waveforms follow.
namespace audio_format_checks {
using namespace native_audio_checks;
struct Format { unsigned tag,bits,channels,valid;bool extensible=false,displaced=false; };
void Put(std::vector<unsigned char>& out,std::uint64_t value,unsigned bytes){
    for(unsigned n=0;n<bytes;++n)out.push_back(static_cast<unsigned char>(value>>(n*8)));
}
void Text(std::vector<unsigned char>& out,const char* text,unsigned bytes){out.insert(out.end(),text,text+bytes);}
void Save(const fs::path& path,const std::vector<unsigned char>& out){std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()),std::streamsize(out.size()));Check(bool(file),"write deterministic WAVE format fixture");}
std::vector<unsigned char> Container(Format f,const std::vector<unsigned char>& raw){
    std::vector<unsigned char> chunks,fmt;Put(fmt,f.extensible?0xfffe:f.tag,2);Put(fmt,f.channels,2);Put(fmt,8000,4);
    Put(fmt,8000*f.channels*(f.bits/8),4);Put(fmt,f.channels*(f.bits/8),2);Put(fmt,f.bits,2);
    if(f.extensible){Put(fmt,22,2);Put(fmt,f.valid,2);Put(fmt,f.channels==1?4:3,4);Put(fmt,f.tag,4);
        const unsigned char tail[]={0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};fmt.insert(fmt.end(),std::begin(tail),std::end(tail));}
    const auto chunk=[&](const char* id,const std::vector<unsigned char>& bytes){Text(chunks,id,4);Put(chunks,bytes.size(),4);
        chunks.insert(chunks.end(),bytes.begin(),bytes.end());if(bytes.size()&1u)chunks.push_back(0);};
    if(f.displaced){chunk("JUNK",{17,29,33});chunk("data",raw);chunk("fmt ",fmt);}else{chunk("fmt ",fmt);chunk("data",raw);}
    std::vector<unsigned char> result;Text(result,"RIFF",4);Put(result,chunks.size()+4,4);Text(result,"WAVE",4);
    result.insert(result.end(),chunks.begin(),chunks.end());return result;
}
struct Samples { std::vector<unsigned char> raw,expected; };
Samples Known(Format f){
    Samples result;
    // 12,000 samples exceed the streaming conversion scratch size for float64.
    const std::int16_t integer[]={-32768,-16384,-1,0,1,16384,32767};
    const double floating[]={-2,-1,-.5,-.0001,-0.0,0.0,1e-40,.0001,.5,1,2};
    const std::int16_t converted[]={-32768,-32768,-16384,-3,0,0,0,3,16384,32767,32767};
    for(unsigned n=0;n<12000*f.channels;++n){
        if(f.tag==3){const auto index=n%std::size(floating);const auto value=floating[index];
            if(f.bits==32){const float single=float(value);std::uint32_t word;std::memcpy(&word,&single,4);Put(result.raw,word,4);}
            else{std::uint64_t word;std::memcpy(&word,&value,8);Put(result.raw,word,8);}Put(result.expected,std::uint16_t(converted[index]),2);
        }else{const auto value=integer[n%std::size(integer)];
            const auto scale=std::int64_t(1)<<(f.bits-16);
            const auto padding=f.bits-f.valid;
            const auto low=(std::uint64_t(scale)-1)&~((std::uint64_t(1)<<padding)-1);
            const auto sample=std::int64_t(value)*scale+std::int64_t(low);
            Put(result.raw,std::uint64_t(sample),f.bits/8);Put(result.expected,std::uint16_t(value),2);}
    }return result;
}
std::vector<unsigned char> ReadCache(const wchar_t* path){
    std::ifstream file(fs::path(path),std::ios::binary);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),{});
    Check(bytes.size()>=44&&!std::memcmp(bytes.data(),"RIFF",4)&&bytes[20]==1&&bytes[34]==16,"playback cache is ordinary PCM16 RIFF");
    return {bytes.begin()+44,bytes.end()};
}
void FormatsAndHistory(){
    const Format formats[]={
        {1,24,1,24},{1,24,2,24},{1,32,1,32},{1,32,2,32},
        {3,32,1,32},{3,32,2,32},{3,64,1,64},{3,64,2,64},
        {1,24,1,20,true},{1,32,2,24,true},{1,16,2,16,true},
        {3,32,2,32,true},{3,64,1,64,true},{1,16,1,16,false,true}};
    for(const auto format:formats)for(const auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}){
        Files files;const auto source=files.directory/L"wide.wav",reference=files.directory/L"expected.wav";const auto samples=Known(format);
        Save(source,Container(format,samples.raw));Save(reference,Container({1,16,format.channels,16},samples.expected));
        Scene f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"select high-bit-depth audio history mode");
        f.Import(files.a);const auto original=wave_audio::Active(&f.audio);f.Import(source);const auto converted=wave_audio::Active(&f.audio);
        Check(f.audio.format.wBitsPerSample==16&&f.audio.format.nChannels==format.channels&&f.audio.format.nSamplesPerSec==8000&&
            f.audio.dataSize==int(samples.expected.size())&&ReadCache(f.audio.path)==samples.expected,"conversion preserves sample rate channel order frame count and every expected PCM16 sample");
        WaveAudioContext oracle{};oracle.mainWindow=f.app->MainWindow();oracle.englishUI=1;
        Check(WaveLoadFile(&oracle,reference.c_str(),f.app->PathWorkspace()),"native loader reads independent expected PCM16 oracle");
        Check(f.audio.waveformColumns==oracle.waveformColumns&&!std::memcmp(f.audio.waveformMax,oracle.waveformMax,oracle.waveformColumns)&&
            !std::memcmp(f.audio.waveformMin,oracle.waveformMin,oracle.waveformColumns),"all normalized WAVE formats match actual native PCM16 waveform exactly");
        WaveCtxReset(&oracle);std::free(oracle.waveformMax);std::free(oracle.waveformMin);
        Check(TryClearBoundAudio(f.app.get())&&f.history.UndoCount()==3,"high-bit-depth import replace and clear are three explicit nodes");
        fs::rename(source,source.native()+L".removed");fs::rename(files.a,files.a.native()+L".removed");
        Check(f.history.History(false)==MMS_OK&&wave_audio::Active(&f.audio)==converted&&ReadCache(f.audio.path)==samples.expected,"undo clear restores normalized samples after source disappearance");f.Playback();
        Check(f.history.History(false)==MMS_OK&&wave_audio::Active(&f.audio)==original&&f.history.History(true)==MMS_OK&&
            wave_audio::Active(&f.audio)==converted&&ReadCache(f.audio.path)==samples.expected,"undo and redo replacement retain both clips independently of original files");
        f.Playback();Check(f.history.History(true)==MMS_OK&&!f.app->WaveEnabled(),"redo clear retires converted playback normally");
    }
}
void MediaEntryAndLegacyCapture(){
    Files files;Scene f;
    for(const Format format:std::initializer_list<Format>{{1u,24u,2u,24u},{3u,64u,1u,64u},{1u,32u,1u,20u,true},{3u,32u,2u,32u,true}}){
        const auto source=files.directory/L"project-audio.wav";const auto samples=Known(format);Save(source,Container(format,samples.raw));
        WaveAudioContext legacy{};legacy.mainWindow=f.app->MainWindow();legacy.englishUI=1;
        Check(WaveLoadFile(&legacy,source.c_str(),f.app->PathWorkspace())&&WaveStartPlayback(&legacy),"ordinary and PMM shared native load path accepts extended WAVE formats");
        Check(legacy.format.wBitsPerSample==16&&ReadCache(legacy.path)==samples.expected&&media::AudioSource(&legacy)==source.native(),"ordinary WAVE cache retains original source metadata for PMM");
        LARGE_INTEGER zero{},before{},after{};SetFilePointerEx(legacy.fileHandle,zero,&before,FILE_CURRENT);
        auto captured=wave_audio::Asset::Capture(legacy,source.c_str(),256ull*1024*1024);
        Check(captured&&SetFilePointerEx(legacy.fileHandle,zero,&after,FILE_CURRENT)&&before.QuadPart==after.QuadPart,"legacy normalized WAVE capture retains exact source read position");
        WaveCtxReset(&legacy);std::free(legacy.waveformMax);std::free(legacy.waveformMin);
        fs::rename(source,source.native()+L".removed");
        {wave_audio::Prepared prepared(f.audio,captured);prepared.Publish();}Check(ReadCache(f.audio.path)==samples.expected,"legacy snapshot is independent of normalized decoder and source lifetime");f.Playback();
        fs::remove(source.native()+L".removed");
    }
    {
        const auto source=files.directory/L"replace-wide.wav";const Format format{1,24,1,24};const auto samples=Known(format);
        Save(source,Container(format,samples.raw));WaveAudioContext reused{};reused.mainWindow=f.app->MainWindow();reused.englishUI=1;
        Check(WaveLoadFile(&reused,source.c_str(),f.app->PathWorkspace()),"seed ordinary extended WAVE cache for a direct PCM replacement");
        const auto cache=std::wstring(reused.path);
        Check(WaveLoadFile(&reused,files.a.c_str(),f.app->PathWorkspace())&&media::AudioSource(&reused)==files.a.native()&&!fs::exists(cache),
            "replacing normalized WAV with direct PCM retires the old cache and cannot keep stale PMM source metadata");
        WaveCtxReset(&reused);std::free(reused.waveformMax);std::free(reused.waveformMin);
    }
    // Truly old PCM24 context, before the shared normalizer existed. Preserve
    // its actual drawn waveform, while playback is now represented as PCM16.
    for(unsigned bits:{24u,32u}){
    const Format rawFormat{1,bits,1,bits};const auto samples=Known(rawFormat);const auto source=files.directory/L"old-wide.wav";
    Save(source,Container(rawFormat,samples.raw));WaveAudioContext old{};old.mainWindow=f.app->MainWindow();old.englishUI=1;
    old.format.wFormatTag=1;old.format.nChannels=1;old.format.nSamplesPerSec=8000;old.format.nAvgBytesPerSec=8000*(bits/8);old.format.nBlockAlign=WORD(bits/8);old.format.wBitsPerSample=WORD(bits);
    old.dataOffset=44;old.dataSize=int(samples.raw.size());old.waveformColumns=3;unsigned char upper[]={27,49,25},lower[]={2,20,25};old.waveformMax=upper;old.waveformMin=lower;
    wcscpy_s(old.path,source.c_str());old.fileHandle=CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(old.fileHandle!=INVALID_HANDLE_VALUE,"open independent raw PCM24 legacy handle");auto captured=wave_audio::Asset::Capture(old,source.c_str(),256ull*1024*1024);
    CloseHandle(old.fileHandle);old.fileHandle=nullptr;fs::rename(source,source.native()+L".removed");
    {wave_audio::Prepared prepared(f.audio,captured);prepared.Publish();}
    Check(ReadCache(f.audio.path)==samples.expected&&f.audio.waveformColumns==3&&!std::memcmp(f.audio.waveformMax,upper,3)&&!std::memcmp(f.audio.waveformMin,lower,3),
        "old PCM24 and PCM32 capture preserve existing waveform bytes and deterministic normalized playback");f.Playback();fs::remove(source.native()+L".removed");
    }
    for(bool extensible:{false,true}){
        const auto source=files.directory/L"pcm8-layout.wav";const std::vector<unsigned char> raw{0,1,127,128,129,254,255,128};
        auto bytes=Container({1,8,1,8,extensible},raw);
        if(!extensible){bytes.insert(bytes.begin()+36,2,0);bytes[16]=18;const auto riff=std::uint32_t(bytes.size()-8);
            for(unsigned n=0;n<4;++n)bytes[4+n]=static_cast<unsigned char>(riff>>(n*8));}
        Save(source,bytes);auto asset=wave_audio::Asset::DecodeWave(source.c_str(),256ull*1024*1024);
        Check(bool(asset),"fmt18 and extensible PCM8 use canonical playback caches");
        std::ifstream input(fs::path(asset->CachePath()),std::ios::binary);std::vector<unsigned char> cache((std::istreambuf_iterator<char>(input)),{});
        Check(cache.size()==44+raw.size()&&cache[20]==1&&cache[34]==8&&std::equal(raw.begin(),raw.end(),cache.begin()+44),
            "canonicalizing PCM8 layout preserves every original byte including unsigned silence");
    }
    for(bool embeddedMarker:{false,true}){
        const auto source=files.directory/L"metadata-marker.wav";const auto samples=Known({1,16,1,16});
        auto bytes=Container({1,16,1,16},samples.raw);std::vector<unsigned char> junk;
        Text(junk,"JUNK",4);Put(junk,embeddedMarker?4:2,4);
        if(embeddedMarker)Text(junk,"data",4);else{junk.push_back(0);junk.push_back('d');}
        bytes.insert(bytes.begin()+36,junk.begin(),junk.end());const auto riff=std::uint32_t(bytes.size()-8);
        for(unsigned n=0;n<4;++n)bytes[4+n]=static_cast<unsigned char>(riff>>(n*8));Save(source,bytes);
        auto asset=wave_audio::Asset::DecodeWave(source.c_str(),256ull*1024*1024,true);
        Check(asset&&ReadCache(asset->CachePath())==samples.expected,
            "metadata data text and trailing d cannot trick the consuming native marker scanner or enter its EOF loop");
        WaveAudioContext legacy{};legacy.mainWindow=f.app->MainWindow();legacy.englishUI=1;
        Check(WaveLoadFile(&legacy,source.c_str(),f.app->PathWorkspace())&&legacy.dataSize==int(samples.expected.size()),
            "actual old WAV reader receives a safe canonical cache for ambiguous metadata");
        WaveCtxReset(&legacy);std::free(legacy.waveformMax);std::free(legacy.waveformMin);
    }
}
void InvalidAndBudgets(){
    Files files;Scene f;NativePoseTestBinding binding(*f.app,f.history);f.Import(files.a);const auto before=f.Digest();
    const auto directory=local_data::File(L"audio-cache");
    const auto caches=[&](){std::set<fs::path> result;for(const auto& entry:fs::directory_iterator(directory))if(entry.is_regular_file())result.insert(entry.path());return result;};
    const auto baseline=caches();const Format format{1,24,1,20,true};const auto samples=Known(format);const auto good=Container(format,samples.raw);
    const auto invalid=files.directory/L"invalid-wide.wav";
    const auto reject=[&](std::vector<unsigned char> bytes){Save(invalid,bytes);NativeAudioPlan plan;
        Check(plan.Begin(*f.app)==MMS_OK&&plan.Read(invalid.c_str())==MMS_E_ARGUMENT&&f.Digest()==before&&f.history.UndoCount()==1,
            "invalid RIFF or sample content cannot modify live clip resources or history");Check(caches()==baseline,"invalid conversion removes every incomplete private cache");};
    for(unsigned offset:{16u,36u,38u,40u,44u,48u,64u}){auto bytes=good;
        if(offset==16){bytes[16]=17;bytes[17]=bytes[18]=bytes[19]=0;}
        else if(offset==36)bytes[offset]=21; // cbSize < 22
        else if(offset==38)bytes[offset]=25; // validBits > container
        else if(offset==40)bytes[offset]=3; // stereo mask on mono
        else if(offset==44)bytes[offset]=2; // unsupported subformat
        else if(offset==48)bytes[offset]=1; // full GUID tail mismatch
        else {bytes[64]=1;bytes[65]=0;bytes[66]=0;bytes[67]=0;} // incomplete data frame
        reject(std::move(bytes));}
    {auto bytes=good;bytes[68]|=1;reject(std::move(bytes));} // nonzero low sample padding
    for(unsigned bits:{32u,64u})for(double value:{std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
        std::vector<unsigned char> raw;if(bits==32){float single=float(value);std::uint32_t word;std::memcpy(&word,&single,4);Put(raw,word,4);}else{std::uint64_t word;std::memcpy(&word,&value,8);Put(raw,word,8);}
        reject(Container({3,bits,1,bits},raw));}
    const auto path=files.directory/L"budget-wide.wav";Save(path,good);
    bool exceeded=false;std::wstring playable,error;WaveAudioContext limited{};
    Check(!media::PrepareAudio(&limited,path.c_str(),playable,error,1024,&exceeded)&&exceeded&&!error.empty()&&caches()==baseline,
        "ordinary format normalization refuses small retained budgets before publishing any source or cache");media::ReleaseAudio(&limited);
    bool success=false;unsigned failures=0;
    for(long limit=0;limit<256&&!success;++limit){NativeAudioPlan plan;failAfter=limit;auto status=plan.Begin(*f.app);if(status==MMS_OK)status=plan.Read(path.c_str());failAfter=-1;
        Check(f.Digest()==before&&f.history.UndoCount()==1,"normalizer allocation refusal retains complete current audio and history");
        if(status==MMS_OK)success=true;else{++failures;Check(status==MMS_E_MEMORY&&caches()==baseline,"failed normalized decode reports memory and leaves no cache");}}
    Check(success&&failures>5,"allocation sweep reaches fully decoded and cached high-bit-depth clip");
}
}
void NativeAudioFormatChecks(){
    audio_format_checks::FormatsAndHistory();audio_format_checks::MediaEntryAndLegacyCapture();audio_format_checks::InvalidAndBudgets();
}
