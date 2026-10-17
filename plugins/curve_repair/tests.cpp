#include "repair_core.hpp"
#include "mikudancestudio/animation_curve.hpp"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <cwchar>
#include <new>

// Only the test thread injects C++ allocation failure. The failure snapshot
// uses Win32 enumeration exclusively, so observing partially-created outputs
// cannot consume another failing C++ allocation.
namespace repair_allocation_fault {
thread_local long remaining=-1;
thread_local const wchar_t* outputPattern=nullptr;
struct Created { bool pmm=false,emm=false,plugin=false,report=false; };
thread_local Created atFailure;
bool EndsWith(const wchar_t* text,const wchar_t* suffix) noexcept {
    const auto n=std::wcslen(text),m=std::wcslen(suffix);
    return n>=m&&!std::wmemcmp(text+n-m,suffix,m);
}
void SnapshotFailure() noexcept {
    atFailure={};if(!outputPattern)return;
    WIN32_FIND_DATAW entry{};const auto handle=FindFirstFileW(outputPattern,&entry);
    if(handle==INVALID_HANDLE_VALUE)return;
    do {if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
        atFailure.pmm|=EndsWith(entry.cFileName,L".pmm");
        atFailure.emm|=EndsWith(entry.cFileName,L".emm");
        atFailure.plugin|=EndsWith(entry.cFileName,L".mms-plugins.json");
        atFailure.report|=EndsWith(entry.cFileName,L".repair.txt");
    }while(FindNextFileW(handle,&entry));
    FindClose(handle);
}
}
void* operator new(std::size_t bytes){
    if(repair_allocation_fault::remaining==0){repair_allocation_fault::SnapshotFailure();throw std::bad_alloc();}
    if(repair_allocation_fault::remaining>0)--repair_allocation_fault::remaining;
    if(auto* result=std::malloc(bytes?bytes:1))return result;throw std::bad_alloc();
}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* pointer)noexcept{std::free(pointer);}
void operator delete[](void* pointer)noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t)noexcept{std::free(pointer);}
void operator delete[](void* pointer,std::size_t)noexcept{std::free(pointer);}

using namespace repair;
namespace ac=mikudancestudio::animation_curve;
namespace {
int checks=0;
void Check(bool ok,const char* text) {++checks;if(!ok)throw std::runtime_error(text);}
template<class Fn> void Reject(Fn fn,const char* text) {bool rejected=false;try{fn();}catch(...){rejected=true;}Check(rejected,text);}
void U32(Bytes& b,std::uint32_t n){for(int i=0;i<4;++i)b.push_back(static_cast<std::uint8_t>(n>>(i*8)));}
void U64(Bytes& b,std::uint64_t n){U32(b,static_cast<std::uint32_t>(n));U32(b,static_cast<std::uint32_t>(n>>32));}
void Set(Bytes& b,std::size_t pos,std::uint32_t n){std::memcpy(b.data()+pos,&n,4);}
std::uint32_t Crc(const Bytes& bytes){std::uint32_t c=~0u;for(auto v:bytes){c^=v;for(int i=0;i<8;++i)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return c^~0u;}
void Record(Bytes& b,const std::string& key,const Bytes& value){U32(b,static_cast<std::uint32_t>(key.size()));U32(b,static_cast<std::uint32_t>(value.size()));b.insert(b.end(),key.begin(),key.end());b.insert(b.end(),value.begin(),value.end());}
void Extension(Bytes& b,const std::vector<ac::NamedTrack>& tracks){
    Bytes values;std::string error;Check(ac::Encode(tracks,values,error),"encode curves");Bytes records;
    Record(records,"org.mikudancestudio.animation_curves.v1",values);Record(records,"org.example.unknown",{1,2,0,255,17});
    const auto standard=b.size();b.insert(b.end(),records.begin(),records.end());
    const char magic[]="MDSXPMM2";b.insert(b.end(),magic,magic+8);U64(b,standard);U64(b,records.size());U32(b,Crc(records));U32(b,0);
}
std::vector<CameraKey> Keys(){
    std::vector<CameraKey> keys(3);const std::uint32_t frames[]={0,44,90};
    for(unsigned i=0;i<3;++i){auto& k=keys[i];k.index=i;k.frame=frames[i];k.previous=i?i-1:0;k.next=i<2?i+1:0;
        for(unsigned c=0;c<8;++c)k.raw[c]=Stored(c==7?45:c==6?-30:c==0?(i==0?3:i==1?0:-1.25):0,c);
        for(auto& ctrl:k.controls)ctrl={20,0,107,127};}
    return keys;
}
std::vector<ac::NamedTrack> Tracks(const std::vector<CameraKey>& keys){
    const char* ids[]={"x","y","z","rx","ry","rz","distance","fov","perspective","parentModel","parentBone"};
    std::vector<ac::NamedTrack> tracks;
    for(unsigned c=0;c<11;++c){ac::NamedTrack t;t.id=std::string("0/0/")+ids[c];t.track.discrete=c>=8;
        for(const auto& k:keys){ac::Key value;value.frame=static_cast<int>(k.frame);value.value=c<8?Value(k.raw[c],c):c==8?k.perspective:c==9?k.parentModel:k.parentBone;
            value.tangent=ac::Tangent::Free;value.interpolation=c<8?ac::Interpolation::Bezier:ac::Interpolation::Step;t.track.keys.push_back(value);}
        if(c<8)for(std::size_t i=1;i<keys.size();++i){auto&a=t.track.keys[i-1];auto&b=t.track.keys[i];const auto&ctrl=keys[i].controls[c<3?c:c<6?3:c==6?4:5];
            const double x1=std::clamp(ctrl[0]/127.0,0.0002,0.9996),x2=std::clamp(ctrl[2]/127.0,x1+0.0002,0.9998),rate=(b.value-a.value)/double(b.frame-a.frame);
            a.outWeight=x1;b.inWeight=1-x2;a.outSlope=ctrl[1]/127.0*rate/x1;b.inSlope=(1-ctrl[3]/127.0)*rate/(1-x2);}
        tracks.push_back(t);}
    return tracks;
}
void CameraRecord(Bytes& b,const CameraKey& k){U32(b,k.frame);U32(b,k.previous);U32(b,k.next);U32(b,k.raw[6]);for(unsigned c=0;c<6;++c)U32(b,k.raw[c]);
    U32(b,static_cast<std::uint32_t>(k.parentModel));U32(b,static_cast<std::uint32_t>(k.parentBone));for(const auto& ctrl:k.controls)for(auto v:ctrl)b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(k.perspective);U32(b,k.raw[7]);b.push_back(0);}
Bytes CameraSection(const std::vector<CameraKey>& keys){Bytes b;CameraRecord(b,keys[0]);U32(b,static_cast<std::uint32_t>(keys.size()-1));
    for(std::size_t i=1;i<keys.size();++i){U32(b,keys[i].index);CameraRecord(b,keys[i]);}return b;}
Bytes Pmm(const std::vector<CameraKey>& keys,bool model=false){
    Bytes b(55,0);std::memcpy(b.data(),"Polygon Movie maker 0002",24);
    if(model){b[54]=1;b.push_back(0);b.push_back(3);b.insert(b.end(),{'a','b','c'});b.push_back(0);b.resize(b.size()+257);
        U32(b,1);b.push_back(1);b.push_back('b');U32(b,1);b.push_back(1);b.push_back('m');U32(b,1);U32(b,0);U32(b,1);U32(b,0);
        b.resize(b.size()+22);b.push_back(1);b.push_back(0);b.resize(b.size()+8);
        b.resize(b.size()+58);U32(b,1);b.resize(b.size()+62);b.resize(b.size()+17);U32(b,1);b.resize(b.size()+21);
        b.resize(b.size()+23);U32(b,1);b.resize(b.size()+27);b.resize(b.size()+31+4+1+16+7);}
    const auto camera=CameraSection(keys);b.insert(b.end(),camera.begin(),camera.end());b.resize(b.size()+37+100,0x3c);return b;
}
Bytes Vmd(const std::vector<CameraKey>& keys){Bytes b(50);std::memcpy(b.data(),"Vocaloid Motion Data 0002",24);U32(b,0);U32(b,0);U32(b,static_cast<std::uint32_t>(keys.size()));
    for(const auto& k:keys){U32(b,k.frame);U32(b,k.raw[6]);for(unsigned c=0;c<6;++c)U32(b,k.raw[c]);for(const auto&ctrl:k.controls){b.push_back(static_cast<std::uint8_t>(ctrl[0]));b.push_back(static_cast<std::uint8_t>(ctrl[2]));b.push_back(static_cast<std::uint8_t>(ctrl[1]));b.push_back(static_cast<std::uint8_t>(ctrl[3]));}U32(b,k.raw[7]);b.push_back(k.perspective);}U32(b,0);U32(b,0);U32(b,0);return b;}
std::vector<CameraKey> Damage(std::vector<CameraKey> keys){const auto original=keys;
    for(unsigned c=0;c<8;++c)for(std::size_t i=1;i+1<keys.size();++i)if(keys[i+1].frame-keys[i].frame>1){const auto lane=c<3?c:c<6?3:c==6?4:5;
        const double a=Value(original[i].raw[c],c),b=Value(original[i+1].raw[c],c);keys[i].raw[c]=Stored(a+(b-a)*LegacyEndpoint(keys[i+1].controls[lane]),c);}return keys;}
void Write(const std::filesystem::path& path,const Bytes& bytes){std::ofstream f(path,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));if(!f)throw std::runtime_error("write fixture");}
void SaveCopyAllocationFailures(const Analysis& analysis,const Bytes& expected){
    namespace fs=std::filesystem;
    struct OwnedFolder {
        fs::path path;
        OwnedFolder(){
            wchar_t parent[MAX_PATH]{},unique[MAX_PATH]{};
            Check(GetTempPathW(MAX_PATH,parent)&&GetTempFileNameW(parent,L"mcr",0,unique),"reserve a unique repair allocation test directory");
            path=unique;
            Check(DeleteFileW(unique)&&CreateDirectoryW(unique,nullptr),"create only the newly reserved repair test directory");
        }
        ~OwnedFolder(){std::error_code ignored;fs::remove_all(path,ignored);}
    } owned;
    const Bytes emm{1,9,3,0,255},plugin{'{','"','t','e','s','t','"',':','1','}'},notes{7,8,9};
    const std::wstring report=L"SaveCopy allocation failure probe";
    Bytes expectedReport{0xef,0xbb,0xbf};for(const auto ch:report)expectedReport.push_back(static_cast<std::uint8_t>(ch));
    unsigned failures=0;bool complete=false;repair_allocation_fault::Created observed;
    for(long allowance=0;allowance<512;++allowance){
        const auto folder=owned.path/(L"attempt-"+std::to_wstring(allowance));fs::create_directory(folder);
        const auto input=folder/L"source-original.pmm",emmInput=folder/L"source-original.emm";
        const fs::path pluginInput(input.wstring()+L".mms-plugins.json"),noteInput=folder/L"unrelated.txt";
        Write(input,analysis.original);Write(emmInput,emm);Write(pluginInput,plugin);Write(noteInput,notes);
        const auto pattern=(folder/L"source-original_repaired_*").wstring();fs::path output;bool memory=false;
        repair_allocation_fault::atFailure={};repair_allocation_fault::outputPattern=pattern.c_str();repair_allocation_fault::remaining=allowance;
        try{output=SaveCopy(analysis,input,report);}
        catch(const std::bad_alloc&){memory=true;}
        catch(...){repair_allocation_fault::remaining=-1;repair_allocation_fault::outputPattern=nullptr;throw;}
        repair_allocation_fault::remaining=-1;repair_allocation_fault::outputPattern=nullptr;
        const auto seen=repair_allocation_fault::atFailure;
        Check(ReadFile(input)==analysis.original&&ReadFile(emmInput)==emm&&ReadFile(pluginInput)==plugin&&ReadFile(noteInput)==notes,
            "SaveCopy allocation failures preserve the original PMM both source sidecars and unrelated files byte for byte");
        unsigned files=0;bool onlyInputs=true;
        for(const auto& entry:fs::directory_iterator(folder)){
            ++files;const auto& path=entry.path();
            if(path!=input&&path!=emmInput&&path!=pluginInput&&path!=noteInput)onlyInputs=false;
        }
        if(memory){
            ++failures;observed.pmm|=seen.pmm;observed.emm|=seen.emm;observed.plugin|=seen.plugin;observed.report|=seen.report;
            Check(files==4&&onlyInputs,"failed SaveCopy removes every newly created output including empty PMM partial sidecars and report");
            Check(output.empty(),"failed SaveCopy never returns a partial output path");
        }else{
            Check(!output.empty()&&files==8&&ReadFile(output)==expected,"successful SaveCopy creates exactly one repaired PMM alongside the preserved inputs");
            auto savedEmm=output;savedEmm.replace_extension(L".emm");
            Check(ReadFile(savedEmm)==emm&&ReadFile(output.wstring()+L".mms-plugins.json")==plugin&&ReadFile(output.wstring()+L".repair.txt")==expectedReport,
                "successful allocation sweep retains complete EMM plugin sidecar and UTF8 report copies");
            complete=true;break;
        }
    }
    Check(complete&&failures>10,"SaveCopy exhaustive allocation sweep reaches successful completion after exercising failure admission");
    Check(observed.pmm&&observed.emm&&observed.plugin&&observed.report,
        "allocation faults were observed after creation of PMM EMM plugin sidecar and report before complete cleanup");
}
void Tests(){
    const auto keys=Keys(),damaged=Damage(keys);const auto tracks=Tracks(keys);auto pmm=Pmm(damaged);Extension(pmm,tracks);
    const auto a=Analyze(pmm);Check(a.patches.size()==1,"exact endpoint recovered");Check(a.patches[0].frame==44&&a.patches[0].after==Stored(0,0),"frame 44 zero restored");
    Check(a.patches[0].before==Stored(-5.587480700341985e-8,0),"independent frame 44 regression value");
    const auto fixed=Apply(a);Check(Analyze(fixed).patches.empty(),"repair is idempotent");
    SaveCopyAllocationFailures(a,fixed);
    for(std::size_t i=0;i<pmm.size();++i)if(i<a.patches[0].offset||i>=a.patches[0].offset+4)Check(pmm[i]==fixed[i],"unrelated byte preserved");
    auto clean=Pmm(keys);Extension(clean,tracks);Check(Analyze(clean).patches.empty(),"clean project unchanged");
    auto model=Pmm(damaged,true);Extension(model,tracks);Check(Analyze(model).patches.size()==1,"model blocks skipped structurally");
    auto small=keys;small[1].raw[0]=Stored(-5.6e-8,0);auto intentional=Pmm(small);Extension(intentional,Tracks(small));Check(Analyze(intentional).patches.empty(),"intentional tiny value preserved");
    auto tinyDamage=Pmm(Damage(small));Extension(tinyDamage,Tracks(small));Check(Analyze(tinyDamage).patches[0].after==small[1].raw[0],"restore intentional tiny original, not zero");
    auto generic=tracks;generic[0].track.keys[1].outSlope=0.5;auto different=Pmm(damaged);Extension(different,generic);Check(Analyze(different).patches.empty(),"unrelated curve geometry skipped");
    auto altered=damaged;altered[0].raw[0]=Stored(123,0);auto mismatch=Pmm(altered);Extension(mismatch,tracks);Check(Analyze(mismatch).patches.empty(),"unrelated edits disqualify channel");
    auto noEvidence=Pmm(damaged);Check(Analyze(noEvidence).patches.empty(),"no original, no guessed repair");
    Check(Analyze(noEvidence,Vmd(keys)).patches.size()==1,"original VMD recovery");
    Check(Analyze(noEvidence,Vmd(damaged)).patches.empty(),"already damaged VMD cannot supply original values");
    auto bothLost=Pmm(damaged);Extension(bothLost,Tracks(damaged));Check(Analyze(bothLost).patches.empty(),"already damaged authored values are not guessed");
    auto other=keys;other[1].frame=43;Reject([&]{Analyze(noEvidence,Vmd(other));},"wrong VMD frames rejected");
    other=keys;other[2].controls[0][1]=1;Reject([&]{Analyze(noEvidence,Vmd(other));},"wrong VMD controls rejected");
    Check(Analyze(pmm,Vmd(other)).patches.size()==1,"authored curves take priority");
    auto crc=pmm;crc[crc.size()-8]^=1;Reject([&]{Analyze(crc);},"bad extension CRC rejected");
    auto duplicate=keys;duplicate[2].index=1;Reject([&]{Analyze(Pmm(duplicate));},"duplicate key index rejected");
    auto broken=keys;broken[1].next=1;Reject([&]{Analyze(Pmm(broken));},"cycle rejected");
    auto invalid=keys;invalid[1].controls[0][0]=128;Reject([&]{Analyze(Pmm(invalid));},"invalid control rejected");
    invalid=keys;invalid[1].raw[0]=0x7fc00000;Reject([&]{Analyze(Pmm(invalid));},"NaN rejected");
    auto counts=Pmm(keys);Set(counts,55+78,0xffffffff);Reject([&]{Analyze(counts);},"hostile camera count rejected");
    auto version=Pmm(keys);version[22]='3';Reject([&]{Analyze(version);},"unsupported PMM dialect rejected");
    const auto full=Pmm(keys);const auto cameraEnd=55+78+4+2*82+37;
    for(std::size_t n=0;n<cameraEnd;++n)Reject([&]{Analyze(Bytes(full.begin(),full.begin()+n));},"truncated camera rejected");
    auto adjacent=keys;adjacent[2].frame=45;Check(Analyze(Pmm(adjacent),Vmd(adjacent)).patches.empty(),"adjacent keys unchanged");
    auto linear=keys;for(auto& k:linear)for(auto& c:k.controls)c={20,20,107,107};Check(Analyze(Pmm(Damage(linear)),Vmd(linear)).patches.empty(),"linear curves unchanged");
    auto rotate=keys;rotate[2].raw[3]=Stored(-4,3);auto rotatePmm=Pmm(Damage(rotate));Extension(rotatePmm,Tracks(rotate));Check(Analyze(rotatePmm).patches.size()==2,"rotation conversion repaired");
    auto crossed=keys;crossed[0].raw[0]=0;crossed[2].frame=61;crossed[2].raw[0]=Stored(-1.2559141f,0);
    for(auto& ctrl:crossed[2].controls)ctrl={57,0,44,127};
    auto crossedPmm=Pmm(Damage(crossed));Extension(crossedPmm,Tracks(crossed));
    Check(Analyze(crossedPmm).patches.size()==1,"actual crossed-X frame 44 regression recovered");
    Check(Analyze(crossedPmm).patches[0].after==0,"actual crossed-X original zero restored");
    auto incomplete=tracks;incomplete.pop_back();auto partial=Pmm(damaged);Extension(partial,incomplete);Reject([&]{Analyze(partial);},"incomplete authoring group rejected");
    for(unsigned i=1;i<=80;++i){auto sample=keys;sample[1].raw[0]=Stored((int(i%3)-1)*1e-9,0);sample[2].raw[0]=Stored(-double(i)/3,0);
        sample[2].controls[0]={double(i%128),double(i%17),double((i*7)%128),127};
        auto brokenPmm=Pmm(Damage(sample));Extension(brokenPmm,Tracks(sample));const auto scan=Analyze(brokenPmm);
        Check(scan.patches.size()==1,"different endpoint and control combinations recovered");
        Check(scan.patches[0].after==sample[1].raw[0],"different original tiny values restored exactly");
        Check(Analyze(Apply(scan)).patches.empty(),"different combinations idempotent");}
    const auto folder=std::filesystem::temp_directory_path()/(L"MMS曲线修复_日本語_"+std::to_wstring(GetCurrentProcessId()));std::filesystem::create_directories(folder);
    const auto path=folder/L"原工程.pmm";Write(path,pmm);Write(folder/L"原工程.emm",{1,2,3});Write(path.wstring()+L".mms-plugins.json",{'{','}'});
    const auto output=SaveCopy(a,path,Report(a,2));Check(ReadFile(path)==pmm,"source preserved");Check(ReadFile(output)==fixed,"copy saved");
    auto emm=output;emm.replace_extension(L".emm");Check(ReadFile(emm)==Bytes({1,2,3}),"EMM copied");
    Check(ReadFile(output.wstring()+L".mms-plugins.json")==Bytes({'{','}'}),"plugin data copied");Check(!ReadFile(output.wstring()+L".repair.txt").empty(),"report saved");
    Write(path,clean);Reject([&]{SaveCopy(a,path,L"test");},"changed source rejected");Check(ReadFile(path)==clean,"changed source preserved");
    std::filesystem::remove_all(folder);
}
void Make(const std::filesystem::path& base,const std::filesystem::path& folder){
    auto bytes=ReadFile(base);const auto old=ReadCamera(bytes,bytes.size());
    const auto offset=old.front().offset;std::size_t end=offset+78+4;for(std::size_t i=1;i<old.size();++i)end=std::max(end,old[i].offset+78);
    std::size_t standard=bytes.size();if(bytes.size()>=32&&std::memcmp(bytes.data()+bytes.size()-32,"MDSXPMM2",8)==0){std::uint64_t n;std::memcpy(&n,bytes.data()+bytes.size()-24,8);standard=static_cast<std::size_t>(n);}
    const auto keys=Keys();auto result=Bytes(bytes.begin(),bytes.begin()+offset);const auto camera=CameraSection(Damage(keys));result.insert(result.end(),camera.begin(),camera.end());result.insert(result.end(),bytes.begin()+end,bytes.begin()+standard);Extension(result,Tracks(keys));
    std::filesystem::create_directories(folder);Write(folder/L"旧误差_日本語.pmm",result);Write(folder/L"原始镜头.vmd",Vmd(keys));Write(folder/L"旧误差_日本語.emm",{'t','e','s','t'});
    Check(Analyze(result).patches.size()==1,"real PMM fixture generated");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==4&&std::wstring(argv[1])==L"--make")Make(argv[2],argv[3]);
    else if(argc>=3&&std::wstring(argv[1])==L"--inspect"){
        const auto result=Analyze(ReadFile(argv[2]),argc>3?ReadFile(argv[3]):Bytes{});std::cout<<"keys="<<result.cameraKeys<<" repairs="<<result.patches.size()<<" skipped="<<result.skippedChannels<<"\n";
    }else if(argc==3&&std::wstring(argv[1])==L"--vmd"){
        auto keys=ReadVmd(ReadFile(argv[2]));Check(!keys.empty(),"real VMD parsed");
        for(std::size_t i=0;i<keys.size();++i){keys[i].index=static_cast<std::uint32_t>(i);keys[i].previous=i?static_cast<std::uint32_t>(i-1):0;keys[i].next=i+1<keys.size()?static_cast<std::uint32_t>(i+1):0;}
        const auto scan=Analyze(Pmm(Damage(keys)),Vmd(keys));Check(scan.usedVmd,"real VMD reference accepted");
        Check(!scan.patches.empty(),"old error reproduced with supplied VMD");Check(Analyze(Apply(scan),Vmd(keys)).patches.empty(),"supplied VMD repair idempotent");
        std::cout<<"VMD keys="<<keys.size()<<" reproduced_repairs="<<scan.patches.size()<<"\n";
    }else Tests();
    std::cout<<"PASS "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<"\n";return 1;}}
