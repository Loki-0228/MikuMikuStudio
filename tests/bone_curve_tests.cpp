#define NOMINMAX
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include <commctrl.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
using namespace mikudancestudio;
namespace ac=animation_curve;
void Check(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
struct Model {
    std::unique_ptr<mdl::ModelRecord> record=std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones=std::vector<mdl::BoneRecord>(2);
    Model(){record->boneCount=2;record->selectedBone=0;record->boneTable=bones.data();record->boneKeys=mdl::BoneKeyPoolCreate(32);
        std::memcpy(bones[0].name,"test",5);std::memcpy(bones[1].name,"other",6);
        for(int b=0;b<2;++b){record->boneKeys[b].rotation[3]=1;record->boneKeys[b].allocated=1;}
        record->boneKeys[0].next=2;record->boneKeys[2].frame=20;record->boneKeys[2].rotation[3]=1;
        record->boneKeys[2].position[0]=10;record->boneKeys[2].allocated=1;
    }
    unsigned char* data(){return reinterpret_cast<unsigned char*>(record.get());}
    ~Model(){bone_scale::Forget(data());mdl::BoneKeyPoolDestroy(record->boneKeys);}
};
int main(){
    Model model;bone_curve::Tracks tracks;
    for(int c=0;c<9;++c){ac::Key a,b;a.frame=0;b.frame=20;a.tangent=b.tangent=ac::Tangent::Free;
        a.value=b.value=c>=6?1:0;a.interpolation=b.interpolation=ac::Interpolation::Bezier;
        if(c==0){b.value=10;a.outSlope=2;b.inSlope=-.5;a.outWeight=.2;b.inWeight=.4;}
        if(c==4)b.value=90;
        tracks[c].keys={a,b};}
    const auto other=model.record->boneKeys[1];std::string error;
    Check(bone_curve::Write(model.data(),0,tracks,error),error.c_str());
    Check(std::memcmp(&other,&model.record->boneKeys[1],sizeof other)==0,"other bone untouched");
    for(int i=0;i<=200;++i){double f=i*.1;bone_scale::Evaluate(model.data(),float(f));bone_curve::Evaluate(model.data(),f);
        Check(std::abs(model.bones[0].trans[0]-ac::Evaluate(tracks[0],f).value)<2e-6,"fractional playback uses exact free curve");}
    bone_curve::Tracks read;Check(bone_curve::Read(model.data(),0,read),"read authored curve");
    auto split=tracks;std::size_t index=0;
    Check(ac::InsertPreservingShape(split[0],9,index,error)==ac::InsertResult::Inserted,"subdivision accepted");
    Check(bone_curve::Write(model.data(),0,split,error),error.c_str());
    Check(model.record->boneKeys[model.record->boneKeys[0].next].frame==9,"insert reflected in native timeline");
    for(int i=0;i<=400;++i)Check(std::abs(ac::Evaluate(read[0],i*.05).value-ac::Evaluate(split[0],i*.05).value)<1e-10,"insertion retains shape");
    auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof app->state);app->ModelSlot(0)=model.data();
    pmm_extension::Document doc;bone_curve::CapturePmm(*app,doc);Check(!doc.records.empty(),"PMM stores free curves");
    bone_curve::Reset();bone_curve::LoadPmm(*app,doc);Check(bone_curve::Read(model.data(),0,read),"PMM restores free curves");
    Check(read[0].keys.size()==3,"PMM restores independent key counts");
    Check(std::abs(ac::Evaluate(read[0],7.25).value-ac::Evaluate(tracks[0],7.25).value)<1e-10,"PMM retains fractional value");
    std::vector<std::uint8_t> block;
    Check(bone_curve::CaptureVmd(model.data(),0,block,error),"VMD encodes free curves");
    Check(bone_curve::VmdBlockSize(block.data(),block.size())==block.size(),"VMD block bounds");
    bone_curve::Reset();Check(bone_curve::ApplyVmd(model.data(),block.data(),block.size(),0,error),error.c_str());
    Check(bone_curve::Read(model.data(),0,read)&&read[0].keys.size()==3,"VMD restores independent key counts");
    Check(std::abs(ac::Evaluate(read[0],7.25).value-ac::Evaluate(split[0],7.25).value)<1e-10,"VMD retains exact curve");
    Check(!bone_curve::VmdBlockSize(block.data(),block.size()-1),"truncated VMD block rejected");
    auto bad=split;bad[6].keys[1].inSlope=100;
    const auto before=model.record->boneKeys[0];
    Check(!bone_curve::Write(model.data(),0,bad,error),"invalid scale rejected");
    Check(std::memcmp(&before,&model.record->boneKeys[0],sizeof before)==0,"failed edit leaves native keys intact");
    Check(bone_curve::Read(model.data(),0,read)&&read[0].keys.size()==3,"failed edit retains curves");
    bone_curve::CaptureUndo(model.data(),0);
    model.record->boneKeys[0].position[0]=123;
    Check(!bone_curve::Has(model.data(),0),"native edits cannot be hidden by stale curve");
    model.record->boneKeys[0]=before;bone_curve::Undo(model.data(),0);
    Check(bone_curve::Has(model.data(),0),"native undo restores authored curve");
    Check(bone_curve::Write(model.data(),0,tracks,error),"editor undo removes inserted frame");
    Check(model.record->boneKeys[model.record->boneKeys[0].next].frame==20,"mirror removes deleted key");
    // Exercise the real host/editor route: display edits remain available in
    // both sources after the selected bone acquired free curves.
    std::vector<mdl::DisplayKey> display(mdl::kDisplayKeyCapacity);
    display[0].visible=1;display[0].allocated=1;model.record->displayKeys=display.data();
    app->EnglishUI()=2;
    HWND parent=CreateWindowExW(0,L"STATIC",L"bone curve test",WS_OVERLAPPEDWINDOW,0,0,1200,800,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(parent!=nullptr,"hidden bone host");app->Hwnd()=parent;
    HWND combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,0,100,150,parent,reinterpret_cast<HMENU>(433),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"X"));SendMessageW(combo,CB_SETCURSEL,0,0);
    // The main MMS interpolation square follows the interval at the playhead,
    // including the two intervals created by an exact subdivision.
    Check(bone_curve::Write(model.data(),0,split,error),"prepare subdivided preview");
    HDC previewDc=CreateCompatibleDC(nullptr);BITMAPINFO image{};
    image.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);image.bmiHeader.biWidth=128;image.bmiHeader.biHeight=-128;
    image.bmiHeader.biPlanes=1;image.bmiHeader.biBitCount=32;void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(previewDc,&image,DIB_RGB_COLORS,&pixels,nullptr,0);
    Check(previewDc&&bitmap,"preview drawing surface");auto oldBitmap=SelectObject(previewDc,bitmap);app->CurveDC()=previewDc;
    auto preview=[&](int frame){
        std::memset(pixels,255,128*128*4);app->CurrentFrame()=frame;
        Check(bone_curve::DrawPreview(*app),"main square draws free curves");GdiFlush();
        auto* bytes=static_cast<unsigned char*>(pixels);return std::vector<unsigned char>(bytes,bytes+128*128*4);
    };
    const auto firstInterval=preview(5),sameInterval=preview(8),secondInterval=preview(15);
    Check(firstInterval==sameInterval,"same interval keeps interpolation preview");
    Check(firstInterval!=secondInterval,"seeking across inserted key switches main preview interval");
    Check(preview(5)==firstInterval,"seeking back restores the interval preview");
    app->CurveDC()=nullptr;SelectObject(previewDc,oldBitmap);DeleteObject(bitmap);DeleteDC(previewDc);
    app->CurrentFrame()=0;Check(bone_curve::Write(model.data(),0,tracks,error),"restore display-edit fixture");
    curve_host::OpenInterpolation(app.get(),false);
    struct Owned { HWND parent,editor=nullptr; } owned{parent};
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM parameter)->BOOL{
        auto& result=*reinterpret_cast<Owned*>(parameter);
        wchar_t cls[80]{};GetClassNameW(window,cls,80);
        if(GetWindow(window,GW_OWNER)==result.parent&&wcscmp(cls,L"MikuDanceStudioCurveEditorWindow")==0)result.editor=window;
        return TRUE;
    },reinterpret_cast<LPARAM>(&owned));
    HWND editor=owned.editor;Check(editor!=nullptr,"bone editor opens");
    Check(SendMessageW(GetDlgItem(editor,1002),CB_GETCOUNT,0,0)==2,"native and free sources available");
    for(int source=0;source<2;++source){
        SendMessageW(GetDlgItem(editor,1002),CB_SETCURSEL,source,0);
        SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1002,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(editor,1002)));
        HWND tree=GetDlgItem(editor,1015);
        std::function<HTREEITEM(HTREEITEM)> find=[&](HTREEITEM item)->HTREEITEM{
            for(;item;item=TreeView_GetNextSibling(tree,item)){TVITEMW data{};data.mask=TVIF_PARAM;data.hItem=item;SendMessageW(tree,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&data));
                if(data.lParam==10)return item;if(auto hit=find(TreeView_GetChild(tree,item)))return hit;}return nullptr;};
        auto item=find(TreeView_GetRoot(tree));Check(item!=nullptr,"display channel exists");
        TreeView_SelectItem(tree,item);SendMessageW(tree,WM_KEYDOWN,VK_RETURN,0);
        HWND canvas=GetDlgItem(editor,1020);const auto point=SendMessageW(editor,WM_APP+300,0,0);
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(canvas,WM_LBUTTONUP,0,point);
        SetWindowTextW(GetDlgItem(editor,1043),source?L"1":L"0");
        SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1058,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(editor,1058)));
        Check(display[0].visible==source,"display edit works with an authored bone in both sources");
        Check(bone_curve::Read(model.data(),0,read)&&read[0].keys.size()==2,"display edit preserves free curves");
    }
    // Value-curve mode changes use the same native selection as the main row.
    HWND tree=GetDlgItem(editor,1015);
    std::function<HTREEITEM(HTREEITEM)> findX=[&](HTREEITEM item)->HTREEITEM{
        for(;item;item=TreeView_GetNextSibling(tree,item)){TVITEMW data{};data.mask=TVIF_PARAM;data.hItem=item;SendMessageW(tree,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&data));
            if(data.lParam==1)return item;if(auto hit=findX(TreeView_GetChild(tree,item)))return hit;}return nullptr;};
    auto xItem=findX(TreeView_GetRoot(tree));Check(xItem!=nullptr,"free bone X curve exists");
    for(unsigned slot=0;;slot=model.record->boneKeys[slot].next){model.record->boneKeys[slot].allocated=model.record->boneKeys[slot].frame==20;if(!model.record->boneKeys[slot].next)break;}
    TreeView_SelectItem(tree,xItem);SendMessageW(tree,WM_KEYDOWN,VK_RETURN,0);
    HWND canvas=GetDlgItem(editor,1020);auto point=SendMessageW(editor,WM_APP+300,1,0);
    SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(canvas,WM_LBUTTONUP,0,point);
    app->SceneModified()=0;
    SendMessageW(GetDlgItem(editor,1045),CB_SETCURSEL,1,0);
    SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1045,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(editor,1045)));
    Check(app->SceneModified()!=0,"bone interpolation edit reaches the real host write");
    unsigned marks=0;
    for(unsigned slot=0;;slot=model.record->boneKeys[slot].next){
        if(model.record->boneKeys[slot].allocated){++marks;Check(model.record->boneKeys[slot].frame==20,"bone interpolation keeps the selected native frame");}
        if(!model.record->boneKeys[slot].next)break;
    }
    Check(marks==1,"bone interpolation does not select the entire native row");
    SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1059,BN_CLICKED),0);
    Check(bone_curve::Read(model.data(),0,read)&&read[0].keys[1].interpolation==tracks[0].keys[1].interpolation,
          "bone curve undo survives native canonicalization and selection synchronization");
    curve_editor::CloseOwned(parent);DestroyWindow(parent);app->Hwnd()=nullptr;model.record->displayKeys=nullptr;
    app->ModelSlot(0)=nullptr;
    std::puts("bone_curve_tests: playback, subdivision, timeline mirror, PMM/VMD, rejection, undo, current-interval preview and display editing passed");
}
