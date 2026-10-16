#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/plugin_storage.hpp"
// ===========================================================================
// VA 0x00408870 - PathToProjectDir  (original: sub_408870, 0xEF bytes)
// VA 0x00458F80 - LoadSceneFile    (original: sub_458F80, 0x589B bytes,
//                                    PHASE A: shell + version dispatch)
// ===========================================================================
// sub_408870 (thiscall, ecx = Destination wchar_t[0x100]): copies the scene
// path to a local, then truncates at the first L"UserFile" occurrence; if
// the result is empty or does not end in L'\\' the destination is cleared.
// Called from the PMM load shell (dst app+0x9F338, src app+0xA0900) and
// from FontSubInit 0x408E70.
//
// sub_458F80 shell (PHASE A - this file):
//   0x458FE6  _wsopen_s(&fd, app+0xA0900, 0x8000, 0x40, 0x80); on error
//             sprintf_s + MessageBoxA (JP 0x52CDD4/0x52DB80, EN
//             "Cannot open file:%d"/"open file").
//   0x459055  app+0xA442C = 0; PathToProjectDir(app+0x9F338, path).
//   0x459070  _read(fd, hdr, 0x1E); the magic "Polygon Movie maker" is matched
//             by the port via memcmp at header offset 0 (19 bytes). An
//             earlier note claimed the original used strstr; that was
//             never checked against the binary.  Defect B3-30.
//             (0x45908A: jnz continue - Ghidra prints the polarity
//             backwards); on miss MessageBoxA "This isn't the data for
//             Polygon Movie Maker!" / JP 0x52DF78 + _close + return.
//   0x4590D3  version field at hdr+0x14: "0001" -> inline v1 loader body
//             (0x45916D..0x45E7F7, fully ported as LoadSceneV1 in
//             pmm_load_v1.cpp; that body owns the fd and the _close on
//             every path.  Verified 2026-08-28: all three stock v1 scenes
//             load with bit-identical kinematic pose blocks (K hashes on
//             every slot), and v1-load -> save-as products are equivalent
//             through the orig round-trip (only orig's own stack-residue
//             nondeterminism remains, same class as the v2 verdict);
//             "0002" -> sub_450000(app, fd) which owns the rest of the
//             stream AND the _close + success tail (its epilogue at
//             0x45E7F7 only restores SEH state); anything else ->
//             "Cannot read this version of pmm!" box + _close.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <io.h>
#include <cstdlib>
#include <new>
#include <stdexcept>

#include "mikudancestudio/mmd_app.hpp"
#include "pmm_io_common.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/effect_api.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/scene_ownership.hpp"
#include "mikudancestudio/ui_message.hpp"
#include "mikudancestudio/ui_language.hpp"

namespace mikudancestudio {

// VA 0x00408870 - wide path -> project directory (truncate at "UserFile").
void PathToProjectDir(wchar_t* destination, const wchar_t* source) {
    wchar_t local[0x100];
    wcscpy_s(local, 0x100, source);
    if (local[0] == L'\0') {
        destination[0] = L'\0';
        return;
    }
    wchar_t* hit = wcsstr(local, L"UserFile");
    if (hit != nullptr) {
        *hit = L'\0';
        wcscpy_s(destination, 0x100, local);
        return;
    }
    const size_t len = wcslen(local);
    if (local[len - 1] == L'\\') {
        wcscpy_s(destination, 0x100, local);
        return;
    }
    destination[0] = L'\0';
}

// VA 0x00408E70.  The original `this` is the inline path context at
// app+0x9F338, not the MMDApp base.  Keep the public port signature while
// restoring the two 256-wchar directory fields exactly:
//   +0   project/UserFile base, +512 executable directory.
void FontSubInit(MMDApp* app, const wchar_t* directory) {
    if (app == nullptr || directory == nullptr)
        return;
    PathResolutionWorkspace& paths = app->PathWorkspace();
    wcscpy_s(paths.executableDirectory, 0x100, directory);
    PathToProjectDir(paths.projectDirectory, paths.executableDirectory);
}

// VA 0x00458F80 - PMM load shell (phase A).
static void KeyPoolLoadFailed(MMDApp* app, int fd) {
    const unsigned language = app->EnglishUI();
    _close(fd);
    ResetAppState(app);
    app->EnglishUI() = language;
    const wchar_t* text[] = {
        L"キーフレームを読み込むためのメモリが不足しています。",
        L"Not enough memory to load all project keyframes.",
        L"内存不足，无法完整加载项目关键帧。"};
    MessageBoxW(app->MainWindow(), text[language <= 2 ? language : 1],
                L"PMM", MB_OK | MB_ICONERROR);
}

void LoadSceneFile() {

    MMDApp* app = g_Block;

    if (!plugins::PrepareProjectLoad(app, app->EnvFileName())) return;
    struct PluginLoadScope { MMDApp* app; ~PluginLoadScope() { plugins::CancelProjectLoad(app); } } pluginLoadScope{app};

    ScopedSceneMutation mutation(*app);

    runtime_log::WritePath("LOAD_SCENE", app->EnvFileName());

    NotifyEffectFileOpen(app->EnvFileName());

    app->SceneModified() = 0;                                  // 0x458FE6
    int fd = -1;
    const errno_t err =
        _wsopen_s(&fd, app->EnvFileName(), 0x8000, 0x40, 0x80);

    if (err != 0) {
        const wchar_t* format[] = {L"%ls\nエラーコード: %d", L"%ls\nError code: %d", L"%ls\n错误码：%d"};
        wchar_t detail[512]{};
        swprintf_s(detail, format[app->EnglishUI() <= kUiChinese ? app->EnglishUI() : kUiEnglish], app->EnvFileName(), err);

        ShowUiMessage(app, app->MainWindow(), UiMessage::OpenProjectError, MB_OK, detail);

        return;
    }

    app->state.windowLayoutReady = 0;              // 0x459055
    PathToProjectDir(app->PathWorkspace().projectDirectory,     // 0x45905C
                      app->EnvFileName());

    char hdr[0x1E]{};
    const int read = _read(fd, hdr, sizeof(hdr));                                       // 0x459070
    if (read != sizeof(hdr) || std::memcmp(hdr, "Polygon Movie maker", 19) != 0) {
        ShowUiMessage(app, app->MainWindow(), UiMessage::InvalidProjectFile, MB_OK, app->EnvFileName());
        _close(fd);                                             // 0x4590C1
        return;
    }

    // version field at hdr+0x14 (5-byte compare incl. NUL as the original).
    if (strncmp(hdr + 0x14, "0001", 5) == 0) {
        // The inline v1 loader body (0x45916D..0x45E7F7) owns the fd and
        // closes it on every path itself (pmm_load_v1.cpp).
        try { LoadSceneV1(app, fd); }
        catch (const pmm_io::UnsupportedText&) {
            const auto language = app->EnglishUI();
            _close(fd); ResetAppState(app); app->EnglishUI() = language;
            original_import::Refuse(app->MainWindow(), language);
        }
        catch (const std::bad_alloc&) { KeyPoolLoadFailed(app, fd); }
        catch (const std::runtime_error&) { _close(fd); ResetAppState(app); ShowUiMessage(app, app->MainWindow(), UiMessage::InvalidProjectFile, MB_OK | MB_ICONERROR); }
        LocalizeUI(app);
        return;
    }
    if (strncmp(hdr + 0x14, "0002", 5) == 0
#if !MMD_ORIGINAL_PROFILE
        || strncmp(hdr + 0x14, "0003", 5) == 0
#endif
    ) {
        try {
            pmm_io::CheckedReads checked(fd);
            LoadSceneV2(app, fd);
        } catch (const pmm_io::UnsupportedText&) {
            const auto language = app->EnglishUI();
            _close(fd); ResetAppState(app); app->EnglishUI() = language;
            original_import::Refuse(app->MainWindow(), language);
        } catch (const std::bad_alloc&) {
            KeyPoolLoadFailed(app, fd);
        } catch(const pmm_io::ShortRead& error) {
            const auto language=app->EnglishUI();
            const wchar_t* formats[]={L"%ls\n位置：%lld、必要なバイト数：%u、読み取ったバイト数：%d",
                L"%ls\nByte offset: %lld; expected bytes: %u; received bytes: %d",
                L"%ls\n文件偏移：%lld；应读取：%u 字节；实际读取：%d 字节"};
            wchar_t detail[512]{};
            swprintf_s(detail,formats[language<=2?language:1],app->EnvFileName(),error.offset,error.expected,error.received);
            _close(fd);
            ResetAppState(app);
            app->EnglishUI()=language;
            ShowUiMessage(app,app->MainWindow(),UiMessage::IncompleteProjectFile,MB_OK|MB_ICONERROR,detail);
        } catch (const std::runtime_error&) {
            const auto language = app->EnglishUI();
            _close(fd); ResetAppState(app); app->EnglishUI() = language;
            ShowUiMessage(app, app->MainWindow(), UiMessage::InvalidProjectFile, MB_OK | MB_ICONERROR);
        }
        LocalizeUI(app);                                     // 0x459106
        return;                                                 // 0x45E7F7
    }
    ShowUiMessage(app, app->MainWindow(), UiMessage::UnsupportedProjectVersion, MB_OK, app->EnvFileName());
    _close(fd);                                                 // 0x459149
}

}  // namespace mikudancestudio
