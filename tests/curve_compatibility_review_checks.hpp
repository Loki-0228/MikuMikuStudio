#pragma once
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/animation_curve.hpp"
namespace mikudancestudio { void ApplyFrameRangeScale(HWND); }
namespace {
void ReviewAutomaticInterpolationUndo() {
    Fixture f;
    HWND window=CreateWindowExW(0,L"STATIC",L"automatic interpolation undo",WS_OVERLAPPED,
        0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(window!=nullptr,"create auto interpolation parent");
    f.model->hwnd=window;
    HWND checkbox=CreateWindowExW(0,L"BUTTON",L"auto",WS_CHILD|BS_CHECKBOX,0,0,30,20,window,
        reinterpret_cast<HMENU>(INT_PTR(panel::kPhysicsFrameCheckbox)),nullptr,nullptr);
    SendMessageW(checkbox,BM_SETCHECK,BST_CHECKED,0);
    for(int frame:{10,20,30}) { f.bones[Fixture::kShoulder].trans[0]=float(frame/10);f.Register(frame); }
    const auto snapshot=[&] {
        std::vector<mdl::BoneKey> keys;
        for(unsigned slot=Fixture::kShoulder;;slot=f.model->boneKeys[slot].next) {
            keys.push_back(f.model->boneKeys[slot]);if(!f.model->boneKeys[slot].next)break;
        }
        return keys;
    };
    const auto before=snapshot();
    f.bones[Fixture::kShoulder].trans[0]=0;f.Register(20);
    const auto edited=snapshot();
    Check(std::memcmp(before[1].interpolation,edited[1].interpolation,16)!=0 &&
          std::memcmp(before[3].interpolation,edited[3].interpolation,16)!=0,
          "automatic overwrite changes both neighbouring control lanes");
    std::puts("auto key registered, undo");
    int frame=20;UndoModelEdit(f.Raw(),&frame,0);const auto undone=snapshot();
    for(std::size_t i=0;i<before.size();++i)
        Check(!std::memcmp(before[i].interpolation,undone[i].interpolation,16),
            "automatic interpolation Undo restores every neighbouring control byte");
    RedoModelEdit(f.Raw(),&frame,0);const auto redone=snapshot();
    for(std::size_t i=0;i<edited.size();++i)
        Check(!std::memcmp(edited[i].interpolation,redone[i].interpolation,16),
            "automatic interpolation Redo restores every neighbouring control byte");
    DestroyWindow(window);f.model->hwnd=nullptr;bone_curve::Forget(f.Raw());bone_scale::Forget(f.Raw());
}
void ReviewRangeScaleAuthoring() {
    Fixture f; f.model->morphCount=0;
    for(int b=0;b<Fixture::kBones;++b)std::snprintf(f.bones[b].name,20,"bone%d",b);
    f.Select(0);f.bones[0].trans[0]=2;f.Register(30);
    const auto scaleSlot=f.model->boneKeys[0].next;
    const bone_scale::Curve scaleCurve={20,5,107,100};
    bone_scale::Matrix matrix;matrix.v[0]=2;
    bone_scale::SetKey(f.Raw(),int(scaleSlot),matrix);bone_scale::SetKeyCurve(f.Raw(),int(scaleSlot),scaleCurve);
    bone_curve::Tracks tracks;
    for(int c=0;c<9;++c) {
        animation_curve::Key a,b;a.frame=0;b.frame=30;
        a.value=c>=6?1:0;b.value=c==0?10:c>=6?2:0;
        a.outWeight=.2;b.inWeight=.25;a.outSlope=c==0?.5:0;b.inSlope=c==0?-.1:0;
        a.tangent=b.tangent=animation_curve::Tangent::Free;
        b.interpolation=animation_curve::Interpolation::Bezier;tracks[c].keys={a,b};
    }
    std::string detail;Check(bone_curve::Write(f.Raw(),Fixture::kShoulder,tracks,detail),"prepare authored range curve");
    auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof(app->state));
    auto renderer=std::make_unique<D3DRenderer>();std::memset(renderer.get(),0,sizeof(*renderer));
    app->Renderer()=renderer.get();app->ModelSlot(0)=f.Raw();app->SetSelectedModelSlot(0);app->EnglishUI()=2;
    auto* previous=g_Block;g_Block=app.get();
    HWND window=CreateWindowExW(0,L"STATIC",L"range scale regression",WS_OVERLAPPED,0,0,100,100,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    app->Hwnd()=f.model->hwnd=window;
    for(const auto entry:{std::pair<int,const wchar_t*>{panel::kScaleFromEdit,L"0"},
                         {panel::kScaleToEdit,L"30"},{panel::kScaleRateEdit,L"2"}})
        Check(CreateWindowExW(0,L"EDIT",entry.second,WS_CHILD,0,0,30,20,window,
            reinterpret_cast<HMENU>(INT_PTR(entry.first)),nullptr,nullptr)!=nullptr,"create range scale edit");
    auto checkbox=CreateWindowExW(0,L"BUTTON",L"bone",WS_CHILD|BS_CHECKBOX,0,0,30,20,window,
        reinterpret_cast<HMENU>(INT_PTR(panel::kScaleBoneCheckbox)),nullptr,nullptr);
    SendMessageW(checkbox,BM_SETCHECK,BST_CHECKED,0);
    std::puts("apply frame scale begin");
    ApplyFrameRangeScale(window);
    std::puts("apply frame scale end");
    bone_curve::Tracks retimed;Check(bone_curve::Read(f.Raw(),Fixture::kShoulder,retimed),"range scale retains authored tracks");
    for(int c=0;c<9;++c) {
        Check(retimed[c].keys.back().frame==60,"range scale retimes every authored lane");
        for(int frame=0;frame<=120;++frame)
            Check(std::abs(animation_curve::Evaluate(retimed[c],frame*.5).value-
                animation_curve::Evaluate(tracks[c],frame*.25).value)<1e-9,"range scale preserves normalized author curve shape");
    }
    auto moved=f.model->boneKeys[0].next;
    Check(moved&&f.model->boneKeys[moved].frame==60&&bone_scale::KeyCurve(f.Raw(),int(moved))==scaleCurve,
        "range scale preserves the independent native scale interpolation curve");
    int frame=0;UndoModelEdit(f.Raw(),&frame,0);
    Check(bone_curve::Read(f.Raw(),Fixture::kShoulder,retimed)&&retimed[0].keys.back().frame==30,
        "range scale Undo restores authored timing");
    RedoModelEdit(f.Raw(),&frame,0);
    Check(bone_curve::Read(f.Raw(),Fixture::kShoulder,retimed)&&retimed[0].keys.back().frame==60,
        "range scale Redo restores authored timing");
    app->ModelSlot(0)=nullptr;app->Hwnd()=nullptr;f.model->hwnd=nullptr;g_Block=previous;DestroyWindow(window);
    bone_curve::Forget(f.Raw());bone_scale::Forget(f.Raw());
}
void ReviewCurveEditingChecks() { ReviewAutomaticInterpolationUndo();ReviewRangeScaleAuthoring(); }
}
