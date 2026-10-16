// External camera-motion fixture through the production loader and renderer.
// The VMD remains an explicit test input rather than a bundled user asset.
#include <cmath>
#include "mikudancestudio/global_key_layout.hpp"
void CheckCameraMotionScene(MMDApp* app, const wchar_t* path) {
    const auto* output = _wgetenv(L"MMS_CAMERA_FIXTURE");
    Check(output && *output, "camera fixture output directory is explicit");
    app->CameraMode() = 1;
    app->EnglishUI() = 1;
    app->CurrentFrame() = 0;
    if (std::filesystem::path(path).extension() == L".pmm") {
        std::puts("CAMERA reopening project in a fresh process");
        wcscpy_s(app->EnvFileName(), kPathFieldCapacity, path);
        LoadSceneFile();
    } else {
        std::puts("CAMERA loading motion");
        LoadVmdFile(path);
        const auto snapshot = [&]() {
            std::vector<mdl::CameraKey> keys;
            for (std::uint32_t index = 0;;) {
                keys.push_back(app->CameraKeys()[index]);
                Check(keys.size() < 10000, "camera snapshot terminates");
                if (!keys.back().next) break;
                index = keys.back().next;
            }
            return keys;
        };
        const auto imported = snapshot();
        const bool bridge = std::getenv("MMS_CAMERA_CURVE_ROUNDTRIP") != nullptr;
        std::vector<double> referenceSamples;
        const auto curveSamples = [&]() {
            std::vector<curve_editor::Channel> channels;
            std::wstring error;
            Check(curve_host::Read(*app, curve_host::Kind::Camera, 0, channels, error),
                "curve editor reads external camera handles");
            std::vector<double> samples;
            for (const auto& channel : channels) {
                for (unsigned tick = 0; tick <= (imported.back().frame + 2) * 4; ++tick) {
                    const double frame = double(tick) / 4;
                    const auto sample = channel.sample ? channel.sample(channel, frame) :
                        animation_curve::Evaluate(channel.track, frame);
                    Check(std::isfinite(sample.value) && std::isfinite(sample.derivative),
                        "curve editor camera samples remain finite");
                    samples.push_back(sample.value);
                }
            }
            return samples;
        };
        if (bridge) {
            std::puts("CAMERA curve editor read/write before PMM roundtrip");
            referenceSamples = curveSamples();
            curve_host::Open(app, false);
            std::vector<curve_editor::Channel> channels;
            std::wstring error;
            Check(curve_host::Read(*app, curve_host::Kind::Camera, 0, channels, error),
                "read native camera authoring channels");
            Check(curve_host::Write(*app, curve_host::Kind::Camera, 0, channels, error),
                "unchanged editor commit accepts external camera handles");
            curve_editor::CloseOwned(app->MainWindow());
        }
        for (const unsigned saveFrame : {0u, 45u, 92u, 93u}) {
            app->CurrentFrame() = saveFrame;
            ReloadModels(app);
            const auto project = std::filesystem::path(output) /
                (L"camera-frame-" + std::to_wstring(saveFrame) + L".pmm");
            wcscpy_s(app->EnvFileName(), kPathFieldCapacity, project.c_str());
            app->SceneModified() = 1;
            std::printf("CAMERA saving PMM at frame %u\n", saveFrame);
            SaveSceneFile(app);
            Check(std::filesystem::is_regular_file(project), "save camera fixture PMM");
            std::printf("CAMERA reopening PMM at frame %u\n", saveFrame);
            ResetAppState(app);
            wcscpy_s(app->EnvFileName(), kPathFieldCapacity, project.c_str());
            LoadSceneFile();
            const auto reopened = snapshot();
            Check(reopened.size() == imported.size(), "PMM retains every imported camera key");
            for (std::size_t i = 0; i < imported.size(); ++i) {
                const auto& a = imported[i]; const auto& b = reopened[i];
                Check(a.frame == b.frame && a.distance == b.distance && a.fov == b.fov &&
                    a.perspective == b.perspective && a.parentModel == b.parentModel &&
                    a.parentBone == b.parentBone &&
                    std::memcmp(a.eye, b.eye, sizeof(a.eye)) == 0 &&
                    std::memcmp(a.target, b.target, sizeof(a.target)) == 0 &&
                    std::memcmp(a.interpolation, b.interpolation, sizeof(a.interpolation)) == 0,
                    "PMM preserves camera payload exactly");
            }
            std::printf("CAMERA reopened PMM frame=%u distance=%g fov=%g\n",
                app->CurrentFrame(), app->CameraDistance(), app->CameraFov());
            if (bridge) {
                const auto samples = curveSamples();
                Check(samples.size() == referenceSamples.size(), "PMM retains all curve samples");
                for (std::size_t i = 0; i < samples.size(); ++i)
                    if (std::abs(samples[i] - referenceSamples[i]) > 1.e-6)
                        Check(false, "editor PMM roundtrip preserves camera evaluation");
                std::puts("PASS editor PMM roundtrip preserves camera evaluation");
            }
        }
    }
    std::uint32_t last = 0, count = 0;
    for (std::uint32_t i = 0;;) {
        const auto& key = app->CameraKeys()[i];
        std::printf("CAMERA key=%u frame=%u prev=%u next=%u position=%g,%g,%g rotation=%g,%g,%g distance=%g fov=%d\n",
            i, key.frame, key.previous, key.next, key.eye[0], key.eye[1], key.eye[2],
            key.target[0], key.target[1], key.target[2], key.distance, key.fov);
        last = std::max(last, key.frame);
        Check(++count < 10000, "camera key list terminates");
        if (!key.next) break;
        i = key.next;
    }
    Check(last < 10000, "camera fixture fits playback budget");
    const auto state = [&](float frame) {
        const float values[] = {app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(),
            app->CameraPitch(), app->CameraYaw(), app->CameraRoll(), app->CameraDistance(), app->CameraFov()};
        bool finite = true;
        for (const float value : values) finite &= std::isfinite(value);
        std::printf("CAMERA frame=%.6f position=%g,%g,%g rotation=%g,%g,%g distance=%g fov=%g finite=%d\n",
            frame, values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], finite);
        Check(finite, "camera state remains finite");
        SetupFrameWorldTransform(app);
        for (const auto kind : {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION}) {
            D3DMATRIX matrix{};
            Check(SUCCEEDED(app->Renderer()->device->GetTransform(kind, &matrix)), "read camera transform");
            for (const auto& row : matrix.m) for (const auto value : row)
                if (!std::isfinite(value)) Check(false, "camera matrix remains finite");
        }
        app->state.messageSeen = 10;
        RenderFrameScene(app);
    };
    std::puts("CAMERA seek every frame");
    for (unsigned frame = 0; frame <= last + 2; ++frame) {
        app->CurrentFrame() = frame;
        ReloadModels(app);
        state(float(frame));
    }
    std::puts("CAMERA playback including fractional frames and terminal key");
    app->PlaybackCursorSeconds() = 0;
    UpdateBoneFrames(app);
    for (unsigned tick = 0; tick <= (last + 2) * 4; ++tick) {
        const float frame = tick * .25f;
        app->PlaybackCursorSeconds() = frame / 30.f;
        PlaybackPoseAdvance(app, 0);
        state(frame);
    }
    std::puts("CAMERA real play command and frame driver");
    app->CurrentFrame() = 0;
    ReloadModels(app);
    app->PlaybackStartsAtCurrentFrame() = 0;
    app->PlaybackLoopEnabled() = 0;
    app->FrameStepPlayback() = 0;
    app->MilliToSec() = .001f; // WinMain initializes the same clock conversion.
    app->TimeNowHigh() = 0;
    app->TimeNowLow() = 1000;
    SetDlgItemTextW(app->MainWindow(), panel::kPlayStartFrameEdit, L"0");
    SetDlgItemTextW(app->MainWindow(), panel::kPlayStopFrameEdit,
        std::to_wstring(last + 2).c_str());
    SendMessageW(app->MainWindow(), WM_COMMAND,
        MAKEWPARAM(panel::kPlayButton, BN_CLICKED),
        reinterpret_cast<LPARAM>(GetDlgItem(app->MainWindow(), panel::kPlayButton)));
    Check(app->PlaybackActive() != 0, "play command starts camera motion");
    for (unsigned tick = 0; tick <= (last + 4) * 2; ++tick) {
        app->TimeNowLow() = 1000 + tick * 1000 / 60;
        app->DeltaTime() = 1.f / 60;
        app->state.messageSeen = 10;
        FrameDriver(app);
        state(float(app->PlaybackCursorSeconds() * 30));
    }
    Check(app->PlaybackCursorSeconds() * 30 >= last,
        "frame driver reaches terminal camera key");
    Check(app->PlaybackActive() == 0, "playback stops past requested range");
    Check(!IsWindowVisible(app->MainWindow()), "camera fixture stayed hidden");
}
