#include "../src/plugins/native_vmd_source.hpp"
#include "mikumikustudio/accessory_motion_io.hpp"
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
namespace {
using VmdDecodeBytes=std::vector<std::uint8_t>;
template<class T>void VmdDecodePut(VmdDecodeBytes& bytes,const T& value){const auto* p=reinterpret_cast<const std::uint8_t*>(&value);bytes.insert(bytes.end(),p,p+sizeof(value));}
void VmdDecodeText(VmdDecodeBytes& bytes,const char* text,std::size_t size){auto start=bytes.size();bytes.resize(start+size);std::memcpy(bytes.data()+start,text,(std::min)(size,std::strlen(text)));}
void VmdDecodeSet(VmdDecodeBytes& bytes,std::size_t at,std::uint32_t value){std::memcpy(bytes.data()+at,&value,4);}
struct VmdDecodeMotion {
    VmdDecodeBytes bytes;std::vector<std::size_t> ends;std::size_t boneFrame,morphValue,cameraCurve,cameraFov,shadowValue;
    explicit VmdDecodeMotion(int version=2){
        VmdDecodeText(bytes,version==1?"Vocaloid Motion Data file":"Vocaloid Motion Data 0002",30);
        VmdDecodeText(bytes,"\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe",version==1?10:20);
        VmdDecodePut(bytes,1u);VmdDecodeText(bytes,"bone",15);boneFrame=bytes.size();VmdDecodePut(bytes,7u);
        for(float value:{1.f,2.f,3.f,0.f,0.f,0.f,1.f})VmdDecodePut(bytes,value);bytes.resize(bytes.size()+64,20);
        VmdDecodePut(bytes,1u);VmdDecodeText(bytes,"face",15);VmdDecodePut(bytes,9u);morphValue=bytes.size();VmdDecodePut(bytes,.25f);ends.push_back(bytes.size());
        VmdDecodePut(bytes,1u);VmdDecodePut(bytes,11u);VmdDecodePut(bytes,-20.f);
        for(float value:{1.f,2.f,3.f,.1f,.2f,.3f})VmdDecodePut(bytes,value);cameraCurve=bytes.size();
        if(version==1)for(unsigned char value:{11,22,33,44})bytes.push_back(value);
        else {for(unsigned i=0;i<24;++i)bytes.push_back(std::uint8_t(i+1));cameraFov=bytes.size();VmdDecodePut(bytes,60);bytes.push_back(7);}
        ends.push_back(bytes.size());VmdDecodePut(bytes,1u);VmdDecodePut(bytes,13u);
        for(float value:{.2f,.3f,.4f,1.f,-1.f,.5f})VmdDecodePut(bytes,value);ends.push_back(bytes.size());
        if(version==2){VmdDecodePut(bytes,1u);VmdDecodePut(bytes,17u);bytes.push_back(2);shadowValue=bytes.size();VmdDecodePut(bytes,.04f);ends.push_back(bytes.size());}
        VmdDecodePut(bytes,1u);VmdDecodePut(bytes,19u);bytes.push_back(1);VmdDecodePut(bytes,0u);ends.push_back(bytes.size());
    }
};
struct VmdDecodeFile {
    std::filesystem::path path=std::filesystem::absolute(L"vmd-decode-\u52a8\u4f5c-"+std::to_wstring(GetCurrentProcessId())+L".vmd");
    ~VmdDecodeFile(){DeleteFileW(path.c_str());}
    void Write(const VmdDecodeBytes& bytes){int fd=-1;Check(_wsopen_s(&fd,path.c_str(),_O_BINARY|_O_WRONLY|_O_CREAT|_O_TRUNC,_SH_DENYRW,_S_IREAD|_S_IWRITE)==0,"open Unicode VMD decoder fixture");
        Check(bytes.empty()||_write(fd,bytes.data(),unsigned(bytes.size()))==int(bytes.size()),"write VMD decoder fixture");_close(fd);}
};
void NativeVmdDecodeChecks(){
    using plugins::DecodeGlobalVmdSource;using plugins::GlobalVmdSource;
    std::puts("  VMD decode: immutable Unicode source and two record versions");std::fflush(stdout);
    for(int version:{1,2}){
        VmdDecodeMotion motion(version);VmdDecodeFile file;file.Write(motion.bytes);GlobalVmdSource out;
        Check(DecodeGlobalVmdSource(nullptr,file.path.c_str(),100,out)==MMS_OK&&out.version==version&&out.source.frameOffset==100,"decode VMD snapshot with preserved frame offset");
        Check(out.source.cameras.size()==1&&out.source.lights.size()==1&&out.source.shadows.size()==std::size_t(version==2),"decode all standard global record families");
        const auto& camera=out.source.cameras[0];
        Check(camera.frame==11&&camera.distance==-20.f&&camera.eye[0]==1&&camera.target[2]==.3f&&camera.parentModel==-1&&camera.parentBone==0,"VMD camera payload maps to owned clipboard record");
        Check(out.source.lights[0].frame==13&&out.source.lights[0].color[0]==.2f&&out.source.lights[0].direction[0]==1.f&&out.source.lights[0].direction[1]==-1.f,"VMD light file RGB and direction order preserved");
        if(version==1){Check(camera.fov==45&&!camera.perspective,"old VMD camera defaults field of view and perspective flag");
            for(unsigned lane=0;lane<6;++lane)Check(camera.interpolation[lane]==11&&camera.interpolation[6+lane]==33&&camera.interpolation[12+lane]==22&&camera.interpolation[18+lane]==44,"VMD1 shared curve expands to six lanes in native row order");}
        else {Check(camera.fov==60&&camera.perspective==1,"VMD2 arbitrary nonzero perspective flag normalizes exactly as legacy");
            for(unsigned lane=0;lane<6;++lane)Check(camera.interpolation[lane]==1+4*lane&&camera.interpolation[6+lane]==3+4*lane&&camera.interpolation[12+lane]==2+4*lane&&camera.interpolation[18+lane]==4+4*lane,"VMD2 interleaved curves transpose into 24 native control bytes");
            Check(out.source.shadows[0].frame==17&&out.source.shadows[0].mode==2&&out.source.shadows[0].distance==.04f,"decode packed nine-byte shadow payload");}
        for(auto end:motion.ends){auto partial=motion.bytes;partial.resize(end);file.Write(partial);
            Check(DecodeGlobalVmdSource(nullptr,file.path.c_str(),0,out)==MMS_OK,"complete optional-section endings remain valid VMD sources");
            partial.resize(end-1);file.Write(partial);GlobalVmdSource unchanged;unchanged.version=99;unchanged.modelName="sentinel";
            Check(DecodeGlobalVmdSource(nullptr,file.path.c_str(),0,unchanged)==MMS_E_ARGUMENT&&unchanged.version==99&&unchanged.modelName=="sentinel","partial count or payload refuses without changing output");}
    }
    std::puts("  VMD decode: standard validation and optional tails");std::fflush(stdout);
    {
        VmdDecodeMotion motion;VmdDecodeFile file;GlobalVmdSource out;out.version=99;out.modelName="unchanged";
        auto checkBad=[&](const VmdDecodeBytes& bytes){file.Write(bytes);Check(DecodeGlobalVmdSource(nullptr,file.path.c_str(),0,out)==MMS_E_ARGUMENT&&out.version==99&&out.modelName=="unchanged","bad VMD numeric or record source leaves output untouched");};
        auto bad=motion.bytes;bad[motion.cameraCurve]=128;checkBad(bad);
        bad=motion.bytes;VmdDecodeSet(bad,motion.cameraFov,180);checkBad(bad);
        bad=motion.bytes;const float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(bad.data()+motion.morphValue,&nan,4);checkBad(bad);
        bad=motion.bytes;std::memcpy(bad.data()+motion.shadowValue,&nan,4);checkBad(bad);
        bad=motion.bytes;VmdDecodeSet(bad,motion.boneFrame,unsigned(frame_limits::maximum)+1u);checkBad(bad);
        bad=motion.bytes;bad[30]='x';checkBad(bad);
        file.Write(motion.bytes);Check(DecodeGlobalVmdSource(nullptr,file.path.c_str(),unsigned(frame_limits::maximum)-10u,out)==MMS_E_ARGUMENT&&out.version==99,"ignored model/display maximum frames still reject offset overflow");
        auto tail=motion.bytes;tail.insert(tail.end(),{'U','N','K','N','O','W','N'});file.Write(tail);Check(DecodeGlobalVmdSource(nullptr,file.path.c_str(),0,out)==MMS_OK,"unknown tail after complete standard records is ignored");
        const auto empty=accessory_motion_io::Encode({});tail=motion.bytes;tail.insert(tail.end(),empty.begin(),empty.end());file.Write(tail);
        Check(DecodeGlobalVmdSource(nullptr,file.path.c_str(),0,out)==MMS_OK,"valid empty accessory tail does not block standard motion");
        accessory_motion_io::Track track;track.name="fixture";track.keys.push_back({});const auto extension=accessory_motion_io::Encode({track});
        tail=motion.bytes;tail.insert(tail.end(),extension.begin(),extension.end());file.Write(tail);out.version=99;
        Check(DecodeGlobalVmdSource(nullptr,file.path.c_str(),0,out)==MMS_OK&&out.accessoryTracks.size()==1&&out.accessoryTracks[0].keys.size()==1,"valid nonempty accessory tail remains owned for scene matching");
        tail.back()^=1;file.Write(tail);Check(DecodeGlobalVmdSource(nullptr,file.path.c_str(),0,out)==MMS_OK,"damaged accessory CRC retains legacy ignored-tail policy");
        tail=motion.bytes;tail.insert(tail.end(),extension.begin(),extension.begin()+12);file.Write(tail);
        Check(DecodeGlobalVmdSource(nullptr,file.path.c_str(),0,out)==MMS_OK,"truncated optional accessory tail is ignored after valid standard sections");
    }
    std::puts("  VMD decode: allocation failures and descriptor cleanup");std::fflush(stdout);
    {
        VmdDecodeMotion motion;VmdDecodeFile file;file.Write(motion.bytes);bool success=false;
        for(long limit=0;limit<128;++limit){GlobalVmdSource out;out.version=99;failAfter=limit;const auto status=DecodeGlobalVmdSource(nullptr,file.path.c_str(),3,out);failAfter=-1;
            if(status==MMS_OK){success=true;break;}Check(status==MMS_E_MEMORY&&out.version==99&&out.source.cameras.empty(),"VMD decode allocation failure leaves original output untouched");
            Check(DeleteFileW(file.path.c_str())!=0,"failed decoder closes its descriptor before returning");file.Write(motion.bytes);}
        Check(success,"VMD decode allocation sweep eventually succeeds");
        GlobalVmdSource out;Check(DecodeGlobalVmdSource(nullptr,nullptr,0,out)==MMS_E_ARGUMENT,"VMD decoder refuses missing path");
        Check(DecodeGlobalVmdSource("?:\\absent.vmd",nullptr,0,out)!=MMS_OK,"VMD decoder reports open errors without changing output");
    }
}
}
