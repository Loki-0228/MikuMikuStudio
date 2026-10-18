// Included inside the image export fixture namespace. WIC decodes PNG/JPEG/BMP
// independently of D3DX. The uncompressed DDS reader also works on Windows 7,
// where the system WIC codecs do not include DDS.
std::vector<unsigned char> ImagePixels(const fs::path& path, UINT* width = nullptr, UINT* height = nullptr) {
    if (_wcsicmp(path.extension().c_str(), L".dds") == 0) {
        std::ifstream stream(path, std::ios::binary);
        std::uint32_t header[32]{};
        stream.read(reinterpret_cast<char*>(header), sizeof header);
        Check(bool(stream) && header[0] == 0x20534444 && header[1] == 124 && header[19] == 32,
            "read independent DDS header");
        Check(header[20] == 0x41 && header[21] == 0 && header[22] == 32 &&
            header[23] == 0xff0000 && header[24] == 0xff00 && header[25] == 0xff && header[26] == 0xff000000,
            "DDS stores uncompressed BGRA with an alpha channel");
        const UINT w = header[4], h = header[3];
        Check((header[2] & 0x1007) == 0x1007 && w > 0 && h > 0 && w <= 16384 && h <= 16384 &&
            header[6] == 0 && header[7] <= 1 && (header[27] & 0x1000) && header[28] == 0,
            "DDS declares one complete 2D texture with valid dimensions");
        // DDSD_PITCH (0x8, Windows SDK ddraw.h) makes the pitch field valid.
        // D3DX writes flags=0x1007 and pitch=0 for this uncompressed format;
        // its tightly packed payload is still width * height * four bytes.
        Check((header[2] & 0x8) ? header[5] == w * 4 : header[5] == 0,
            "DDS row pitch is checked only when its header declares it");
        std::vector<unsigned char> pixels(std::size_t(w) * h * 4);
        Check(fs::file_size(path) == sizeof header + pixels.size(), "DDS has exactly one complete BGRA pixel payload");
        stream.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        Check(bool(stream), "read actual DDS pixels");
        if (width) *width = w;
        if (height) *height = h;
        return pixels;
    }
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))), "create independent image decoder");
    Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder)) && SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)), "decode exported image");
    Check(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "decode straight-alpha BGRA pixels");
    UINT w = 0, h = 0;
    Check(SUCCEEDED(converter->GetSize(&w, &h)), "read encoded dimensions");
    Check(w > 0 && h > 0 && w <= 16384 && h <= 16384, "encoded image dimensions are valid");
    std::vector<unsigned char> pixels(std::size_t(w) * h * 4);
    Check(SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data())),
        "read encoded pixels");
    if (width) *width = w;
    if (height) *height = h;
    return pixels;
}

void CheckBackgroundAlpha(const fs::path& path, bool transparent, bool empty = false) {
    UINT width = 0, height = 0;
    const auto pixels = ImagePixels(path, &width, &height);
    Check(width == 64 && height == 48, "alpha export retains the requested image dimensions");
    unsigned clear = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        if (!transparent) {
            Check(pixels[i + 3] == 255, "opaque image formats retain an opaque scene background");
        } else if (pixels[i + 3] == 0) {
            ++clear;
            Check(pixels[i] == 0 && pixels[i + 1] == 0 && pixels[i + 2] == 0,
                "transparent pixels contain no hidden background colour");
        }
    }
    if (transparent) Check(clear > 64, "alpha image formats export a transparent background automatically");
    if (empty) Check(clear == width * height, "post-process AA preserves empty-background alpha");
}

void TransparentImages(MMDApp* app) {
    Check(seq::FormatFromPath(L"C:/directory.png/picture.JPG") == 1 &&
        seq::FormatFromPath(L"picture.JPEG") == 1 && seq::FormatFromPath(L"picture.PNG") == 2 &&
        seq::FormatFromPath(L"picture.DDS") == 3 && seq::FormatFromPath(L"picture.png.bad") == -1 &&
        seq::FormatFromPath(L"directory.png/picture") == -1 && seq::FormatFromPath(nullptr) == -1,
        "image format uses only the final extension, including uppercase and JPEG");
    for (int format = 0; format < std::size(seq::kFormats); ++format)
        Check(seq::SupportsAlpha(format) == (format == 2 || format == 3), "only PNG and DDS automatically retain alpha");
    Check(!seq::SupportsAlpha(-1) && !seq::SupportsAlpha(7), "invalid formats cannot enable transparent capture");
    for (int language : {0, 1, 2})
        CheckBackgroundAlpha(output / (L"单张_" + std::to_wstring(language) + L"_0.png"), true);

    app->GroundGridEnabled() = false;
    app->GroundShadowEnabled() = false;
    seq::Options options;
    options.directory = output.wstring(); options.width = 64; options.height = 48; options.end = 0;
    for (auto algorithm : {antialias::Algorithm::MmdOriginal, antialias::Algorithm::Msaa2,
        antialias::Algorithm::Msaa4, antialias::Algorithm::Fxaa, antialias::Algorithm::Taa, antialias::Algorithm::Smaa}) {
        if (!antialias::Supported(app, algorithm)) {
            std::printf("SKIP alpha AA mode=%u: device does not support this algorithm\n", unsigned(algorithm));
            continue;
        }
        std::wstring error;
        Check(antialias::Select(app, algorithm, error), "select AA for alpha export");
        options.prefix = L"alpha-aa-" + std::to_wstring(unsigned(algorithm)) + L"_";
        RunExport(app, options);
        CheckBackgroundAlpha(seq::FramePath(options, 1), true, true);
    }
    std::wstring error;
    Check(antialias::Select(app, antialias::Algorithm::MmdOriginal, error), "restore native AA");
    for (int format : {0, 1, 3}) {
        options.format = format; options.prefix = L"auto-format-" + std::to_wstring(format) + L"_";
        RunExport(app, options);
        CheckBackgroundAlpha(seq::FramePath(options, 1), format == 3, format == 3);
    }

    // Exercise all seven real single-image menu paths. A .png component in the
    // directory must not select the encoder or alpha behaviour for the file.
    const auto directory = output / L"still-png-directory.png";
    fs::create_directories(directory);
    Check(MH_CreateHook(reinterpret_cast<void*>(&GetSaveFileNameW), &StillSaveDialog, nullptr) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(&GetSaveFileNameW)) == MH_OK, "control still-image format selection");
    const int width = app->RenderWidth(), height = app->RenderHeight();
    app->RenderWidth() = 64; app->RenderHeight() = 48;
    for (int format = 0; format < std::size(seq::kFormats); ++format) {
        const wchar_t* extension = format == 1 ? L".JPEG" : format == 2 ? L".PNG" :
            format == 3 ? L".DDS" : seq::kFormats[format].extension;
        stillPath = directory / (L"still_" + std::to_wstring(format) + extension);
        stillPickers = 0;
        SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
        Check(stillPickers == 1 && app->RecordingWindow(), "single-image format starts through its menu command");
        for (unsigned step = 0; app->RecordingWindow() && step < 12; ++step) {
            app->state.messageSeen = 1; coordinator_fixture::Drive(app);
        }
        Check(!app->RecordingWindow() && fs::is_regular_file(stillPath) && fs::file_size(stillPath) > 0,
            "all seven single-image formats finish with the requested filename");
        if (format <= 3) CheckBackgroundAlpha(stillPath, seq::SupportsAlpha(format), seq::SupportsAlpha(format));
    }
    app->RenderWidth() = width; app->RenderHeight() = height;
    MH_DisableHook(reinterpret_cast<void*>(&GetSaveFileNameW)); MH_RemoveHook(reinterpret_cast<void*>(&GetSaveFileNameW));
    ResetAppState(app);

    // Half coverage is independent of model shaders. Decode the actual files
    // to catch premultiplied RGB accidentally being stored as straight alpha.
    Microsoft::WRL::ComPtr<IDirect3DSurface9> surface;
    auto* device = app->Renderer()->device;
    Check(SUCCEEDED(device->CreateRenderTarget(2, 2, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
        FALSE, &surface, nullptr)), "create semi-transparent image fixture");
    Check(SUCCEEDED(device->ColorFill(surface.Get(), nullptr, 0x80402010)), "fill premultiplied sample");
    for (int format : {2, 3}) {
        const auto path = output / (std::wstring(L"half-alpha") + seq::kFormats[format].extension);
        Check(SUCCEEDED(seq::SaveSurface(path.c_str(), format, surface.Get(), true)), "write half-alpha image");
        const auto half = ImagePixels(path);
        Check(half.size() == 16, "half-alpha image retains its dimensions");
        for (std::size_t i = 0; i < half.size(); i += 4)
            Check(half[i] == 32 && half[i + 1] == 64 && half[i + 2] == 128 && half[i + 3] == 128,
                "PNG and DDS store straight alpha without dark fringes");
    }
}
