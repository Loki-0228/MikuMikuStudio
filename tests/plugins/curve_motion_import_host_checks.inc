// Include after numpad_view_host_checks.inc. Uses the real curve editor's
// Read/Save actions, production file wrappers and bound native publication.
#include "mikumikustudio/animation_curve_motion.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <filesystem>
namespace curve_motion_import_host_checks {
std::wstring selected;
HWND expectedOwner=nullptr;
unsigned picked=0;
BOOL WINAPI Pick(LPOPENFILENAMEW parameters){
    ++picked;Check(parameters&&parameters->hwndOwner==expectedOwner,"curve action uses the editor as its actual file-dialog owner");
    Check(selected.size()+1<parameters->nMaxFile,"owned Unicode curve motion path fits the picker buffer");
    wcscpy_s(parameters->lpstrFile,parameters->nMaxFile,selected.c_str());return TRUE;
}
struct Wire {
    std::vector<std::uint8_t> data;
    void U8(unsigned value){data.push_back(std::uint8_t(value));}
    void U32(std::uint32_t value){for(unsigned i=0;i<4;++i)U8(value>>(8*i));}
    void F32(float value){std::uint32_t bits;std::memcpy(&bits,&value,4);U32(bits);}
    void Text(const char* text,std::size_t size){const auto count=std::min(std::strlen(text),size);data.insert(data.end(),text,text+count);data.resize(data.size()+size-count);}
};
const std::array<std::array<unsigned,4>,6> controls{{{100,10,20,115},{0,127,127,0},{127,0,0,127},
    {100,10,20,115},{0,127,127,0},{20,20,107,107}}};
std::vector<std::uint8_t> Input(bool legacy,bool delayed){
    Wire out;out.Text(legacy?"Vocaloid Motion Data file":"Vocaloid Motion Data 0002",30);out.Text("curve raw",legacy?10:20);
    out.U32(0);out.U32(0);out.U32(2);
    for(unsigned key=0;key<2;++key){out.U32((delayed?3:0)+key*23);out.F32(key?-12.f:-45.f);
        for(unsigned axis=0;axis<3;++axis)out.F32(key?float(11+axis*3):float(-2+int(axis)*5));
        for(unsigned axis=0;axis<3;++axis)out.F32(key?float(axis+1)*-.25f:float(axis+1)*.125f);
        for(unsigned lane=0;lane<(legacy?1u:6u);++lane){const auto& c=controls[lane];out.U8(c[0]);out.U8(c[2]);out.U8(c[1]);out.U8(c[3]);}
        if(!legacy){out.U32(key?90:30);out.U8(key);}
    }
    out.U32(0);if(!legacy)out.U32(0);out.U32(0);return out.data;
}
void Write(const std::wstring& path,const std::vector<std::uint8_t>& data){
    const auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"create owned Unicode raw camera input");DWORD written=0;
    Check(WriteFile(file,data.data(),DWORD(data.size()),&written,nullptr)&&written==data.size(),"write complete independent raw camera input");
    Check(CloseHandle(file)!=FALSE,"close owned camera input");
}
std::vector<std::uint8_t> Read(const std::wstring& path){
    const auto file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"open actual curve motion export");LARGE_INTEGER size{};
    Check(GetFileSizeEx(file,&size)&&size.QuadPart>=0&&size.QuadPart<1024*1024,"sparse curve export remains a small native file");
    std::vector<std::uint8_t> result(std::size_t(size.QuadPart));DWORD got=0;
    Check(ReadFile(file,result.data(),DWORD(result.size()),&got,nullptr)&&got==result.size(),"read complete actual curve export");CloseHandle(file);return result;
}
std::uint32_t U32(const std::vector<std::uint8_t>& bytes,std::size_t offset){
    Check(offset+4<=bytes.size(),"native export integer lies within file");std::uint32_t result=0;
    for(unsigned i=0;i<4;++i)result|=std::uint32_t(bytes[offset+i])<<(i*8);return result;
}
void Click(HWND editor,int id){const auto button=GetDlgItem(editor,id);Check(button!=nullptr,"real curve motion/history button exists");
    SendMessageW(editor,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(button));}
void Run(MMDApp* app,plugins::SharedHistory& history){
    using Mode=plugins::SharedHistory::RecordingMode;
    Check(!history.UndoCount()&&!history.RedoCount(),"curve raw import fixture begins with empty common history");
    Hook(&GetUiOpenFileNameW,&Pick);Hook(&GetUiSaveFileNameW,&Pick);
    for(bool legacy:{false,true})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(bool delayed:{false,true}){
        std::printf("  real raw curve import VMD%u mode%u first%u\n",legacy?1u:2u,unsigned(mode),delayed?3u:0u);std::fflush(stdout);
        numpad_view_host_checks::Original original(*app,history);
        std::memset(app->CameraKeys(),0,mdl::KeyPoolCapacity(app->CameraKeys())*sizeof(mdl::CameraKey));
        auto& root=app->CameraKeys()[0];root.parentModel=-1;root.fov=45;root.distance=-45;
        app->CameraMode()=1;app->CurrentFrame()=7;app->state.followCameraEnabled=0;app->CameraParentModel()=-1;
        Check(history.SetMode(mode)==MMS_OK,"configure actual motion read as an explicit commit in both recording modes");
        const auto directory=std::filesystem::temp_directory_path();const auto tag=std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        const auto input=(directory/(L"MMS-曲线原生输入-"+tag+L".vmd")).native();
        const auto output=(directory/(L"MMS-曲线原生输出-"+tag+L".vmd")).native();Write(input,Input(legacy,delayed));
        curve_editor::CloseOwned(app->MainWindow());curve_host::Open(app,false);
        const auto editor=FindWindowW(L"MikuMikuStudioCurveEditorWindow",nullptr);Check(editor!=nullptr,"open actual scene curve editor for raw VMD import");
        const auto sources=GetDlgItem(editor,1002);Check(SendMessageW(sources,CB_SETCURSEL,0,0)==0,"choose actual camera source");
        SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1002,CBN_SELCHANGE),reinterpret_cast<LPARAM>(sources));
        const auto before=*app->CameraKeys();curve_host::AuthoringTable author;std::wstring wide;
        Check(curve_host::CaptureAuthoring(*app,author,wide),"capture original authoring before actual read-motion action");
        expectedOwner=editor;selected=input;const auto calls=picked;Click(editor,1003);
        Check(picked==calls+1&&history.UndoCount()==1&&app->CurrentFrame()==7,"actual curve Read action uses the file decoder and publishes one explicit common step without seeking");
        const auto first=app->CameraKeys()[0].next;const auto last=delayed?app->CameraKeys()[first].next:first;
        Check(first&&last&&app->CameraKeys()[last].frame==(delayed?26u:23u)&&!app->CameraKeys()[last].next&&
            (!delayed||app->CameraKeys()[first].frame==3),"actual raw import keeps sparse source keys and adds only the required zero anchor");
        for(unsigned key:{0u,last})for(unsigned lane=0;lane<6;++lane)for(unsigned control=0;control<4;++control)
            Check(app->CameraKeys()[key].interpolation[control][lane]==controls[legacy?0:lane][control],
                "actual window import writes every original root and endpoint control byte without sorting or linearizing X");
        if(delayed)for(unsigned lane=0;lane<6;++lane)for(unsigned control=0;control<4;++control)
            Check(app->CameraKeys()[first].interpolation[control][lane]==std::array<unsigned,4>{20,20,107,107}[control],
                "only the new constant zero-to-first prefix receives legal diagonal native timing");
        std::vector<curve_editor::Channel> channels;Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,wide),"read actual canonical imported camera authoring");
        Check(curve_motion::CanCaptureNativeCamera(channels),"canonical imported eleven-channel source remains native-ready for export");
        for(unsigned lane=0;lane<8;++lane){const auto& channel=channels[lane];Check(channel.sample&&channel.nativeControls.size()==(delayed?3u:2u),
            "actual source retains the canonical native sampler and sparse raw control metadata");
            if(channel.sample){const auto sample=channel.sample(channel,7.25);Check(std::isfinite(sample.value)&&std::isfinite(sample.derivative),
                "crossed and endpoint native timing draws finite value and speed curves");}}
        selected=output;Click(editor,1004);const auto bytes=Read(output);
        Check(U32(bytes,50)==0&&U32(bytes,54)==0&&U32(bytes,58)==(delayed?3u:2u),"actual curve export retains native sparse keys plus a necessary zero anchor instead of replacing them with dense proxies");
        for(unsigned key:{0u,delayed?2u:1u})for(unsigned lane=0;lane<6;++lane){const auto offset=62+key*61+32+lane*4;const auto& c=controls[legacy?0:lane];
            Check(offset+4<=bytes.size()&&bytes[offset]==c[0]&&bytes[offset+1]==c[2]&&bytes[offset+2]==c[1]&&bytes[offset+3]==c[3],
                "actual Save action keeps all six file-order raw control lanes including the first key");}
        Click(editor,1059);auto restored=*app->CameraKeys();restored.selected=before.selected;
        Check(history.UndoCount()==0&&history.RedoCount()==1&&!std::memcmp(&restored,&before,sizeof(before))&&
            curve_host::AuthoringUnchanged(*app,author)&&app->CurrentFrame()==7,"actual editor Undo restores pre-import native root and raw author state without moving the playhead");
        Click(editor,1060);Check(history.UndoCount()==1&&app->CameraKeys()[0].next&&app->CameraKeys()[app->CameraKeys()[0].next].frame==(delayed?3u:23u),
            "actual editor Redo restores the explicit raw VMD import from history");
        curve_editor::CloseOwned(app->MainWindow());Check(DeleteFileW(input.c_str())&&DeleteFileW(output.c_str()),"remove only the owned raw camera input/export fixtures");
    }
    MH_DisableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));MH_RemoveHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));
    MH_DisableHook(reinterpret_cast<void*>(&GetUiSaveFileNameW));MH_RemoveHook(reinterpret_cast<void*>(&GetUiSaveFileNameW));selected.clear();expectedOwner=nullptr;
    std::puts("PASS actual curve-window VMD1/2 raw import/export, crossed/endpoint controls and common history");
}
}
