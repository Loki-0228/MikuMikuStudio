// Included inside host_app_lifecycle after Boundary and LeaseProbe. This is
// the actual legacy-path modern-video consumer, not a SourceAdmission test.
namespace video_close {
std::atomic<bool> opening{false}, observationFailed{false};
std::atomic<DWORD> decoderThread{0};
std::atomic<HANDLE> decoderHandle{nullptr};
std::atomic<unsigned> starts{0}, stops{0};
std::atomic<HRESULT> stopResult{E_PENDING};
bool opened = false, released = false;
unsigned releases = 0;
fs::path supplied, owned;
std::vector<unsigned char> originalBytes;
decltype(&MFStartup) realStartup = nullptr;
decltype(&MFShutdown) realShutdown = nullptr;
decltype(&media::ReleaseVideo) realRelease = nullptr;

HRESULT WINAPI Startup(ULONG version, DWORD flags) {
    const HRESULT result = realStartup(version, flags);
    if (opening.load() && SUCCEEDED(result) && GetCurrentThreadId() != thread) {
        if (starts.fetch_add(1) != 0) observationFailed = true;
        else {
            const DWORD identity = GetCurrentThreadId();
            const HANDLE handle = OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, identity);
            if (!handle) observationFailed = true;
            decoderThread = identity; decoderHandle = handle;
        }
    }
    return result;
}
HRESULT WINAPI Shutdown() {
    const bool decoder = GetCurrentThreadId() == decoderThread.load();
    const HRESULT result = realShutdown();
    if (decoder) { stopResult = result; ++stops; }
    return result;
}
void Release(MMDApp* value) {
    if (!opened || value != app) { realRelease(value); return; }
    Check(GetCurrentThreadId() == thread && ++releases == 1 && cleanupCalls == 1 &&
          !cacheShutdownCalls && !released && media::HasVideo(value) && decoderHandle.load() &&
          WaitForSingleObject(decoderHandle.load(), 0) == WAIT_TIMEOUT &&
          LeaseProbe() == ERROR_SHARING_VIOLATION,
          "actual cleanup reaches the live modern-video worker while the texture cache lease is still held");
    std::puts("APP_VIDEO_CLOSE release-enter worker-alive=1 texture-lease-held=1");
    realRelease(value);
    Check(!media::HasVideo(value) && WaitForSingleObject(decoderHandle.load(), 0) == WAIT_OBJECT_0 &&
          starts.load() == 1 && stops.load() == 1 && SUCCEEDED(stopResult.load()) && !observationFailed.load(),
          "real ReleaseVideo returns only after its observed MF runtime and actual worker thread have ended");
    Check(LeaseProbe() == ERROR_SHARING_VIOLATION && cache_manager::CaptureActiveScene() && !cacheShutdownCalls,
          "decoder retirement precedes manager shutdown without releasing the live texture lease early");
    released = true;
    std::puts("APP_VIDEO_CLOSE release-return worker-exited=1 MF-stopped=1 texture-lease-held=1");
}
void Hooks() {
    Hook(&MFStartup, &Startup, reinterpret_cast<void**>(&realStartup));
    Hook(&MFShutdown, &Shutdown, reinterpret_cast<void**>(&realShutdown));
    Hook(&media::ReleaseVideo, &Release, reinterpret_cast<void**>(&realRelease));
}
void Frame(const BITMAPINFOHEADER* dib, bool red) {
    Check(dib && dib->biSize == sizeof(*dib) && dib->biWidth == 64 && dib->biHeight == 48 &&
          dib->biPlanes == 1 && dib->biBitCount == 32 && dib->biCompression == BI_RGB,
          "actual decoder publishes the supplied fixture's bottom-up RGB32 frame");
    const auto* pixels = reinterpret_cast<const unsigned char*>(dib + 1);
    const auto* top = pixels + ((48 - 1 - 8) * 64 + 8) * 4;
    const auto* bottom = pixels + (8 * 64 + 8) * 4;
    const unsigned channel = red ? 2u : 1u;
    Check(top[channel] > 235 && top[0] < 20 && top[red ? 1 : 2] < 20 &&
          bottom[0] > 235 && bottom[1] < 20 && bottom[2] < 20,
          "real frame pixels retain the expected time, BGR channels and vertical orientation");
}
void Start(MMDApp* value) {
    Check(value == app && initialized && produced && !opened && !media::HasVideo(value),
          "video case starts after actual WinMain initialization and a real texture cache producer");
    const auto* fixture = _wgetenv(L"MMS_PMM_APP_VIDEO_FIXTURE");
    Check(fixture && *fixture, "video case requires an explicit supplied read-only encoded fixture");
    supplied = fs::path(fixture);
    Check(supplied.is_absolute() && fs::is_regular_file(supplied) && supplied.extension() == L".mp4",
          "video fixture is a known absolute MP4 input, not a guessed project or cache location");
    originalBytes = Bytes(supplied); owned = root / L"video-owned.mp4";
    Check(!originalBytes.empty() && !fs::exists(owned) && CopyFileW(supplied.c_str(), owned.c_str(), TRUE) &&
          Bytes(owned) == originalBytes && owned.native().size() < 256,
          "copy the supplied fixture into this fresh installation without modifying or deleting its original");
    wcscpy_s(value->AviBackgroundPath(), 256, owned.c_str());
    opening = true;
    LoadAviFile(value); // The real production resolver, decoder and overlay path.
    opening = false;
    Check(media::HasVideo(value) && value->AviBackgroundEnabled() && value->AviFrameWidth() == 64 &&
          value->AviFrameHeight() == 48 && value->AviStreamEndFrame() >= 239 &&
          !std::wcscmp(value->AviBackgroundPath(), owned.c_str()) &&
          !value->AviFile() && !value->AviStream() && !value->AviFrameReader(),
          "real LoadAviFile chooses and activates its modern path decoder rather than a VfW or fake fallback");
    Check(starts.load() == 1 && !stops.load() && decoderThread.load() != thread && decoderHandle.load() &&
          WaitForSingleObject(decoderHandle.load(), 0) == WAIT_TIMEOUT && !observationFailed.load(),
          "one observed non-UI Media Foundation worker remains alive after real loading");
    std::wstring error;
    const auto* dib = media::VideoFrame(value, 0, error, true);
    Check(error.empty(), "actual initial video decode reports no error"); Frame(dib, true);
    dib = media::VideoFrame(value, 1.1, error, true);
    Check(error.empty(), "actual forward video decode reports no error"); Frame(dib, false);
    const auto previous = media::VideoFrameRevision(value);
    dib = media::VideoFrame(value, 6.1, error, false);
    Check(dib && error.empty() && previous && media::VideoFrameRevision(value) &&
          WaitForSingleObject(decoderHandle.load(), 0) == WAIT_TIMEOUT,
          "queue an asynchronous seek before close while the actual decoder thread is still alive");
    Check(!value->SceneModified() && !SceneSaveInProgress() && LeaseProbe() == ERROR_SHARING_VIOLATION,
          "real media load keeps this clean fixture closable with its unrelated managed texture lease protected");
    opened = true;
    std::puts("APP_VIDEO_CLOSE loaded-via-production=1 actual-frames=2 async-seek-queued=1");
}
void BeforeCacheShutdown() {
    Check(opened && released && releases == 1 && decoderHandle.load() &&
          WaitForSingleObject(decoderHandle.load(), 0) == WAIT_OBJECT_0 && stops.load() == 1 &&
          !media::HasVideo(app) && Bytes(owned) == originalBytes && Bytes(supplied) == originalBytes,
          "actual WinMain enters cache shutdown only after real video thread retirement, with both media copies intact");
    std::puts("APP_VIDEO_CLOSE cache-shutdown-entry worker-exited=1 media-retained=1");
}
void Finish() {
    Check(opened && released && releases == 1 && starts.load() == 1 && stops.load() == 1 &&
          !observationFailed.load() && decoderHandle.load() &&
          WaitForSingleObject(decoderHandle.load(), 0) == WAIT_OBJECT_0 &&
          Bytes(owned) == originalBytes && Bytes(supplied) == originalBytes,
          "video lifecycle ends with one fully exited worker and unchanged owned and supplied encoded bytes");
    Check(CloseHandle(decoderHandle.exchange(nullptr)) != FALSE,
          "release only the observational thread handle after actual WinMain returns");
}
} // namespace video_close
