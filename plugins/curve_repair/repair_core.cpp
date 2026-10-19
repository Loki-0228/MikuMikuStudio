#include "repair_core.hpp"
#include "mikudancestudio/animation_curve.hpp"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace repair {
namespace ac = mikudancestudio::animation_curve;
namespace {
constexpr double pi = 3.14159265358979323846;
const char* names[] = {"x", "y", "z", "rx", "ry", "rz", "distance", "fov"};
constexpr std::size_t fieldOffsets[] = {16,20,24,28,32,36,12,73};
[[noreturn]] void Fail(const char* text) { throw std::runtime_error(text); }
struct Reader {
    const Bytes& bytes;
    std::size_t end, pos = 0;
    void skip(std::size_t n) { if(pos > end || n > end-pos) Fail("Truncated file or invalid count."); pos += n; }
    void records(std::size_t n, std::size_t stride) {
        if(!stride || pos > end || n > (end-pos)/stride) Fail("Invalid record count.");
        skip(n*stride);
    }
    std::uint8_t u8() { skip(1); return bytes[pos-1]; }
    std::uint32_t u32() { skip(4); std::uint32_t n; std::memcpy(&n, bytes.data()+pos-4, 4); return n; }
    std::uint64_t u64() { const auto low=u32(); return low | (std::uint64_t(u32())<<32); }
    void string8() { const auto n=u8(); skip(n); }
    void names32() { const auto n=u32(); if(n>end-pos) Fail("Invalid name count."); for(std::uint32_t i=0;i<n;++i) string8(); }
};
std::uint32_t At(const Bytes& bytes, std::size_t pos) { Reader r{bytes,bytes.size(),pos}; return r.u32(); }
void Put(Bytes& bytes, std::size_t pos, std::uint32_t n) {
    if(pos>bytes.size() || bytes.size()-pos<4) Fail("Invalid patch offset.");
    std::memcpy(bytes.data()+pos,&n,4);
}
std::uint32_t Crc(const Bytes& bytes, std::size_t first, std::size_t last) {
    std::uint32_t crc=0xffffffff;
    for(auto i=first;i<last;++i) {
        crc^=bytes[i];
        for(int b=0;b<8;++b) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));
    }
    return crc^0xffffffff;
}
std::vector<ac::NamedTrack> Curves(const Bytes& bytes, std::size_t& standard) {
    standard=bytes.size();
    std::vector<ac::NamedTrack> tracks;
    if(bytes.size()<32) return tracks;
    const auto* marker=bytes.data()+bytes.size()-32;
    // New writes use MMSXPMM2; files from before the rename keep MDSXPMM2.
    if(std::memcmp(marker,"MMSXPMM2",8) && std::memcmp(marker,"MDSXPMM2",8)) return tracks;
    Reader tail{bytes,bytes.size(),bytes.size()-24};
    const auto prefix=tail.u64(), size=tail.u64(); const auto crc=tail.u32(), reserved=tail.u32();
    if(reserved || prefix<55 || prefix>bytes.size()-32 || size!=bytes.size()-32-prefix)
        Fail("Invalid PMM extension size.");
    standard=static_cast<std::size_t>(prefix);
    if(Crc(bytes,standard,bytes.size()-32)!=crc) Fail("Invalid PMM extension checksum.");
    Reader r{bytes,bytes.size()-32,standard}; std::set<std::string> seen;
    while(r.pos<r.end) {
        const auto keySize=r.u32(), valueSize=r.u32();
        if(!keySize || keySize>255) Fail("Invalid PMM extension key.");
        const auto keyStart=r.pos; r.skip(keySize);
        std::string key(reinterpret_cast<const char*>(bytes.data()+keyStart),keySize);
        if(key.find('\0')!=std::string::npos || !seen.insert(key).second) Fail("Invalid PMM extension records.");
        const auto valueStart=r.pos; r.skip(valueSize);
        if(key=="org.mikudancestudio.animation_curves.v1") {
            std::string error;
            if(!ac::Decode(bytes.data()+valueStart,valueSize,tracks,error)) Fail("Invalid authored curve data.");
        }
    }
    return tracks;
}
unsigned Lane(unsigned c) { return c<3?c:c<6?3:c==6?4:5; }
bool Geometry(const ac::Key& a,const ac::Key& b,const std::array<double,4>& ctrl) {
    const double x1=std::clamp(ctrl[0]/127.0,0.0002,0.9996);
    const double x2=std::clamp(ctrl[2]/127.0,x1+0.0002,0.9998);
    const double rate=(b.value-a.value)/double(b.frame-a.frame);
    return b.interpolation==ac::Interpolation::Bezier && a.outWeight==x1 && b.inWeight==1-x2 &&
           a.outSlope==ctrl[1]/127.0*rate/x1 && b.inSlope==(1-ctrl[3]/127.0)*rate/(1-x2);
}
bool Finite(std::uint32_t raw) { float f; std::memcpy(&f,&raw,4); return std::isfinite(f); }
struct Handle {
    HANDLE h;
    explicit Handle(HANDLE value):h(value) { if(h==INVALID_HANDLE_VALUE) Fail("Cannot open file. Check the path, permissions, and whether the file is in use."); }
    ~Handle() { CloseHandle(h); }
    Handle(const Handle&)=delete;
};
Bytes ReadHandle(HANDLE h) {
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(h,&size)||size.QuadPart<0||size.QuadPart>kMaxFileBytes) Fail("File exceeds the 512 MiB limit or cannot be read.");
    Bytes bytes(static_cast<std::size_t>(size.QuadPart)); std::size_t pos=0;
    while(pos<bytes.size()) {
        DWORD got=0, want=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-pos,1048576));
        if(!::ReadFile(h,bytes.data()+pos,want,&got,nullptr)||!got) Fail("Cannot read the complete file.");
        pos+=got;
    }
    return bytes;
}
void NewFile(const std::filesystem::path& path,const Bytes& bytes,std::vector<std::filesystem::path>& created) {
    Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    created.push_back(path);
    std::size_t pos=0;
    while(pos<bytes.size()) {
        DWORD done=0, want=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-pos,1048576));
        if(!WriteFile(h.h,bytes.data()+pos,want,&done,nullptr)||!done) Fail("Cannot write the repair copy.");
        pos+=done;
    }
    if(!FlushFileBuffers(h.h)) Fail("Cannot flush the repair copy.");
}
}

std::vector<CameraKey> ReadCamera(const Bytes& bytes,std::size_t standard) {
    if(standard>bytes.size() || standard<55 || std::memcmp(bytes.data(),"Polygon Movie maker 0002",24))
        Fail("Only standard PMM 0002 projects are supported.");
    Reader r{bytes,standard,54}; const auto models=r.u8();
    std::set<unsigned> slots;
    for(unsigned m=0;m<models;++m) {
        const auto slot=r.u8(); if(slot==255 || !slots.insert(slot).second) Fail("Invalid model slot.");
        r.string8(); r.string8(); r.skip(257);
        auto p=r.pos; const auto bones=r.u32(); r.pos=p; r.names32();
        p=r.pos; const auto morphs=r.u32(); r.pos=p; r.names32();
        const auto ik=r.u32(); r.records(ik,4); const auto rigid=r.u32(); r.records(rigid,4);
        r.skip(22); const auto groups=r.u8(); r.skip(groups); r.skip(8);
        r.records(bones,58); auto count=r.u32(); r.records(count,62);
        r.records(morphs,17); count=r.u32(); r.records(count,21);
        const std::uint64_t display=14ull+ik+8ull*rigid;
        if(display>standard) Fail("Invalid display key size.");
        r.skip(static_cast<std::size_t>(display)); count=r.u32(); r.records(count,static_cast<std::size_t>(display)+4);
        r.records(bones,31); r.records(morphs,4); r.records(ik,1); r.records(rigid,16); r.skip(7);
    }
    std::map<std::uint32_t,CameraKey> pool;
    const auto readKey=[&](std::uint32_t index) {
        CameraKey k; k.index=index; k.offset=r.pos; r.skip(78);
        k.frame=At(bytes,k.offset); k.previous=At(bytes,k.offset+4); k.next=At(bytes,k.offset+8);
        if(k.frame>0x7fffffff) Fail("Invalid camera frame.");
        for(unsigned c=0;c<8;++c) {
            k.raw[c]=At(bytes,k.offset+fieldOffsets[c]);
            if((c<7&&!Finite(k.raw[c])) || (c==7&&(k.raw[c]<1||k.raw[c]>179))) Fail("Invalid camera value.");
        }
        std::memcpy(&k.parentModel,bytes.data()+k.offset+40,4); std::memcpy(&k.parentBone,bytes.data()+k.offset+44,4);
        for(unsigned lane=0;lane<6;++lane) for(unsigned j=0;j<4;++j) {
            const auto v=bytes[k.offset+48+lane*4+j]; if(v>127) Fail("Invalid camera Bezier control."); k.controls[lane][j]=v;
        }
        k.perspective=bytes[k.offset+72]; if(k.perspective>1) Fail("Invalid camera projection.");
        if(!pool.emplace(index,k).second) Fail("Duplicate camera index.");
    };
    readKey(0); const auto count=r.u32();
    if(count>(r.end-r.pos)/82) Fail("Invalid camera count.");
    for(std::uint32_t i=0;i<count;++i) {const auto index=r.u32(); readKey(index);}
    r.skip(37); // Current camera pose; subsequent project bytes stay opaque.
    std::vector<CameraKey> ordered; std::uint32_t current=0;
    do {
        const auto it=pool.find(current);
        if(it==pool.end() || ordered.size()>=pool.size()) Fail("Broken camera key links.");
        const auto& k=it->second;
        if(ordered.empty() ? (k.frame!=0||k.previous!=0) :
           (k.frame<=ordered.back().frame||k.previous!=ordered.back().index)) Fail("Unordered camera key links.");
        ordered.push_back(k); current=k.next;
    } while(current);
    if(ordered.size()!=pool.size()) Fail("Unreachable camera keys.");
    return ordered;
}

std::vector<CameraKey> ReadVmd(const Bytes& bytes) {
    if(bytes.size()<54||std::memcmp(bytes.data(),"Vocaloid Motion Data 0002",24)) Fail("Only VMD 0002 is supported.");
    Reader r{bytes,bytes.size(),50}; auto n=r.u32(); r.records(n,111); n=r.u32(); r.records(n,23); n=r.u32();
    if(n>(r.end-r.pos)/61) Fail("Truncated VMD camera data.");
    std::vector<CameraKey> keys;
    for(std::uint32_t i=0;i<n;++i) {
        CameraKey k; k.frame=r.u32(); k.raw[6]=r.u32();
        for(unsigned c=0;c<6;++c) k.raw[c]=r.u32();
        for(auto& ctrl:k.controls) {ctrl[0]=r.u8();ctrl[2]=r.u8();ctrl[1]=r.u8();ctrl[3]=r.u8(); for(auto v:ctrl) if(v>127) Fail("Invalid VMD camera Bezier control.");}
        k.raw[7]=r.u32(); k.perspective=r.u8();
        if(k.frame>0x7fffffff||k.raw[7]<1||k.raw[7]>179||k.perspective>1) Fail("Invalid VMD camera value.");
        for(unsigned c=0;c<7;++c) if(!Finite(k.raw[c])) Fail("Non-finite VMD value.");
        keys.push_back(k);
    }
    std::sort(keys.begin(),keys.end(),[](const auto&a,const auto&b){return a.frame<b.frame;});
    for(std::size_t i=1;i<keys.size();++i) if(keys[i].frame==keys[i-1].frame) Fail("Duplicate VMD camera frame.");
    return keys;
}

float LegacyEndpoint(const std::array<double,4>& control) {
    if(control[0]==control[1]&&control[2]==control[3]) return 0;
    const float x1=float(control[0])/127.0f,x2=float(control[2])/127.0f;
    float u=0.5f,half=0.25f;
    for(int i=0;i<12;++i) {
        const float v=1.0f-u,v3=v*3.0f;
        const float x=(v3*v*u)*x1+(v3*u*u)*x2+u*u*u;
        if(x==0) break;
        u=x>=0?u-half:u+half; half*=0.5f;
    }
    const float v=1.0f-u,v3=v*3.0f;
    return (v3*v*u)*float(control[1])/127.0f+(v3*u*u)*float(control[3])/127.0f+u*u*u;
}
double Value(std::uint32_t raw,unsigned c) {
    if(c==7) return raw;
    float f; std::memcpy(&f,&raw,4); return c>=3&&c<6?f*180.0/pi:f;
}
std::uint32_t Stored(double v,unsigned c) {
    if(!std::isfinite(v)||std::abs(v)>1e8 || (c==7&&(v<1||v>179))) Fail("Unrepresentable reference value.");
    if(c==7) return static_cast<std::uint32_t>(std::lround(v));
    const float f=c>=3&&c<6?float(v*pi/180):float(v); std::uint32_t raw; std::memcpy(&raw,&f,4); return raw;
}

Analysis Analyze(Bytes pmm,const Bytes& originalVmd) {
    if(pmm.size()>kMaxFileBytes||originalVmd.size()>kMaxFileBytes) Fail("File exceeds the 512 MiB limit.");
    std::size_t standard=0; const auto tracks=Curves(pmm,standard); const auto keys=ReadCamera(pmm,standard);
    Analysis result; result.cameraKeys=keys.size();
    std::array<const ac::Track*,8> authored{};
    for(unsigned c=0;c<8;++c) for(const auto& t:tracks) if(t.id==std::string("0/0/")+names[c]) {
        if(authored[c]) Fail("Duplicate authored camera channel."); authored[c]=&t.track; result.hasAuthoredCamera=true;
    }
    if(result.hasAuthoredCamera) {
        for(const auto* t:authored) if(!t||t->keys.empty()||t->keys.front().frame!=0||t->discrete)
            Fail("Incomplete authored camera data.");
        for(const char* id:{"perspective","parentModel","parentBone"}) {
            const auto name=std::string("0/0/")+id;
            const auto count=std::count_if(tracks.begin(),tracks.end(),[&](const auto&t){return t.id==name;});
            const auto it=std::find_if(tracks.begin(),tracks.end(),[&](const auto&t){return t.id==name;});
            if(count!=1||!it->track.discrete||it->track.keys.empty()||it->track.keys.front().frame!=0)
                Fail("Incomplete authored camera data.");
        }
    }
    std::vector<CameraKey> reference;
    if(!result.hasAuthoredCamera && !originalVmd.empty()) {
        reference=ReadVmd(originalVmd);
        if(reference.size()!=keys.size()) Fail("Reference VMD has a different camera key sequence.");
        for(std::size_t i=0;i<keys.size();++i)
            if(keys[i].frame!=reference[i].frame||keys[i].controls!=reference[i].controls||keys[i].parentModel!=-1||
               keys[i].perspective!=reference[i].perspective) Fail("Reference VMD does not match this project's camera sequence.");
        result.usedVmd=true;
    }
    for(unsigned c=0;c<8;++c) {
        const auto* track=authored[c];
        if((result.hasAuthoredCamera && (!track||track->discrete||track->keys.size()!=keys.size())) ||
           (!result.hasAuthoredCamera && reference.empty())) {++result.skippedChannels;continue;}
        bool valid=true; std::vector<Patch> proposed;
        for(std::size_t i=0;i<keys.size();++i) {
            if(track && (track->keys[i].frame!=static_cast<int>(keys[i].frame)||std::abs(track->keys[i].value)>1e8)) {valid=false;break;}
            const double original=track?track->keys[i].value:Value(reference[i].raw[c],c);
            const auto correct=track?Stored(original,c):reference[i].raw[c];
            if(keys[i].raw[c]==correct) continue;
            if(i==0||i+1==keys.size()||keys[i+1].frame-keys[i].frame==1) {valid=false;break;}
            const auto& ctrl=keys[i+1].controls[Lane(c)];
            if(track&&!Geometry(track->keys[i],track->keys[i+1],ctrl)) {valid=false;break;}
            const auto next=track?track->keys[i+1].value:Value(reference[i+1].raw[c],c);
            const auto legacy=Stored(original+(next-original)*LegacyEndpoint(ctrl),c);
            if(legacy!=keys[i].raw[c]||legacy==correct) {valid=false;break;}
            proposed.push_back({keys[i].offset+fieldOffsets[c],legacy,correct,keys[i].frame,c,
                track?Evidence::AuthoredCurve:Evidence::OriginalVmd});
        }
        if(valid) result.patches.insert(result.patches.end(),proposed.begin(),proposed.end());
        else ++result.skippedChannels;
    }
    result.original=std::move(pmm); return result;
}
Bytes Apply(const Analysis& a) {
    Bytes copy=a.original; std::set<std::size_t> offsets;
    for(const auto& p:a.patches) {
        if(!offsets.insert(p.offset).second || At(copy,p.offset)!=p.before) Fail("The repair plan no longer matches its source.");
        Put(copy,p.offset,p.after);
    }
    return copy;
}
Bytes ReadFile(const std::filesystem::path& path) {
    Handle h(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    return ReadHandle(h.h);
}
std::filesystem::path SaveCopy(const Analysis& a,const std::filesystem::path& source,const std::wstring& report) {
    if(a.patches.empty()) Fail("No verified repairs are available.");
    Handle input(CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(ReadHandle(input.h)!=a.original) Fail("The source project changed. Scan it again before saving.");
    const auto copy=Apply(a);
    SYSTEMTIME time{}; GetLocalTime(&time); wchar_t suffix[80];
    swprintf_s(suffix,L"_repaired_%04u%02u%02u_%02u%02u%02u_%03u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);
    auto output=source.parent_path()/(source.stem().wstring()+suffix+L".pmm");
    std::vector<std::filesystem::path> created;
    try {
        NewFile(output,copy,created);
        auto from=source; from.replace_extension(L".emm"); auto to=output; to.replace_extension(L".emm");
        if(std::filesystem::exists(from)) NewFile(to,ReadFile(from),created);
        from=source.wstring()+L".mms-plugins.json"; to=output.wstring()+L".mms-plugins.json";
        if(std::filesystem::exists(from)) NewFile(to,ReadFile(from),created);
        const int size=WideCharToMultiByte(CP_UTF8,0,report.c_str(),static_cast<int>(report.size()),nullptr,0,nullptr,nullptr);
        if(size<=0) Fail("Cannot encode the repair report.");
        Bytes utf8(static_cast<std::size_t>(size)+3); utf8[0]=0xef;utf8[1]=0xbb;utf8[2]=0xbf;
        WideCharToMultiByte(CP_UTF8,0,report.c_str(),static_cast<int>(report.size()),reinterpret_cast<char*>(utf8.data()+3),size,nullptr,nullptr);
        NewFile(output.wstring()+L".repair.txt",utf8,created);
        if(ReadFile(output)!=copy) Fail("Repair copy verification failed.");
    } catch(...) { for(auto it=created.rbegin();it!=created.rend();++it) DeleteFileW(it->c_str()); throw; }
    return output;
}
std::wstring Report(const Analysis& a,unsigned language,bool detailed) {
    const auto tr=[&](const wchar_t* zh,const wchar_t* en,const wchar_t* ja){return language==0?ja:language==1?en:zh;};
    std::wostringstream out; out.imbue(std::locale::classic()); out<<std::setprecision(12);
    out<<tr(L"镜头关键帧：",L"Camera keys: ",L"カメラキー: ")<<a.cameraKeys<<L"\r\n"
       <<tr(L"可恢复数值：",L"Recoverable values: ",L"復元可能な値: ")<<a.patches.size()<<L"\r\n"
       <<tr(L"依据不足而跳过的通道：",L"Channels skipped for lack of evidence: ",L"根拠不足でスキップしたチャンネル: ")<<a.skippedChannels<<L"\r\n\r\n";
    if(!a.hasAuthoredCamera&&!a.usedVmd) out<<tr(L"工程未保留可用的原始镜头曲线。可选择原始镜头 VMD 后重新扫描。\r\n",L"No original camera curves found. Select the original camera VMD and scan again.\r\n",L"元のカメラ曲線がありません。元のカメラ VMD を選択して再スキャンしてください。\r\n");
    for(const auto& p:a.patches) {
        const std::string name=names[p.channel];
        out<<tr(L"帧 ",L"Frame ",L"フレーム ")<<p.frame<<L"  "<<std::wstring(name.begin(),name.end())<<L"  "<<Value(p.before,p.channel)<<L" -> "<<Value(p.after,p.channel)
           <<(p.evidence==Evidence::AuthoredCurve?tr(L"  [工程原始曲线]",L"  [authored curve]",L"  [元の曲線]"):tr(L"  [原始 VMD]",L"  [original VMD]",L"  [元の VMD]"));
        if(detailed) out<<L"  (0x"<<std::hex<<p.before<<L" -> 0x"<<p.after<<L", +0x"<<p.offset<<std::dec<<L")";
        out<<L"\r\n";
    }
    out<<L"\r\n"<<tr(L"仅修复与旧版端点计算结果逐位匹配的数值。旋转以角度显示。未列出的内容保持原样；未检出不代表工程没有其他问题。\r\n副本保存在原目录，保留原文件，并复制同名 EMM 和已有插件数据文件。用 MMS 打开修复副本查看效果。",
        L"Only values matching the old endpoint calculation bit for bit are repaired. Rotations are shown in degrees. Unlisted content is unchanged; no matches does not rule out other issues.\r\nThe copy stays beside the original, with existing EMM and plugin sidecars copied. Open the repaired copy in MMS.",
        L"旧版の端点計算結果とビット単位で一致する値のみ復元します。回転は度で表示します。未記載の内容は保持します。検出なしでも他の問題は否定できません。\r\n同じフォルダーにコピーを保存し、元のファイルと EMM・プラグインデータを保持します。修復コピーを MMS で開いてください。");
    return out.str();
}
}
