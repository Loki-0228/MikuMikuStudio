#include "mikumikustudio/wave_audio_asset.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/native_media.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace mikumikustudio::wave_audio {
namespace {
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit File(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~File() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
struct Lease { AssetPtr asset; bool dirty=true; };
using Leases=std::map<const WaveAudioContext*,Lease>;
Leases& ActiveFiles() {
    static Leases files;
    return files;
}
std::uint64_t Sum(std::uint64_t a, std::uint64_t b) noexcept { return b > UINT64_MAX-a ? UINT64_MAX : a+b; }
void Budget(std::uint64_t bytes, std::uint64_t maximum) { if (bytes > maximum) throw std::bad_alloc(); }
void Read(HANDLE file, void* output, DWORD bytes) {
    DWORD got = 0;
    if (!ReadFile(file, output, bytes, &got, nullptr) || got != bytes) throw std::invalid_argument("incomplete PCM audio");
}
void Seek(HANDLE file, std::uint64_t offset) {
    LARGE_INTEGER position; position.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) throw std::invalid_argument("cannot seek PCM audio");
}
std::uint64_t Length(HANDLE file) {
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(file, &value) || value.QuadPart < 0) throw std::invalid_argument("cannot size PCM audio");
    return std::uint64_t(value.QuadPart);
}
std::uint16_t U16(const unsigned char* p) { return std::uint16_t(p[0]) | std::uint16_t(p[1]) << 8; }
std::uint32_t U32(const unsigned char* p) { return std::uint32_t(U16(p)) | std::uint32_t(U16(p+2)) << 16; }
void Put16(unsigned char* p, unsigned value) { p[0] = static_cast<unsigned char>(value); p[1] = static_cast<unsigned char>(value >> 8); }
void Put32(unsigned char* p, std::uint32_t value) { Put16(p, value); Put16(p+2, value >> 16); }
void Format(const WAVEFORMATEX& f) {
    const auto align = unsigned(f.nChannels) * unsigned(f.wBitsPerSample / 8);
    const auto average = std::uint64_t(f.nSamplesPerSec) * align;
    if (f.wFormatTag != WAVE_FORMAT_PCM || (f.nChannels != 1 && f.nChannels != 2) ||
        (f.wBitsPerSample != 8 && f.wBitsPerSample != 16) || !f.nSamplesPerSec || f.nBlockAlign != align ||
        average != f.nAvgBytesPerSec || average > unsigned(DSBSIZE_MAX) / 2)
        throw std::invalid_argument("unsupported or inconsistent PCM audio format");
}
struct WaveSource {
    WAVEFORMATEX format{};
    unsigned encoding=0, validBits=0;
    std::uint64_t offset=0;
    std::uint32_t bytes=0;
    bool canonical=false;
};
bool LegacyDataHeader(HANDLE file,std::uint64_t dataOffset) {
    if(dataOffset<44)return false;
    // The original reader searches for the first literal data marker after
    // the fixed PCM header. Preserve metadata-bearing PCM files only when
    // that search reaches the actual validated chunk, not metadata contents.
    Seek(file,36);std::array<unsigned char,65536> bytes{};unsigned phase=0;
    for(std::uint64_t at=36;at<dataOffset-4;){
        const auto count=DWORD((std::min)(std::uint64_t(bytes.size()),dataOffset-4-at));Read(file,bytes.data(),count);
        for(DWORD n=0;n<count;++n){
            if(!phase){if(bytes[n]=='d')phase=1;}
            else if(bytes[n]==(phase==2?'t':'a')){
                if(++phase==4)return at+n-3==dataOffset-8;
            }else phase=0; // The old getc scanner consumes a mismatched byte.
        }
        at+=count;
    }return false;
}
WaveSource InspectWave(HANDLE file) {
    const auto length=Length(file);std::array<unsigned char,12> header{};Seek(file,0);Read(file,header.data(),12);
    if(std::memcmp(header.data(),"RIFF",4)||std::memcmp(header.data()+8,"WAVE",4))throw std::invalid_argument("not RIFF WAVE audio");
    const auto limit=std::uint64_t(U32(header.data()+4))+8;
    if(limit>length||limit<12)throw std::invalid_argument("invalid RIFF WAVE size");
    WaveSource result;bool format=false,data=false,canonicalFormat=false;
    for(std::uint64_t offset=12;offset<limit;){
        if(limit-offset<8)throw std::invalid_argument("truncated WAVE chunk");
        std::array<unsigned char,8> chunk{};Seek(file,offset);Read(file,chunk.data(),8);
        const auto size=U32(chunk.data()+4);const auto start=offset+8;
        if(size>limit-start)throw std::invalid_argument("WAVE chunk exceeds RIFF bounds");
        if(!std::memcmp(chunk.data(),"fmt ",4)){
            if(format||size<16)throw std::invalid_argument("invalid WAVE format chunk");
            std::array<unsigned char,40> bytes{};Read(file,bytes.data(),(std::min)(size,std::uint32_t(bytes.size())));
            auto& f=result.format;f.wFormatTag=U16(bytes.data());f.nChannels=U16(bytes.data()+2);
            f.nSamplesPerSec=U32(bytes.data()+4);f.nAvgBytesPerSec=U32(bytes.data()+8);
            f.nBlockAlign=U16(bytes.data()+12);f.wBitsPerSample=U16(bytes.data()+14);
            result.encoding=f.wFormatTag;result.validBits=f.wBitsPerSample;
            if(size==17||(size>=18&&U16(bytes.data()+16)>size-18))throw std::invalid_argument("invalid WAVE format extension length");
            if(f.wFormatTag==0xfffe){
                if(size<40||U16(bytes.data()+16)<22)throw std::invalid_argument("incomplete extensible WAVE format");
                static const unsigned char tail[]={0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
                if(std::memcmp(bytes.data()+28,tail,sizeof tail))throw std::invalid_argument("unknown extensible WAVE subtype");
                result.encoding=U32(bytes.data()+24);result.validBits=U16(bytes.data()+18);
                const auto mask=U32(bytes.data()+20);
                if(mask&&mask!=(f.nChannels==1?4u:3u))throw std::invalid_argument("unsupported WAVE channel layout");
            }
            const auto bits=f.wBitsPerSample,channels=f.nChannels;
            const bool pcm=result.encoding==1&&(bits==8||bits==16||bits==24||bits==32);
            const bool floating=result.encoding==3&&(bits==32||bits==64);
            const auto align=unsigned(channels)*unsigned(bits/8);const auto average=std::uint64_t(f.nSamplesPerSec)*align;
            if((channels!=1&&channels!=2)||(!pcm&&!floating)||!f.nSamplesPerSec||f.nBlockAlign!=align||
                average!=f.nAvgBytesPerSec||!result.validBits||result.validBits>bits||(floating&&result.validBits!=bits))
                throw std::invalid_argument("unsupported or inconsistent WAVE format");
            canonicalFormat=offset==12&&(size==16||(size==18&&!U16(bytes.data()+16)))&&f.wFormatTag==1&&(bits==8||bits==16);
            format=true;
        }else if(!std::memcmp(chunk.data(),"data",4)){
            if(data||size>INT_MAX-44)throw std::invalid_argument("invalid WAVE data chunk");
            data=true;result.offset=start;result.bytes=size;
        }
        const auto padded=std::uint64_t(size)+(size&1u);
        if(padded>limit-start)throw std::invalid_argument("missing WAVE chunk padding");offset=start+padded;
    }
    if(!format||!data||result.bytes%result.format.nBlockAlign)throw std::invalid_argument("missing or unaligned WAVE audio");
    result.canonical=canonicalFormat&&LegacyDataHeader(file,result.offset);
    return result;
}
// Only playback is converted. Source paths still identify the original file.
// No whole-source allocation or Media Foundation codec is needed for WAV.
void ReadPcm(HANDLE file,const WaveSource& source,WAVEFORMATEX& format,std::vector<unsigned char>& pcm,std::uint64_t budget) {
    format=source.format;format.wFormatTag=1;format.cbSize=0;
    const bool copy=source.encoding==1&&source.validBits==format.wBitsPerSample&&(format.wBitsPerSample==8||format.wBitsPerSample==16);
    const auto frames=std::uint64_t(source.bytes)/source.format.nBlockAlign;
    if(!copy){format.wBitsPerSample=16;format.nBlockAlign=format.nChannels*2;
        const auto average=std::uint64_t(format.nSamplesPerSec)*format.nBlockAlign;
        if(average>UINT32_MAX)throw std::invalid_argument("PCM byte rate overflow");format.nAvgBytesPerSec=DWORD(average);}
    Format(format);
    const auto count=frames*format.nBlockAlign;
    if(count>INT_MAX-44)throw std::bad_alloc();
    Budget(Sum(count,count+44+(count&1u)),budget);pcm.resize(std::size_t(count));Seek(file,source.offset);
    if(copy){if(count)Read(file,pcm.data(),DWORD(count));return;}
    std::array<unsigned char,65536> scratch{};std::size_t written=0;auto remaining=source.bytes;
    const auto inputBytes=unsigned(source.format.wBitsPerSample/8),align=unsigned(source.format.nBlockAlign);
    const auto block=unsigned(scratch.size())/align*align;
    while(remaining){
        const auto bytes=(std::min)(remaining,block);Read(file,scratch.data(),bytes);remaining-=bytes;
        for(unsigned at=0;at<bytes;at+=inputBytes){
            std::int64_t sample=0;
            if(source.encoding==3){
                double value=0;
                if(inputBytes==4){float single;std::memcpy(&single,scratch.data()+at,4);value=single;}
                else std::memcpy(&value,scratch.data()+at,8);
                if(!std::isfinite(value))throw std::invalid_argument("non-finite WAVE sample");
                sample=value>=1?32767:value<=-1?-32768:std::int64_t(value*32768.0);
            }else{
                std::uint64_t raw=0;for(unsigned n=0;n<inputBytes;++n)raw|=std::uint64_t(scratch[at+n])<<(n*8);
                const auto bits=source.format.wBitsPerSample;const auto padding=unsigned(bits)-source.validBits;
                if(padding&&(raw&((std::uint64_t(1)<<padding)-1)))throw std::invalid_argument("nonzero WAVE sample padding");
                if(bits==8)sample=(std::int64_t(raw)-128)*256;
                else if(bits==16)sample=raw>=0x8000?std::int64_t(raw)-0x10000:std::int64_t(raw);
                else {const auto high=raw>>(bits-16);sample=high>=0x8000?std::int64_t(high)-0x10000:std::int64_t(high);}
            }
            Put16(pcm.data()+written,unsigned(std::uint16_t(sample)));written+=2;
        }
    }
}
void Dispose(WaveAudioContext& audio) noexcept {
    WaveCtxReset(&audio);
    std::free(audio.waveformMax); std::free(audio.waveformMin);
    audio.waveformMax = audio.waveformMin = nullptr;
    if (audio.directSound) { audio.directSound->Release(); audio.directSound = nullptr; }
}
}
struct Asset::Impl {
    WAVEFORMATEX format{};
    std::vector<unsigned char> pcm, maximum, minimum;
    std::wstring source, cache;
    HANDLE cacheLock = INVALID_HANDLE_VALUE;
    bool cacheOwned = false;
    ~Impl() {
        if (cacheLock != INVALID_HANDLE_VALUE) CloseHandle(cacheLock);
        if (cacheOwned) DeleteFileW(cache.c_str());
    }
    void Waveform(std::uint64_t budget) {
        const float average = float(format.nAvgBytesPerSec);
        const int align = format.nBlockAlign;
        const int columnBytes = int(average * (1.f/30.f) / 13.f / float(align)) * align;
        if (columnBytes <= 0) return;
        const auto columns = pcm.size() / unsigned(columnBytes) + 1;
        if (columns > INT_MAX) throw std::bad_alloc();
        Budget(Sum(Sum(pcm.size(), pcm.size()+44), Sum(columns, columns)), budget);
        maximum.resize(columns); minimum.resize(columns);
        std::uint64_t cursor = 0, consumed = 0;
        const bool wide = format.wBitsPerSample == 16;
        const unsigned silence = wide ? 0 : 128;
        for (std::size_t column = 0; column < columns; ++column) {
            const auto time = double((float(column) / 390.f) * average);
            if (time > INT_MAX) throw std::invalid_argument("PCM waveform position overflow");
            const auto position = std::uint64_t(time);
            if (position > consumed) { const auto skip = (position-consumed)/unsigned(align)*unsigned(align); cursor += skip; consumed += skip; }
            const auto available = cursor < pcm.size() ? (std::min)(std::uint64_t(columnBytes), std::uint64_t(pcm.size())-cursor) : 0;
            const auto byte = [&](unsigned offset) { return offset < available ? pcm[std::size_t(cursor)+offset] : static_cast<unsigned char>(silence); };
            int low = 0, high = 0;
            for (unsigned offset = 0; offset < unsigned(columnBytes); offset += unsigned(align)) {
                for (unsigned channel = 0; channel < format.nChannels; ++channel) {
                    const auto at = offset + channel * (wide ? 2u : 1u);
                    const int sample = wide ? int(std::int8_t(byte(at+1)))*256 + int(std::int8_t(byte(at))) : int(std::int8_t(byte(at)));
                    low = (std::min)(low, sample); high = (std::max)(high, sample);
                }
            }
            const auto shift = wide ? 15 : 7;
            maximum[column] = static_cast<unsigned char>((high*25 >> shift)+25);
            minimum[column] = static_cast<unsigned char>((low*25 >> shift)+25);
            consumed += unsigned(columnBytes); cursor += available;
            if (available < unsigned(columnBytes)) break;
        }
    }
    void Cache() {
        const auto directory = local_data::File(L"audio-cache");
        if (directory.empty()) throw std::invalid_argument("audio cache directory unavailable");
        std::filesystem::create_directories(directory);
        static std::atomic<std::uint64_t> next{1};
        File file;
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const auto nonce = next.fetch_add(1);
            if (!nonce || nonce == UINT64_MAX) throw std::bad_alloc();
            const auto candidate = directory / (std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(nonce)+L".wav");
            const auto path = candidate.wstring();
            if (path.size() >= 256) throw std::invalid_argument("audio cache path too long");
            cache = path;
            file.value = CreateFileW(cache.c_str(), GENERIC_WRITE|GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file.value != INVALID_HANDLE_VALUE) { cacheOwned = true; break; }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) throw std::invalid_argument("cannot create private PCM cache");
        }
        if (file.value == INVALID_HANDLE_VALUE) throw std::invalid_argument("private PCM cache name exhausted");
        std::array<unsigned char,44> header{};
        std::memcpy(header.data(), "RIFF", 4); Put32(header.data()+4, std::uint32_t(pcm.size())+36+unsigned(pcm.size()&1u));
        std::memcpy(header.data()+8, "WAVEfmt ", 8); Put32(header.data()+16, 16); Put16(header.data()+20, 1);
        Put16(header.data()+22, format.nChannels); Put32(header.data()+24, format.nSamplesPerSec); Put32(header.data()+28, format.nAvgBytesPerSec);
        Put16(header.data()+32, format.nBlockAlign); Put16(header.data()+34, format.wBitsPerSample);
        std::memcpy(header.data()+36, "data", 4); Put32(header.data()+40, std::uint32_t(pcm.size()));
        const auto write = [&](const void* bytes, DWORD length) { DWORD written=0;
            if (!WriteFile(file.value, bytes, length, &written, nullptr) || written!=length) throw std::invalid_argument("cannot write private PCM cache"); };
        write(header.data(), DWORD(header.size())); if (!pcm.empty()) write(pcm.data(), DWORD(pcm.size()));
        if(pcm.size()&1u){const unsigned char padding=0;write(&padding,1);}
        if (!FlushFileBuffers(file.value)) throw std::invalid_argument("cannot flush private PCM cache");
        CloseHandle(file.value);file.value=INVALID_HANDLE_VALUE;
        cacheLock = CreateFileW(cache.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (cacheLock == INVALID_HANDLE_VALUE) throw std::invalid_argument("cannot protect private PCM cache");
    }
};
Asset::Asset(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Asset::~Asset() = default;
const wchar_t* Asset::Source() const noexcept { return impl_->source.c_str(); }
const wchar_t* Asset::CachePath() const noexcept { return impl_->cache.c_str(); }
std::uint64_t Asset::RetainedBytes() const noexcept {
    std::uint64_t bytes = sizeof(*this)+sizeof(Impl)+4*sizeof(void*);
    for (const auto* vector : {&impl_->pcm, &impl_->maximum, &impl_->minimum}) bytes=Sum(bytes, vector->capacity());
    bytes=Sum(bytes, (impl_->source.capacity()+impl_->cache.capacity()+2)*sizeof(wchar_t));
    return Sum(bytes, impl_->pcm.size()+44+(impl_->pcm.size()&1u));
}
bool Asset::Equals(const Asset& other) const noexcept {
    const auto& a=*impl_; const auto& b=*other.impl_;
    return a.source==b.source && a.format.wFormatTag==b.format.wFormatTag && a.format.nChannels==b.format.nChannels &&
        a.format.nSamplesPerSec==b.format.nSamplesPerSec && a.format.nAvgBytesPerSec==b.format.nAvgBytesPerSec &&
        a.format.nBlockAlign==b.format.nBlockAlign && a.format.wBitsPerSample==b.format.wBitsPerSample &&
        a.pcm==b.pcm && a.maximum==b.maximum && a.minimum==b.minimum;
}
AssetPtr Asset::DecodeWave(const wchar_t* source,std::uint64_t maximumBytes,bool nativePassThrough) {
    if(!source||!*source||wcsnlen_s(source,256)>=256)throw std::invalid_argument("audio source path too long");
    File input(CreateFileW(source,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(input.value==INVALID_HANDLE_VALUE)throw std::invalid_argument("audio file unavailable");
    const auto wave=InspectWave(input.value);
    if(nativePassThrough&&wave.canonical){Format(wave.format);return {};}
    auto result=std::make_unique<Impl>();result->source=source;
    ReadPcm(input.value,wave,result->format,result->pcm,maximumBytes);
    result->Waveform(maximumBytes);result->Cache();
    auto asset=AssetPtr(new Asset(std::move(result)));Budget(asset->RetainedBytes(),maximumBytes);return asset;
}
AssetPtr Asset::Decode(const wchar_t* source, std::uint64_t maximumBytes) {
    if (!source || !*source || wcsnlen_s(source,256)>=256) throw std::invalid_argument("audio source path too long");
    const auto* dot=std::wcsrchr(source,L'.');if(dot&&_wcsicmp(dot,L".wav")==0)return DecodeWave(source,maximumBytes);
    WaveAudioContext decoding{};
    struct Cleanup { WaveAudioContext& audio; ~Cleanup() { Dispose(audio); } } cleanup{decoding};
    std::wstring playable,error;bool exceeded=false;
    if (!media::PrepareAudio(&decoding,source,playable,error,maximumBytes/2,&exceeded)) {
        if(exceeded)throw std::bad_alloc();throw std::invalid_argument("audio decode failed");
    }
    auto asset=DecodeWave(playable.c_str(),maximumBytes);
    // The source is metadata, never a dependency of history playback.
    asset->impl_->source=source;
    Budget(asset->RetainedBytes(),maximumBytes);return asset;
}
AssetPtr Asset::Capture(const WaveAudioContext& audio,const wchar_t* source,std::uint64_t maximumBytes) {
    if(auto asset=Active(&audio))return asset;
    if(!Stopped(audio)||!source||!*source||wcsnlen_s(source,256)>=256)throw std::invalid_argument("audio source is not stopped");
    if(audio.dataSize<0||audio.dataOffset<0||!audio.format.nBlockAlign||audio.dataSize%audio.format.nBlockAlign||audio.waveformColumns<0||
        (audio.waveformColumns&&(!audio.waveformMax||!audio.waveformMin)))throw std::invalid_argument("invalid live PCM audio");
    auto result=std::make_unique<Impl>();result->source=source;result->format=audio.format;result->format.cbSize=0;
    // Use an independent read handle. Capturing a stopped legacy clip must
    // preserve its read cursor even when later allocations or file IO fail.
    std::array<wchar_t,32768> finalPath{};const wchar_t* path=audio.path;
    if(audio.fileHandle&&audio.fileHandle!=INVALID_HANDLE_VALUE){
        const auto size=GetFinalPathNameByHandleW(audio.fileHandle,finalPath.data(),DWORD(finalPath.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        if(!size||size>=finalPath.size())throw std::invalid_argument("cannot identify live PCM backing");path=finalPath.data();
    }
    File opened(CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(opened.value==INVALID_HANDLE_VALUE||std::uint64_t(audio.dataOffset)+unsigned(audio.dataSize)>Length(opened.value))throw std::invalid_argument("live PCM backing is unavailable");
    if(audio.fileHandle&&audio.fileHandle!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION a{},b{};
        if(!GetFileInformationByHandle(audio.fileHandle,&a)||!GetFileInformationByHandle(opened.value,&b)||a.dwVolumeSerialNumber!=b.dwVolumeSerialNumber||
            a.nFileIndexHigh!=b.nFileIndexHigh||a.nFileIndexLow!=b.nFileIndexLow)throw std::invalid_argument("live PCM backing identity changed");}
    const auto wave=InspectWave(opened.value);
    if(wave.offset!=unsigned(audio.dataOffset)||wave.bytes!=unsigned(audio.dataSize)||
        wave.format.nChannels!=audio.format.nChannels||wave.format.nSamplesPerSec!=audio.format.nSamplesPerSec||
        wave.format.nBlockAlign!=audio.format.nBlockAlign||wave.format.nAvgBytesPerSec!=audio.format.nAvgBytesPerSec||
        wave.format.wBitsPerSample!=audio.format.wBitsPerSample)throw std::invalid_argument("live PCM format changed");
    const auto waveformBytes=std::uint64_t(audio.waveformColumns)*2;
    Budget(waveformBytes,maximumBytes);ReadPcm(opened.value,wave,result->format,result->pcm,maximumBytes-waveformBytes);
    if(audio.waveformColumns){result->maximum.assign(audio.waveformMax,audio.waveformMax+audio.waveformColumns);result->minimum.assign(audio.waveformMin,audio.waveformMin+audio.waveformColumns);}
    result->Cache();auto asset=AssetPtr(new Asset(std::move(result)));Budget(asset->RetainedBytes(),maximumBytes);return asset;
}
struct Prepared::Impl {
    WaveAudioContext& live;
    std::array<unsigned char,sizeof(WaveAudioContext)> expected{};
    WaveAudioContext staged{};
    AssetPtr before,target;
    Leases::node_type ready,retired;
    std::uint64_t bytes=0;
    bool published=false;
    explicit Impl(WaveAudioContext& context):live(context),before(Active(&context)) {
        if(!Stopped(context))throw std::invalid_argument("active audio feed");
        std::memcpy(expected.data(),&context,expected.size());
        staged.timelineDC=context.timelineDC;staged.mainWindow=context.mainWindow;staged.englishUI=context.englishUI;staged.volume=context.volume;
    }
    void Initialize(AssetPtr asset) {
        target=std::move(asset);
        Leases prepared;prepared.emplace(&live,Lease{target,true});ready=prepared.extract(prepared.begin());
        if(!target)return;
        const auto& value=*target->impl_;
        staged.format=value.format;staged.bufferBytes=int(std::uint64_t(staged.format.nAvgBytesPerSec)*2);staged.dataSize=int(value.pcm.size());staged.dataOffset=44;
        staged.waveformColumns=int(value.maximum.size());wcscpy_s(staged.path,value.cache.c_str());
        const auto allocate=[&](const auto& vector,unsigned char*& out){if(vector.empty())return;out=static_cast<unsigned char*>(std::malloc(vector.size()));if(!out)throw std::bad_alloc();std::memcpy(out,vector.data(),vector.size());};
        allocate(value.maximum,staged.waveformMax);allocate(value.minimum,staged.waveformMin);
        staged.fileHandle=CreateFileW(staged.path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(staged.fileHandle==INVALID_HANDLE_VALUE){staged.fileHandle=nullptr;throw std::invalid_argument("private PCM cache unavailable");}Seek(staged.fileHandle,44);
        staged.directSound=live.directSound;
        if(staged.directSound)staged.directSound->AddRef();
        else {if(FAILED(DirectSoundCreate(nullptr,&staged.directSound,nullptr))||!staged.directSound)throw std::invalid_argument("DirectSound device unavailable");
            if(FAILED(staged.directSound->SetCooperativeLevel(staged.mainWindow,DSSCL_PRIORITY)))throw std::invalid_argument("DirectSound cooperation failed");}
        DSBUFFERDESC descriptor{};descriptor.dwSize=sizeof descriptor;descriptor.dwFlags=0x81E0;descriptor.dwBufferBytes=unsigned(staged.bufferBytes);descriptor.lpwfxFormat=&staged.format;
        if(FAILED(staged.directSound->CreateSoundBuffer(&descriptor,&staged.streamingBuffer,nullptr))||!staged.streamingBuffer)throw std::invalid_argument("DirectSound buffer unavailable");
        if(FAILED(staged.streamingBuffer->SetVolume(staged.volume)))throw std::invalid_argument("DirectSound volume rejected");
        DSBCAPS caps{};caps.dwSize=sizeof caps;
        if(FAILED(staged.streamingBuffer->GetCaps(&caps))||caps.dwBufferBytes!=unsigned(staged.bufferBytes))throw std::invalid_argument("DirectSound buffer size unavailable");
        bytes=Sum(sizeof staged,Sum(std::uint64_t(staged.waveformColumns)*2,caps.dwBufferBytes));
    }
    ~Impl(){Dispose(staged);}
    bool Current()const noexcept{return !published&&Stopped(live)&&!std::memcmp(&live,expected.data(),expected.size())&&Active(&live)==before;}
};
Prepared::Prepared(WaveAudioContext& live,AssetPtr target) {
    auto value=std::make_unique<Impl>(live);value->Initialize(std::move(target));impl_=std::move(value);
}
Prepared::~Prepared()=default;
bool Prepared::IsCurrent()const noexcept{return impl_->Current();}
void Prepared::Publish()noexcept{
    auto& p=*impl_;if(!p.Current())std::terminate();
    KillTimer(p.live.mainWindow,100);
    media::SwapAudioCaches(&p.live,&p.staged);
    std::swap(p.live,p.staged);
    p.retired=ActiveFiles().extract(&p.live);if(p.ready.empty()||!ActiveFiles().insert(std::move(p.ready)).inserted)std::terminate();p.published=true;
}
std::uint64_t Prepared::ResourceBytes()const noexcept{return impl_->bytes;}
AssetPtr Active(const WaveAudioContext* context)noexcept{const auto at=ActiveFiles().find(context);return at==ActiveFiles().end()?AssetPtr{}:at->second.asset;}
const wchar_t* Source(const WaveAudioContext* context)noexcept{const auto at=ActiveFiles().find(context);return at==ActiveFiles().end()||!at->second.asset?nullptr:at->second.asset->Source();}
bool NeedsRefresh(const WaveAudioContext* context)noexcept{const auto at=ActiveFiles().find(context);return at!=ActiveFiles().end()&&at->second.dirty;}
void Refreshed(const WaveAudioContext* context)noexcept{const auto at=ActiveFiles().find(context);if(at!=ActiveFiles().end())at->second.dirty=false;}
void Forget(WaveAudioContext* context)noexcept{ActiveFiles().erase(context);}
bool Stopped(const WaveAudioContext& context)noexcept{return !context.feedThread||WaveStopState(const_cast<WaveAudioContext*>(&context))==2;}
}
#endif
