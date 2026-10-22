// Included by the genuine parser/owner fixture. Observers always forward real
// CRT/extension operations except the explicitly requested valid-fd sync fault.
constexpr unsigned kBufferedCameraCount = 2501;
constexpr char kBufferedUnknown[] = "test.mms.preread.unknown";
mdl::CameraKey BufferedCamera(unsigned index) {
    auto key = CameraRoot(); key.frame = index * 3; key.previous = index ? index - 1 : 0;
    key.next = index + 1 < kBufferedCameraCount ? index + 1 : 0;
    key.distance -= index * .03125f; key.target[0] += index * .125f;
    return key;
}
void MatchBufferedCameras() {
    for (unsigned i = 0; i < kBufferedCameraCount; ++i) {
        const auto expected = BufferedCamera(i); const auto& actual = app->CameraKeys()[i];
        Check(actual.frame == expected.frame && actual.previous == expected.previous && actual.next == expected.next &&
            actual.distance == expected.distance && actual.fov == expected.fov && actual.perspective == expected.perspective &&
            actual.parentModel == expected.parentModel && actual.parentBone == expected.parentBone &&
            !std::memcmp(actual.target, expected.target, sizeof(actual.target)) &&
            !std::memcmp(actual.eye, expected.eye, sizeof(actual.eye)) &&
            !std::memcmp(actual.interpolation, expected.interpolation, sizeof(actual.interpolation)),
            "all real sparse camera keys, links and interpolation bytes survive buffered and direct parsing");
    }
}
void WriteBufferedBytes(const fs::path& path, const std::vector<unsigned char>& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())); output.close();
    Check(bool(output), "write only this fixture's isolated parser input");
}
void BufferedProjectCases(const fs::path& ordinary, const fs::path& v1,
                          const mdl::CameraKey& camera, const mdl::LightKey& light) {
    struct Switch { bool prior = pmm_io::bufferedReads; ~Switch() { pmm_io::bufferedReads = prior; } } restore;
    std::puts("STAGE genuine V1 optional EOF with independent buffered/direct switches");
    for (bool buffered : {false, true}) { pmm_io::bufferedReads = buffered; Normal(v1, camera, light); }
    Normal(ordinary, camera, light);
    Check(mdl::KeyPoolEnsure(app->CameraKeys(), kBufferedCameraCount), "reserve owned native camera storage for a real multi-window body");
    for (unsigned i = 0; i < kBufferedCameraCount; ++i) app->CameraKeys()[i] = BufferedCamera(i);
    const std::vector<unsigned char> unknown(pmm_io::kReadBufferBytes + 731, 0x67);
    const unsigned versions = MMD_ORIGINAL_PROFILE ? 1u : 2u;
    for (unsigned version = 0; version < versions; ++version) {
        const auto savedPath = directory / (version ? L"large-0003.pmm" : L"large-0002.pmm");
        app->CurrentFrame() = version ? frame_limits::legacyMaximum + 1 : 12;
        Check(SaveSceneCopy(app, savedPath.c_str()), "the real writer saves thousands of native sparse records without fake standard padding");
        const auto saved = Bytes(savedPath);
        Check(saved.size() > pmm_io::kReadBufferBytes * 2 &&
            !std::memcmp(saved.data(), version ? "Polygon Movie maker 0003" : "Polygon Movie maker 0002", 24),
            "the real serializer selects the expected version and standard payload spans multiple windows");
        std::uint64_t boundary = saved.size(); pmm_extension::Document document;
        int fd = _wopen(savedPath.c_str(), _O_BINARY | _O_RDONLY); Check(fd >= 0, "open the writer's own extension-bearing fixture");
        Check(pmm_extension::Read(fd, document, boundary) == pmm_extension::ReadResult::Loaded && _close(fd) == 0,
            "decode the real writer's standard boundary before constructing extension variants");
        Check(boundary > pmm_io::kReadBufferBytes * 2 && boundary <= saved.size(), "only actual native standard records account for the large prefix");
        const auto standard = directory / (version ? L"large-0003-std.pmm" : L"large-0002-std.pmm");
        WriteBufferedBytes(standard, {saved.begin(), saved.begin() + static_cast<std::size_t>(boundary)});
        std::vector<fs::path> sources{standard};
#if MMD_ORIGINAL_PROFILE
        // SaveSceneCopy may append its own backup-name metadata even in this
        // profile. The real original parser must still ignore those bytes.
        sources.push_back(savedPath);
#endif
#if !MMD_ORIGINAL_PROFILE
        const auto extended = directory / (version ? L"large-0003-mms.pmm" : L"large-0002-mms.pmm");
        WriteBufferedBytes(extended, {saved.begin(), saved.begin() + static_cast<std::size_t>(boundary)});
        pmm_extension::Set(document, kBufferedUnknown, unknown);
        fd = _wopen(extended.c_str(), _O_BINARY | _O_RDWR); Check(fd >= 0 && _lseeki64(fd, 0, SEEK_END) == boundary,
            "append an unknown opaque record beyond the real standard prefix on the same writable file");
        Check(pmm_extension::Append(fd, document) && _close(fd) == 0, "the production extension writer supplies valid lengths and CRC");
        const auto rejected = directory / (version ? L"large-0003-mds.pmm" : L"large-0002-mds.pmm");
        auto old = Bytes(extended); std::memcpy(old.data() + old.size() - pmm_extension::kTrailerSize, "MDSXPMM2", 8);
        WriteBufferedBytes(rejected, old);
        const auto invalid = directory / (version ? L"large-0003-bad.pmm" : L"large-0002-bad.pmm");
        auto bad = Bytes(extended); bad[bad.size() - 8] ^= 1; WriteBufferedBytes(invalid, bad);
        sources.insert(sources.end(), {extended, rejected, invalid});
#endif
        for (const auto& source : sources) {
            const auto before = Bytes(source); std::uint64_t directCalls = 0;
            for (bool buffered : {false, true}) {
                pmm_io::bufferedReads = buffered; Arm(source, Fault::None, false); expectedBoundary = boundary;
                const bool loaded = LoadSceneFile(source.c_str()); Finish(loaded, true);
                Check(app->CurrentFrame() == (version ? frame_limits::legacyMaximum + 1 : 12),
                    "buffering preserves the real saved current frame including an actual 0003 writer selection");
                MatchBufferedCameras();
                Check(Bytes(source) == before && inputReads == pmm_io::readStatistics.systemReads + 1,
                    "an independent real CRT observer agrees with body counters plus the original shell header read");
                if (pmm_io::readStatistics.bytes != boundary - 30 || pmm_io::readStatistics.requests <= kBufferedCameraCount)
                    std::printf("DIAG body bytes=%llu expected=%llu requests=%llu realCalls=%llu\n",
                        static_cast<unsigned long long>(pmm_io::readStatistics.bytes), static_cast<unsigned long long>(boundary - 30),
                        static_cast<unsigned long long>(pmm_io::readStatistics.requests), static_cast<unsigned long long>(inputReads));
                Check(pmm_io::readStatistics.bytes == boundary - 30 && pmm_io::readStatistics.requests > kBufferedCameraCount,
                    "the standard body delivers its exact consumed bytes through the real native parser");
#if !MMD_ORIGINAL_PROFILE
                const bool valid = source == extended;
                const auto* actual = pmm_extension::Find(app->PmmProjectSettings().document, kBufferedUnknown);
                Check(extensionCalls == 1 && (valid ? actual && actual->value == unknown : actual == nullptr),
                    "only valid MMS records apply; absent, old-marker and damaged CRC input keep their original rejection semantics");
                if (buffered && source != standard) Check(bodyRewinds == 1,
                    "real prefetch crosses the standard boundary and is rewound once before actual extension decoding");
                if (valid && buffered) {
                    const auto roundtrip = directory / (version ? L"roundtrip-0003.pmm" : L"roundtrip-0002.pmm");
                    Check(SaveSceneCopy(app, roundtrip.c_str()), "save the actual buffered workspace through the production writer");
                    int copy = _wopen(roundtrip.c_str(), _O_BINARY | _O_RDONLY);
                    pmm_extension::Document again; std::uint64_t copyBoundary = 0;
                    Check(copy >= 0 && pmm_extension::Read(copy, again, copyBoundary) == pmm_extension::ReadResult::Loaded && _close(copy) == 0,
                        "decode the real save of the buffered workspace");
                    const auto* retained = pmm_extension::Find(again, kBufferedUnknown);
                    Check(retained && retained->value == unknown && copyBoundary == boundary,
                        "the unknown record and actual standard-body extent survive genuine save without rereading the source");
                }
#else
                Check(!extensionCalls, "the original-compatible parser never applies an extension");
#endif
                if (!buffered) directCalls = inputReads;
                else Check(inputReads > 0 && inputReads < directCalls && pmm_io::readStatistics.buffered > 0,
                    "the same complete native payload uses fewer actual CRT reads through the real buffered path");
                std::printf("DIAG real buffered PMM version=%u file=%ls buffered=%u CRT=%llu requests=%llu bytes=%llu rewinds=%llu\n",
                    version ? 3u : 2u, source.filename().c_str(), unsigned(buffered),
                    static_cast<unsigned long long>(inputReads), static_cast<unsigned long long>(pmm_io::readStatistics.requests),
                    static_cast<unsigned long long>(pmm_io::readStatistics.bytes), static_cast<unsigned long long>(bodyRewinds));
            }
        }
#if !MMD_ORIGINAL_PROFILE
        pmm_io::bufferedReads = true; Arm(extended, Fault::None, false); expectedBoundary = boundary; refuseSync = true;
        const bool loaded = LoadSceneFile(extended.c_str()); Finish(loaded, true); MatchBufferedCameras();
        Check(bodyRewinds == 1 && !extensionCalls && app->PmmProjectSettings().document.records.empty(),
            "a valid-fd sync failure refuses the real extension handoff, closes once and retains the parsed standard scene");
        refuseSync = false;
#endif
    }
    std::puts("STAGE actual strict PMM truncated-field cleanup in buffered and direct modes");
    const auto truncated = directory / L"read-truncated.pmm";
    auto shortBytes = Bytes(ordinary); shortBytes.resize(64); WriteBufferedBytes(truncated, shortBytes);
    for (bool buffered : {false, true}) {
        pmm_io::bufferedReads = buffered; const auto tails = tailCalls, errors = messages;
        Arm(truncated, Fault::ReadFailure, false);
        const bool loaded = LoadSceneFile(truncated.c_str()); Finish(loaded, false);
        Check(tailCalls == tails && messages == errors + 1 && Bytes(truncated) == shortBytes,
            "real native short-read failure has no success tail, partial objects or source rewrite");
    }
    pmm_io::bufferedReads = true; Normal(ordinary, camera, light); Normal(v1, camera, light);
}
