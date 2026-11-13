// Internal detached-representation checks only. The including TU supplies
// Check(bool,const char*) and the operator-new injection counter failAfter.
// No app writes, production registration, sampling or PMM coverage is implied.
#include "../src/animation/negative_authoring.hpp"
#include <new>
#include <type_traits>

namespace {
namespace negative_authoring_checks {
namespace au = mikumikustudio::negative_frames::authoring;
namespace ac = mikumikustudio::animation_curve;
namespace ce = mikumikustudio::curve_editor;
namespace fl = mikumikustudio::frame_limits;
constexpr std::array<au::Family,7> families{au::Family::Bone, au::Family::Morph, au::Family::Camera,
    au::Family::Light, au::Family::Shadow, au::Family::Gravity, au::Family::Accessory};
constexpr std::array<unsigned,7> laneCounts{9,1,11,6,2,6,12};
static_assert(std::is_same<au::Ptr, std::shared_ptr<const au::KeyData>>::value, "key ownership is const");
static_assert(std::is_same<au::CurvesPtr, std::shared_ptr<const au::Curves>>::value, "curve ownership is const");
static_assert(!std::is_default_constructible<au::Curves>::value &&
    !std::is_copy_constructible<au::Curves>::value && !std::is_copy_assignable<au::Curves>::value,
    "curves can only be published through validation");
static_assert(!std::is_default_constructible<au::KeyData>::value &&
    !std::is_copy_constructible<au::KeyData>::value && !std::is_copy_assignable<au::KeyData>::value,
    "keys can only be published through validation");
static_assert(std::is_same<decltype(std::declval<const au::Curves&>().Data()), const au::Track&>::value,
    "frozen track is read only");
static_assert(std::is_same<decltype(std::declval<const au::KeyData&>().Native()), const au::NativeData&>::value,
    "native data is read only");
static_assert(noexcept(au::Same(std::declval<const au::Ptr&>(), std::declval<const au::Ptr&>())) &&
    noexcept(au::RetainedBytes(std::declval<const au::Ptr&>())) &&
    noexcept(std::declval<const au::Curves&>().RetainedBytes()) &&
    noexcept(std::declval<const au::Curves&>().Data()) &&
    noexcept(std::declval<const au::KeyData&>().FamilyKind()) &&
    noexcept(std::declval<const au::KeyData&>().Semantics()) &&
    noexcept(std::declval<const au::KeyData&>().Native()) &&
    noexcept(std::declval<const au::KeyData&>().Author()), "read-only access never throws");

bool IsDiscrete(au::Family family, unsigned i) {
    switch (family) {
    case au::Family::Camera: return i == 8 || i == 9 || i == 10;
    case au::Family::Shadow: return i == 0;
    case au::Family::Gravity: return i == 4 || i == 5;
    case au::Family::Accessory: return i >= 8 && i <= 11;
    default: return false;
    }
}
au::Track Seed(au::Family family) {
    au::Track data; data.family = family;
    unsigned count = 0;
    for (std::size_t i = 0; i < families.size(); ++i) if (families[i] == family) count = laneCounts[i];
    for (unsigned i = 0; i < count; ++i) {
        au::Lane lane; lane.index = i; lane.track.discrete = IsDiscrete(family, i);
        lane.interpolationGroup = std::string(80, 'g') + std::to_string(i);
        for (int j = 0; j < 3; ++j) {
            ac::Key key;
            key.frame = j == 0 ? -60 - int(i) : j == 1 ? -10 + int(i) : 50 + int(i);
            // No lane has a zero-frame anchor, including the camera parent lanes.
            if (!key.frame) key.frame = 1;
            key.value = lane.track.discrete ? double(j % 2) : i + .25 + j;
            key.inSlope = .125; key.outSlope = -.25;
            key.inWeight = .2; key.outWeight = .3;
            key.tangent = ac::Tangent::Free;
            key.interpolation = lane.track.discrete ? ac::Interpolation::Step : ac::Interpolation::Bezier;
            lane.track.keys.push_back(key);
        }
        data.lanes.push_back(std::move(lane));
    }
    return data;
}
std::array<double,6> Geometry(const ac::Key& a, const ac::Key& b) {
    return {a.value, a.outWeight, a.outSlope, b.value, b.inWeight, b.inSlope};
}
au::Track Camera() {
    auto data = Seed(au::Family::Camera);
    for (unsigned i = 0; i < 8; ++i) {
        auto& lane = data.lanes[i]; lane.nativeCameraTiming = true;
        lane.nativeControls = {{0,0,127,127}, {127,32,0,96}, {0,127,127,0}};
        lane.controlGeometry.push_back({-1,0,-1,0});
        for (std::size_t end = 1; end < lane.track.keys.size(); ++end) {
            const auto& a = lane.track.keys[end - 1]; const auto& b = lane.track.keys[end];
            lane.controlGeometry.push_back({a.outWeight,a.outSlope,b.inWeight,b.inSlope});
            ce::NativeCameraInterval interval;
            interval.first = a.frame; interval.last = b.frame;
            interval.sourceFirst = -200; interval.sourceLast = 200;
            interval.sampleFirst = -120.5 + 20.0 * double(end); interval.sampleLast = -80.25 + 20.0 * double(end);
            interval.sourceValueFirst = -2.25; interval.sourceValueLast = 7.125;
            interval.controls = lane.nativeControls[end]; interval.geometry = Geometry(a,b);
            interval.oneFrameHold = true; interval.transportTiming = true;
            lane.preservedCameraIntervals.push_back(interval);
        }
    }
    return data;
}
au::NativeData Native(au::Family family) {
    au::NativeData result;
    if (family == au::Family::Bone) {
        result.boneControlsPresent = result.scaleControlsPresent = true;
        for (unsigned i = 0; i < 16; ++i) result.boneControls[i] = std::uint8_t(i * 7);
        result.scaleControls = {0,32,127,96};
    }
    if (family == au::Family::Camera) {
        result.cameraControlsPresent = true;
        for (unsigned row = 0; row < 4; ++row) for (unsigned lane = 0; lane < 6; ++lane)
            result.cameraControls[row * 6 + lane] = std::uint8_t(row * 30 + lane);
    }
    return result;
}
au::Ptr Authored(const au::Track& data, const au::NativeData& native = {}) {
    return au::Make(data.family, au::Mode::Authored, native, au::Freeze(data));
}
template<class F> void Rejected(F action) {
    bool invalid = false, other = false;
    try { action(); } catch (const std::invalid_argument&) { invalid = true; } catch (...) { other = true; }
    Check(invalid && !other, "bad authoring input throws invalid_argument specifically");
}
template<class F> void Changed(const au::Track& data, F change) {
    auto modified = data; change(modified);
    const auto a = Authored(data), b = Authored(modified);
    Check(!au::Same(a,b) && !au::Same(b,a), "deep semantic comparison detects the changed authored field");
}

void FamiliesAndNative() {
    Check(au::Same({}, {}) && au::RetainedBytes({}) == 0, "null is the free legacy default");
    for (std::size_t f = 0; f < families.size(); ++f) {
        const auto family = families[f];
        const auto source = Seed(family);
        const auto native = Native(family);
        au::Ptr legacy;
        bool threw = false;
        failAfter = 0;
        try { legacy = au::Make(family, au::Mode::LegacyLinear, {}); } catch (...) { threw = true; }
        const auto remaining = failAfter; failAfter = -1;
        Check(!threw && !legacy && remaining == 0, "every family shares allocation-free legacy semantics");
        const auto stored = au::Make(family, au::Mode::Native, native);
        const auto author = Authored(source, native);
        const auto independent = Authored(source, native);
        Check(stored && stored->FamilyKind() == family && stored->Semantics() == au::Mode::Native && !stored->Author(),
            "native key has its family and explicit native semantics");
        Check(author->FamilyKind() == family && author->Semantics() == au::Mode::Authored &&
            author->Author()->Data().family == family && author->Author()->Data().lanes.size() == laneCounts[f],
            "all seven families retain their complete canonical lane sets");
        Check(author.get() != independent.get() && author->Author().get() != independent->Author().get() &&
            au::Same(author,independent) && au::Same(stored,au::Make(family,au::Mode::Native,native)),
            "independently allocated equal data compares equal");
        Check(!au::Same(legacy,stored) && !au::Same(stored,legacy) && !au::Same(stored,author),
            "legacy native and authored remain different semantics");
        for (unsigned i = 0; i < laneCounts[f]; ++i) {
            const auto& lane = author->Author()->Data().lanes[i];
            Check(lane.index == i && lane.track.discrete == IsDiscrete(family,i) &&
                lane.track.keys.front().frame == -60 - int(i) && lane.track.keys.back().frame == 50 + int(i),
                "each lane keeps independent sparse signed frames without a zero anchor");
        }
        auto extremes = source;
        for (auto& lane : extremes.lanes) {
            lane.track.keys.front().frame = fl::minimum; lane.track.keys.back().frame = fl::maximum;
        }
        Check(bool(Authored(extremes)), "every family accepts full signed spans wider than int32");
        auto single = source;
        for (auto& lane : single.lanes) lane.track.keys.resize(1);
        Check(bool(Authored(single)), "one negative key per lane is complete authoring");
        for (unsigned i = 0; i < laneCounts[f]; ++i) {
            auto invalid = source; invalid.lanes[i].track.discrete = !invalid.lanes[i].track.discrete;
            Rejected([&] { au::Freeze(invalid); });
            if (IsDiscrete(family,i)) {
                invalid = source; invalid.lanes[i].track.keys[0].value = .5;
                Rejected([&] { au::Freeze(invalid); });
            }
        }
    }
    Check(!au::Same(au::Make(au::Family::Morph,au::Mode::Native,{}),
        au::Make(au::Family::Light,au::Mode::Native,{})), "family participates in deep equality");
    auto native = Native(au::Family::Bone);
    const auto unknown = au::Make(au::Family::Bone,au::Mode::Native,native);
    native.physicsPresent = true;
    const auto enabled = au::Make(au::Family::Bone,au::Mode::Native,native);
    native.physicsDisabled = true;
    const auto disabled = au::Make(au::Family::Bone,au::Mode::Native,native);
    Check(!au::Same(unknown,enabled) && !au::Same(enabled,disabled) && !au::Same(unknown,disabled),
        "unknown physics is distinct from explicit enabled and disabled physics");
    auto bone = Seed(au::Family::Bone);
    au::NativeData physics; physics.physicsPresent = true;
    Check(bool(Authored(bone,physics)), "authored bone may keep physics without raw controls");
    Check(!au::Same(Authored(bone),Authored(bone,Native(au::Family::Bone))), "raw controls presence is semantic");
    auto camera = Seed(au::Family::Camera);
    Check(!au::Same(Authored(camera),Authored(camera,Native(au::Family::Camera))), "camera controls presence is semantic");
    au::NativeData zeroBone, zeroCamera;
    zeroBone.boneControlsPresent = zeroBone.scaleControlsPresent = true;
    zeroCamera.cameraControlsPresent = true;
    Check(!au::Same(Authored(bone),Authored(bone,zeroBone)) &&
        !au::Same(Authored(camera),Authored(camera,zeroCamera)), "presence alone differs even when every control is zero");
    for (const auto family : {au::Family::Bone,au::Family::Camera}) {
        const auto initial = Native(family);
        const auto key = au::Make(family,au::Mode::Native,initial);
        const unsigned bytes = family == au::Family::Bone ? 20 : 24;
        for (unsigned i = 0; i < bytes; ++i) {
            auto changed = initial;
            if (family == au::Family::Camera) ++changed.cameraControls[i];
            else if (i < 16) ++changed.boneControls[i];
            else changed.scaleControls[i - 16] = std::uint8_t(changed.scaleControls[i - 16] == 127 ?
                126 : changed.scaleControls[i - 16] + 1);
            Check(!au::Same(key,au::Make(family,au::Mode::Native,changed)), "every native control byte is compared");
        }
    }
    const auto raw = Native(au::Family::Camera);
    const auto held = au::Make(au::Family::Camera,au::Mode::Native,raw);
    for (unsigned row = 0; row < 4; ++row) for (unsigned lane = 0; lane < 6; ++lane)
        Check(held->Native().cameraControls[row * 6 + lane] == row * 30 + lane,
            "camera controls preserve the interpolation four-by-six row order");
}

void EqualityAndOwnership() {
    const auto source = Seed(au::Family::Morph);
    for (unsigned field = 0; field < 8; ++field) Changed(source,[&](au::Track& data) {
        auto& key = data.lanes[0].track.keys[1];
        switch (field) {
        case 0: ++key.frame; break;
        case 1: key.value += .125; break;
        case 2: key.inSlope += .125; break;
        case 3: key.outSlope += .125; break;
        case 4: key.inWeight += .01; break;
        case 5: key.outWeight += .01; break;
        case 6: key.interpolation = ac::Interpolation::Linear; break;
        case 7: key.tangent = ac::Tangent::Flat; break;
        }
    });
    Changed(source,[](au::Track& data) { data.lanes[0].track.keys.pop_back(); });
    Changed(source,[](au::Track& data) { data.lanes[0].interpolationGroup += "changed"; });
    auto exact = Camera();
    for (unsigned component = 0; component < 4; ++component) {
        Changed(exact,[&](au::Track& data) { data.lanes[0].nativeControls[0][component] = 64; });
        Changed(exact,[&](au::Track& data) { data.lanes[0].controlGeometry[0][component] += .25; });
        Changed(exact,[&](au::Track& data) { data.lanes[0].preservedCameraIntervals[0].controls[component] = 64; });
    }
    Changed(exact,[](au::Track& data) { data.lanes[0].preservedCameraIntervals.pop_back(); });
    Changed(exact,[](au::Track& data) { data.lanes[0].nativeControls.pop_back(); });
    Changed(exact,[](au::Track& data) { data.lanes[0].controlGeometry.pop_back(); });
    auto timingOnly = Seed(au::Family::Camera);
    Changed(timingOnly,[](au::Track& data) { data.lanes[0].nativeCameraTiming = true; });
    // Isolate a slice so endpoint changes do not invalidate its neighbour.
    exact.lanes[0].preservedCameraIntervals.resize(1);
    for (unsigned field = 0; field < 10; ++field) Changed(exact,[&](au::Track& data) {
        auto& lane = data.lanes[0]; auto& interval = lane.preservedCameraIntervals[0];
        switch (field) {
        case 0: --interval.first; --lane.track.keys[0].frame; break;
        case 1: ++interval.last; ++lane.track.keys[1].frame; break;
        case 2: --interval.sourceFirst; break;
        case 3: ++interval.sourceLast; break;
        case 4: interval.sampleFirst += .125; break;
        case 5: interval.sampleLast += .125; break;
        case 6: interval.sourceValueFirst += .125; break;
        case 7: interval.sourceValueLast += .125; break;
        case 8: interval.oneFrameHold = false; break;
        case 9: interval.transportTiming = false; break;
        }
    });
    for (unsigned field = 0; field < 6; ++field) Changed(exact,[&](au::Track& data) {
        auto& lane = data.lanes[0]; auto& a = lane.track.keys[0]; auto& b = lane.track.keys[1];
        switch (field) {
        case 0: a.value += .125; break;
        case 1: a.outWeight += .01; break;
        case 2: a.outSlope += .125; break;
        case 3: b.value += .125; break;
        case 4: b.inWeight += .01; break;
        case 5: b.inSlope += .125; break;
        }
        lane.preservedCameraIntervals[0].geometry = Geometry(a,b);
    });
    const auto original = Camera();
    const auto expected = Authored(original,Native(au::Family::Camera));
    au::Ptr held, shared;
    std::weak_ptr<const au::Curves> lifetime;
    {
        auto input = original; auto native = Native(au::Family::Camera);
        auto curves = au::Freeze(input); lifetime = curves;
        held = au::Make(input.family,au::Mode::Authored,native,curves);
        shared = au::Make(input.family,au::Mode::Authored,native,curves);
        input.family = au::Family::Bone;
        for (auto& lane : input.lanes) {
            lane.index = 99; lane.track.keys[0].value += 7; lane.track.discrete = !lane.track.discrete;
            lane.nativeControls.clear(); lane.controlGeometry.clear(); lane.preservedCameraIntervals.clear();
            lane.interpolationGroup.assign("mutated"); lane.nativeCameraTiming = false;
        }
        input.lanes.clear(); native.cameraControls.fill(127);
        Check(au::Same(held,expected), "freezing and native construction deeply detach every input field");
    }
    Check(!lifetime.expired() && au::Same(held,expected) && held->Author() == shared->Author(),
        "multiple immutable rows own complete curves after the original input dies");
    for (unsigned lane = 0; lane < 8; ++lane)
        Check(held->Author()->Data().lanes[lane].preservedCameraIntervals == original.lanes[lane].preservedCameraIntervals,
            "every exact camera interval field survives freezing");
    held.reset(); Check(!lifetime.expired() && au::Same(shared,expected), "second row independently retains curves");
    shared.reset(); Check(lifetime.expired(), "last row releases curves without a registry or live source");
    auto large = Camera();
    auto& lane = large.lanes[0]; lane.track.keys.resize(2);
    lane.track.keys[0].frame = fl::minimum; lane.track.keys[1].frame = fl::maximum;
    lane.preservedCameraIntervals.resize(1);
    auto& interval = lane.preservedCameraIntervals[0];
    interval.first = interval.sourceFirst = fl::minimum; interval.last = interval.sourceLast = fl::maximum;
    interval.sampleFirst = fl::minimum; interval.sampleLast = fl::maximum;
    interval.geometry = Geometry(lane.track.keys[0],lane.track.keys[1]);
    Check(bool(Authored(large)), "exact intervals accept full signed source and target spans using int64");
    const std::array<std::array<double,4>,4> edgeControls{{{127,0,0,127}, {0,0,0,127}, {127,0,127,127}, {0,0,127,127}}};
    for (const auto& controls : edgeControls) {
        interval.controls = controls; lane.nativeControls[1] = controls;
        Check(bool(Authored(large)), "native camera crossed and endpoint X controls are legal");
    }
}

void BadInput() {
    const auto source = Seed(au::Family::Morph);
    const auto camera = Camera();
    Rejected([&] { auto data = source; data.family = au::Family(99); au::Freeze(data); });
    for (const auto family : families) {
        auto data = Seed(family); data.lanes.pop_back(); Rejected([&] { au::Freeze(data); });
        data = Seed(family); data.lanes.push_back(data.lanes[0]); Rejected([&] { au::Freeze(data); });
        data = Seed(family); data.lanes[0].index = 1; Rejected([&] { au::Freeze(data); });
        data = Seed(family); data.lanes[0].track.keys.clear(); Rejected([&] { au::Freeze(data); });
        if (data.lanes.size() > 1) {
            data = Seed(family); std::swap(data.lanes[0],data.lanes[1]); Rejected([&] { au::Freeze(data); });
        }
    }
    const double inf = (std::numeric_limits<double>::infinity)();
    const double nan = (std::numeric_limits<double>::quiet_NaN)();
    for (unsigned variant = 0; variant < 17; ++variant) {
        auto data = source; auto& key = data.lanes[0].track.keys[1];
        switch (variant) {
        case 0: key.value = nan; break;
        case 1: key.inSlope = inf; break;
        case 2: key.outSlope = nan; break;
        case 3: key.inWeight = inf; break;
        case 4: key.outWeight = nan; break;
        case 5: key.inWeight = 0; break;
        case 6: key.outWeight = 1; break;
        case 7: key.interpolation = ac::Interpolation(99); break;
        case 8: key.tangent = ac::Tangent(99); break;
        case 9: key.frame = data.lanes[0].track.keys[0].frame; break;
        case 10: key.frame = data.lanes[0].track.keys[0].frame - 1; break;
        case 11: data.lanes[0].track.keys.front().frame = fl::minimum - 1; break;
        case 12: data.lanes[0].track.keys.back().frame = fl::maximum + 1; break;
        case 13: key.inWeight = .9; break;
        case 14: data.lanes[0].nativeControls.push_back({0,256,0,0}); break;
        case 15: data.lanes[0].nativeControls.push_back({0,nan,0,0}); break;
        case 16: data.lanes[0].controlGeometry.push_back({0,0,inf,0}); break;
        }
        Rejected([&] { au::Freeze(data); });
    }
    {
        auto raw = camera;
        raw.lanes[0].nativeControls[0] = {255,128,0,255};
        const auto frozen = au::Freeze(raw);
        Check(frozen->Data().lanes[0].nativeControls[0] == raw.lanes[0].nativeControls[0],
            "unsampled first-key raw metadata survives without normalizing native source bytes");
        raw.lanes[0].nativeControls[1][0] = 128;
        Rejected([&] { au::Freeze(raw); });
    }
    for (unsigned variant = 0; variant < 24; ++variant) {
        auto data = camera; auto& lane = data.lanes[0]; auto& value = lane.preservedCameraIntervals[0];
        switch (variant) {
        case 0: value.first = value.last; break;
        case 1: value.sourceFirst = value.sourceLast; break;
        case 2: value.first = fl::minimum - 1; break;
        case 3: value.last = fl::maximum + 1; break;
        case 4: value.sourceFirst = fl::minimum - 1; break;
        case 5: value.sourceLast = fl::maximum + 1; break;
        case 6: value.sampleFirst = nan; break;
        case 7: value.sampleLast = inf; break;
        case 8: value.sampleFirst = value.sourceFirst - 1.; break;
        case 9: value.sampleLast = value.sourceLast + 1.; break;
        case 10: value.sampleLast = value.sampleFirst; break;
        case 11: value.sourceValueFirst = inf; break;
        case 12: value.sourceValueLast = nan; break;
        case 13: value.controls[0] = -1; break;
        case 14: value.controls[2] = 128; break;
        case 15: value.controls[3] = nan; break;
        case 16: value.geometry[0] += 1; break;
        case 17: value.geometry[1] = nan; break;
        case 18: value.oneFrameHold = value.transportTiming = false; break;
        case 19: --value.first; break;
        case 20: ++value.last; break;
        case 21: lane.track.keys[1].interpolation = ac::Interpolation::Linear; break;
        case 22: lane.preservedCameraIntervals[1] = value; break;
        case 23: std::swap(lane.preservedCameraIntervals[0],lane.preservedCameraIntervals[1]); break;
        }
        Rejected([&] { au::Freeze(data); });
    }
    for (unsigned component = 0; component < 6; ++component) {
        auto data = camera; data.lanes[0].preservedCameraIntervals[0].geometry[component] += .01;
        Rejected([&] { au::Freeze(data); });
    }
    for (const auto family : families) {
        if (family == au::Family::Camera) continue;
        auto data = Seed(family); data.lanes[0].nativeCameraTiming = true;
        Rejected([&] { au::Freeze(data); });
        data.lanes[0].nativeCameraTiming = false;
        data.lanes[0].preservedCameraIntervals = camera.lanes[0].preservedCameraIntervals;
        Rejected([&] { au::Freeze(data); });
    }
    for (unsigned i = 8; i < 11; ++i) {
        auto data = camera; data.lanes[i].nativeCameraTiming = true; Rejected([&] { au::Freeze(data); });
        data.lanes[i].nativeCameraTiming = false;
        data.lanes[i].preservedCameraIntervals = data.lanes[0].preservedCameraIntervals;
        Rejected([&] { au::Freeze(data); });
    }
    auto noTiming = camera; noTiming.lanes[0].nativeCameraTiming = false;
    Rejected([&] { au::Freeze(noTiming); });
    const auto curves = au::Freeze(source);
    Rejected([&] { au::Make(au::Family(99),au::Mode::LegacyLinear,{}); });
    Rejected([&] { au::Make(au::Family::Morph,au::Mode(99),{}); });
    Rejected([&] { au::Make(au::Family::Morph,au::Mode::Native,{},curves); });
    Rejected([&] { au::Make(au::Family::Morph,au::Mode::LegacyLinear,{},curves); });
    for (const auto family : families) {
        Rejected([&] { au::Make(family,au::Mode::Authored,{}); });
        if (family != au::Family::Morph) Rejected([&] { au::Make(family,au::Mode::Authored,{},curves); });
        au::NativeData hidden; hidden.physicsDisabled = true;
        Rejected([&] { au::Make(family,au::Mode::LegacyLinear,hidden); });
        if (family != au::Family::Bone) {
            auto native = Native(family); native.physicsPresent = true;
            Rejected([&] { au::Make(family,au::Mode::Native,native); });
        }
    }
    for (unsigned i = 0; i < 44; ++i) {
        au::NativeData hidden;
        if (i < 16) hidden.boneControls[i] = 1;
        else if (i < 20) hidden.scaleControls[i - 16] = 1;
        else hidden.cameraControls[i - 20] = 1;
        Rejected([&] { au::Make(au::Family::Bone,au::Mode::LegacyLinear,hidden); });
    }
    for (const auto family : {au::Family::Bone,au::Family::Camera}) {
        const auto native = Native(family);
        Rejected([&] { au::Make(family,au::Mode::Native,{}); });
        Rejected([&] { au::Make(family,au::Mode::LegacyLinear,native); });
        const unsigned count = family == au::Family::Bone ? 20 : 24;
        for (unsigned i = 0; i < count; ++i) {
            auto invalid = native;
            if (family == au::Family::Camera) invalid.cameraControls[i] = 128;
            else if (i < 16) invalid.boneControls[i] = 128;
            else invalid.scaleControls[i - 16] = 128;
            Rejected([&] { au::Make(family,au::Mode::Native,invalid); });
        }
        for (const auto other : families) if (other != family)
            Rejected([&] { au::Make(other,au::Mode::Native,native); });
    }
    for (bool bonePresent : {false,true}) {
        au::NativeData partial; partial.boneControlsPresent = bonePresent; partial.scaleControlsPresent = !bonePresent;
        const auto bone = au::Freeze(Seed(au::Family::Bone));
        Rejected([&] { au::Make(au::Family::Bone,au::Mode::Authored,partial,bone); });
        Rejected([&] { au::Make(au::Family::Bone,au::Mode::Native,partial); });
    }
    au::NativeData physics; physics.physicsPresent = true;
    Rejected([&] { au::Make(au::Family::Bone,au::Mode::LegacyLinear,physics); });
}

void BudgetAndFailure() {
    auto source = Camera();
    source.lanes.reserve(32);
    for (auto& lane : source.lanes) {
        lane.track.keys.reserve(40); lane.interpolationGroup.reserve(240);
        lane.nativeControls.reserve(30); lane.controlGeometry.reserve(31); lane.preservedCameraIntervals.reserve(32);
    }
    const auto native = Native(au::Family::Camera);
    const auto expected = Authored(source,native);
    const auto independent = Authored(source,native);
    const auto& curves = *expected->Author(); const auto& data = curves.Data();
    std::uint64_t lower = sizeof(au::Curves) + data.lanes.capacity() * sizeof(au::Lane);
    for (const auto& lane : data.lanes) {
        lower += lane.track.keys.capacity() * sizeof(ac::Key) + lane.interpolationGroup.capacity() + 1;
        lower += lane.nativeControls.capacity() * sizeof(std::array<double,4>);
        lower += lane.controlGeometry.capacity() * sizeof(std::array<double,4>);
        lower += lane.preservedCameraIntervals.capacity() * sizeof(ce::NativeCameraInterval);
    }
    Check(curves.RetainedBytes() >= lower + 2 * sizeof(void*), "curve budget includes every capacity and control-block allowance");
    Check(au::RetainedBytes(expected) >= sizeof(au::KeyData) + curves.RetainedBytes() + 2 * sizeof(void*),
        "row budget includes both complete objects and both shared control blocks");
    const auto shared = au::Make(data.family,au::Mode::Authored,native,expected->Author());
    Check(au::RetainedBytes(shared) == au::RetainedBytes(expected), "each sharing row conservatively pays the full curve budget");
    const auto nativeOnly = au::Make(au::Family::Camera,au::Mode::Native,native);
    Check(au::RetainedBytes(nativeOnly) >= sizeof(au::KeyData) + 2 * sizeof(void*), "native-only rows include shared ownership cost");
    bool same = false; std::uint64_t bytes = 0, curveBytes = 0;
    failAfter = 0;
    same = au::Same(expected,independent); bytes = au::RetainedBytes(expected); curveBytes = curves.RetainedBytes();
    const auto remaining = failAfter; failAfter = -1;
    Check(same && remaining == 0 && bytes >= curveBytes, "deep comparison and retained-byte accounting allocate nothing");
    auto saturated = (std::numeric_limits<std::uint64_t>::max)() - 3;
    au::detail::Charge(saturated,2,4);
    Check(saturated == (std::numeric_limits<std::uint64_t>::max)(), "budget multiplication saturates before overflow");
    au::detail::Charge(saturated,1); au::detail::Charge(saturated,0,16);
    Check(saturated == (std::numeric_limits<std::uint64_t>::max)(), "saturated addition remains saturated");
    saturated = 1; au::detail::Charge(saturated,(std::numeric_limits<std::uint64_t>::max)(),2);
    Check(saturated == (std::numeric_limits<std::uint64_t>::max)(), "large products never wrap");
    // Measure once, then fail every allocation in the same detached operation.
    // No diagnostics or validation-oracle allocation occurs under injection.
    constexpr long allowance = 100000;
    au::Ptr measured; failAfter = allowance;
    try { measured = Authored(source,native); } catch (...) { failAfter = -1; throw; }
    const long cost = allowance - failAfter; failAfter = -1;
    Check(cost > 4 && cost < allowance, "measure all deep-copy and ownership allocation stages");
    for (long point = 0; point < cost; ++point) {
        auto published = expected; bool oom = false, wrong = false;
        failAfter = point;
        try { published = Authored(source,native); }
        catch (const std::bad_alloc&) { oom = true; }
        catch (...) { wrong = true; }
        failAfter = -1;
        Check(oom && !wrong && published == expected, "OOM propagates and cannot publish a partial key or curve");
        Check(au::Same(Authored(source,native),expected), "every failed allocation leaves original inputs unchanged and recovers");
    }
    for (const auto family : families) {
        const auto original = Native(family); au::Ptr seed = au::Make(family,au::Mode::Native,original);
        au::Ptr completed; failAfter = allowance;
        try { completed = au::Make(family,au::Mode::Native,original); } catch (...) { failAfter = -1; throw; }
        const long allocations = allowance - failAfter; failAfter = -1;
        Check(allocations > 0, "native object construction allocates owned storage");
        for (long point = 0; point < allocations; ++point) {
            auto published = seed; bool oom = false, wrong = false;
            failAfter = point;
            try { published = au::Make(family,au::Mode::Native,original); }
            catch (const std::bad_alloc&) { oom = true; } catch (...) { wrong = true; }
            failAfter = -1;
            Check(oom && !wrong && published == seed && au::Same(seed,au::Make(family,au::Mode::Native,original)),
                "every native allocation failure preserves its input and recovers");
        }
    }
}
}
void NativeNegativeAuthoringChecks() {
    Check(failAfter == -1, "negative authoring checks start outside allocation injection");
    negative_authoring_checks::FamiliesAndNative();
    negative_authoring_checks::EqualityAndOwnership();
    negative_authoring_checks::BadInput();
    negative_authoring_checks::BudgetAndFailure();
    Check(failAfter == -1, "negative authoring checks restore allocation injection");
}
}
