// ===========================================================================
// The one rule that decides which of a model's name fields reaches the UI.
// ===========================================================================
// MMDApp::EnglishUI() (x86 0xA0B4C / x64 0xA1B90, state field 658252) is
// *two*-valued in the original image: 0 = Japanese UI, 1 = English UI, and
// every original call site tests it as a boolean (`test al, al`), so a
// non-zero byte always meant "the English UI".  The port added a third value,
// 2 = Simplified Chinese (menu command 260 cycles 0 -> 1 -> 2), which is *not*
// English: the Chinese UI shows the names the model itself carries, exactly
// like the Japanese UI, and only translates the application's fixed vocabulary
// (ui_translation.cpp's dictionary + the zh column of LocalizeUI's table).
//
// `!= 0` is therefore the wrong test for every site that picks a *name*: under
// the Chinese UI it sent the bone lists to the English name fields while the
// morph lists (already using `== 1`) stayed on the model's own names - the
// reported "bone list English, facial list Japanese" split.  Name selection
// must go through UiUsesEnglishNames() so all lists agree.
#pragma once

#include <cstdio>

namespace mikumikustudio {

constexpr unsigned char kUiJapanese = 0;  // 0xA0B4C == 0
constexpr unsigned char kUiEnglish = 1;   // 0xA0B4C == 1
constexpr unsigned char kUiChinese = 2;   // port-only third state (menu 260)

// Model-internal names can follow the detected UI language or use a fixed
// Japanese/English preference. Auto is the default and preserves old behavior.
constexpr unsigned char kModelInfoLanguageAuto = 0;
constexpr unsigned char kModelInfoLanguageJapanese = 1;
constexpr unsigned char kModelInfoLanguageEnglish = 2;
inline unsigned char gModelInfoLanguagePreference = kModelInfoLanguageAuto;
inline bool gModelNameAutoTranslation = false;

inline void SetModelNameAutoTranslation(bool enabled) {
    gModelNameAutoTranslation = enabled;
}

inline void SetModelInfoLanguagePreference(unsigned char preference) {
    gModelInfoLanguagePreference = preference <= kModelInfoLanguageEnglish
        ? preference : kModelInfoLanguageAuto;
}

inline void LoadModelInfoLanguagePreference() {
    FILE* file = nullptr;
    int preference = kModelInfoLanguageAuto;
    int translate = 0;
    if (fopen_s(&file, "Data\\model_info_language.ini", "rt") == 0) {
        if (fscanf_s(file, "%d", &preference) != 1 || preference < 0 || preference > 2)
            preference = kModelInfoLanguageAuto;
        if (fscanf_s(file, "%d", &translate) != 1 || translate != 1)
            translate = 0;
        fclose(file);
    }
    SetModelInfoLanguagePreference(static_cast<unsigned char>(preference));
    SetModelNameAutoTranslation(translate != 0);
}

inline void SaveModelInfoLanguagePreference() {
    FILE* file = nullptr;
    if (fopen_s(&file, "Data\\model_info_language.ini", "wt") == 0) {
        fprintf(file, "%u %u\n", static_cast<unsigned>(gModelInfoLanguagePreference),
                gModelNameAutoTranslation ? 1u : 0u);
        fclose(file);
    }
}

inline unsigned char ModelInfoLanguage(unsigned char uiLanguage) {
    if (gModelInfoLanguagePreference == kModelInfoLanguageJapanese)
        return kUiJapanese;
    if (gModelInfoLanguagePreference == kModelInfoLanguageEnglish)
        return kUiEnglish;
    return uiLanguage;
}

// Windows LANGID primary language (low 10 bits), independent of region.
constexpr unsigned char UiLanguageFromWindows(unsigned short languageId) {
    return (languageId & 0x3ff) == 0x04 ? kUiChinese
         : (languageId & 0x3ff) == 0x11 ? kUiJapanese : kUiEnglish;
}

constexpr unsigned char ResolveStartupUiLanguage(int savedLanguage,
                                                unsigned short userLanguage) {
    return savedLanguage >= kUiJapanese && savedLanguage <= kUiChinese
        ? static_cast<unsigned char>(savedLanguage)
        : UiLanguageFromWindows(userLanguage);
}

// True when model-internal labels should use English fields (nameEn). The UI
// language itself remains unchanged by this model-only preference.
inline bool UiUsesEnglishNames(unsigned char englishUI) {
    return ModelInfoLanguage(englishUI) == kUiEnglish;
}

}  // namespace mikumikustudio
