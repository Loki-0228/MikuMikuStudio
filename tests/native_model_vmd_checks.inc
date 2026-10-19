#include "../src/plugins/native_model_vmd_source.hpp"
namespace {
std::uint32_t ModelVmdTestCrc(const VmdDecodeBytes& bytes){
    std::uint32_t crc=~0u;for(auto byte:bytes){crc^=byte;for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1u)?0xedb88320u:0u);}return ~crc;
}
VmdDecodeBytes ModelVmdWithNames(VmdDecodeBytes bytes,const extended_names::MotionNames& names){
    const auto tail=extended_names::EncodeMotion(names,bytes.size(),ModelVmdTestCrc(bytes));bytes.insert(bytes.end(),tail.begin(),tail.end());return bytes;
}
const mdl::BoneKey* ModelVmdBoneAt(const mdl::ModelRecord& model,unsigned bone,unsigned frame){
    unsigned slot=bone;for(std::size_t visited=0;visited<mdl::BoneKeyCapacity(model.boneKeys);++visited){
        const auto& key=model.boneKeys[slot];if(key.frame==frame)return &key;if(!key.next)return nullptr;slot=key.next;
    }return nullptr;
}
VmdDecodeBytes ModelVmdFixtureBytes(int version=2){
    auto bytes=VmdDecodeMotion(version).bytes;std::memset(bytes.data()+30,0,version==1?10:20);std::memcpy(bytes.data()+30,"fixture",7);return bytes;
}
void ModelVmdDistinctCurveChecks(){
    using namespace plugins;ModelPasteFixture fixture;auto bytes=ModelVmdFixtureBytes();const std::size_t boneStart=54;
    std::memset(bytes.data()+boneStart,0,15);std::memcpy(bytes.data()+boneStart,fixture.second.bones[0].name,15);
    // Distinct values in every meaningful cell make channel/control transposes observable.
    for(unsigned channel=0;channel<4;++channel)for(unsigned control=0;control<4;++control)
        bytes[boneStart+47+channel*16+control*4]=std::uint8_t(10+control*25+channel);
    VmdDecodeFile file;file.Write(bytes);ModelVmdSource source;
    Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,source)==MMS_OK,"distinct model VMD interpolation decodes");
    mdl::BoneClipboardRecord legacy{};std::memcpy(legacy.name,bytes.data()+boneStart,15);std::memcpy(&legacy.frame,bytes.data()+boneStart+15,4);
    std::memcpy(legacy.position,bytes.data()+boneStart+19,12);std::memcpy(legacy.rotation,bytes.data()+boneStart+31,16);
    // This follows the old loader's sequential 4-byte reads, independent of the new offset table.
    for(unsigned channel=0;channel<4;++channel)for(unsigned control=0;control<4;++control)
        legacy.interpolation[channel+4*control]=bytes[boneStart+47+channel*16+control*4];
    Check(!std::memcmp(legacy.interpolation,source.source.bones[0].key.interpolation,16),"all sixteen distinct interpolation bytes match old VMD reader");
    std::shared_ptr<HistoryChange> change;Check(PlanModelVmdImport(*fixture.app,0,source,change)==MMS_OK&&change,"distinct curve model import plans");
    ModelPasteCommit(fixture,change);ResetBoneKeyCursor(fixture.second.raw());
    {bone_scale::Registration scale(bone_scale::Matrix{},bone_scale::LinearCurve());
        Check(RegisterBoneKey(fixture.second.raw(),reinterpret_cast<unsigned char*>(&legacy),5,0),"real registrar accepts old VMD decoded curve");}
    const auto* actual=ModelVmdBoneAt(*fixture.first.m,0,12);const auto* oracle=ModelVmdBoneAt(*fixture.second.m,0,12);
    Check(actual&&oracle&&!std::memcmp(actual->interpolation,oracle->interpolation,16),"imported sixteen-channel controls match real legacy registrar");
}
void ModelVmdUnicodeChecks(){
    using namespace plugins;ModelPasteFixture fixture;auto& app=*fixture.app;
    const std::wstring a=L"\u9aa8\u9abc\u957f\u540d\u7532",b=L"\u9aa8\u9abc\u957f\u540d\u4e59",m=L"\u8868\u60c5\u957f\u540d";
    auto* oldA=fixture.first.bones[0].jpText;auto* oldB=fixture.first.bones[1].jpText;auto* oldM=fixture.first.morphs[0].jpText;
    struct Restore {ModelPasteFixture& f;wchar_t *a,*b,*m;~Restore(){f.history.Reset();f.history.Collect();f.first.bones[0].jpText=a;f.first.bones[1].jpText=b;f.first.morphs[0].jpText=m;}} restore{fixture,oldA,oldB,oldM};
    fixture.first.bones[0].jpText=const_cast<wchar_t*>(a.c_str());fixture.first.bones[1].jpText=const_cast<wchar_t*>(b.c_str());fixture.first.morphs[0].jpText=const_cast<wchar_t*>(m.c_str());
    std::memcpy(fixture.first.bones[1].name,fixture.first.bones[0].name,20);
    auto standard=ModelVmdFixtureBytes();VmdDecodeSet(standard,standard.size()-4,2u);
    VmdDecodeText(standard,"placeholder",20);standard.push_back(1);VmdDecodeText(standard,"placeholder",20);standard.push_back(0);
    extended_names::MotionNames names;names.model=L"fixture";names.bones={b,a};names.morphs={m};names.boneOwners={0};names.morphOwners={0};names.ikOwners={UINT32_MAX,1};
    auto bytes=ModelVmdWithNames(standard,names);VmdDecodeFile file;file.Write(bytes);ModelVmdSource source;
    Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,source)==MMS_OK&&source.hasUnicodeNames,"checksum-bound Unicode model dictionary decodes");
    auto resolved=source;Check(ResolveModelVmdTargets(app,0,resolved)==MMS_OK&&resolved.source.bones[0].targetIndex==1&&resolved.source.morphs[0].targetIndex==0&&
        resolved.source.displays[0].ikTargets==std::vector<int>({-1,0}),"Unicode identities resolve exact bone morph and nullable IK owners despite encoded-name collision");
    const ModelKeyState before(fixture.first);const auto digest=fixture.first.Digest();std::shared_ptr<HistoryChange> change;
    Check(PlanModelVmdImport(app,0,source,change)==MMS_OK&&change&&fixture.first.Digest()==digest,"Unicode model import plans without native mutation");
    ModelPasteCommit(fixture,change);const auto* bone=ModelVmdBoneAt(*fixture.first.m,1,12);
    Check(bone&&bone->position[0]==1,"Unicode resolved import writes intended bone with aliased encoded name");
    bool morphFound=false,displayFound=false;for(std::size_t i=0;i<mdl::MorphKeyCapacity(fixture.first.m->morphKeys);++i){const auto& key=fixture.first.m->morphKeys[i];if(key.frame==14&&key.value==.25f)morphFound=true;}
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(fixture.first.m->displayKeys);++i){const auto& key=fixture.first.m->displayKeys[i];if(key.frame==24&&key.ikStates&&!key.ikStates[0])displayFound=true;}
    Check(morphFound&&displayFound,"Unicode import publishes morph and mapped IK payloads in the same transaction");
    const ModelKeyState after(fixture.first);source={};Check(fixture.history.History(false)==MMS_OK&&before.Matches(fixture.first)&&fixture.history.History(true)==MMS_OK&&after.Matches(fixture.first),"Unicode imported identities and payloads survive source destruction undo redo");
    auto assertMapping=[&](const extended_names::MotionNames& dictionary,int expected){file.Write(ModelVmdWithNames(standard,dictionary));ModelVmdSource item;
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,item)==MMS_OK&&ResolveModelVmdTargets(app,0,item)==MMS_OK&&item.source.bones[0].targetIndex==expected,"Unicode absent or ambiguous targets are explicitly skipped");};
    auto missing=names;missing.bones[0]=L"absent";assertMapping(missing,-1);
    fixture.first.bones[1].jpText=const_cast<wchar_t*>(a.c_str());auto ambiguous=names;ambiguous.bones[0]=a;assertMapping(ambiguous,-1);fixture.first.bones[1].jpText=const_cast<wchar_t*>(b.c_str());
    bytes.back()^=1;file.Write(bytes);ModelVmdSource unchanged;unchanged.version=99;
    Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,unchanged)!=MMS_OK&&unchanged.version==99,"damaged Unicode checksum preserves decoder output");
    auto mismatched=names;mismatched.boneOwners.clear();file.Write(ModelVmdWithNames(standard,mismatched));
    Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,unchanged)!=MMS_OK&&unchanged.version==99,"Unicode dictionary owner count mismatch rejects complete standard payload atomically");
}

VmdDecodeBytes ModelVmdWithScale(VmdDecodeBytes prefix,bone_scale_io::VmdExtension extension){
    bone_scale_io::PrefixFingerprint fingerprint;fingerprint.Append(prefix.data(),prefix.size());
    extension.prefixBytes=fingerprint.bytes();extension.prefixCrc32=fingerprint.crc32();
    const auto payload=bone_scale_io::EncodeExtension(extension);prefix.insert(prefix.end(),payload.begin(),payload.end());
    VmdDecodePut(prefix,std::uint64_t(payload.size()));prefix.insert(prefix.end(),{'M','M','S','X','V','M','D','X'});return prefix;
}
void NativeModelVmdScaleChecks(){
    using namespace plugins;
    std::puts("  model VMD scale: immutable tail codec, offsets and atomic history");
    for(bool curveOnly:{false,true})for(int version:{1,2}){
        ModelPasteFixture fixture;auto bytes=ModelVmdFixtureBytes(version);const auto boneStart=std::size_t(version==1?44:54);
        std::memset(bytes.data()+boneStart,0,15);std::memcpy(bytes.data()+boneStart,fixture.first.bones[0].name,15);
        bone_scale_io::VmdExtension extension;bone_scale_io::VmdExtension::Entry entry;
        std::memcpy(entry.name.data(),bytes.data()+boneStart,15);entry.frame=7;entry.scale=curveOnly?bone_scale::Matrix{}:Scale(2.25f);
        entry.curve={7,29,88,119};extension.entries.push_back(entry);
        auto unmatched=entry;unmatched.frame=999;unmatched.scale=Scale(13);extension.entries.push_back(unmatched);
        unmatched=entry;unmatched.name={};std::memcpy(unmatched.name.data(),"absent",6);extension.entries.push_back(unmatched);
        VmdDecodeFile file;file.Write(ModelVmdWithScale(bytes,extension));ModelVmdSource source;
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),100,source)==MMS_OK&&source.scales.entries.size()==3,"encoded scale/curve tail decodes from original immutable file bytes");
        const ModelKeyState before(fixture.first);const auto priorScale=bone_scale::Key(fixture.first.raw(),0);const auto priorCurve=bone_scale::KeyCurve(fixture.first.raw(),0);
        const auto digest=fixture.first.Digest();std::shared_ptr<HistoryChange> change;
        Check(PlanModelVmdImport(*fixture.app,0,source,change)==MMS_OK&&change&&fixture.first.Digest()==digest,"standard and scale records plan privately together at a separate insertion offset");
        ModelPasteCommit(fixture,change);const auto* key=ModelVmdBoneAt(*fixture.first.m,0,107);
        Check(key!=nullptr,"scaled VMD uses source frame plus explicit offset");const auto slot=int(key-fixture.first.m->boneKeys);
        Check(bone_scale::Key(fixture.first.raw(),slot)==entry.scale&&bone_scale::KeyCurve(fixture.first.raw(),slot)==entry.curve,"scale and non-linear curve reach the same imported key including identity matrix");
        Check(fixture.app->CurrentFrame()==5&&fixture.history.UndoCount()==1,"scaled VMD preserves playhead and creates only one common history node");
        const ModelKeyState after(fixture.first);Check(PlanModelVmdImport(*fixture.app,0,source,change)==MMS_OK&&!change&&fixture.history.UndoCount()==1,"identical standard and scale import is a complete no-op");
        source={};Check(DeleteFileW(file.path.c_str()),"remove scale source before history traversal");
        Check(fixture.history.History(false)==MMS_OK&&before.Matches(fixture.first)&&bone_scale::Key(fixture.first.raw(),0)==priorScale&&bone_scale::KeyCurve(fixture.first.raw(),0)==priorCurve,"scale VMD undo restores standard and sidecar state in one step");
        fixture.app->CurrentFrame()=31;fixture.app->CameraMode()=1;fixture.app->SelectedModelSlot()=1;
        Check(fixture.history.History(true)==MMS_OK&&after.Matches(fixture.first)&&bone_scale::Key(fixture.first.raw(),slot)==entry.scale&&bone_scale::KeyCurve(fixture.first.raw(),slot)==entry.curve&&fixture.app->CurrentFrame()==31,"scale VMD redo retains all owned data after source deletion and UI target change");
    }
    {
        ModelPasteFixture fixture;auto standard=ModelVmdFixtureBytes();std::memset(standard.data()+54,0,15);std::memcpy(standard.data()+54,fixture.first.bones[0].name,15);
        VmdDecodeBytes repeated(standard.begin()+54,standard.begin()+165);standard.insert(standard.begin()+165,repeated.begin(),repeated.end());VmdDecodeSet(standard,50,2u);
        bone_scale_io::VmdExtension extension;bone_scale_io::VmdExtension::Entry entry;std::memcpy(entry.name.data(),standard.data()+54,15);entry.frame=7;entry.scale=Scale(3);extension.entries.push_back(entry);
        auto bytes=ModelVmdWithScale(standard,extension);VmdDecodeFile file;file.Write(bytes);ModelVmdSource source;
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,source)==MMS_OK&&ResolveModelVmdTargets(*fixture.app,0,source)==MMS_OK&&source.source.bones[0].scale==bone_scale::Matrix{}&&source.source.bones[1].scale==entry.scale,"repeated standard bone/frame maps scale to final overwritten import record");
        std::shared_ptr<HistoryChange> change;Check(PlanModelVmdImport(*fixture.app,0,source,change)==MMS_OK&&change,"duplicate standard source with unique final key plans");ModelPasteCommit(fixture,change);
        const auto* key=ModelVmdBoneAt(*fixture.first.m,0,12);Check(key&&bone_scale::Key(fixture.first.raw(),int(key-fixture.first.m->boneKeys))==entry.scale,"duplicate standard records do not lose a valid final scale assignment");
        auto damaged=bytes;damaged[damaged.size()-17]^=1;file.Write(damaged);
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,source)==MMS_OK&&source.scales.entries.empty(),"damaged scale payload CRC follows legacy ignored-tail policy");
        damaged=bytes;damaged[54+19]^=1;file.Write(damaged);
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,source)==MMS_OK&&source.scales.entries.empty(),"changed standard prefix invalidates entire scale tail");
        damaged=bytes;std::uint64_t wrong=UINT64_MAX;std::memcpy(damaged.data()+damaged.size()-16,&wrong,8);file.Write(damaged);
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,source)==MMS_OK&&source.scales.entries.empty(),"impossible footer payload size is ignored without affecting standard data");
        // An extra byte between standard data and scale is not a verified prefix boundary.
        damaged=standard;damaged.push_back(0);damaged=ModelVmdWithScale(damaged,extension);file.Write(damaged);
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,source)==MMS_OK&&source.scales.entries.empty(),"scale prefix must exactly end at standard plus recognized dictionary boundary");
        file.Write(bytes);bool completed=false;
        for(long limit=0;limit<256;++limit){ModelVmdSource untouched;untouched.version=99;failAfter=limit;const auto status=DecodeModelVmdSource(nullptr,file.path.c_str(),5,untouched);failAfter=-1;
            if(status==MMS_OK){completed=true;break;}Check(status==MMS_E_MEMORY&&untouched.version==99,"scale decode allocation failure preserves caller destination");}
        Check(completed,"scale decode allocation failure sweep reaches successful immutable decode");
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,source)==MMS_OK,"restore complete scale source for prepare allocation failures");
        completed=false;const auto digest=fixture.first.Digest();const auto count=fixture.history.UndoCount();
        for(long limit=0;limit<1024;++limit){failAfter=limit;const auto status=PlanModelVmdImport(*fixture.app,0,source,change);failAfter=-1;
            if(status==MMS_OK){completed=true;break;}Check(status==MMS_E_MEMORY&&!change&&fixture.first.Digest()==digest&&fixture.history.UndoCount()==count,"scale source mapping and planning allocation failures leave all native families and history untouched");}
        Check(completed,"scale planning allocation sweep reaches success");
    }
    {
        ModelPasteFixture fixture;const std::wstring target=L"\u7f29\u653e\u9aa8\u9abc\u957f\u540d";auto* old=fixture.first.bones[1].jpText;
        struct Restore{ModelPasteFixture& f;wchar_t* old;~Restore(){f.history.Reset();f.history.Collect();f.first.bones[1].jpText=old;}}restore{fixture,old};fixture.first.bones[1].jpText=const_cast<wchar_t*>(target.c_str());
        auto standard=ModelVmdFixtureBytes();const auto placeholder=extended_names::MotionIndexName(0);std::memcpy(standard.data()+54,placeholder.data(),15);
        extended_names::MotionNames names;names.model=L"fixture";names.bones={target};names.morphs={L"face"};names.boneOwners={0};names.morphOwners={0};
        bone_scale_io::VmdExtension extension;bone_scale_io::VmdExtension::Entry entry;entry.name=placeholder;entry.frame=7;entry.scale=Scale(4);entry.curve={9,31,90,123};extension.entries.push_back(entry);
        VmdDecodeFile file;file.Write(ModelVmdWithScale(ModelVmdWithNames(standard,names),extension));ModelVmdSource source;
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),5,source)==MMS_OK&&source.hasUnicodeNames&&source.scales.entries.size()==1,"scale prefix fingerprint includes the complete Unicode identity dictionary");
        Check(ResolveModelVmdTargets(*fixture.app,0,source)==MMS_OK&&source.source.bones[0].targetIndex==1&&source.source.bones[0].scale==entry.scale&&source.source.bones[0].scaleCurve==entry.curve,"Unicode placeholder scale resolves through private target identities without thread-local state");
        auto remapped=source;fixture.first.bones[1].jpText=old;Check(ResolveModelVmdTargets(*fixture.app,0,remapped)==MMS_OK&&remapped.source.bones[0].targetIndex==-1&&remapped.source.bones[0].scale==bone_scale::Matrix{},"repeat Unicode resolution clears a previously matched scale when target disappears");fixture.first.bones[1].jpText=const_cast<wchar_t*>(target.c_str());
        std::shared_ptr<HistoryChange> change;Check(PlanModelVmdImport(*fixture.app,0,source,change)==MMS_OK&&change,"Unicode scale import plans atomically");ModelPasteCommit(fixture,change);
        const auto* key=ModelVmdBoneAt(*fixture.first.m,1,12);Check(key&&bone_scale::Key(fixture.first.raw(),int(key-fixture.first.m->boneKeys))==entry.scale,"Unicode scale reaches its intended destination bone");
    }
}

void NativeModelVmdChecks(){
    using namespace plugins;
    ModelVmdDistinctCurveChecks();ModelVmdUnicodeChecks();NativeModelVmdScaleChecks();
    for(bool displaySection:{false,true}){
        ModelPasteFixture fixture;auto bytes=ModelVmdFixtureBytes();const auto motion=VmdDecodeMotion();
        std::memset(bytes.data()+54,0,15);std::memcpy(bytes.data()+54,fixture.first.bones[0].name,15);
        const auto morphStart=std::size_t(54+111+4);std::memset(bytes.data()+morphStart,0,15);std::memcpy(bytes.data()+morphStart,fixture.first.morphs[0].name,15);
        bytes.resize(motion.ends[0]);
        if(displaySection){for(unsigned i=0;i<4;++i)VmdDecodePut(bytes,0u);}
        VmdDecodeFile file;file.Write(bytes);ModelVmdSource source;
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),100,source)==MMS_OK&&source.source.resetDisplayCursor==displaySection,"complete empty display section differs from absent display section");
        ResetBoneKeyCursor(fixture.second.raw());auto bone=source.source.bones[0].key;
        {bone_scale::Registration scale(bone_scale::Matrix{},bone_scale::LinearCurve());Check(RegisterBoneKey(fixture.second.raw(),reinterpret_cast<unsigned char*>(&bone),100,0),"early VMD bone oracle registers");}
        ResetMorphKeyCursor(fixture.second.raw());const auto morph=source.source.morphs[0].key;
        Check(RegisterMorphKeyFromRecord(fixture.second.raw(),reinterpret_cast<const unsigned char*>(&morph),100),"early VMD morph oracle registers");
        if(displaySection)ResetDisplayKeyCursor(fixture.second.raw());
        const auto last=fixture.app->LastRegisteredFrame();std::shared_ptr<HistoryChange> change;
        Check(PlanModelVmdImport(*fixture.app,0,source,change)==MMS_OK&&change,"early VMD plans without optional records");ModelPasteCommit(fixture,change);
        Check(fixture.first.m->searchCursor==fixture.second.m->searchCursor&&fixture.first.m->maxFrame==fixture.second.m->maxFrame,"early VMD preserves legacy final allocation cursor and range");
        Check(fixture.app->LastRegisteredFrame()==(std::max)(last,fixture.first.m->maxFrame),"VMD advances registration extent in the same transaction");
        Check(fixture.history.History(false)==MMS_OK&&fixture.app->LastRegisteredFrame()==last,"VMD undo restores prior registration extent");
    }
    for(int version:{1,2}){
        VmdDecodeFile file;auto bytes=ModelVmdFixtureBytes(version);file.Write(bytes);ModelVmdSource source;
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),100,source)==MMS_OK,"model VMD1/2 decodes into private records");
        Check(source.version==version&&source.modelName=="fixture"&&source.source.frameOffset==100,"model VMD retains version name offset");
        Check(source.source.bones.size()==1&&source.source.morphs.size()==1&&source.source.displays.size()==1,"model decoder owns all three families");
        const auto& bone=source.source.bones[0];Check(bone.key.frame==7&&bone.key.position[0]==1&&bone.key.rotation[3]==1,"bone transform/frame layout decoded");
        Check(bone.scale==bone_scale::Matrix{}&&bone.scaleCurve==bone_scale::LinearCurve(),"standard VMD explicitly uses identity scale and linear curve");
        Check(source.source.morphs[0].key.value==.25f&&source.source.displays[0].frame==19,"morph and display records preserve values");
        for(unsigned lane=0;lane<16;++lane)Check(bone.key.interpolation[lane]==20,"native interpolation byte transposition");
        const auto boneStart=std::size_t(30+(version==1?10:20)+4);bytes[boneStart+49]=0x63;bytes[boneStart+50]=0x0f;file.Write(bytes);
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_OK&&source.source.bones[0].key.physicsDisabled,"physics sentinel reads unused VMD interpolation word");
        for(std::size_t cut:{std::size_t(0),boneStart+110,bytes.size()-1}){auto truncated=bytes;truncated.resize(cut);file.Write(truncated);source.version=99;
            Check(DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)!=MMS_OK&&source.version==99,"truncated model data preserves caller output");}
    }
    {
        VmdDecodeFile file;auto bytes=ModelVmdFixtureBytes();const auto morphValue=VmdDecodeMotion().morphValue;const float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(bytes.data()+morphValue,&nan,4);file.Write(bytes);ModelVmdSource source;
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_OK&&source.source.morphs[0].key.value==0,"legacy nonfinite morph weights normalize to zero");
        source.version=99;Check(DecodeModelVmdSource(nullptr,file.path.c_str(),unsigned(frame_limits::maximum)-10,source)==MMS_E_ARGUMENT&&source.version==99,"ignored global maximum still guards model frame offset");
        const auto curveAt=std::size_t(54+47);bytes[curveAt]=128;file.Write(bytes);
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_E_ARGUMENT&&source.version==99,"invalid imported interpolation rejects before output publication");
        bytes=ModelVmdFixtureBytes();bytes.insert(bytes.end(),8,0);bytes.insert(bytes.end(),{'M','M','S','X','V','M','D','X'});file.Write(bytes);
        Check(DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_OK&&source.scales.entries.empty(),"malformed scale footer retains legacy ignored-extension policy");
        file.Write(VmdDecodeMotion().bytes);Check(DecodeModelVmdSource(nullptr,file.path.c_str(),0,source)==MMS_E_ARGUMENT,"camera motion cannot enter model import");
    }
    {
        VmdDecodeFile file;file.Write(ModelVmdFixtureBytes());bool complete=false;
        for(long limit=0;limit<128;++limit){ModelVmdSource source;source.version=99;failAfter=limit;const auto status=DecodeModelVmdSource(nullptr,file.path.c_str(),0,source);failAfter=-1;
            if(status==MMS_OK){complete=true;break;}Check(status==MMS_E_MEMORY&&source.version==99,"model decoder OOM preserves destination");}
        Check(complete,"model decoder allocation sweep reaches success");
    }
    {
        ModelPasteFixture fixture;auto& app=*fixture.app;auto source=ModelVmdSource{};
        Check(CaptureModelPasteSource(app,source.source)==MMS_OK,"capture three families for immutable import fixture");source.source.frameOffset=100;
        fixture.ClearSource();std::shared_ptr<HistoryChange> change;
        Check(PlanModelVmdImport(app,0,source,change)==MMS_OK&&change,"model VMD plan does not need clipboard");
        ModelPasteCommit(fixture,change);
        Check(fixture.history.History(false)==MMS_OK&&fixture.history.History(true)==MMS_OK,"model immutable import undo redo independent of original source");
    }
    {
        ModelPasteFixture fixture;auto& app=*fixture.app;ModelVmdSource source;Check(CaptureModelPasteSource(app,source.source)==MMS_OK,"capture rename guard fixture");
        std::shared_ptr<HistoryChange> change;Check(PlanModelVmdImport(app,0,source,change)==MMS_OK&&change,"model names snapshot plans");
        const auto old=fixture.first.bones[0].nameEn[0];fixture.first.bones[0].nameEn[0]^=1;
        std::unique_ptr<PreparedHistoryChange> candidate;Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate,"name change invalidates initial import even when key matching used native name");
        fixture.first.bones[0].nameEn[0]=old;
    }
}
}
