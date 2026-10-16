// Include after edit_history_host_checks.inc. All writes under test dispatch
// actual WM_COMMAND routes; private planners are never called by this probe.
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace global_registration_host_checks {
using Kind = curve_host::Kind;
using Channels = std::vector<curve_editor::Channel>;
constexpr std::array<UINT, 4> commands{452, 468, 487, 565};
constexpr std::array<Kind, 4> kinds{Kind::Camera, Kind::Light, Kind::Accessory, Kind::Shadow};
constexpr double pi = 3.14159265358979323846;
constexpr double inputPi = 3.141592025756836;

bool Near(double a, double b) { return std::abs(a-b) < 0.00003 * (1 + std::abs(b)); }
std::string Text(MMDApp& app, int id) {
    char text[256]{}; GetDlgItemTextA(app.MainWindow(), id, text, int(sizeof text)); return text;
}
void Text(MMDApp& app, int id, const char* value) {
    Check(GetDlgItem(app.MainWindow(), id) != nullptr, "actual registration input control exists");
    SetDlgItemTextA(app.MainWindow(), id, value);
}
template<class Key> unsigned Find(Key* keys, unsigned frame) {
    const auto capacity = mdl::KeyPoolCapacity(keys); unsigned slot = 0;
    for (std::size_t hop = 0; hop < capacity; ++hop) {
        Check(slot < capacity, "global registration chain index remains inside owned pool");
        if (keys[slot].frame == frame) return slot;
        if (!keys[slot].next) return UINT_MAX;
        slot = keys[slot].next;
    }
    Check(false, "global registration chain terminates"); return UINT_MAX;
}
struct Bytes {
    std::vector<unsigned char> data;
    void Add(const void* p, std::size_t n) {
        if (n) { const auto* begin = static_cast<const unsigned char*>(p); data.insert(data.end(), begin, begin+n); }
    }
    template<class T> void Add(const T& value) { Add(&value, sizeof value); }
    template<class T> void Pool(T* keys) {
        Add(keys); const auto n=mdl::KeyPoolCapacity(keys), identity=mdl::KeyPoolIdentity(keys);
        Add(n); Add(identity); Add(keys, n*sizeof(T));
    }
};
struct View {
    std::array<unsigned char, 7> values{};
    explicit View(MMDApp& a) {
        values[0]=a.CameraMode(); values[1]=a.SelectedModelSlot(); values[2]=a.SelectedAccessorySlot();
        for (unsigned i=0;i<4;++i) values[3+i]=a.state.globalTrackSelected[i];
    }
    bool Same(MMDApp& a) const { return values==View(a).values; }
};
struct Context {
    MMDApp& app; plugins::SharedHistory& history; MmsSceneApiV1 scene; int (*value)();
    std::array<void*,6> pools{}; std::array<std::uint64_t,6> identities{};
    std::array<std::size_t,6> capacities{}; Bytes modelState;
    mdl::AccessoryRecord* other; mdl::AccessoryRecord otherState;
    Context(MMDApp& a, plugins::SharedHistory& h, MmsSceneApiV1 s, int(*v)())
        :app(a),history(h),scene(s),value(v),other(a.AccessorySlot(1)),otherState(*other) {
        unsigned at=0; const auto save=[&](auto* pool){pools[at]=pool;identities[at]=mdl::KeyPoolIdentity(pool);
            capacities[at++]=mdl::KeyPoolCapacity(pool);};
        save(a.CameraKeys());save(a.LightKeys());save(a.ShadowKeys());save(a.GravityKeys());save(a.AccessoryKeys(0));save(a.AccessoryKeys(1));
        Models(modelState);
    }
    void Models(Bytes& b) const {
        for(unsigned slot=0;slot<2;++slot){const auto& m=*mdl::Mdl(app.ModelSlot(slot));
            b.Add(m.undoRings,sizeof m.undoRings);b.Add(m.undoState);b.Add(m.undoDirty);b.Add(m.redoDirty);
            b.Add(m.boneKeys,m.boneCount*sizeof(mdl::BoneKey));
            for(unsigned i=0;i<m.boneCount;++i){b.Add(m.boneTable[i].trans);b.Add(m.boneTable[i].rotQuat);b.Add(m.bonePhysicsState[i]);}
            for(unsigned i=0;i<m.morphCount;++i)b.Add(m.morphs[i].value);
        }
    }
    std::uint64_t Revision() const {std::uint64_t n=0;Check(history.Revision(n)==MMS_OK,"global registration revision readable");return n;}
    void Stable() const {
        unsigned at=0;bool same=true;const auto test=[&](auto* p){same &= pools[at]==p&&identities[at]==mdl::KeyPoolIdentity(p)&&capacities[at]==mdl::KeyPoolCapacity(p);++at;};
        test(app.CameraKeys());test(app.LightKeys());test(app.ShadowKeys());test(app.GravityKeys());test(app.AccessoryKeys(0));test(app.AccessoryKeys(1));
        Check(same,"global registration keeps real sufficiently sized pools and stable ownership");
        Bytes now;Models(now);Check(now.data==modelState.data,"global registration/history preserves both models' dirty author poses and legacy rings");
        Check(other==app.AccessorySlot(1)&&!std::memcmp(other->position,otherState.position,sizeof other->position)&&
            !std::memcmp(other->rotation,otherState.rotation,sizeof other->rotation)&&other->scale==otherState.scale&&
            other->parentModel==otherState.parentModel&&other->parentBone==otherState.parentBone&&other->opacity==otherState.opacity,
            "global registration/history preserves the unrelated accessory author pose");
        MmsSceneQueryV1 q{};q.h=plugins::Header<MmsSceneQueryV1>();q.flags=MMS_SCENE_CURRENT_TIME;MmsSnapshotV1 snap{};
        Check(scene.acquire(scene.context,&q,&snap)==MMS_OK,"global registration scene snapshot acquired");
        const auto revision=snap.sceneRevision;scene.release(scene.context,snap.handle);
        Check(revision==Revision(),"global native and real plugin edits share one public revision");
    }
    void Command(UINT id){SendMessageW(app.MainWindow(),WM_COMMAND,id,0);Stable();}
    void Plugin(const wchar_t* label){edit_history_host_checks::Menu(&app,label);edit_history_host_checks::FlushObjectUi(&app);Stable();}
    Channels Read(Kind kind) const {Channels out;std::wstring error;Check(curve_host::Read(app,kind,0,out,error),"global track and authored curves readable");return out;}
    unsigned Slot(unsigned family,unsigned frame) const {
        switch(family){case 0:return Find(app.CameraKeys(),frame);case 1:return Find(app.LightKeys(),frame);
            case 2:return Find(app.AccessoryKeys(0),frame);default:return Find(app.ShadowKeys(),frame);}
    }
    Bytes Snapshot() const {
        Bytes b;b.Pool(app.CameraKeys());b.Pool(app.LightKeys());b.Pool(app.ShadowKeys());b.Pool(app.GravityKeys());
        for(int i=0;i<255;++i){b.Pool(app.AccessoryKeys(i));if(auto* object=app.AccessorySlot(i))b.Add(*object);}
        for(int i=0;i<3;++i){b.Add(app.CameraPositionAxis(i));b.Add(app.CameraRotation()[i]);b.Add(app.LightDirection()[i]);b.Add(app.LightColor()[i]);}
        b.Add(app.CameraDistance());b.Add(app.CameraFov());b.Add(app.CameraPerspective());b.Add(app.CameraParentModel());b.Add(app.CameraParentBone());
        b.Add(app.ShadowMode());b.Add(app.ShadowDistance());b.Add(app.CurrentFrame());b.Add(app.LastRegisteredFrame());b.Add(app.SceneModified());
        b.Add(app.PhysicsResetPending());b.Add(app.CameraTrackCursor());b.Add(app.LightTrackCursor());b.Add(app.ShadowTrackCursor());
        b.Add(app.CameraTrackActive());b.Add(app.LightTrackActive());b.Add(app.ShadowTrackActive());
        b.Add(View(app).values);for(int i=0;i<2;++i)b.Add(app.AccessorySlot(i)->rowSelected);
        for(int id:{478,479,480,481,482,483,484,485,561}){const auto text=Text(app,id);b.Add(text.data(),text.size());const char z=0;b.Add(z);}
        b.Add(history.UndoCount());b.Add(history.RedoCount());b.Add(Revision());Models(b);return b;
    }
    void Same(const Bytes& before,const char* message) const {
        const auto after=Snapshot();if(before.data!=after.data){
            std::size_t offset=0;while(offset<before.data.size()&&offset<after.data.size()&&before.data[offset]==after.data[offset])++offset;
            std::printf("DIFF global snapshot offset=%zu before-size=%zu after-size=%zu\n",offset,before.data.size(),after.data.size());std::fflush(stdout);}
        Check(before.data==after.data,message);
    }
    void Sample(unsigned family,const Channels& channels,int frame) const {
        std::array<double,12> live{};std::size_t count=0;
        if(family==0){for(int i=0;i<3;++i){live[i]=app.CameraPositionAxis(i);live[i+3]=app.CameraRotation()[i]*180/pi;}
            live[6]=app.CameraDistance();live[7]=app.CameraFov();live[8]=app.CameraPerspective();live[9]=app.CameraParentModel();live[10]=app.CameraParentBone();count=11;}
        else if(family==1){for(int i=0;i<3;++i){live[i]=app.LightDirection()[i];live[i+3]=app.LightColor()[i];}count=6;}
        else if(family==2){const auto& a=*app.AccessorySlot(0);for(int i=0;i<3;++i){live[i]=a.position[i];live[i+3]=a.rotation[i]*180/pi;}
            live[6]=a.scale;live[7]=a.opacity;live[8]=a.visible;live[9]=a.shadowEnabled;live[10]=a.parentModel;live[11]=a.parentBone;count=12;}
        else {live[0]=app.ShadowMode();live[1]=app.ShadowDistance();count=2;}
        bool same=channels.size()==count;
        for(std::size_t i=0;i<count&&i<channels.size();++i){const auto& channel=channels[i];
            const auto expected=channel.sample?channel.sample(channel,frame).value:animation_curve::Evaluate(channel.track,frame).value;
            if(!Near(live[i],expected)){std::printf("DIFF family=%u channel=%zu frame=%d actual=%.9g expected=%.9g\n",family,i,frame,live[i],expected);same=false;}}
        Check(same&&app.CurrentFrame()==frame,"history samples only its affected track at the current playhead");
    }
};

void MarkAll(MMDApp& app) {
    app.CameraKeys()[0].selected=1;app.LightKeys()[0].selected=1;app.ShadowKeys()[0].selected=1;app.GravityKeys()[0].selected=1;
    app.AccessoryKeys(0)[0].selected=1;app.AccessoryKeys(1)[0].selected=1;
}
void OnlyMark(Context& c,unsigned family,unsigned slot) {
    unsigned count=0;const auto scan=[&](auto* pool){for(std::size_t i=0;i<mdl::KeyPoolCapacity(pool);++i)count+=pool[i].selected!=0;};
    scan(c.app.CameraKeys());scan(c.app.LightKeys());scan(c.app.ShadowKeys());scan(c.app.GravityKeys());
    for(int i=0;i<255;++i)scan(c.app.AccessoryKeys(i));
    bool selected=false;switch(family){case 0:selected=c.app.CameraKeys()[slot].selected!=0;break;case 1:selected=c.app.LightKeys()[slot].selected!=0;break;
        case 2:selected=c.app.AccessoryKeys(0)[slot].selected!=0;break;default:selected=c.app.ShadowKeys()[slot].selected!=0;}
    Check(count==1&&selected,"registration and redo clear every global/accessory mark and select only the registered key");
}
void Input(Context& c,unsigned family,bool second=false) {
    auto& a=c.app;a.CameraMode()=1;a.SelectedAccessorySlot()=0;
    if(family==0){for(int i=0;i<3;++i){a.CameraPositionAxis(i)=float((second?7:3)+i);a.CameraRotation()[i]=float(.125*(i+1));}
        a.CameraDistance()=second?-24.f:-32.f;a.CameraFov()=second?48.f:36.f;a.CameraPerspective()=0;a.CameraParentModel()=1;a.CameraParentBone()=second?1:0;
        CheckDlgButton(a.MainWindow(),panel::kPhysicsFrameCheckbox,BST_UNCHECKED);}
    else if(family==1){for(int i=0;i<3;++i){a.LightDirection()[i]=float((second?.5:.25)*(i+1));a.LightColor()[i]=float((second?.125:.25)*(i+1));}}
    else if(family==2){const std::array<const char*,7> first{"1.23456789","-12.34567","3.75","180.1259","-45.1259","90.12599","2.1256789"};
        const std::array<const char*,7> next{"2.5","-3.75","8.125","45","-90","180","1.5"};
        for(int i=0;i<7;++i)Text(a,478+i,(second?next:first)[i]);Text(a,485,"0.12345");
        auto& object=*a.AccessorySlot(0);object.parentModel=1;object.parentBone=second?1:0;object.opacity=.625f;object.visible=1;object.shadowEnabled=1;}
    else {a.ShadowMode()=second?1:2;Text(a,561,second?"12000.9":"1234.9876");}
}
void RootInput(Context& c,unsigned family) {
    auto& a=c.app;
    if(family==0){const auto& k=a.CameraKeys()[0];for(int i=0;i<3;++i){a.CameraPositionAxis(i)=k.eye[i];a.CameraRotation()[i]=k.target[i];}
        a.CameraDistance()=k.distance;a.CameraFov()=float(k.fov);a.CameraPerspective()=k.perspective;a.CameraParentModel()=k.parentModel;a.CameraParentBone()=k.parentBone;
        CheckDlgButton(a.MainWindow(),panel::kPhysicsFrameCheckbox,BST_UNCHECKED);}
    else if(family==1){const auto& k=a.LightKeys()[0];std::memcpy(a.LightDirection(),k.direction,sizeof k.direction);std::memcpy(a.LightColor(),k.color,sizeof k.color);}
    else if(family==2){const auto& k=a.AccessoryKeys(0)[0];auto& object=*a.AccessorySlot(0);object.parentModel=k.parentModel;object.parentBone=k.parentBone;
        object.opacity=k.opacity;object.visible=k.visible;object.shadowEnabled=k.shadowEnabled;
        for(int i=0;i<7;++i){char text[32]{};const double v=i<3?k.position[i]:i<6?k.rotation[i-3]/inputPi*180:k.scale;sprintf_s(text,"%.5g",v);Text(a,478+i,text);}
        std::memcpy(object.position,k.position,sizeof k.position);std::memcpy(object.rotation,k.rotation,sizeof k.rotation);object.scale=k.scale;}
    else {const auto& k=a.ShadowKeys()[0];a.ShadowMode()=k.mode;a.ShadowDistance()=k.distance;char text[32]{};
        sprintf_s(text,"%d",int(std::lround(10000-double(k.distance)*100000)));Text(a,561,text);}
}
void VerifyInput(Context& c,unsigned family,unsigned slot,bool second) {
    if(family==0){const auto& k=c.app.CameraKeys()[slot];Check(k.eye[0]==(second?7.f:3.f)&&k.parentModel==1&&k.parentBone==(second?1:0)&&
        c.app.CameraPositionX()==k.eye[0],"camera actual button records current pose and valid bone parent without seeking");}
    else if(family==1){const auto& k=c.app.LightKeys()[slot];Check(k.direction[0]==(second?.5f:.25f)&&k.color[0]==(second?.125f:.25f)&&
        c.app.LightDirection()[0]==k.direction[0],"light actual button records current direction and colour without seeking");}
    else if(family==2){const auto& k=c.app.AccessoryKeys(0)[slot];const auto& object=*c.app.AccessorySlot(0);
        const std::array<double,7> expected=second?std::array<double,7>{2.5,-3.75,8.125,45,-90,180,1.5}:
            std::array<double,7>{1.23456,-12.345,3.75,180.125,-45.125,90.1259,2.12567};
        bool input=true;for(unsigned i=0;i<3;++i){input &= k.position[i]==float(expected[i])&&object.position[i]==k.position[i];
            input &= k.rotation[i]==float(expected[i+3]/180*inputPi)&&object.rotation[i]==k.rotation[i];}
        Check(input&&k.scale==float(expected[6])&&object.scale==k.scale&&k.parentModel==1&&k.parentBone==(second?1:0)&&k.opacity==.625f&&object.opacity==.625f,
            "accessory button atomically parses seven-byte numeric inputs and preserves opacity outside 478..484");}
    else {const float distance=float((10000-(second?9999:1234))/100000.0);const auto& k=c.app.ShadowKeys()[slot];
        Check(k.mode==(second?1:2)&&k.distance==distance&&c.app.ShadowDistance()==distance,
            "shadow button truncates seven-character float input clamps range and commits live distance with the key");}
}
void VerifyHistoryReadout(Context& c) {
    auto& a=c.app;char expected[96]{};
    const auto slider=[&](int id,int value){
        const auto control=GetDlgItem(a.MainWindow(),id);
        Check(control!=nullptr,"history readout slider exists in real host");
        const auto minimum=int(SendMessageW(control,TBM_GETRANGEMIN,0,0));
        const auto maximum=int(SendMessageW(control,TBM_GETRANGEMAX,0,0));
        Check(SendMessageW(control,TBM_GETPOS,0,0)==(std::clamp)(value,minimum,maximum),
            "native history slider reflects restored live value within its control range");
    };
    sprintf_s(expected,"%.6g",double(a.CameraFov()));
    Check(Text(a,panel::kFovEdit)==expected,"native history refreshes camera FOV text even in model view");
    slider(panel::kFovSlider,int(std::lround((std::clamp)(a.CameraFov(),1.f,125.f))));
    for(unsigned axis=0;axis<3;++axis){
        const int color=int(double(a.LightColor()[axis])*256.0);
        sprintf_s(expected,"%3d",color);
        Check(Text(a,461+int(axis))==expected,"native history refreshes light colour text without resampling keys");
        sprintf_s(expected,"%+.1f",double(a.LightDirection()[axis]));
        Check(Text(a,464+int(axis))==expected,"native history refreshes light direction text without resampling keys");
        slider(455+int(axis),color);slider(458+int(axis),int(double(a.LightDirection()[axis])*100.0));
    }
    const int shadow=int(std::lround(10000.0-double(a.ShadowDistance())*100000.0));
    sprintf_s(expected,"%d",shadow);
    Check(Text(a,panel::kSelfShadowRangeEdit)==expected,"native history refreshes shadow distance text");
    slider(panel::kSelfShadowRangeSlider,shadow);
    const int modeIds[]={panel::kEditOffCheckbox,panel::kEditMode1Checkbox,panel::kEditMode2Checkbox};
    for(int mode=0;mode<3;++mode)Check(IsDlgButtonChecked(a.MainWindow(),modeIds[mode])==(a.ShadowMode()==mode?BST_CHECKED:BST_UNCHECKED),
        "native history refreshes shadow mode without changing it");
    const auto* accessory=a.AccessorySlot(a.SelectedAccessorySlot());
    Check(accessory!=nullptr,"native history readout uses the user's selected accessory");
    for(int id=478;id<=485;++id){
        const double value=id<481?double(accessory->position[id-478]):id<484?double(accessory->rotation[id-481])/inputPi*180.0:
            id==484?double(accessory->scale):double(accessory->opacity);
        sprintf_s(expected,id==485?"%3.2f":"%3.4f",value);
        Check(Text(a,id)==expected,"native history refreshes selected accessory pose opacity and scale text");
    }
}
void RoundTrip(Context& c,unsigned family) {
    std::printf("RUN actual global registration family=%u command=%u\n",family,commands[family]);std::fflush(stdout);
    auto& a=c.app;const int pluginBefore=c.value();const Channels original=c.Read(kinds[family]);
    a.CurrentFrame()=10;Input(c,family);MarkAll(a);a.SceneModified()=0;
    a.SelectGlobalTimelineTrack(family==0?GlobalTimelineTrack::Light:GlobalTimelineTrack::Camera);
    a.AccessorySlot(0)->rowSelected=0;a.AccessorySlot(1)->rowSelected=1;
    const auto revision=c.Revision();c.Command(commands[family]);
    const auto slot=c.Slot(family,10);Check(slot!=UINT_MAX&&slot&&c.history.UndoCount()==1&&!c.history.RedoCount()&&a.SceneModified()&&c.Revision()>revision,
        "actual global button inserts exactly one common-history node");VerifyInput(c,family,slot,false);OnlyMark(c,family,slot);
    bool rows=true;for(unsigned i=0;i<4;++i)rows &= a.state.globalTrackSelected[i]==
        (family!=2&&i==(family==0?0u:family==1?1u:2u));
    Check(rows&&a.AccessorySlot(0)->rowSelected==(family==2)&&!a.AccessorySlot(1)->rowSelected,
        "registration of an unselected target makes precisely its timeline row selected");
    const Channels first=c.Read(kinds[family]);c.Plugin(L"Edit probe once");Input(c,family,true);
    a.GlobalTrackSelected(GlobalTimelineTrack::Gravity)=1;a.AccessorySlot(1)->rowSelected=1;
    const View registeredView(a);const auto registeredRow=a.AccessorySlot(0)->rowSelected;
    c.Command(commands[family]);
    Check(registeredView.Same(a)&&a.AccessorySlot(0)->rowSelected==registeredRow&&a.AccessorySlot(1)->rowSelected,
        "registration of an already selected camera light shadow or accessory preserves the other selected rows");
    Check(c.Slot(family,10)==slot&&c.history.UndoCount()==3&&c.value()==pluginBefore+1,"global overwrite reuses its slot around a real plugin edit");
    VerifyInput(c,family,slot,true);const Channels second=c.Read(kinds[family]);
    a.CurrentFrame()=5;a.CameraMode()=0;a.SetSelectedModelSlot(1);a.SelectedAccessorySlot()=1;
    a.SelectGlobalTimelineTrack(GlobalTimelineTrack::Gravity);a.AccessorySlot(1)->rowSelected=1;const View view(a);
    c.Plugin(L"Edit probe undo");Check(view.Same(a)&&a.AccessorySlot(1)->rowSelected,"plugin undo preserves user's current timeline view selection");c.Sample(family,first,5);
    c.Command(400);Check(c.value()==pluginBefore&&c.history.UndoCount()==1,"native undo traverses only the intervening plugin edit");
    c.Command(400);Check(c.Slot(family,10)==UINT_MAX&&!c.history.UndoCount()&&c.history.RedoCount()==3,"native undo removes global insertion as one step");
    VerifyHistoryReadout(c);
    Check(view.Same(a)&&a.AccessorySlot(1)->rowSelected,"native history readout preserves the current model mode track and accessory selection");
    c.Sample(family,original,5);Check(a.CameraKeys()[0].selected&&a.LightKeys()[0].selected&&a.ShadowKeys()[0].selected&&
        a.GravityKeys()[0].selected&&a.AccessoryKeys(0)[0].selected&&a.AccessoryKeys(1)[0].selected,"undo restores all global and accessory marks captured at registration");
    a.CurrentFrame()=0;a.SelectedAccessorySlot()=0;RootInput(c,family);a.SceneModified()=0;const auto same=c.Snapshot();c.Command(commands[family]);
    c.Same(same,"same-data root registration preserves every mark live value text dirty flag revision and redo");
    a.CurrentFrame()=15;const View redoView(a);c.Plugin(L"Edit probe redo");c.Sample(family,first,15);OnlyMark(c,family,slot);
    Check(redoView.Same(a)&&a.AccessorySlot(1)->rowSelected,"redo preserves timeline view selected after undo");
    c.Command(401);c.Plugin(L"Edit probe redo");c.Sample(family,second,15);
    Check(c.value()==pluginBefore+1&&c.history.UndoCount()==3,"mixed global/native plugin redo restores the complete sequence");
    const View readoutView(a);const auto firstRow=a.AccessorySlot(0)->rowSelected,secondRow=a.AccessorySlot(1)->rowSelected;
    c.Command(400);c.Sample(family,first,15);VerifyHistoryReadout(c);
    Check(c.history.UndoCount()==2&&c.history.RedoCount()==1,"native undo refreshes global controls after exactly one committed change");
    c.Command(401);c.Sample(family,second,15);VerifyHistoryReadout(c);
    Check(c.history.UndoCount()==3&&!c.history.RedoCount()&&readoutView.Same(a)&&
        a.AccessorySlot(0)->rowSelected==firstRow&&a.AccessorySlot(1)->rowSelected==secondRow,
        "native redo refreshes all global readouts while preserving the user's view selection");
    // Existing nonzero successors must not turn a frame-zero overwrite into a
    // recycled second root; the first three records stay available afterwards.
    a.CurrentFrame()=0;Input(c,family);c.Command(commands[family]);
    Check(c.Slot(family,0)==0&&c.Slot(family,10)==slot&&c.history.UndoCount()==4,"frame-zero overwrite with an existing successor retains the true root and chain");
    VerifyInput(c,family,0,false);
    if(family==0){
        Check(a.CameraKeys()[0].interpolation[0][0]==12&&a.CameraKeys()[0].interpolation[1][0]==34,
            "unchecked camera registration preserves the existing root interpolation");
        CheckDlgButton(a.MainWindow(),panel::kPhysicsFrameCheckbox,BST_CHECKED);c.Command(452);
        bool defaults=true;const auto* bytes=reinterpret_cast<const unsigned char*>(a.CameraKeys()[0].interpolation);
        for(unsigned i=0;i<24;++i)defaults &= bytes[i]==(i<12?20:107);
        Check(defaults&&c.history.UndoCount()==5,"checked camera registration replaces existing controls with the native defaults");
        c.Plugin(L"Edit probe undo");Check(a.CameraKeys()[0].interpolation[0][0]==12&&a.CameraKeys()[0].interpolation[1][0]==34,
            "plugin undo restores the original camera interpolation after reset registration");
        CheckDlgButton(a.MainWindow(),panel::kPhysicsFrameCheckbox,BST_UNCHECKED);
    }
    c.Command(400);
    c.Plugin(L"Edit probe undo");c.Command(400);c.Plugin(L"Edit probe undo");
    Check(c.value()==pluginBefore&&!c.history.UndoCount(),"global family returns to the original native tracks and plugin state");
    if(family==1){const auto restored=c.Read(Kind::Light);bool curves=restored.size()==original.size();
        for(std::size_t i=0;i<restored.size()&&i<original.size();++i){const auto& x=restored[i].track;const auto& y=original[i].track;curves &= x.keys.size()==y.keys.size();
            for(std::size_t j=0;j<x.keys.size()&&j<y.keys.size();++j){const auto& l=x.keys[j];const auto& r=y.keys[j];
                curves &= l.frame==r.frame&&l.value==r.value&&l.inSlope==r.inSlope&&l.outSlope==r.outSlope&&l.inWeight==r.inWeight&&
                    l.outWeight==r.outWeight&&l.interpolation==r.interpolation&&l.tangent==r.tangent;}}
        Check(curves,"undo restores original authored light keys and all custom tangent metadata after reconciliation");}
    Check(c.history.Reset()==MMS_OK,"completed global subcase releases its undone history");c.history.Collect();a.CurrentFrame()=0;a.SceneModified()=0;
}

void ImportIdentity(Context& c) {
    std::puts("RUN global registration and private accessory import share reversible pool identities");std::fflush(stdout);
    auto& a=c.app;wchar_t path[256]{};Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_ACCESSORY",path,256),"mixed import fixture exists");
    Check(!a.AccessorySlot(2),"mixed global/import destination slot starts empty");
    const auto* old=a.AccessoryKeys(2);const auto oldIdentity=mdl::KeyPoolIdentity(old);
    a.CurrentFrame()=30;Input(c,0);c.Command(452);
    Check(c.Slot(0,30)!=UINT_MAX&&c.history.UndoCount()==1,"global registration precedes object import in the common journal");
    LoadAccessoryFile(path);edit_history_host_checks::FlushObjectUi(&a);
    Check(a.AccessorySlot(2)&&c.history.UndoCount()==2,"actual bound accessory loader imports the third object as one history node");
    auto* object=a.AccessorySlot(2);const auto* imported=a.AccessoryKeys(2);const auto importedIdentity=mdl::KeyPoolIdentity(imported);
    Check(importedIdentity&&importedIdentity!=oldIdentity,"private import publishes its new owned accessory key pool");
    c.Plugin(L"Edit probe undo");
    Check(!a.AccessorySlot(2)&&a.AccessoryKeys(2)==old&&mdl::KeyPoolIdentity(a.AccessoryKeys(2))==oldIdentity&&c.history.UndoCount()==1,
        "undo import restores the inactive key pool and its original identity");
    c.Command(400);Check(c.Slot(0,30)==UINT_MAX&&!c.history.UndoCount()&&c.history.RedoCount()==2,
        "global undo succeeds after import undo restored every observed accessory pool identity");
    c.Plugin(L"Edit probe redo");c.Command(401);edit_history_host_checks::FlushObjectUi(&a);
    Check(a.AccessorySlot(2)==object&&a.AccessoryKeys(2)==imported&&mdl::KeyPoolIdentity(a.AccessoryKeys(2))==importedIdentity&&
        c.Slot(0,30)!=UINT_MAX&&c.history.UndoCount()==2,"native/plugin redo restores global registration then the same retained imported object");
    c.Plugin(L"Edit probe undo");c.Command(400);
    Check(!a.AccessorySlot(2)&&!c.history.UndoCount()&&c.history.Reset()==MMS_OK,"mixed global/import history returns to its original scene");
    c.history.Collect();a.CurrentFrame()=0;a.SceneModified()=0;
}

void Rejections(Context& c) {
    std::puts("RUN global registration rejection and no legacy fallback");std::fflush(stdout);
    const auto reject=[&](const char* reason){for(unsigned family=0;family<commands.size();++family){
        Input(c,family);c.app.CurrentFrame()=30;MarkAll(c.app);c.app.SceneModified()=0;const auto before=c.Snapshot();
        std::printf("RUN rejected global registration reason=%s command=%u\n",reason,commands[family]);std::fflush(stdout);
        c.Command(commands[family]);c.Same(before,"refused global registration leaves pools marks pose controls dirty and journal unchanged");}};
    c.app.PlaybackActive()=1;reject("playback");c.app.PlaybackActive()=0;
    const auto pending=reinterpret_cast<std::uint64_t(*)()>(GetProcAddress(GetModuleHandleW(L"manager_edit_probe.dll"),"GetProbePending"));
    Check(pending!=nullptr,"real DLL exposes pending transaction state");
    c.Plugin(L"Edit probe pending");Check(pending()!=0,"real DLL retains its unprepared transaction");reject("pending");c.Plugin(L"Edit probe cancel");
    c.Plugin(L"Edit probe prepared");Check(pending()!=0,"real DLL retains its prepared transaction");reject("prepared");c.Plugin(L"Edit probe cancel");
    Check(!c.history.UndoCount()&&!c.history.RedoCount(),"rejected global registrations never enter shared or legacy history");
}

void Run(MMDApp* app) {
    using namespace plugins;
    const auto initial=GetModuleHandleW(L"manager_edit_probe.dll");
    const auto initialFlags=reinterpret_cast<std::uint64_t(*)()>(GetProcAddress(initial,"GetProbeFlags"));
    Check(initial&&initialFlags&&(initialFlags()&2)&&!(initialFlags()&1),"ordinary global host keeps mms.edit unavailable");
    wchar_t model[256]{},accessory[256]{};Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",model,256)&&
        GetEnvironmentVariableW(L"MMS_SCENE_TEST_ACCESSORY",accessory,256),"global host model and accessory fixtures supplied");
    LoadModelFile(app,model,false);LoadModelFile(app,model,false);LoadAccessoryFile(accessory);LoadAccessoryFile(accessory);
    Check(app->ModelSlot(0)&&app->ModelSlot(1)&&app->AccessorySlot(0)&&app->AccessorySlot(1),"global probe loads two real models and two real accessories");
    // One genuine authored track, installed before the controlled journal. Its
    // non-native tangents must survive undo after intermediate reconciliation.
    Channels light;std::wstring error;Check(curve_host::Read(*app,Kind::Light,0,light,error)&&light.size()==6,"original light track readable");
    for(auto& channel:light){auto first=channel.track.keys.front();first.frame=0;first.interpolation=animation_curve::Interpolation::Bezier;
        first.tangent=animation_curve::Tangent::Free;first.inSlope=first.outSlope=.0125;auto last=first;last.frame=20;
        channel.track.keys={first,last};}
    Check(curve_host::Write(*app,Kind::Light,0,light,error)&&curve_host::Has(app,Kind::Light),"global fixture installs authored light tangents before binding");
    app->CameraKeys()[0].interpolation[0][0]=12;app->CameraKeys()[0].interpolation[1][0]=34;
    for(unsigned i=0;i<2;++i){auto& m=*mdl::Mdl(app->ModelSlot(i));m.boneTable[0].trans[1]=i?8.5f:4.25f;m.bonePhysicsState[0]=1;}
    app->AccessorySlot(1)->position[0]=77.25f;app->AccessorySlot(1)->rotation[1]=.375f;
    app->CameraMode()=1;app->SelectedAccessorySlot()=0;
    Shutdown();Check(InitializeHistoryTestRuntime(app),"explicit global registration runtime attaches one native/plugin journal");
    auto* history=NativeHistory(app);Check(history&&BoundNativePoseHistory(app)==history,"global registration explicit history binding exists");
    const auto library=GetModuleHandleW(L"manager_edit_probe.dll");const auto value=reinterpret_cast<int(*)()>(GetProcAddress(library,"GetProbeValue"));
    const auto flags=reinterpret_cast<std::uint64_t(*)()>(GetProcAddress(library,"GetProbeFlags"));
    Check(value&&flags&&(flags()&1)&&!(flags()&2),"real global probe DLL negotiates controlled edit service");
    auto scene=CreateSceneStore(app);SceneClient client;Context c(*app,*history,scene->Api(client),value);
    for(unsigned family=0;family<commands.size();++family)RoundTrip(c,family);
    ImportIdentity(c);Rejections(c);std::puts("PASS actual camera light accessory shadow registration and real plugin history");
}
}
