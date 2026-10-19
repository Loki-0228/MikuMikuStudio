// Include after vmd_model_host_checks.inc and bone_curve_sync_host_checks.inc.
// The live PMD/PMX object, production LoadVmdFile entry, plugin DLL and common
// journal stay real. Original isolates/restores the native bone-pool fixture.
namespace vmd_model_curves_host_checks {
using Bytes=std::vector<std::uint8_t>;
namespace ac=animation_curve;
struct Original {
    bone_curve_sync_host_checks::Original bone;
    mdl::ModelRecord& model;
    std::vector<mdl::MorphRecord> morphs;
    std::vector<std::uint32_t> cursors,indices;
    Bytes active;
    morph_curve::AuthoredLayers layers;
    std::array<char,sizeof(mdl::ModelRecord::name)> name{};
    Original(MMDApp& app,plugins::SharedHistory& history):bone(app,history),model(*mdl::Mdl(app.ModelSlot(0))),
        morphs(model.morphs,model.morphs+model.morphCount) {
        std::string error;Check(morph_curve::CaptureLayers(bone.raw,layers,error),"capture original real-model morph author layers without synchronization");
        if(model.morphKeyCursors)cursors.assign(model.morphKeyCursors,model.morphKeyCursors+model.morphCount);
        if(model.morphKeyIndices)indices.assign(model.morphKeyIndices,model.morphKeyIndices+model.morphCount);
        if(model.morphTrackActive)active.assign(model.morphTrackActive,model.morphTrackActive+model.morphCount);
        std::memcpy(name.data(),model.name,name.size());std::memset(model.name,0,sizeof(model.name));std::memcpy(model.name,"curve-vmd",9);
    }
    ~Original() {
        Check(bone.history.Reset()==MMS_OK,"release real model VMD tail participants before restoring author layers");bone.history.Collect();
        morph_curve::SwapLayers(bone.raw,layers);
        std::memcpy(model.morphs,morphs.data(),morphs.size()*sizeof(morphs[0]));
        if(!cursors.empty())std::memcpy(model.morphKeyCursors,cursors.data(),cursors.size()*sizeof(cursors[0]));
        if(!indices.empty())std::memcpy(model.morphKeyIndices,indices.data(),indices.size()*sizeof(indices[0]));
        if(!active.empty())std::memcpy(model.morphTrackActive,active.data(),active.size());
        std::memcpy(model.name,name.data(),name.size());
    }
};
unsigned Bone(const mdl::ModelRecord& model) {
    for(unsigned i=0;i<model.boneCount;++i) {
        const auto& bone=model.boneTable[i];const auto type=bone.type;
        if(bone.name[0]&&strnlen_s(bone.name,20)<15&&model.boneKeys[i].frame==0&&!model.boneKeys[i].next&&
            !bone.hasRigidBody&&(type==mdl::BoneType::RotateMove||type==mdl::BoneType::Move))return i;
    }
    Check(false,"real PMD/PMX has an empty ordinary named bone track for legal full free-curve VMD import");return 0;
}
int Morph(const mdl::ModelRecord& model) {
    for(unsigned i=0;i<model.morphCount;++i)if(model.morphs[i].name[0]&&strnlen_s(model.morphs[i].name,20)<15&&
        static_cast<unsigned>(model.morphs[i].panel)>=1&&static_cast<unsigned>(model.morphs[i].panel)<=4)return int(i);
    Check(false,"real PMD/PMX has an ordinary short named morph for the VMD author tail");return -1;
}
std::string Hex(const char* raw) {
    constexpr char hex[]="0123456789abcdef";std::string out;
    for(unsigned i=0;i<20;++i){const auto c=static_cast<unsigned char>(raw[i]);out+=hex[c>>4];out+=hex[c&15];}return out;
}
ac::Track MorphTrack() {
    ac::Track track;
    for(unsigned i=0;i<2;++i) {
        ac::Key key;key.frame=int(i*8);key.value=double(.23f+.11f*float(i));key.tangent=ac::Tangent::Free;
        key.interpolation=ac::Interpolation::Bezier;key.inWeight=.31;key.outWeight=.19;key.inSlope=.0078125;key.outSlope=-.00390625;track.keys.push_back(key);
    }return track;
}
bone_curve::Tracks BoneTracks() {
    bone_curve::Tracks tracks;
    for(unsigned lane=0;lane<9;++lane)for(unsigned i=0;i<3;++i) {
        ac::Key key;key.frame=int(i*8);key.tangent=ac::Tangent::Free;key.interpolation=ac::Interpolation::Bezier;
        key.inWeight=.31;key.outWeight=.19;
        key.value=lane==0?3.+double(i):lane==1?4.:lane==2?5.:lane==4?720.125+double(i)*37.:lane==6?(i?5.:2.5):lane>=6?1.:0.;
        key.inSlope=lane>=6?0:.0078125;key.outSlope=lane>=6?0:-.00390625;tracks[lane].keys.push_back(key);
    }
    // Sparse authoring must survive even though the compatibility mirror uses
    // the union of every channel's frames.
    tracks[2].keys.erase(tracks[2].keys.begin()+1);return tracks;
}
void Append(vmd_model_host_checks::File& file,const mdl::ModelRecord& model,unsigned bone,int morph,
            const bone_curve::Tracks& tracks,const ac::Track& weight) {
    Bytes payload,block;std::string error;
    Check(morph_curve::EncodePayload({{morph_curve::StableId(morph,model.morphs[morph].name),weight}},payload,error)&&
        morph_curve::EncodeVmdBlock(payload,block,error),"real VMD fixture encodes the unchanged morph author codec");file.Raw(block.data(),block.size());
    std::vector<ac::NamedTrack> named;
    for(unsigned lane=0;lane<9;++lane)named.push_back({"bone/"+std::to_string(bone)+"/"+std::to_string(lane)+"/"+Hex(model.boneTable[bone].name),tracks[lane]});
    Check(ac::Encode(named,payload,error),"real VMD fixture encodes all nine exact bone lanes");
    file.Raw("MMSBCRV1",8);file.Put(std::uint32_t(payload.size()));file.Raw(payload.data(),payload.size());
    bone_scale_io::VmdExtension scales;bone_scale_io::PrefixFingerprint prefix;prefix.Append(file.bytes.data(),file.bytes.size());
    scales.prefixBytes=prefix.bytes();scales.prefixCrc32=prefix.crc32();
    for(unsigned i=0;i<2;++i) {
        bone_scale_io::VmdExtension::Entry entry;std::memcpy(entry.name.data(),model.boneTable[bone].name,strnlen_s(model.boneTable[bone].name,15));
        entry.frame=i*8;entry.scale.v[0]=i?4.f:2.f;entry.scale.v[1]=i?3.f:1.5f;entry.curve={7,31,88,119};scales.entries.push_back(entry);
    }
    const auto tail=bone_scale_io::EncodeExtension(scales);file.Raw(tail.data(),tail.size());file.Put(std::uint64_t(tail.size()));file.Raw("MMSXVMDX",8);
    FILE* stream=nullptr;Check(!_wfopen_s(&stream,file.path.c_str(),L"wb")&&stream,"open real model VMD containing both exact author blocks");
    Check(std::fwrite(file.bytes.data(),1,file.bytes.size(),stream)==file.bytes.size(),"write real model VMD with dictionary-free morph bone and scale sequence");
    Check(!std::fclose(stream),"close real model author-tail VMD before immutable loading");
}
Bytes Encode(const bone_curve::Tracks& tracks) {
    std::vector<ac::NamedTrack> named;for(unsigned i=0;i<9;++i)named.push_back({std::to_string(i),tracks[i]});
    Bytes bytes;std::string error;Check(ac::Encode(named,bytes,error),"encode real model exact nine-lane author comparison");return bytes;
}
Bytes Encode(const ac::Track& track) {
    Bytes bytes;std::string error;Check(ac::Encode({{"weight",track}},bytes,error),"encode real model exact morph author comparison");return bytes;
}
void DumpScale(const char* label,const bone_scale::Matrix& matrix,const bone_scale::Curve& curve) {
    std::printf("DIAG model VMD scale %s matrix=",label);
    for(unsigned i=0;i<9;++i){std::uint32_t bits=0;std::memcpy(&bits,&matrix.v[i],sizeof(bits));std::printf("%s%.9g[%08x]",i?",":"",double(matrix.v[i]),unsigned(bits));}
    std::printf(" curve=%u,%u,%u,%u\n",unsigned(curve[0]),unsigned(curve[1]),unsigned(curve[2]),unsigned(curve[3]));
}
bone_curve::Tracks Shift(bone_curve::Tracks tracks,unsigned offset) {
    for(auto& track:tracks) {
        for(auto& key:track.keys)key.frame+=int(offset);
        if(offset) {auto anchor=track.keys.front();anchor.frame=0;anchor.inSlope=anchor.outSlope=0;
            track.keys.front().interpolation=ac::Interpolation::Step;track.keys.insert(track.keys.begin(),anchor);}
    }return tracks;
}
struct ScaleMirror {
    bone_scale::Matrix first,second;
    bone_scale::Curve firstCurve=bone_scale::LinearCurve(),secondCurve=bone_scale::LinearCurve();
};
ScaleMirror LegacyScaleMirror(const mdl::ModelRecord& live,unsigned bone,unsigned offset,
    const bone_curve::Tracks& source,const bone_scale_io::VmdExtension& extension,
    const bone_scale::Matrix& rootScale,const bone_scale::Curve& rootCurve) {
    // This private model calls the actual pre-migration author writer. No
    // renderer, application pointer, borrowed bone table, or live cursor is
    // installed. In particular, preserve CurveTime's original 12-float-step
    // endpoint behavior rather than assuming Sample(frame-zero)==Key(0).
    struct Oracle {
        std::unique_ptr<mdl::ModelRecord> model=std::make_unique<mdl::ModelRecord>();
        mdl::BoneRecord record{};
        std::array<std::uint32_t,1> cursor{},indices{};
        std::array<unsigned char,1> active{},dirty{},selected{};
        unsigned char* Raw(){return reinterpret_cast<unsigned char*>(model.get());}
        explicit Oracle(const mdl::BoneRecord& value):record(value) {
            model->boneCount=1;model->boneTable=&record;model->boneKeys=mdl::BoneKeyPoolCreate(8);
            model->boneKeyCursors=cursor.data();model->boneKeyIndices=indices.data();model->boneTrackActive=active.data();
            model->bonePhysicsState=dirty.data();model->boneSelection=selected.data();record.parent=record.slotIndex=-1;
            record.jpText=record.enText=nullptr;record.ikLinks=nullptr;record.ikLinkCount=0;
        }
        ~Oracle(){bone_curve::Forget(Raw());bone_scale::Forget(Raw());mdl::BoneKeyPoolDestroy(model->boneKeys);}
        unsigned At(unsigned frame) const {
            unsigned slot=0;for(std::size_t count=0;count<mdl::BoneKeyCapacity(model->boneKeys);++count) {
                const auto& key=model->boneKeys[slot];if(key.frame==frame)return slot;if(!key.next)break;slot=key.next;
            }Check(false,"private old author oracle has the requested native mirror frame");return 0;
        }
    } oracle(live.boneTable[bone]);
    auto* keys=oracle.model->boneKeys;keys[0]=live.boneKeys[bone];keys[0].previous=keys[0].next=0;keys[0].allocated=0;
    bone_scale::SetKey(oracle.Raw(),0,rootScale);bone_scale::SetKeyCurve(oracle.Raw(),0,rootCurve);
    for(unsigned i=0;i<2;++i) {
        const unsigned slot=offset?i+1:i;auto& key=keys[slot];key={};key.frame=offset+i*8;key.allocated=1;key.rotation[3]=1;
        key.previous=slot?slot-1:0;key.next=i?0:slot+1;
        for(unsigned lane=0;lane<4;++lane){key.interpolation[lane]=key.interpolation[lane+4]=20;key.interpolation[lane+8]=key.interpolation[lane+12]=107;}
    }
    if(offset)keys[0].next=1;
    Check(bone_scale_io::ApplyImportedExtension(oracle.Raw(),extension,int(offset))==2,"private old scale importer assigns both source footer matrices and curves");
    std::string error;const auto tracks=Shift(source,offset);
    Check(bone_curve::Write(oracle.Raw(),0,tracks,error),"actual old bone author writer builds the independent scale-mirror oracle");
    const auto first=oracle.At(offset),second=oracle.At(offset+8);
    return {bone_scale::Key(oracle.Raw(),int(first)),bone_scale::Key(oracle.Raw(),int(second)),
        bone_scale::KeyCurve(oracle.Raw(),int(first)),bone_scale::KeyCurve(oracle.Raw(),int(second))};
}
struct Snapshot {
    model_paste_host_checks::Snapshot native;
    std::unique_ptr<bone_curve::AuthoringState> bones;
    morph_curve::AuthoredLayers morphs;
    bone_scale::AuthoringState scale;
    const std::uint32_t maximum,last;const int cursor;
    Snapshot(MMDApp& app,unsigned char* raw):native(*mdl::Mdl(raw)),bones(bone_curve::AuthoringState::Capture(raw)),
        scale(bone_scale::AuthoringState::Capture(raw)),maximum(mdl::Mdl(raw)->maxFrame),last(app.LastRegisteredFrame()),cursor(mdl::Mdl(raw)->searchCursor) {
        std::string error;Check(morph_curve::CaptureLayers(raw,morphs,error),"capture complete raw real-model morph layers for common-history comparison");
    }
    bool Matches(MMDApp& app,unsigned char* raw) const {
        const auto& model=*mdl::Mdl(raw);return native.Matches(model)&&bones->Equals(*bone_curve::AuthoringState::Capture(raw))&&
            morph_curve::LayersUnchanged(raw,morphs)&&scale.Equals(bone_scale::AuthoringState::Capture(raw))&&model.maxFrame==maximum&&
            app.LastRegisteredFrame()==last&&model.searchCursor==cursor;
    }
};
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    using edit_history_host_checks::Menu;
    Check(app&&app->ModelSlot(0)&&!history.UndoCount()&&!history.RedoCount(),"real author VMD starts with a loaded PMD/PMX and empty shared journal");
    Original original(*app,history);auto* raw=app->ModelSlot(0);auto& model=*mdl::Mdl(raw);
    const auto bone=Bone(model);const int morph=Morph(model);const int initial=value();
    const auto sourceBone=BoneTracks();const auto sourceMorph=MorphTrack();
    bone_scale::Matrix sourceFirst,sourceSecond;sourceFirst.v[0]=2;sourceFirst.v[1]=1.5f;sourceSecond.v[0]=4;sourceSecond.v[1]=3;
    for(int version:{1,2})for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly})for(unsigned offset:{0u,947u}) {
        std::printf("  real model author VMD version=%d mode=%u offset=%u bone=%u morph=%d\n",version,unsigned(mode),offset,bone,morph);std::fflush(stdout);
        Check(history.SetMode(mode)==MMS_OK,"configure real model author VMD recording mode");
        app->CameraMode()=0;app->SelectedModelSlot()=0;app->CurrentFrame()=int(offset);model.selectedBone=int(bone);
        // The legacy nonlinear sampler has a nonzero fraction at frame zero.
        // Equal pre-offset and first-import bases keep this legal repeated
        // file case idempotent without changing its historical float math.
        bone_scale::SetKey(raw,int(bone),sourceFirst);bone_scale::SetKeyCurve(raw,int(bone),bone_scale::LinearCurve());
        Check(model.boneKeys[bone].frame==0&&bone_scale::Key(raw,int(bone))==sourceFirst&&bone_scale::KeyCurve(raw,int(bone))==bone_scale::LinearCurve(),
            "real temporary VMD fixture root scale exactly matches the first footer basis for same-valued repeated import");
        const Snapshot before(*app,raw);const auto ring=model.undoState[0],redo=model.undoState[1];
        vmd_model_host_checks::File file(version,model,bone,morph);Append(file,model,bone,morph,sourceBone,sourceMorph);
        plugins::ModelVmdSource source;Check(plugins::DecodeModelVmdSource(nullptr,file.path.c_str(),offset,source)==MMS_OK&&source.scales.entries.size()==2,
            "real fixture's CRC-bound source footer contains both scale entries");
        Check(source.scales.entries[0].frame==0&&source.scales.entries[1].frame==8&&source.scales.entries[0].scale==sourceFirst&&source.scales.entries[1].scale==sourceSecond&&
            source.scales.entries[0].curve==bone_scale::Curve{7,31,88,119}&&source.scales.entries[1].curve==bone_scale::Curve{7,31,88,119}&&
            bone_scale::CurveTime({7,31,88,119},0.f)>0.f,"source shear matrices remain exact while the legacy nonlinear 12-step sampler retains its nonzero frame-zero fraction");
        const auto scaleOracle=LegacyScaleMirror(model,bone,offset,sourceBone,source.scales,bone_scale::Key(raw,int(bone)),bone_scale::KeyCurve(raw,int(bone)));
        Menu(app,L"Edit probe prepared");LoadVmdFile(file.path.c_str());
        Check(before.Matches(*app,raw)&&!history.UndoCount(),"pending real plugin transaction blocks the whole model author-tail VMD");Menu(app,L"Edit probe cancel");
        Menu(app,L"Edit probe once");LoadVmdFile(file.path.c_str());
        Check(history.UndoCount()==2&&value()==initial+1&&app->CurrentFrame()==int(offset),"LoadVmdFile imports both exact author families and scale as one real shared-history node");
        const auto expectedBone=Shift(sourceBone,offset);bone_curve::Tracks actualBone;
        Check(bone_curve::ReadDetached(raw,int(bone),actualBone)&&Encode(actualBone)==Encode(expectedBone),"real loader preserves sparse nine-lane Free handles Euler winding and offset anchors exactly");
        auto expectedMorph=sourceMorph;
        for(auto& key:expectedMorph.keys)key.frame+=int(offset);
        auto& first=expectedMorph.keys.front();first.inSlope=first.outSlope=0;first.inWeight=first.outWeight=1./3.;first.interpolation=ac::Interpolation::Linear;first.tangent=ac::Tangent::Free;
        // Merge keeps the destination root and any unrelated native keys. Use
        // raw pristine layers to locate the imported endpoint, then compare
        // its exact fields rather than replacing the whole target track.
        morph_curve::AuthoredLayers authored;std::vector<ac::NamedTrack> weights;std::string error;
        Check(morph_curve::CaptureLayers(raw,authored,error)&&morph_curve::LayersTracks(authored,weights,error),"real loader retains pristine imported morph authoring");
        const auto id=morph_curve::StableId(morph,model.morphs[morph].name);
        const auto found=std::find_if(weights.begin(),weights.end(),[&](const auto& named){return named.id==id;});
        Check(found!=weights.end(),"real imported morph tail belongs to the actual PMD/PMX morph identity");
        ac::Track imported;
        for(const auto& key:expectedMorph.keys){const auto at=std::find_if(found->track.keys.begin(),found->track.keys.end(),[&](const auto& value){return value.frame==key.frame;});
            Check(at!=found->track.keys.end(),"real morph author retains each imported absolute frame");imported.keys.push_back(*at);}
        Check(Encode(imported)==Encode(expectedMorph),"real morph tail preserves retained exact handles and the original first-key normalization");
        const auto a=vmd_model_scale_host_checks::FindKey(model,bone,offset),b=vmd_model_scale_host_checks::FindKey(model,bone,offset+8);
        const auto firstScale=scaleOracle.first,secondScale=scaleOracle.second;
        const auto actualFirst=bone_scale::Key(raw,int(a)),actualSecond=bone_scale::Key(raw,int(b));
        const auto actualFirstCurve=bone_scale::KeyCurve(raw,int(a)),actualSecondCurve=bone_scale::KeyCurve(raw,int(b));
        const bool scaleSame=actualFirst==firstScale&&actualSecond==secondScale&&actualFirstCurve==scaleOracle.firstCurve&&actualSecondCurve==scaleOracle.secondCurve;
        if(!scaleSame) {
            std::printf("DIAG model VMD scale version=%d mode=%u offset=%u bone=%u type=%u slots=%u,%u raw-frames=%u,%u curve-time-zero=%.9g\n",
                version,unsigned(mode),offset,bone,unsigned(model.boneTable[bone].type),a,b,model.boneKeys[a].frame,model.boneKeys[b].frame,
                double(bone_scale::CurveTime({7,31,88,119},0.f)));
            DumpScale("first-actual",actualFirst,actualFirstCurve);DumpScale("first-expected",firstScale,scaleOracle.firstCurve);
            DumpScale("second-actual",actualSecond,actualSecondCurve);DumpScale("second-expected",secondScale,scaleOracle.secondCurve);
            plugins::ModelVmdSource decoded;const auto decode=plugins::DecodeModelVmdSource(nullptr,file.path.c_str(),offset,decoded);
            std::printf("DIAG model VMD scale source-status=%d entries=%zu bone-block=%zu morph-block=%zu\n",int(decode),decoded.scales.entries.size(),decoded.source.boneCurveBlock.size(),decoded.source.morphCurveBlock.size());
            for(std::size_t i=0;i<decoded.scales.entries.size();++i){const auto& entry=decoded.scales.entries[i];
                std::printf("DIAG model VMD scale source-entry=%zu frame=%u name=",i,entry.frame);for(unsigned char value:entry.name)std::printf("%02x",unsigned(value));std::puts("");
                DumpScale("source-entry",entry.scale,entry.curve);}
            std::fflush(stdout);
        }
        Check(scaleSame,
            "real bone author mirror uses preceding scale-tail shear basis and its legacy linear scale controls");
        const Snapshot after(*app,raw);LoadVmdFile(file.path.c_str());
        Check(history.UndoCount()==2&&after.Matches(*app,raw)&&app->CurrentFrame()==int(offset),"identical complete real model author VMD is a no-op without moving the playhead");
        Check(DeleteFileW(file.path.c_str()),"remove real model author source before interleaved plugin history traversal");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");
        app->CameraMode()=1;app->SelectedModelSlot()=1;app->CurrentFrame()=29;Menu(app,L"Edit probe undo");
        Check(before.Matches(*app,raw)&&value()==initial+1&&app->CurrentFrame()==29,"actual common undo restores all raw author and native state with source offline and another UI target");
        Menu(app,L"Edit probe undo");Check(value()==initial&&!history.UndoCount(),"earlier real plugin node remains separately undoable before the model VMD");
        Menu(app,L"Edit probe redo");app->CurrentFrame()=37;SendMessageW(app->MainWindow(),WM_COMMAND,401,0);
        Check(after.Matches(*app,raw)&&value()==initial+1&&app->CurrentFrame()==37,"native common redo restores deleted-source model author data between real plugin nodes");
        Menu(app,L"Edit probe redo");Check(value()==initial+2&&history.UndoCount()==3,"later real plugin node remains separately redoable after exact model VMD");
        for(unsigned step=0;step<3;++step)Menu(app,L"Edit probe undo");
        Check(before.Matches(*app,raw)&&value()==initial&&model.undoState[0]==ring&&model.undoState[1]==redo,"real author VMD returns to raw baseline without legacy ring registration");
        Check(history.Reset()==MMS_OK,"release real author VMD journal before the next version mode and offset");history.Collect();
    }
    std::puts("PASS real PMD/PMX LoadVmdFile exact nine-lane bone and morph VMD tails, scale basis, both history modes, offsets, no-op and deleted-source plugin interleaving");
}
}
