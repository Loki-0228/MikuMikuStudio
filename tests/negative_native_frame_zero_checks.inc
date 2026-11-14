#include "../src/animation/negative_native_frame_zero.hpp"
#include "../src/animation/negative_key_sampling.hpp"
#include <limits>

template<class Check>
void CheckNativeFrameZeroProjection(Check check) {
    namespace n = mikumikustudio::negative_frames;
    namespace z = n::native_zero;
    namespace s = n::sampling;
    namespace a = n::authoring;
    namespace m = mikumikustudio::mdl;

    m::CameraKey camera{};
    camera.eye[0]=32.f;camera.eye[1]=-2.f;camera.eye[2]=3.f;
    camera.target[0]=.125f;camera.target[1]=-.25f;camera.target[2]=.5f;
    camera.distance=-40.f;camera.fov=42;camera.perspective=1;
    camera.parentModel=4;camera.parentBone=17;
    for (unsigned i=0;i<4;++i) for (unsigned j=0;j<6;++j)
        camera.interpolation[i][j]=static_cast<std::uint8_t>(200+6*i+j);
    const auto cameraHead=z::Read(camera);
    check(cameraHead && cameraHead->family==z::Family::Camera &&
          cameraHead->values==z::Values{32.,-2.,3.,.125,-.25,.5,-40.,42.,1.,4.,17.},
          "camera zero stores all raw values in pose units");
    const auto& rawCamera=std::get<z::CameraHeadAuthor>(cameraHead->author);
    for (unsigned i=0;i<24;++i)
        check(rawCamera.controls[i]==200+i,"camera zero preserves row-major controls, including unused head bytes");

    m::LightKey light{};
    light.direction[0]=1.f;light.direction[1]=-2.f;light.direction[2]=3.f;
    light.color[0]=.5f;light.color[1]=.25f;light.color[2]=.125f;
    const auto lightHead=z::Read(light);
    check(lightHead && lightHead->family==z::Family::Light &&
          lightHead->values==z::Values{1.,-2.,3.,.5,.25,.125} &&
          std::holds_alternative<std::monostate>(lightHead->author),"light zero preserves direction and color");

    m::SelfShadowKey shadow{};
    shadow.mode=2;shadow.distance=.875f;
    const auto shadowHead=z::Read(shadow);
    check(shadowHead && shadowHead->family==z::Family::Shadow &&
          shadowHead->values==z::Values{2.,.875},"shadow zero stores step mode and distance");

    m::GravityKey gravity{};
    gravity.acceleration=8.f;gravity.direction[0]=1.f;gravity.direction[1]=-2.f;gravity.direction[2]=3.f;
    gravity.noise=17;gravity.noiseEnabled=1;gravity.selected=1;
    gravity.reserved1[0]=0xA3;gravity.reserved1[1]=0x95;
    const auto gravityHead=z::Read(gravity);
    check(gravityHead && gravityHead->family==z::Family::Gravity &&
          gravityHead->values==z::Values{8.,1.,-2.,3.,17.,1.} &&
          gravity.selected==1 && gravity.reserved1[0]==0xA3 && gravity.reserved1[1]==0x95,
          "gravity zero copies the enabled byte without reading it as the full mode word or changing native bytes");

    m::AccessoryKey accessory{};
    accessory.position[0]=1.f;accessory.position[1]=2.f;accessory.position[2]=3.f;
    accessory.rotation[0]=.125f;accessory.rotation[1]=-.25f;accessory.rotation[2]=.5f;
    accessory.scale=2.f;accessory.opacity=.75f;accessory.visible=1;accessory.shadowEnabled=1;
    accessory.parentModel=22;accessory.parentBone=300;
    const auto accessoryHead=z::Read(accessory);
    check(accessoryHead && accessoryHead->family==z::Family::Accessory &&
          accessoryHead->values==z::Values{1.,2.,3.,.125,-.25,.5,2.,.75,1.,1.,22.,300.},
          "accessory zero copies key pose parent fields, not a logical attachment");

    m::BoneKey bone{};
    bone.position[0]=4.f;bone.position[1]=5.f;bone.position[2]=6.f;bone.rotation[3]=1.f;
    for (unsigned i=0;i<16;++i) bone.interpolation[i]=static_cast<std::uint8_t>(150+i);
    bone.physicsDisabled=1;
    mikumikustudio::bone_scale::Matrix scale;
    scale.v={2.f,.5f,0.f,0.f,3.f,0.f,0.f,0.f,1.f};
    mikumikustudio::bone_scale::Curve scaleCurve{2,13,111,125};
    const auto boneHead=z::Read(bone,scale,scaleCurve);
    check(boneHead && boneHead->family==z::Family::Bone &&
          boneHead->values==z::Values{4.,5.,6.,0.,0.,0.,1.,2.,.5,0.,0.,3.,0.,0.,0.,1.},
          "bone zero preserves the registered matrix, including shear");
    const auto& rawBone=std::get<z::BoneHeadAuthor>(boneHead->author);
    for (unsigned i=0;i<16;++i)
        check(rawBone.controls[i]==150+i,"bone zero preserves raw controls without treating them as signed interval controls");
    check(rawBone.scaleControls==scaleCurve && rawBone.physicsDisabled==1,
          "bone zero retains scale controls and raw physics metadata separately");

    m::MorphKey morph{};
    morph.value=2.5f;
    const auto morphHead=z::Read(morph);
    check(morphHead && morphHead->family==z::Family::Morph &&
          morphHead->values==z::Values{2.5},"morph zero preserves values beyond the slider range");

    // A caller-supplied detached effective value is not recovered from the
    // row or live pose, even if those inputs subsequently change.
    auto effective=boneHead->values;effective[0]=11.;
    const auto boneProjection=z::Project(*boneHead,effective);
    check(boneProjection && boneProjection->head.values[0]==4. &&
          boneProjection->effective[0]==11. && !boneProjection->LegacyEndpoint().author,
          "effective model value is separate from the raw head and does not enable raw controls");
    bone.position[0]=99.f;bone.interpolation[0]=0;scale.v[1]=9.f;scaleCurve[0]=0;effective[0]=0.;
    check(boneProjection->head.values[0]==4. && boneProjection->head.values[8]==.5 &&
          boneProjection->effective[0]==11. &&
          std::get<z::BoneHeadAuthor>(boneProjection->head.author).controls[0]==150 &&
          std::get<z::BoneHeadAuthor>(boneProjection->head.author).scaleControls[0]==2,
          "projection keeps copies and does not alias mutable native rows or scale state");

    a::Track curve;
    curve.family=z::Family::Morph;
    curve.lanes.resize(1);
    curve.lanes[0].track.keys.resize(2);
    curve.lanes[0].track.keys[0].frame=0;
    curve.lanes[0].track.keys[0].value=9.;
    curve.lanes[0].track.keys[1].frame=7;
    curve.lanes[0].track.keys[1].value=10.;
    const auto frozen=a::Freeze(curve);
    const auto morphProjection=z::Project(*morphHead,{3.5},frozen);
    check(morphProjection && morphProjection->frozenAuthor==frozen &&
          morphProjection->LegacyEndpoint().frame==0 && !morphProjection->LegacyEndpoint().author &&
          morphProjection->LegacyEndpoint().values[0]==3.5,
          "frozen positive curve is retained but cannot drive or replace legacy zero");
    check(!z::Project(*boneHead,boneHead->values,frozen),"foreign-family frozen curve is refused");
    z::Values invalid{};
    invalid[0]=(std::numeric_limits<double>::infinity)();
    check(!z::Project(*morphHead,invalid),"nonfinite effective values are refused");
    invalid[0]=2.*(std::numeric_limits<float>::max)();
    check(!z::Project(*morphHead,invalid),"effective values outside the signed sampler's numeric range are refused");

    // The real negative sampler exposes what would change if raw upper
    // authoring were activated by the projection.
    n::detail::Key lower;
    lower.frame=-8;lower.values={1.,.125};
    s::Result legacy{},projected{},native{};
    n::detail::Key oldZero;
    oldZero.values=shadowHead->values;
    const auto shadowProjection=z::Project(*shadowHead,shadowHead->values);
    auto nativeZero=shadowProjection->LegacyEndpoint();
    nativeZero.author=a::Make(z::Family::Shadow,a::Mode::Native,{});
    check(s::Interval(z::Family::Shadow,lower,oldZero,-4,legacy) &&
          s::Interval(z::Family::Shadow,lower,shadowProjection->LegacyEndpoint(),-4,projected) &&
          s::Interval(z::Family::Shadow,lower,nativeZero,-4,native) &&
          projected.values==legacy.values && legacy.values[1]==.5 && native.values[1]==.125,
          "legacy seek parity holds at the shadow seam and activating native metadata would change it");
    lower.values=cameraHead->values;lower.values[0]=0.;
    oldZero.values=cameraHead->values;
    const auto cameraProjection=z::Project(*cameraHead,cameraHead->values);
    a::NativeData cameraControls;
    cameraControls.cameraControlsPresent=true;
    for (unsigned i=0;i<6;++i) {
        cameraControls.cameraControls[i]=32;cameraControls.cameraControls[i+6]=0;
        cameraControls.cameraControls[i+12]=96;cameraControls.cameraControls[i+18]=0;
    }
    nativeZero=cameraProjection->LegacyEndpoint();
    nativeZero.author=a::Make(z::Family::Camera,a::Mode::Native,cameraControls);
    check(s::Interval(z::Family::Camera,lower,oldZero,-4,legacy) &&
          s::Interval(z::Family::Camera,lower,cameraProjection->LegacyEndpoint(),-4,projected) &&
          s::Interval(z::Family::Camera,lower,nativeZero,-4,native) &&
          projected.values==legacy.values && legacy.values[0]==16. && native.values[0]<8.,
          "legacy camera midpoint matches the authorless zero, whereas a valid activated native upper curve bends it");

    lower.values={.5};
    check(s::Interval(z::Family::Morph,lower,morphProjection->LegacyEndpoint(),-4,projected) &&
          projected.values[0]==2.,
          "the projected sample uses the supplied effective value, not the raw weight or the frozen curve");

    camera.frame=light.frame=shadow.frame=gravity.frame=accessory.frame=bone.frame=morph.frame=1;
    check(!z::Read(camera) && !z::Read(light) && !z::Read(shadow) && !z::Read(gravity) &&
          !z::Read(accessory) && !z::Read(bone,scale,scaleCurve) && !z::Read(morph),
          "all continuous families refuse nonzero rows");
}
