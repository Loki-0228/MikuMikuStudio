#ifndef MMS_PLUGIN_MENU_H
#define MMS_PLUGIN_MENU_H
#include <Windows.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Optional C ABI, obtained from MMSGetMenuHostApi in the host executable.
   All calls and callbacks run on the owner window's UI thread, outside DllMain.
   The DLL owns its labels, commands and shortcuts; the host owns HMENUs.
   Unregister before destroying context/unloading the DLL. Callbacks must not
   throw. Only command/shortcut callbacks may change registrations. */
#define MMS_MENU_ABI_VERSION 1u
#define MMS_MENU_COMMAND_COUNT 64u
enum MmsMenuModifier { MMS_MENU_CONTROL=1u, MMS_MENU_SHIFT=2u, MMS_MENU_ALT=4u };
typedef struct MmsPluginMenu {
    uint32_t structureSize, abiVersion;
    HWND owner;
    const char* pluginId;
    void* context;
    /* language: 0 Japanese, 1 English, 2 Chinese. Returned text is borrowed. */
    const wchar_t* (__cdecl *title)(void*,uint32_t language);
    /* Clear/repopulate this popup only. Command IDs: firstCommand + [0,63]. */
    void (__cdecl *populate)(void*,HMENU,uint32_t firstCommand,uint32_t language);
    void (__cdecl *command)(void*,uint32_t commandOffset);
    /* Optional. Return nonzero when consumed; only initial WM_KEYDOWNs arrive. */
    int (__cdecl *shortcut)(void*,uint32_t virtualKey,uint32_t modifiers);
} MmsPluginMenu;
typedef struct MmsMenuHostApi {
    uint32_t structureSize, abiVersion;
    /* Returns a unique nonzero token, or zero on invalid/duplicate registration. */
    uint64_t (__cdecl *registerMenu)(const MmsPluginMenu*);
    void (__cdecl *unregisterMenu)(uint64_t token);
    void (__cdecl *refreshMenu)(uint64_t token);
} MmsMenuHostApi;
typedef int (__cdecl *MmsGetMenuHostApiFn)(uint32_t version,MmsMenuHostApi*);
#ifdef __cplusplus
}
#endif
#endif
