#pragma once
#include <Windows.h>
#include <string>
#include <vector>

namespace mikumikustudio {
class MMDApp;
enum class UiMessage {
    DeleteModel, DeleteAccessory, DeleteLipFrames, DeleteEyeFrames, DeleteBrowFrames,
    NewScene, OpenUnsaved, QuitUnsaved, QuitEnhanced, StopRecording,
    PasteBone, PasteAccessory, NoBoneClipboard, NoAccessoryClipboard,
    SelectBone, SelectAccessory, SavePoseSelection, DirectSoundError, OpenModelError,
    OpenProjectError, InvalidProjectFile, UnsupportedProjectVersion,
    IncompleteProjectFile, BrokenProjectKeyChain,
    PasteMorph, NoMorphClipboard, SelectMorph,
    DeleteModelWithHistory, DeleteAccessoryWithHistory, NewSceneRefused, ProjectRecoveryPending, ProjectRecoveryFailed,
    ProjectEffectRecoveryFailed, ProjectSaveRecoveryFailed, MissingFramePasteTrack, Count
};
// Fixed application text only. A Unicode name may be appended verbatim.
int ShowUiMessage(MMDApp* app, HWND owner, UiMessage message, UINT flags,
                  const wchar_t* detail = nullptr);

enum class ProjectModelIssue { MissingFile, StructureMismatch, MissingAccessory, MissingV1Model };
inline constexpr INT_PTR kLocateProjectEffect = 6;
// Preserve the PMM loader's result codes: 1 = locate/fit, 2 = close,
// 3 = skip/locate, 4 = cancel/skip, 5 = cancel (missing/mismatch respectively).
// MissingAccessory reuses the MissingFile dialog (resource 814/815), its three
// buttons and its result codes: 1 = select the accessory file, 2 = close,
// 3 = skip the accessory, 4 = cancel the whole project. MissingFile and
// StructureMismatch keep their existing text, resources, layout and codes.
// MissingV1Model allows locate (1), close (2) or cancel (4).
// The name is shown verbatim, including any Unicode file or object name.
INT_PTR ShowProjectModelDialog(MMDApp* app, HINSTANCE instance, HWND owner,
                              ProjectModelIssue issue, const wchar_t* modelName);
enum class ProjectEffectAction { Replace, Skip, Cancel, Back };
ProjectEffectAction SelectProjectEffectReplacement(MMDApp& app, HWND owner,
    const std::vector<std::wstring>& missing, bool openingProject,
    std::wstring& original, std::wstring& replacement);
}
