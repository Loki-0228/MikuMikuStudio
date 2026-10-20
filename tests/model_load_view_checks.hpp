#pragma once
#include "mikumikustudio/model.hpp"
#include <array>
#include <cmath>

inline void CheckModelLoadView(mikumikustudio::MMDApp* app, const wchar_t* modelPath) {
    using namespace mikumikustudio;
    const auto state = [&](const char* label) {
        float focus[3]{};
        ResolveFrameFocus(app, static_cast<int>(app->CameraReferenceMode()), focus);
        SetupFrameWorldTransform(app);
        D3DMATRIX world{}, view{}, projection{};
        auto* device = app->Renderer()->device;
        device->GetTransform(D3DTS_WORLD, &world);
        device->GetTransform(D3DTS_VIEW, &view);
        device->GetTransform(D3DTS_PROJECTION, &projection);
        const auto rect = app->ViewportRect();
        std::printf("LOAD_VIEW %s mode=%u track=%u reference=%d camera=%.5f,%.5f,%.5f eye=%.5f,%.5f,%.5f focus=%.5f,%.5f,%.5f worldY=%.5f viewY=%.5f aspect=%.5f projection=%.5f,%.5f rect=%ld,%ld,%ld,%ld\n",
            label, unsigned(app->CameraMode()), unsigned(app->state.followCameraEnabled),
            int(app->CameraReferenceMode()), app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(),
            app->ViewOffsetX(), app->ViewOffsetY(), app->CameraDistance(), focus[0], focus[1], focus[2],
            world._42, view._42, app->Renderer()->aspectRatio, projection._11, projection._22,
            rect.left, rect.top, rect.right, rect.bottom);
        std::array<float, 32> result{};
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column) {
                for (int k = 0; k < 4; ++k)
                    result[row * 4 + column] += world.m[row][k] * view.m[k][column];
                result[16 + row * 4 + column] = projection.m[row][column];
            }
        return result;
    };
    state("before-load");
    LoadModelFile(app, modelPath, false);
    Check(app->SelectedModel() != nullptr, "load model for view consistency check");
    const auto initial = state("after-load");
    const auto unchanged = [&](const char* label) {
        const auto current = state(label);
        bool equal = true;
        for (std::size_t i = 0; i < initial.size(); ++i)
            equal = equal && std::isfinite(current[i]) && std::abs(initial[i] - current[i]) < 0.0001f;
        Check(equal, label);
    };
    for (int i = 0; i < 3; ++i) { app->state.messageSeen = 1; FrameDriver(app); }
    unchanged("after-frames");
    PostViewRefresh(app); unchanged("after-panel-refresh");
    SelectionReeval(app); unchanged("after-selection-refresh");
    app->CurrentFrame() = 10; ReloadModels(app); SelectionReeval(app);
    unchanged("after-frame-seek");
    app->CurrentFrame() = 0; ReloadModels(app); SelectionReeval(app);
    unchanged("after-frame-zero");
    HWND combo = GetDlgItem(app->MainWindow(), panel::kMainComboModel);
    const auto modelRow = app->MainModelComboSelection();
    SendMessageW(combo, CB_SETCURSEL, 0, 0); ApplyModelComboSelection(app);
    unchanged("camera-panel");
    SendMessageW(combo, CB_SETCURSEL, modelRow, 0); ApplyModelComboSelection(app);
    unchanged("model-panel-again");
    Check(!IsWindowVisible(app->MainWindow()), "view consistency probe stays hidden");
}
