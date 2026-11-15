#pragma once
#include "../src/window/window_settings.hpp"
#include <clocale>
#include <cstdio>
#include <limits>

namespace window_settings_regression {
template<class Check, class Persist>
void Run(mikumikustudio::MMDApp& app, Check check, Persist persist) {
    using namespace mikumikustudio;
    constexpr float fallback = -0.75f;
    check(wcscpy_s(app.DirModel(), 1000, L"data\\models") == 0, "initialize fixture model directory");
    const std::string original = std::setlocale(LC_NUMERIC, nullptr);
    check(std::setlocale(LC_NUMERIC, "C") != nullptr, "select C numeric locale");
    // Keep the host's previous behavior for values below the normal float range.
    const float subnormal = std::strtof("1.401298464324817e-45", nullptr);
    const float underflow = std::strtof("-1e-1000", nullptr);
    std::string baseline;
    for (const char* decimal : {".", ","}) {
        if (*decimal == ',') {
            for (const char* locale : {"de-DE", "German_Germany.1252", "de_DE.utf8", "de_DE.UTF-8", "de_DE"}) {
                if (std::setlocale(LC_NUMERIC, locale) && std::string_view(std::localeconv()->decimal_point) == ",")
                    break;
            }
        }
        check(std::string_view(std::localeconv()->decimal_point) == decimal, "verify test numeric decimal separator");
        std::printf("Window settings decimal separator: %s (%s)\n", decimal, std::setlocale(LC_NUMERIC, nullptr));
        const auto read = [](const char* number) {
            return settings_json::Float(settings_json::Object(std::string("{\"sidebarRatio\":") + number + '}'),
                "sidebarRatio", fallback);
        };
        check(read("0.8125") == 0.8125f && read("-8.125E-1") == -0.8125f && read("8.125e+0") == 8.125f &&
            read("-1") == -1.0f && std::signbit(read("-0")), "read signed JSON decimals, integers and exponents");
        check(read("3.4028234663852886e38") == (std::numeric_limits<float>::max)() &&
            read("1e100") == fallback && read("-1e100") == fallback, "preserve finite float limit and overflow fallback");
        check(read("1.401298464324817e-45") == subnormal && read("-1e-1000") == underflow &&
            std::signbit(read("-1e-1000")) == std::signbit(underflow), "preserve subnormal and underflow behavior");
        for (const char* text : {"{}", "{\"sidebarRatio\":null}", "{\"sidebarRatio\":true}", "{\"sidebarRatio\":\"0.8125\"}"}) {
            check(settings_json::Float(settings_json::Object(text), "sidebarRatio", fallback) == fallback,
                "keep fallback for missing or non-numeric setting");
        }
        for (const char* malformed : {"0,8125", "+1", ".5", "1.", "01", "1e", "0x1p0", "0.8junk", "NaN", "inf"}) {
            bool rejected = false;
            try { (void)read(malformed); }
            catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "reject malformed or non-JSON numeric syntax");
        }

        const auto capture = [&](float ratio) {
            app.SidebarRatio() = ratio;
            return window_settings::Snapshot(app, -12, 14, 1024, 720, 1, 0, 1, 0, 1);
        };
        const auto snapshot = capture(0.8125f);
        const auto window = settings_json::Object(snapshot);
        check(window.At("sidebarRatio").text == "0.812500" &&
            settings_json::Float(window, "sidebarRatio", fallback) == app.SidebarRatio(), "save valid six-decimal JSON and round-trip");
        check(settings_json::Integer(window, "x", 0) == -12 && window.At("width").UInt() == 1024 &&
            window.At("menu12d").UInt() == 1 && window.At("modelDirectory").String() == "data\\models",
            "save adjacent window values and escaped path unchanged");
        if (*decimal == '.') baseline = snapshot;
        else check(snapshot == baseline, "the whole window snapshot is byte-identical across numeric locales");
        persist(snapshot);

        check(settings_json::Object(capture(-0.0f)).At("sidebarRatio").text == "-0.000000" &&
            settings_json::Object(capture(0.12345675f)).At("sidebarRatio").text == "0.123457",
            "preserve fixed-format rounding and negative zero");
        for (const float ratio : {-1.0f, std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            const auto sentinel = settings_json::Object(capture(ratio));
            check(sentinel.At("sidebarRatio").text == "-1.000000" &&
                settings_json::Float(sentinel, "sidebarRatio", fallback) == -1.0f, "preserve the six-decimal default sentinel");
        }
        check(std::string_view(std::localeconv()->decimal_point) == decimal, "settings leave the caller's locale unchanged");
    }
    check(std::setlocale(LC_NUMERIC, original.c_str()) != nullptr, "restore the test process numeric locale");
}
}
