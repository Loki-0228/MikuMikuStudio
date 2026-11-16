#pragma once
#include <string>
#include <string_view>
#include <cmath>
#include <cstdint>
#include <limits>
#include <algorithm>

namespace mikumikustudio::bake {
struct Settings {
    float erp = 0.2f;
    float cfm = 0.f;
    int substeps = 1;
    int rateMultiplier = 1;
    int baseRateHz = 60;
    std::wstring name;
    int solverIterations = 10;
    // Preset reference for ERP/CFM; old presets retain their authored step rate.
    int parameterReferenceRateHz = 60;
};
enum class SettingsError {
    None, InvalidErp, InvalidCfm, InvalidSubsteps, InvalidMultiplier,
    InvalidBaseRate, InvalidName, InvalidJson, UnsupportedVersion,
    MissingField, IoRead, IoWrite, TooLarge, InvalidIterations, InvalidReferenceRate
};
// The two legacy factors remain readable in old presets. The editor exposes
// their product as one rate; solver iterations never subdivide time.
inline int SimulationRate(const Settings& s) noexcept {
    if (s.baseRateHz < 1 || s.substeps < 1 || s.rateMultiplier < 1) return 0;
    std::int64_t rate = s.baseRateHz;
    for (int factor : {s.substeps, s.rateMultiplier}) {
        if (rate > (std::numeric_limits<int>::max)() / factor) return 0;
        rate *= factor;
    }
    return static_cast<int>(rate);
}
inline bool ValidParameters(const Settings& s) noexcept {
    return SimulationRate(s) > 0 && s.solverIterations > 0 && s.parameterReferenceRateHz > 0 &&
        std::isfinite(s.erp) && s.erp >= 0 && s.erp <= 1 &&
        std::isfinite(s.cfm) && s.cfm >= 0;
}
inline float PerStepFraction(float reference, double ratio) noexcept {
    if (reference <= 0 || reference >= 1 || ratio == 1) return reference;
    return static_cast<float>(-std::expm1(std::log1p(-double(reference)) * ratio));
}
struct SolverParameters { float erp, cfm; int solverIterations; };
inline SolverParameters SolverSettings(const Settings& settings, int rate) noexcept {
    SolverParameters s{settings.erp, settings.cfm, settings.solverIterations};
    if (rate > 0 && rate != settings.parameterReferenceRateHz) {
        const double ratio = double(settings.parameterReferenceRateHz) / rate;
        if (s.cfm > 0) {
            // Bullet's spring/damper conversion: ERP=h*k/(h*k+d), CFM=1/(h*k+d).
            const double denominator = 1.0 - s.erp + s.erp * ratio;
            s.erp = static_cast<float>(s.erp * ratio / denominator);
            s.cfm = static_cast<float>((std::min)(double((std::numeric_limits<float>::max)()), s.cfm / denominator));
        } else {
            s.erp = PerStepFraction(s.erp, ratio);
        }
    }
    return s;
}
Settings BulletDefaults();
bool ValidateSettings(const Settings&, SettingsError&);
bool DecodePreset(std::string_view, Settings&, SettingsError&);
std::string EncodePreset(const Settings&);
bool LoadPreset(const wchar_t*, Settings&, SettingsError&);
bool SavePreset(const wchar_t*, const Settings&, SettingsError&);
}