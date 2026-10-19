#pragma once
#include <Windows.h>

namespace mikumikustudio {
class MMDApp;
enum class UiMessage {
    DeleteModel, DeleteAccessory, DeleteLipFrames, DeleteEyeFrames, DeleteBrowFrames,
    NewScene, OpenUnsaved, QuitUnsaved, QuitEnhanced, StopRecording,
    PasteBone, PasteAccessory, NoBoneClipboard, NoAccessoryClipboard,
    SelectBone, SelectAccessory, SavePoseSelection, DirectSoundError, OpenModelError,
    OpenProjectError, InvalidProjectFile, UnsupportedProjectVersion,
    IncompleteProjectFile, BrokenProjectKeyChain,
    PasteMorph, NoMorphClipboard, SelectMorph, Count
};
// Fixed application text only. A Unicode name may be appended verbatim.
int ShowUiMessage(MMDApp* app, HWND owner, UiMessage message, UINT flags,
                  const wchar_t* detail = nullptr);

enum class ProjectModelIssue { MissingFile, StructureMismatch };
// Preserve the PMM loader's result codes: 1 = locate/fit, 2 = close,
// 3 = skip/locate, 4 = cancel/skip, 5 = cancel (missing/mismatch respectively).
INT_PTR ShowProjectModelDialog(MMDApp* app, HINSTANCE instance, HWND owner,
                              ProjectModelIssue issue, const wchar_t* modelName);
}
