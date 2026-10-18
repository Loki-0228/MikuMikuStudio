// This file is included after the independent VMD fixture and assertion
// helpers. Literal file-order controls catch lane swaps without sharing the
// production control-index mapping.
void TestCameraCompatibilityMatrix() {
    VmdFile file;
    CameraInput first, second;
    first.frame = 3; second.frame = 26;
    first.distance = -45; second.distance = -12;
    first.eye = {-2, 7, 9}; second.eye = {11, -8, 29};
    first.target = {.125, -.25, .375}; second.target = {-.5, .75, -.875};
    first.fov = 30; second.fov = 90; second.perspective = 1;
    first.block = {{20,107,20,107, 20,107,20,107, 20,107,20,107,
                    20,107,20,107, 20,107,20,107, 20,107,20,107}};
    second.block = {{18,103,95,12, 4,76,112,19, 55,111,5,124,
                     7,120,119,9, 34,95,96,45, 1,126,17,104}};
    // Reverse record order: the lane belongs to the interval ending at that
    // frame, not to file order or to the outgoing key.
    file.cameras = {second, first};
    const auto bytes = Build(file);
    cm::Motion imported; std::string error;
    if (!cm::Decode(bytes.data(), bytes.size(), cm::Kind::Camera, imported, error)) {
        Check(false, "six-lane standard camera imports: " + error); return;
    }
    const char* ids[] = {"x", "y", "z", "rx", "ry", "rz", "distance", "fov"};
    const int laneFor[] = {0,1,2,3,3,3,4,5};
    const double controls[][4] = {{18,95,103,12},{4,112,76,19},{55,5,111,124},
                                  {7,119,120,9},{34,96,95,45},{1,17,126,104}};
    const double degrees = 180.0 / 3.14159265358979323846;
    const double starts[] = {-2,7,9,.125*degrees,-.25*degrees,.375*degrees,-45,30};
    const double ends[] = {11,-8,29,-.5*degrees,.75*degrees,-.875*degrees,-12,90};
    for (std::size_t channel = 0; channel < 8; ++channel) {
        const auto* track = Find(imported, ids[channel]);
        Check(track && track->keys.size() == 2, std::string(ids[channel]) + " keeps both sparse keys");
        if (!track || track->keys.size() != 2) continue;
        Check(track->keys[0].frame == 3 && track->keys[1].frame == 26, "camera import sorts keys without shifting frames");
        const auto& lane = controls[laneFor[channel]];
        for (double offset : {0.0, .25, 3.0, 7.0, 11.5, 19.0, 23.0}) {
            const double eased = EaseFraction(lane[0], lane[1], lane[2], lane[3], offset / 23.0);
            CheckNear(ac::Evaluate(*track, 3 + offset).value,
                      starts[channel] + (ends[channel] - starts[channel]) * eased, 1e-6,
                      std::string("six-lane native camera mapping: ") + ids[channel]);
        }
    }
    const auto* perspective = Find(imported, "perspective");
    Check(perspective && perspective->discrete, "perspective remains a discrete camera channel");
    if (perspective) {
        Check(ac::Evaluate(*perspective, 25.99).value == 0 && ac::Evaluate(*perspective, 26).value == 1,
              "perspective switches at the ending key, not inside the interval");
    }

    std::vector<std::uint8_t> encoded;
    if (!cm::Encode(imported, encoded, error)) { Check(false, "camera re-export succeeds: " + error); return; }
    cm::Motion exact;
    Check(cm::Decode(encoded.data(), encoded.size(), cm::Kind::Camera, exact, error) && SameTracks(imported, exact),
          "camera export/import retains sparse frames, handles and discrete data in the extension");
    const auto standard = WithoutTail(encoded); BodyCounts counts; std::string parseError;
    BodyCounts originalCounts;std::string originalParse;
    Check(ParseBody(bytes,originalCounts,originalParse)&&ParseBody(standard, counts, parseError) && counts.end == standard.size() && counts.cameras == 2 && counts.hasIkSection&&
        std::memcmp(bytes.data()+originalCounts.cameraStart+4,standard.data()+counts.cameraStart+4,2*61)==0,
          "camera re-export retains both sparse standard records including every native control byte and the complete display/IK boundary");
    cm::Motion proxy;
    if (!cm::Decode(standard.data(), standard.size(), cm::Kind::Camera, proxy, error)) {
        Check(false, "camera proxy imports without the extension: " + error); return;
    }
    for (const char* id : ids) {
        const auto* original = Find(imported, id); const auto* sampled = Find(proxy, id);
        Check(original && sampled, "camera proxy exposes each standard lane");
        if (!original || !sampled) continue;
        for (int frame = 3; frame <= 26; ++frame) {
            const double expected = ac::Evaluate(*original, frame).value;
            const double tolerance = std::strcmp(id, "fov") == 0 ? .500001 : 2e-5;
            CheckNear(ac::Evaluate(*sampled, frame).value, expected, tolerance,
                      std::string("extension-free integer camera sample: ") + id);
        }
    }
    // A second standard-only pass cannot rely on the extension to hide proxy
    // field-order or unit errors introduced on the first export.
    std::vector<std::uint8_t> reencoded;
    Check(cm::Encode(proxy, reencoded, error), "standard-only camera can be exported again");
    const auto secondStandard = WithoutTail(reencoded); cm::Motion secondProxy;
    Check(cm::Decode(secondStandard.data(), secondStandard.size(), cm::Kind::Camera, secondProxy, error),
          "second standard-only camera pass decodes");
    for (const char* id : ids) {
        const auto* a = Find(proxy, id); const auto* b = Find(secondProxy, id);
        if (!a || !b) { Check(false, "second camera pass keeps each channel"); continue; }
        for (int frame = 3; frame <= 26; ++frame)
            CheckNear(ac::Evaluate(*b, frame).value, ac::Evaluate(*a, frame).value, 2e-5,
                      std::string("standard-only camera second-pass stability: ") + id);
    }
}

void TestLegacyCameraLayoutAndBoundaries() {
    VmdFile file; file.legacy = true;
    CameraInput first, second; second.frame = 23;
    first.distance = -45; second.distance = -12;
    first.eye = {-2,7,9}; second.eye = {11,-8,29};
    first.target = {.125,-.25,.375}; second.target = {-.5,.75,-.875};
    first.block = {{20,107,20,107}}; second.block = {{18,103,95,12}};
    file.cameras = {first,second};
    auto bytes = Build(file);
    // A real display record, including an IK entry, follows the light section
    // directly in VMD1. Treating it as a shadow section loses this boundary.
    bytes.resize(bytes.size() - 4); Cursor display;
    display.u32(1); display.u32(5); display.u8(1); display.u32(1);
    display.text("bone",20); display.u8(0);
    bytes.insert(bytes.end(), display.bytes.begin(), display.bytes.end());
    cm::Motion imported; std::string error;
    if (!cm::Decode(bytes.data(),bytes.size(),cm::Kind::Camera,imported,error)) {
        Check(false,"VMD1 36-byte camera and display/IK layout imports: "+error); return;
    }
    const char* ids[] = {"x","y","z","rx","ry","rz","distance"};
    const double degrees = 180.0 / 3.14159265358979323846;
    const double starts[] = {-2,7,9,.125*degrees,-.25*degrees,.375*degrees,-45};
    const double ends[] = {11,-8,29,-.5*degrees,.75*degrees,-.875*degrees,-12};
    const double eased = EaseFraction(18,95,103,12,7.0/23.0);
    for (std::size_t i=0;i<7;++i) {
        const auto* track=Find(imported,ids[i]); Check(track&&track->keys.size()==2,"VMD1 camera keeps both keys");
        if (track) CheckNear(ac::Evaluate(*track,7).value,starts[i]+(ends[i]-starts[i])*eased,1e-6,
                             std::string("VMD1 shared interpolation lane: ")+ids[i]);
    }
    const auto* fov=Find(imported,"fov"); const auto* perspective=Find(imported,"perspective");
    Check(fov && ac::Evaluate(*fov,7).value==45,"VMD1 supplies the original FOV 45 default");
    Check(perspective && ac::Evaluate(*perspective,23).value==0,"VMD1 supplies the original perspective default");
    BodyCounts counts; std::string parseError;
    Check(ParseBody(bytes,counts,parseError)&&counts.legacy&&counts.cameras==2&&counts.ikMasters==1&&counts.end==bytes.size(),
          "independent VMD1 walk reaches its display/IK boundary without a shadow section");
    // All optional section cutoffs remain legal, while partial camera and IK
    // records are refused without replacing the caller's existing motion.
    for (std::size_t size : {std::size_t(40+4+4+4+2*36), std::size_t(40+4+4+4+2*36+4)}) {
        cm::Motion shortMotion;
        Check(cm::Decode(bytes.data(),size,cm::Kind::Camera,shortMotion,error),"VMD1 complete optional cutoff imports");
    }
    for (std::size_t size : {std::size_t(40+4+4+4+35), bytes.size()-1}) {
        cm::Motion untouched=MotionFor(cm::Kind::Morph), expected=untouched;
        Check(!cm::Decode(bytes.data(),size,cm::Kind::Camera,untouched,error)&&SameTracks(untouched,expected),
              "VMD1 partial record is refused without replacing output");
    }
}
