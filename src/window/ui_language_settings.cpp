#include "mikudancestudio/ui_language.hpp"
#include "mikudancestudio/local_data_path.hpp"
#include "settings_json.hpp"
#include "ui_language_settings.hpp"

namespace mikudancestudio {
void language_preferences::SaveTo(const std::filesystem::path& settings) {
    const std::string json = "{\"modelInfo\":" + std::to_string(gModelInfoLanguagePreference) +
        ",\"autoTranslate\":" + std::to_string(gModelNameAutoTranslation ? 1 : 0) + "}";
    system_settings::Store(settings).WriteSection("language", json);
}
void language_preferences::LoadFrom(const std::filesystem::path& settings,const std::filesystem::path& legacy) {
    int preference = kModelInfoLanguageAuto, translate = 0;
    settings_json::Json json;std::string body;
    auto result=system_settings::Store(settings).ReadSection("language",body);
    if(result==system_settings::Result::Ok)try{json=settings_json::Object(body);}catch(...){result=system_settings::Result::Invalid;}
    if (result==system_settings::Result::Ok) {
        preference = settings_json::Integer(json, "modelInfo", preference);
        translate = settings_json::Integer(json, "autoTranslate", translate);
    } else if (result == system_settings::Result::Missing) {
        FILE* file = nullptr;
        const auto& path = legacy;
        if (!path.empty() && _wfopen_s(&file, path.c_str(), L"rt") == 0) {
            if (fscanf_s(file, "%d", &preference) != 1) preference = kModelInfoLanguageAuto;
            if (fscanf_s(file, "%d", &translate) != 1) translate = 0;
            fclose(file);
        }
        SetModelInfoLanguagePreference(preference >= 0 && preference <= 2 ? static_cast<unsigned char>(preference) : kModelInfoLanguageAuto);
        SetModelNameAutoTranslation(translate == 1);
        system_settings::Store(settings).CompareExchangeSection("language", nullptr, "{\"modelInfo\":" + std::to_string(gModelInfoLanguagePreference) +
            ",\"autoTranslate\":" + std::to_string(gModelNameAutoTranslation ? 1 : 0) + "}");
        return;
    }
    SetModelInfoLanguagePreference(preference >= 0 && preference <= 2 ? static_cast<unsigned char>(preference) : kModelInfoLanguageAuto);
    SetModelNameAutoTranslation(translate == 1);
}
void SaveModelInfoLanguagePreference(){language_preferences::SaveTo(system_settings::Path());}
void LoadModelInfoLanguagePreference(){language_preferences::LoadFrom(system_settings::Path(),local_data::File(L"model_info_language.ini"));}
}
