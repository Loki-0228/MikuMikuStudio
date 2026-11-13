#include "mikumikustudio/negative_frames.hpp"
#define NOMINMAX
#include "mikumikustudio/save_compatibility.hpp"
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/original_file_encoding.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include <Windows.h>
#include <array>
#include <filesystem>
#include <map>
#include <set>
#include <type_traits>
#include <algorithm>

namespace mikumikustudio::save_compatibility {
namespace {
const wchar_t* Text(const Report& r,const wchar_t* zh,const wchar_t* en,const wchar_t* ja) {
    return r.language==2?zh:r.language==0?ja:en;
}
void Detail(Report& report,const std::wstring& reason) {
    if(std::find(report.details.begin(),report.details.end(),reason)!=report.details.end())return;
    if(report.details.size()<6)report.details.push_back(reason);else ++report.omittedDetails;
}
void Invalid(Report& report,const std::wstring& reason) {
    report.invalidData=true;
    if(report.reason.empty())report.reason=reason;
}
void InspectFrame(Report& report,std::int64_t frame,const wchar_t* field) {
    if(frame<0 || frame>frame_limits::maximum)Invalid(report,field);
    else if(frame>frame_limits::legacyMaximum) {
        report.needsNewPmm=true;
        Detail(report,std::wstring(field)+L" = "+std::to_wstring(frame)+Text(report,L"：超过原版帧号上限。",L": exceeds the original frame limit.",L"：元のフレーム上限を超えています。"));
    }
}
template<class K> void InspectKeys(Report& report,const K* keys,std::size_t dense,
                                  std::size_t capacity,std::size_t original,const std::wstring& track) {
    if(dense>capacity || (dense && !keys)) {
        Invalid(report,track+L": missing dense keys");
        return;
    }
    const auto live=[&](std::uint32_t index) {
        return index<capacity && (index<dense || keys[index].frame!=0);
    };
    std::size_t used=0;
    bool extended=false,frameReported=false;
    for(std::size_t i=0;keys&&i<capacity;++i){
        if(i>=dense&&!keys[i].frame)continue;
        // A newer header cannot make an invalid reference or frame readable.
        // Check every live key, even after the legacy capacity is exceeded.
        // The initial key's previous field is a legacy deletion sentinel,
        // not a link. The writer clears it without changing the live pool.
        const bool initial=i<dense && keys[i].frame==0;
        if((!initial && !live(keys[i].previous)) || !live(keys[i].next) || keys[i].frame>frame_limits::maximum) {
            Invalid(report,track+L": invalid key "+std::to_wstring(i));
            return;
        }
        if(keys[i].frame>frame_limits::legacyMaximum && !frameReported) {
            Detail(report,track+L" / "+Text(report,L"关键帧 ",L"key ",L"キー ")+std::to_wstring(i)+L" = "+std::to_wstring(keys[i].frame)+
                Text(report,L"：帧号超过原版上限。",L": frame exceeds the original limit.",L"：フレームが元の上限を超えています。"));frameReported=true;
        }
        extended|=++used>original || keys[i].frame>frame_limits::legacyMaximum;
    }
    if(used>original)Detail(report,track+L": "+std::to_wstring(used)+L" / "+std::to_wstring(original)+
        Text(report,L"（关键帧数 / 原版上限）",L" (keys / original limit)",L"（キー数 / 元の上限）"));
    report.needsNewPmm|=extended;
}
void InspectPath(Report& report,const wchar_t* path,const std::wstring& field){
    char bytes[256]{};
#if MMD_ORIGINAL_PROFILE
    const bool encoded=original_file_encoding::Fixed(bytes,sizeof bytes,path);
#else
    const bool encoded=text_encoding::EncodeProjectPath(bytes,sizeof bytes,path);
#endif
    if(!encoded){
        report.needsNewPmm=true;
        report.canCopyLegacy=false;
        Detail(report,field+Text(report,
            L"无法无损写入 255 字节路径字段：",L" cannot fit losslessly in the 255-byte path field: ",
            L"を255バイトのパス欄に損失なく保存できません：")+path);
    } else if(!text_encoding::IsLegacyPathEncoding(bytes,path)){
        // The wire bytes matter here: a CP932 spelling may exist but would
        // point to a different resource when the complete PMM is reopened.
        report.canCopyLegacy=false;
        Detail(report,field+Text(report,
            L"需要 MMS 的路径编码：",L" requires the MMS path encoding: ",L"にはMMSのパスエンコードが必要です：")+path);
    }
}
bool RawName(const char* value,std::size_t size,std::set<std::string>& seen){
    const std::string bytes(value,strnlen_s(value,size));std::wstring wide;std::string roundtrip;
    if(text_encoding::HasUtf8Bom(bytes.c_str())||!text_encoding::Decode(bytes.c_str(),932,wide)||
       !text_encoding::EncodeExact(wide.c_str(),932,roundtrip)||roundtrip!=bytes)return false;
    return seen.insert(bytes).second;
}
// Exempt a collision only when every member has just its unchanged initial
// key. Never infer this from the physics flag or discard any saved records.
bool DefaultKey(const mdl::BoneKey& key) {
    if(key.frame || key.previous || key.next || key.physicsDisabled)return false;
    for(float value:key.position)if(value!=0)return false;
    for(int i=0;i<4;++i)if(key.rotation[i]!=(i==3?1.0f:0.0f))return false;
    for(int i=0;i<16;++i)if(key.interpolation[i]!=(i<8?20:107))return false;
    return true;
}
bool DefaultKey(const mdl::MorphKey& key) {
    return !key.frame && !key.previous && !key.next && key.value==0;
}
template<class R,class K>
std::set<std::string> DefaultBindings(const R* records,unsigned count,const K* keys,std::size_t capacity) {
    std::map<std::string,bool> groups;
    if(!records||!keys||count>capacity)return {};
    for(unsigned i=0;i<count;++i) {
        const std::string name(records[i].name,strnlen_s(records[i].name,sizeof records[i].name));
        const auto [it,inserted]=groups.emplace(name,true);
        it->second=it->second && DefaultKey(keys[i]);
    }
    std::set<std::string> result;
    for(const auto& [name,unused]:groups)if(unused && !name.empty())result.insert(name);
    return result;
}
bool PmmNamesFit(const mdl::ModelRecord& model,std::wstring& issue,unsigned lang) {
    const auto text=[&](const wchar_t* zh,const wchar_t* en,const wchar_t* ja){return lang==2?zh:lang==0?ja:en;};
    const auto prefix=std::wstring(text(L"模型 ",L"Model: ",L"モデル "))+text_encoding::ModelName(model,false)+L" / ";
    // PMM stores both model captions as length-prefixed strings (255 bytes).
    // Bone/morph English captions and display-group names come from the model
    // file; they are not the short identity fields saved in the PMM.
    std::string bytes;
    for(bool english : {false,true})
    {
        char caption[256]{};
        if(!text_encoding::EncodeDisplay(caption,sizeof caption,text_encoding::ModelName(model,english).c_str())) {
            issue=prefix+text(L"模型名称超过 255 字节。",L"Model caption exceeds 255 bytes.",L"モデル名が255バイトを超えています。");return false;
        }
    }
    const auto fits=[&](const auto& record,const std::wstring& fullName,
                        std::map<std::string,std::string>& identities,const std::set<std::string>& defaults,const wchar_t* kind) {
        const std::string identity(record.name,strnlen_s(record.name,sizeof record.name));
        std::string unicode;
        if(!text_encoding::EncodeExact(fullName.c_str(),CP_UTF8,unicode)) {
            issue=prefix+kind+L"「"+fullName+L"」"+text(L"包含无效的 Unicode 字符。",L" contains invalid Unicode.",L"に無効なUnicode文字があります。");return false;
        }
        const auto [found,inserted]=identities.emplace(identity,unicode);
        if(!inserted && found->second!=unicode && !defaults.count(identity)) {
            issue=prefix+kind+L"「"+fullName+L"」"+text(L"与另一名称共用同一个旧绑定名。",L" shares a legacy binding name with another entry.",L"が別の名前と同じ旧識別名を使用しています。");return false;
        }
        // PMD may occupy all twenty bytes. Otherwise require the exact current
        // PMX producer token, not an arbitrary prefix or a fabricated alias.
        std::string legacy;
        if(text_encoding::EncodeExact(fullName.c_str(),932,legacy) && legacy==identity)return true;
        char canonical[sizeof record.name]{};
        text_encoding::EncodePmxName(canonical,sizeof canonical,fullName.c_str());
        if(!identity.empty() && identity==std::string(canonical,strnlen_s(canonical,sizeof canonical)))return true;
        issue=prefix+kind+L"「"+fullName+L"」"+(identity.empty()?
            text(L"的旧绑定名为空，原版无法按名称区分。",L" has an empty legacy binding name.",L"の旧識別名が空です。"):
            text(L"的旧绑定名与原名不一致。",L" has a legacy binding name that differs from its source name.",L"の旧識別名が元の名前と一致しません。"));
        return false;
    };
    std::map<std::string,std::string> bones,morphs;
    const auto defaultBones=DefaultBindings(model.boneTable,model.boneCount,model.boneKeys,mdl::BoneKeyCapacity(model));
    const auto defaultMorphs=DefaultBindings(model.morphs,model.morphCount,model.morphKeys,mdl::MorphKeyCapacity(model));
    for(unsigned i=0;i<model.boneCount;++i)
        if(!fits(model.boneTable[i],text_encoding::BoneName(model.boneTable[i],false),bones,defaultBones,text(L"骨骼",L"bone ",L"ボーン")))return false;
    for(unsigned i=0;i<model.morphCount;++i)
        if(!fits(model.morphs[i],text_encoding::MorphName(model.morphs[i],false),morphs,defaultMorphs,text(L"表情",L"morph ",L"モーフ")))return false;
    return true;
}
}
Report InspectPmm(MMDApp& app){
    Report report;report.language=app.EnglishUI();
    const auto signedFrame=[&](std::int64_t frame,const wchar_t* field) {
        if(frame<frame_limits::minimum || frame>frame_limits::maximum)Invalid(report,field);
        else if(frame<0){report.canCopyLegacy=false;
            Detail(report,std::wstring(field)+L" = "+std::to_wstring(frame)+Text(report,L"：负数帧仅保留在完整工程中。",L": negative frame is retained in the complete project only.",L"：負のフレームは完全な工程にのみ保存されます。"));}
        else InspectFrame(report,frame,field);
    };
    signedFrame(app.CurrentFrame(),Text(report,L"当前帧",L"Current frame",L"現在フレーム"));
    signedFrame(app.TimelineStartFrame(),Text(report,L"时间轴起始帧",L"Timeline start frame",L"タイムライン開始フレーム"));
    if(const auto count=negative_frames::Count(&app)) {
        report.canCopyLegacy=false;Detail(report,Text(report,L"负数关键帧数量：",L"Negative keyframe count: ",L"負のキーフレーム数：")+std::to_wstring(count));
    }
    InspectFrame(report,app.LastRegisteredFrame(),Text(report,L"最后登记帧",L"Last registered frame",L"最終登録フレーム"));
    std::int32_t bounds[2]{};bool validBounds=true;
    for(unsigned i=0;i<2;++i) {
        char text[256]{};
        GetWindowTextA(GetDlgItem(app.MainWindow(),i?panel::kPlayStopFrameEdit:panel::kPlayStartFrameEdit),text,sizeof text);
        if(!frame_limits::ParsePlaybackBound(text,bounds[i])) {
            Invalid(report,L"playback range");validBounds=false;
        }
    }
    if(validBounds) {
        std::int32_t from=0,to=0;
        // An inverted pair uses the right value as a distance from the end.
        // Ordered negative endpoints still mean literal negative frames.
        const bool relative=bounds[0]>bounds[1] && frame_limits::ResolveRange(
            bounds[0],bounds[1],app.LastRegisteredFrame(),from,to);
        signedFrame(bounds[0],Text(report,L"播放起始帧",L"Playback start",L"再生開始"));
        signedFrame(relative?to:bounds[1],Text(report,L"播放结束帧",L"Playback end",L"再生終了"));
    }
    for(int slot=0;slot<kModelSlotCount;++slot){
        const auto* raw=app.ModelSlot(slot);if(!raw)continue;const auto& m=*mdl::Mdl(raw);
        const auto track=std::wstring(Text(report,L"模型 ",L"Model ",L"モデル "))+std::to_wstring(slot)+L"「"+text_encoding::ModelName(m,false)+L"」";
        InspectKeys(report,m.boneKeys,m.boneCount,mdl::BoneKeyCapacity(m),mdl::kBoneKeyCapacity,track+Text(report,L" / 骨骼关键帧",L" / bone keys",L" / ボーンキー"));
        InspectKeys(report,m.morphKeys,m.morphCount,mdl::MorphKeyCapacity(m),mdl::kMorphKeyCapacity,track+Text(report,L" / 表情关键帧",L" / morph keys",L" / モーフキー"));
        InspectKeys(report,m.displayKeys,1,mdl::KeyPoolCapacity(m.displayKeys),mdl::kDisplayKeyCapacity,track+Text(report,L" / 显示与 IK 关键帧",L" / display/IK keys",L" / 表示・IKキー"));
        InspectFrame(report,m.maxFrame,(track+Text(report,L" / 时间轴上限",L" / timeline limit",L" / タイムライン上限")).c_str());
        for(const auto group:{true,false}) {
            const auto count=group?mdl::DisplayGroupCount(m):mdl::FacialFrameCount(m);
            if(count>255){report.needsNewPmm=true;Detail(report,track+(group?Text(report,L" / 显示分组",L" / display groups",L" / 表示枠"):
                Text(report,L" / 表情显示项",L" / facial rows",L" / 表情表示項目"))+L": "+std::to_wstring(count)+L" / 255");}
        }
        std::wstring issue;
        if(!PmmNamesFit(m,issue,app.EnglishUI())) {
            report.namesNeedExtension=true;
            if(report.nameIssue.empty())report.nameIssue=issue;
            Detail(report,issue);
        }
        const auto bindings=[&](const auto* records,unsigned count,bool bone) {
            const auto defaults=[&] {
                if constexpr(std::is_same_v<std::decay_t<decltype(*records)>,mdl::BoneRecord>)
                    return DefaultBindings(records,count,m.boneKeys,mdl::BoneKeyCapacity(m));
                else return DefaultBindings(records,count,m.morphKeys,mdl::MorphKeyCapacity(m));
            }();
            std::set<std::string> seen;std::map<std::string,std::wstring> first;
            for(unsigned i=0;i<count;++i) {
                const auto& record=records[i];
                const auto name=[&]{if constexpr(std::is_same_v<std::decay_t<decltype(record)>,mdl::BoneRecord>)
                    return text_encoding::BoneName(record,false);else return text_encoding::MorphName(record,false);}();
                const auto field=(bone?Text(report,L"骨骼",L"bone ",L"ボーン"):Text(report,L"表情",L"morph ",L"モーフ"))+
                    std::wstring(L"[")+std::to_wstring(i)+L"]「"+name+L"」";
                const std::string bytes(record.name,strnlen_s(record.name,20));
                const auto [previous,inserted]=first.emplace(bytes,field);
                std::wstring legacyName;std::string legacyRoundtrip;
                const bool cp932=!bytes.empty() && text_encoding::Decode(bytes.c_str(),932,legacyName) &&
                    text_encoding::EncodeExact(legacyName.c_str(),932,legacyRoundtrip) && legacyRoundtrip==bytes;
                // Valid CP932-looking bytes alone do not prove that the
                // original PMX loader produces the same identity. Its old CRT
                // fallback may yield an empty/different token for Unicode.
                std::string fullLegacy;
                const bool originalToken=(text_encoding::EncodeExact(name.c_str(),932,fullLegacy) && fullLegacy==bytes) ||
                    text_encoding::MatchesOldPmxName(record.name,name.c_str(),sizeof record.name);
                if(!originalToken){
                    report.canCopyLegacy=false;
                    Detail(report,track+L" / "+field+Text(report,
                        L"：原版读取器会产生不同的绑定名，仅完整工程可保留此绑定。",
                        L": the original loader produces a different binding; only the complete project preserves this identity.",
                        L"：元のローダーでは別の識別名になるため、完全な工程にのみ保持されます。"));
                }
                const bool rawValid=RawName(record.name,20,seen);
                if(!rawValid && !(defaults.count(bytes) && cp932)) {
                    report.canCopyLegacy=false;
                    Detail(report,track+L" / "+field+(inserted?
                        Text(report,L"：旧绑定名字节不是有效的原版编码。",L": legacy binding bytes are not valid original encoding.",L"：旧識別名のバイトが元の文字コードでは無効です。"):
                        std::wstring(Text(report,L"：旧绑定名重复，与 ",L": duplicate legacy binding, also used by ",L"：旧識別名が重複しています："))+previous->second));
                }
            }
        };
        bindings(m.boneTable,m.boneCount,true);bindings(m.morphs,m.morphCount,false);
        InspectPath(report,m.path,track+Text(report,L" / 模型路径",L" / model path",L" / モデルパス"));
        std::set<std::string> modelName;
        if(!RawName(m.name,sizeof(m.name),modelName)){report.canCopyLegacy=false;
            Detail(report,track+Text(report,L" / 模型短名称不是有效的原版编码。",L" / short model name is not valid original encoding.",L" / 短いモデル名が元の文字コードでは無効です。"));}
    }
    InspectKeys(report,app.CameraKeys(),1,mdl::KeyPoolCapacity(app.CameraKeys()),10000,Text(report,L"相机关键帧",L"camera keys",L"カメラキー"));
    InspectKeys(report,app.LightKeys(),1,mdl::KeyPoolCapacity(app.LightKeys()),10000,Text(report,L"照明关键帧",L"light keys",L"照明キー"));
    InspectKeys(report,app.ShadowKeys(),1,mdl::KeyPoolCapacity(app.ShadowKeys()),10000,Text(report,L"阴影关键帧",L"shadow keys",L"影キー"));
    InspectKeys(report,app.GravityKeys(),1,mdl::KeyPoolCapacity(app.GravityKeys()),10000,Text(report,L"重力关键帧",L"gravity keys",L"重力キー"));
    for(int slot=0;slot<255;++slot){
        if(!app.AccessorySlot(slot))continue;
        InspectKeys(report,app.AccessoryKeys(slot),1,mdl::KeyPoolCapacity(app.AccessoryKeys(slot)),10000,Text(report,L"附件关键帧 ",L"Accessory keys ",L"アクセサリキー ")+std::to_wstring(slot));
        InspectPath(report,app.AccessorySlot(slot)->sourcePath,
            Text(report,L"附件路径 ",L"Accessory path ",L"アクセサリパス ")+std::to_wstring(slot));
    }
    const wchar_t* paths[]{app.WavePath(),app.AviBackgroundPath(),app.PictureBackgroundPath()};
    const wchar_t* labels[]{Text(report,L"音频路径",L"Audio path",L"音声パス"),Text(report,L"背景视频路径",L"Background video path",L"背景動画パス"),Text(report,L"背景图片路径",L"Background image path",L"背景画像パス")};
    for(unsigned i=0;i<3;++i)InspectPath(report,paths[i],labels[i]);
    if(report.needsNewPmm||report.invalidData)report.canCopyLegacy=false;
    return report;
}
Report InspectMotion(const mdl::ModelRecord& m){
    Report report;report.namesNeedExtension=!extended_names::LegacyNamesFit(m,true);
    std::set<std::string> bones,morphs;
    for(unsigned i=0;i<m.boneCount;++i)
        if(strnlen_s(m.boneTable[i].name,20)>15||!RawName(m.boneTable[i].name,15,bones))report.canCopyLegacy=false;
    for(unsigned i=0;i<m.morphCount;++i)
        if(strnlen_s(m.morphs[i].name,20)>15||!RawName(m.morphs[i].name,15,morphs))report.canCopyLegacy=false;
    return report;
}
bool CanSavePmd(const mdl::ModelRecord& m) {
    const auto fixedGroups = m.displayRootBone ? 3u : 2u;
    if (mdl::DisplayGroupCount(m) > 255 || mdl::FacialFrameCount(m) > 255 ||
        m.boneCount > 65535 || m.morphCount > 65535 ||
        !extended_names::CanSavePmdNames(m)) return false;
    const auto* rows = static_cast<const mdl::FrameGroup*>(m.rbGroups);
    for (std::uint32_t i = 0; rows && i < m.rigidBodyCount; ++i)
        if (mdl::FrameGroupIndex(rows[i]) >= 255 + fixedGroups ||
            mdl::FrameTargetIndex(rows[i]) > 65535) return false;
    return true;
}
bool CopyPrefix(const wchar_t* source,std::uint64_t bytes,std::wstring& destination){
    try {
    destination.clear();const std::filesystem::path original(source);
    HANDLE input=CreateFileW(source,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(input==INVALID_HANDLE_VALUE)return false;
    HANDLE output=INVALID_HANDLE_VALUE;std::filesystem::path path;
    struct Handles {
        HANDLE& input; HANDLE& output;
        ~Handles(){if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);}
    } handles{input,output};
    for(std::uint32_t suffix=0;suffix<UINT32_MAX;++suffix){
        path=original.parent_path()/(original.stem().wstring()+L".compat"+(suffix?L"."+std::to_wstring(suffix):L"")+original.extension().wstring());
        output=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(output!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_FILE_EXISTS)break;
    }
    bool success=output!=INVALID_HANDLE_VALUE;std::array<std::uint8_t,65536> buffer{};
    while(success&&bytes){const DWORD want=static_cast<DWORD>((std::min)(bytes,std::uint64_t(buffer.size())));DWORD got=0,written=0;
        success=ReadFile(input,buffer.data(),want,&got,nullptr)&&got==want&&WriteFile(output,buffer.data(),got,&written,nullptr)&&written==got;bytes-=got;}
    if(success)success=FlushFileBuffers(output)!=FALSE;
    CloseHandle(input);input=INVALID_HANDLE_VALUE;if(output!=INVALID_HANDLE_VALUE){CloseHandle(output);output=INVALID_HANDLE_VALUE;if(!success)DeleteFileW(path.c_str());}
    if(success)destination=path.wstring();return success;
    } catch (const std::exception&) { return false; }
}
std::wstring Describe(const Report& report,const std::wstring& copy,bool attempted){
    const unsigned lang=report.language<=2?report.language:1;
    const wchar_t* saved[]={L"完全なデータを保存しました。元の MMD 形式に収まらない項目があります。",L"The complete file was saved. Some fields cannot be fully represented in original MMD format.",L"完整文件已保存。部分字段不能完整写入原版 MMD 格式。"};
    const wchar_t* copied[]={L"\n互換コピーを保存しました。名前は旧形式の短い表記です。\n",L"\nA compatible copy was saved using the legacy short names.\n",L"\n已自动保存兼容副本，其中名称采用旧格式的短名称。\n"};
    const wchar_t* refused[]={L"\n互換コピーは作成していません。完全なファイルは MMS で編集を続けられます。",L"\nNo compatible copy was created. You can continue editing the complete file in MMS.",L"\n未生成兼容副本；完整文件仍可在 MMS 中继续编辑。"};
    const wchar_t* failed[]={L"\n互換コピーの書き込みに失敗しました。完全なファイルは保存されています。",L"\nWriting the compatible copy failed. The complete file was saved.",L"\n兼容副本写入失败，完整文件已保存。"};
    std::wstring message=saved[lang];message+=!copy.empty()?copied[lang]:attempted?failed[lang]:refused[lang];message+=copy;
    for(const auto& detail:report.details)message+=L"\n\n• "+detail;
    if(report.omittedDetails)message+=L"\n\n"+std::to_wstring(report.omittedDetails)+Text(report,L" 项其他字段存在相同限制。",L" additional fields have compatibility limits.",L" 件のフィールドにも互換性の制限があります。");
    return message;
}
void Notify(MMDApp& app,const Report& report,const std::wstring& copy,bool attempted){
    if(!report.needsNewPmm&&!report.namesNeedExtension)return;
    auto translated=report;translated.language=app.EnglishUI();
    const auto message=Describe(translated,copy,attempted);
    if(app.MainWindow())MessageBoxW(app.MainWindow(),message.c_str(),L"MMS",MB_OK|MB_ICONINFORMATION);
}
}
