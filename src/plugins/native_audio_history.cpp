#include "native_audio_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_object_commands.hpp"
#include "native_history_commands.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/native_media.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/wave_audio_asset.hpp"
#include <commdlg.h>
#include <array>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <stdexcept>

namespace mikumikustudio::plugins {
namespace {
std::uint64_t Add(std::uint64_t a,std::uint64_t b)noexcept{return b>UINT64_MAX-a?UINT64_MAX:a+b;}
bool Ready(MMDApp& app,bool replay=false)noexcept{
    const bool own=replay&&NativeObjectPublicationAuthorized(&app);
    return (!app.SceneMutationInProgress()||own)&&(!bake::UiLocked()||own)&&!SceneSaveBlocksEdits()&&
        !NativeObjectUiPending(&app)&&!app.PlaybackActive()&&!app.FrameStepPlayback()&&!app.state.recordPlaybackStartPending&&
        !GetCapture()&&!app.RecordingWindow()&&!app.FrameRangeDialog()&&!app.state.frameCopyDialog&&!app.GravitySettingDialog()&&
        !app.GroundShadowColorDialog()&&!app.AccessoryOrderArray()&&!app.state.accessoryEditDialogOpen&&!NativeModelOutlineDialogBlocks(app)&&
        app.Audio()&&wave_audio::Stopped(*app.Audio());
}
bool Remember(MMDApp& app){const auto state=GetMenuState(GetMenu(app.MainWindow()),0x12D,MF_BYCOMMAND);return state!=UINT(-1)&&(state&MF_CHECKED);}
std::wstring Parent(const wchar_t* path){return std::filesystem::absolute(path).lexically_normal().parent_path().native();}
void ShowError(MMDApp& app,MmsResult result)noexcept{
    runtime_log::Write("PLUGIN_ABI audio edit refused: %d",result);
    // The common journal reports a refusal without changing the current clip.
    MessageBeep(MB_ICONWARNING);
}
thread_local bool picking=false;
}
struct NativeAudioPlan::State final:HistoryChange,std::enable_shared_from_this<State>{
    MMDApp& app;SharedHistory* const history;WaveAudioContext* const audio;
    const std::uint64_t binding,scene;std::uint64_t revision=0,serial=1;
    std::array<unsigned char,sizeof(WaveAudioContext)> original{};
    std::array<wchar_t,256> originalPath{};
    const unsigned char originalEnabled,originalDevice;
    wave_audio::AssetPtr before,after;
    bool read=false,changed=false,published=false,applied=false;
    State(MMDApp& a,SharedHistory& h):app(a),history(&h),audio(a.Audio()),binding(BoundNativePoseGeneration(&a)),scene(a.SceneGeneration()),
        originalEnabled(a.WaveEnabled()),originalDevice(a.DirectSoundAvailable()){
        if(wcsnlen_s(a.WavePath(),256)>=256)throw std::invalid_argument("unterminated audio source name");
        std::memcpy(original.data(),audio,original.size());std::memcpy(originalPath.data(),a.WavePath(),originalPath.size()*sizeof(wchar_t));
        if(originalEnabled)before=wave_audio::Asset::Capture(*audio,a.WavePath(),h.MaxBytes());
    }
    bool Structure()const noexcept{return BoundNativePoseHistory(&app)==history&&app.Audio()==audio&&app.SceneGeneration()==scene;}
    bool InitialCurrent()const noexcept{
        if(!Structure()||BoundNativePoseGeneration(&app)!=binding||app.WaveEnabled()!=originalEnabled||app.DirectSoundAvailable()!=originalDevice||
            std::memcmp(original.data(),audio,original.size())||std::memcmp(originalPath.data(),app.WavePath(),originalPath.size()*sizeof(wchar_t)))return false;
        std::uint64_t current=0;return history->Revision(current)==MMS_OK&&current==revision;
    }
    MmsResult ReadReady()const noexcept{
        if(!InitialCurrent())return MMS_E_STALE;
        if(auto status=history->CheckNativeNavigation())return status;if(auto status=history->CheckNativeEdit())return status;
        return Ready(app)?MMS_OK:MMS_E_BUSY;
    }
    bool Matches(const wave_audio::AssetPtr& asset)const noexcept{
        if(!Structure()||wave_audio::Active(audio)!=asset||bool(app.WaveEnabled())!=bool(asset))return false;
        return !std::wcscmp(app.WavePath(),asset?asset->Source():L"")&&bool(app.DirectSoundAvailable())==bool(audio->directSound);
    }
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<State> source;const bool forward,first;const std::uint64_t serial,binding;
        std::unique_ptr<wave_audio::Prepared> prepared;
        std::uint64_t bytes=0;bool consumed=false;
        Candidate(std::shared_ptr<State> s,bool direction):source(std::move(s)),forward(direction),first(!source->published),serial(source->serial),binding(BoundNativePoseGeneration(&source->app)){}
        bool IsCurrent()const noexcept override{
            return !consumed&&source->serial==serial&&forward!=source->applied&&source->Structure()&&BoundNativePoseGeneration(&source->app)==binding&&
                Ready(source->app,!first)&&(first?source->InitialCurrent():source->Matches(forward?source->before:source->after))&&prepared&&prepared->IsCurrent();
        }
        void Publish()noexcept override{
            if(!IsCurrent())std::terminate();auto& s=*source;prepared->Publish();const auto& target=forward?s.after:s.before;
            wcscpy_s(s.app.WavePath(),256,target?target->Source():L"");s.app.WaveEnabled()=target?1:0;s.app.DirectSoundAvailable()=s.audio->directSound?1:0;
            s.published=true;s.applied=forward;++s.serial;consumed=true;
        }
        bool TargetRetainedBytes(std::uint64_t& out)const noexcept override{out=bytes;return true;}
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();return Boundary([&]()->MmsResult{
            if(!read||!changed||forward==applied||!Structure()||(!published&&!InitialCurrent())||
                (published&&!Matches(forward?before:after)))return MMS_E_STALE;
            if(!Ready(app,published))return MMS_E_BUSY;if(serial==UINT64_MAX)return MMS_E_NOT_READY;
            auto next=std::make_unique<Candidate>(shared_from_this(),forward);
            next->prepared=std::make_unique<wave_audio::Prepared>(*audio,forward?after:before);
            next->bytes=Add(RetainedBytes(),next->prepared->ResourceBytes());
            if(!next->IsCurrent())return MMS_E_STALE;out=std::move(next);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override{
        auto bytes=std::uint64_t(sizeof(*this)+4*sizeof(void*));if(before)bytes=Add(bytes,before->RetainedBytes());
        if(after&&after!=before)bytes=Add(bytes,after->RetainedBytes());return bytes;
    }
    bool RequiresPhysicsReset()const noexcept override{return false;}
};
MmsResult NativeAudioPlan::Begin(MMDApp& app)noexcept{
    if(state_)return MMS_E_BUSY;return Boundary([&]()->MmsResult{
        auto* history=BoundNativePoseHistory(&app);if(!history)return MMS_E_NOT_READY;
        if(auto status=history->CheckNativeNavigation())return status;if(auto status=history->CheckNativeEdit())return status;
        if(!Ready(app))return MMS_E_BUSY;
        auto next=std::make_shared<State>(app,*history);if(auto status=history->Revision(next->revision))return status;
        if(!next->InitialCurrent())return MMS_E_STALE;state_=std::move(next);return MMS_OK;
    });
}
MmsResult NativeAudioPlan::Read(const wchar_t* path)noexcept{
    if(!state_)return MMS_E_NOT_READY;state_->read=false;state_->after.reset();return Boundary([&]()->MmsResult{
        if(auto status=state_->ReadReady())return status;if(!path||!*path)return MMS_E_ARGUMENT;
        const auto absolute=std::filesystem::absolute(path).lexically_normal().native();if(absolute.size()>=256)return MMS_E_ARGUMENT;
        auto asset=wave_audio::Asset::Decode(absolute.c_str(),state_->history->MaxBytes());
        if(auto status=state_->ReadReady())return status;
        state_->changed=!state_->before||!state_->before->Equals(*asset);state_->after=state_->changed?std::move(asset):state_->before;
        state_->read=true;return MMS_OK;
    });
}
MmsResult NativeAudioPlan::Clear()noexcept{
    if(!state_)return MMS_E_NOT_READY;if(auto status=state_->ReadReady())return status;
    state_->after.reset();state_->changed=bool(state_->before);state_->read=true;return MMS_OK;
}
MmsResult NativeAudioPlan::Take(std::shared_ptr<HistoryChange>& out)noexcept{
    if(out)return MMS_E_ARGUMENT;if(!state_||!state_->read)return MMS_E_NOT_READY;if(auto status=state_->ReadReady())return status;
    if(state_->changed)out=state_;return MMS_OK;
}
namespace {
MmsResult Commit(MMDApp& app,NativeAudioPlan& plan,bool& changed,const char* label)noexcept{
    changed=false;return Boundary([&]()->MmsResult{
        std::shared_ptr<HistoryChange> change;if(auto status=plan.Take(change))return status;if(!change)return MMS_OK;
        auto* history=BoundNativePoseHistory(&app);if(!history)return MMS_E_STALE;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if(auto status=history->PrepareNative(label,std::move(change),transaction,SharedHistory::NativeIntent::ExplicitCommit))return status;
        if(auto status=history->CommitNative(std::move(transaction)))return status;changed=true;
        Boundary([&]()->MmsResult{bake::ScopedUiInputGate gate;PublishBoundPoseEdit(app,*history,false);RefreshBoundAudioUi(app);return MMS_OK;});return MMS_OK;
    });
}
}
MmsResult ImportBoundAudio(MMDApp& app,const wchar_t* path,bool& changed)noexcept{
    changed=false;NativeAudioPlan plan;if(auto status=plan.Begin(app))return status;if(auto status=plan.Read(path))return status;
    return Commit(app,plan,changed,"Import audio");
}
bool TryLoadBoundAudio(MMDApp* app,const wchar_t* path,bool rememberDirectory)noexcept{
    if(!app||!BoundNativePoseHistory(app))return false;
    const auto result=Boundary([&]()->MmsResult{if(!path||!*path)return MMS_E_ARGUMENT;const auto folder=rememberDirectory?Parent(path):std::wstring{};
        if(folder.size()>=kPathFieldCapacity)return MMS_E_ARGUMENT;bool changed=false;const auto status=ImportBoundAudio(*app,path,changed);
        if(status==MMS_OK&&changed&&rememberDirectory)wcscpy_s(app->DirWave(),kPathFieldCapacity,folder.c_str());return status;});
    if(result)ShowError(*app,result);return true;
}
bool TryPickBoundAudio(MMDApp* app)noexcept{
    if(!app||!BoundNativePoseHistory(app))return false;if(picking)return true;
    struct Scope{Scope(){picking=true;}~Scope(){picking=false;}} scope;
    const auto result=Boundary([&]()->MmsResult{
        NativeAudioPlan plan;if(auto status=plan.Begin(*app))return status;
        const bool remember=Remember(*app);const auto base=std::filesystem::path(app->ExeDir());
        auto initial=remember?std::filesystem::path(app->DirWave()):std::filesystem::path(L"UserFile/Wave");if(initial.is_relative())initial=base/initial;
        const auto folder=std::filesystem::absolute(initial).lexically_normal().native();wchar_t path[256]{};
        OPENFILENAMEW picker{};picker.lStructSize=sizeof picker;picker.hwndOwner=app->FloatingWindow()?app->FloatingWindow():app->MainWindow();
        picker.lpstrFilter=L"Audio (*.wav;*.mp3;*.aac;*.adts;*.m4a;*.wma)\0*.wav;*.mp3;*.aac;*.adts;*.m4a;*.wma\0";
        picker.lpstrFile=path;picker.nMaxFile=256;picker.lpstrInitialDir=folder.c_str();picker.lpstrDefExt=L"wav";
        picker.lpstrTitle=app->EnglishUI()==1?L"Open audio":app->EnglishUI()==2?L"载入音频":L"音声ファイルを開く";picker.Flags=OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
        std::array<wchar_t,32768> original{};if(!GetCurrentDirectoryW(DWORD(original.size()),original.data()))return MMS_E_FAILED;
        struct Directory{const wchar_t* path;~Directory(){SetCurrentDirectoryW(path);}} restore{original.data()};
        app->state.enterKeyState=1;const bool selected=GetUiOpenFileNameW(&picker)!=FALSE;
        if(!SetCurrentDirectoryW(original.data()))return MMS_E_FAILED;if(!selected)return MMS_OK;
        const auto parent=remember?Parent(path):std::wstring{};if(parent.size()>=kPathFieldCapacity)return MMS_E_ARGUMENT;
        if(auto status=plan.Read(path))return status;bool changed=false;const auto status=Commit(*app,plan,changed,"Import audio");
        if(status==MMS_OK&&changed&&remember)wcscpy_s(app->DirWave(),kPathFieldCapacity,parent.c_str());return status;
    });if(result)ShowError(*app,result);return true;
}
bool TryClearBoundAudio(MMDApp* app)noexcept{
    if(!app||!BoundNativePoseHistory(app))return false;
    const auto result=Boundary([&]()->MmsResult{NativeAudioPlan plan;if(auto status=plan.Begin(*app))return status;
        if(auto status=plan.Clear())return status;bool changed=false;return Commit(*app,plan,changed,"Remove audio");});
    if(result)ShowError(*app,result);return true;
}
bool RefreshBoundAudioUi(MMDApp& app)noexcept{
    auto* audio=app.Audio();if(!audio||!wave_audio::NeedsRefresh(audio))return true;
    if(g_Block!=&app||!audio->timelineDC||!app.MainWindow()||!IsWindow(app.MainWindow()))return false;
    return Boundary([&]()->MmsResult{TimelineDrawTicks(app.TimelineStartFrame(),app.SidebarWidth());
        RECT area{6,0x5f,app.SidebarWidth()-3,0x97};if(!InvalidateRect(app.MainWindow(),&area,FALSE))return MMS_E_FAILED;
        wave_audio::Refreshed(audio);return MMS_OK;})==MMS_OK;
}
void ClearLegacyAudio(MMDApp* app)noexcept{
    if(!app)return;Boundary([&]()->MmsResult{ClearWaveAudio(app);app->SceneModified()=1;
        if(g_Block==app&&app->Audio()&&app->Audio()->timelineDC)TimelineDrawTicks(app->TimelineStartFrame(),app->SidebarWidth());
        if(app->MainWindow())InvalidateRect(app->MainWindow(),nullptr,FALSE);return MMS_OK;});
}
}
#endif
