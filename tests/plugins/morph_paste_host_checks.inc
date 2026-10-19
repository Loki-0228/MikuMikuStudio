#include "mikumikustudio/clipboard_layout.hpp"
namespace morph_paste_host_checks {
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    using edit_history_host_checks::Menu;
    auto& model=*mdl::Mdl(app->ModelSlot(0));Check(model.morphCount&&model.morphs,"real host has morph paste targets");
    const unsigned targetMorph=model.morphCount>1?1:0;
    const auto oldMode=history.Mode();const auto frame=app->CurrentFrame();const auto slot=app->SelectedModelSlot();const auto camera=app->CameraMode();
    auto* oldClipboard=app->MorphClipboard();const auto counts=app->ClipboardCounts();
    const auto weight=model.morphs[targetMorph].value;const auto cursor=model.searchCursor;
    const auto initialPlugin=value();
    std::array<mdl::MorphClipboardRecord,3> clipboard{};
    for(unsigned i=0;i<clipboard.size();++i){std::memcpy(clipboard[i].name,model.morphs[targetMorph].name,20);clipboard[i].frame=i==2?0:i*7;clipboard[i].value=.2123f+float(i)*.2f;}
    app->MorphClipboard()=clipboard.data();app->ClipboardCounts()={};app->ClipboardCounts().morphs=unsigned(clipboard.size());
    app->CameraMode()=0;app->SelectedModelSlot()=0;app->CurrentFrame()=317;
    const auto command=[&]{SendMessageW(app->MainWindow(),WM_COMMAND,421,0);};
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        Check(history.SetMode(mode)==MMS_OK,"real morph paste mode configured");
        const auto before=std::vector<mdl::MorphKey>(model.morphKeys,model.morphKeys+mdl::MorphKeyCapacity(model.morphKeys));
        model.morphs[targetMorph].value=.876543f;
        Menu(app,L"Edit probe prepared");command();
        Check(!history.UndoCount()&&!std::memcmp(model.morphKeys,before.data(),before.size()*sizeof(mdl::MorphKey)),"pending plugin refuses actual morph paste atomically");Menu(app,L"Edit probe cancel");
        Menu(app,L"Edit probe once");command();
        Check(history.UndoCount()==2&&value()==initialPlugin+1&&model.morphs[targetMorph].value==.876543f,"real multiple morph clipboard records enter one explicit history node and retain dirty weight");
        const auto after=std::vector<mdl::MorphKey>(model.morphKeys,model.morphKeys+mdl::MorphKeyCapacity(model.morphKeys));
        unsigned at=targetMorph;while(model.morphKeys[at].frame<317&&model.morphKeys[at].next)at=model.morphKeys[at].next;
        Check(model.morphKeys[at].frame==317&&model.morphKeys[at].value==clipboard[2].value,"real repeated clipboard frame uses last matching source record");
        app->MorphClipboard()=nullptr;app->ClipboardCounts()={};
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(value()==initialPlugin+1&&!std::memcmp(model.morphKeys,before.data(),before.size()*sizeof(mdl::MorphKey)),"real morph paste undo restores keys between plugin edits");
        app->CameraMode()=1;app->SelectedModelSlot()=1;app->CurrentFrame()=29;Menu(app,L"Edit probe redo");
        Check(!std::memcmp(model.morphKeys,after.data(),after.size()*sizeof(mdl::MorphKey))&&model.morphs[targetMorph].value==.876543f&&app->CurrentFrame()==29,
            "real morph paste redo owns clipboard bytes and preserves unregistered weight across selector/time changes");
        Menu(app,L"Edit probe redo");for(int i=0;i<3;++i)Menu(app,L"Edit probe undo");
        Check(value()==initialPlugin&&history.Reset()==MMS_OK,"real morph paste restores original journal");history.Collect();
        app->MorphClipboard()=clipboard.data();app->ClipboardCounts()={};app->ClipboardCounts().morphs=unsigned(clipboard.size());
        app->CameraMode()=0;app->SelectedModelSlot()=0;app->CurrentFrame()=317;
    }
    Check(history.SetMode(oldMode)==MMS_OK,"restore history mode after actual morph paste");
    model.morphs[targetMorph].value=weight;model.searchCursor=cursor;app->MorphClipboard()=oldClipboard;app->ClipboardCounts()=counts;
    app->CurrentFrame()=frame;app->SelectedModelSlot()=slot;app->CameraMode()=camera;app->SceneModified()=0;
    std::puts("PASS actual morph-only paste, repeated source records, dirty weights and plugin history in both modes");
}
}
