// Internal row/retained-source integration. No author registration or signed
// sampler is exposed by these checks; the production v1 rows remain legacy.
#include "../src/animation/negative_key_data.hpp"
namespace {
namespace negative_key_data_checks {
namespace data = negative_frames::detail;
namespace authors = negative_frames::authoring;
using Key = data::Key;
authors::NativeData Native() {
    authors::NativeData result;
    result.boneControlsPresent=result.scaleControlsPresent=true;
    result.boneControls={20,20,20,20,20,20,20,20,107,107,107,107,107,107,107,107};
    result.scaleControls={20,20,107,107};return result;
}
void CompareAndAccount() {
    Key legacy;legacy.frame=-20;legacy.values[0]=3;
    Key same=legacy;same.selected=true;same.clipboardOffset=123;
    Check(data::SameKeyData(legacy,same),"negative semantic comparison still ignores only transient selection and clipboard offsets");
    auto controls=Native();Key first=legacy;
    first.author=authors::Make(authors::Family::Bone,authors::Mode::Native,controls);
    Key second=first;second.author=authors::Make(authors::Family::Bone,authors::Mode::Native,controls);
    Check(first.author!=second.author && data::SameKeyData(first,second),"equal immutable author allocations compare by complete semantics");
    Check(!data::SameKeyData(legacy,first),"legacy interpolation is not equivalent to native default controls");
    controls.physicsPresent=true;controls.physicsDisabled=false;
    second.author=authors::Make(authors::Family::Bone,authors::Mode::Native,controls);
    Check(!data::SameKeyData(first,second),"unknown physics and explicit enabled physics are different authored states");
    first=second;controls.physicsDisabled=true;
    second.author=authors::Make(authors::Family::Bone,authors::Mode::Native,controls);
    Check(!data::SameKeyData(first,second),"per-key physics-only edits cannot be optimized away as a no-op");
    first=second;controls.scaleControls[1]=21;
    second.author=authors::Make(authors::Family::Bone,authors::Mode::Native,controls);
    Check(!data::SameKeyData(first,second),"scale control-only edits are authored changes");
    first=second;controls.boneControls[4]=21;
    second.author=authors::Make(authors::Family::Bone,authors::Mode::Native,controls);
    Check(!data::SameKeyData(first,second),"native interpolation-only edits are authored changes");
    std::vector<Key> rows;rows.reserve(8);rows.push_back(first);rows.push_back(first);rows.push_back(legacy);
    const auto amount=authors::RetainedBytes(first.author);
    const auto expected=std::uint64_t(rows.capacity())*sizeof(Key)+2*amount;
    Check(amount>0 && data::RetainedKeyBytes(rows)==expected,"retained negative rows charge capacity and each immutable payload reference conservatively");
    failAfter=0;
    const auto sameBytes=data::RetainedKeyBytes(rows);const bool unchanged=data::SameKeyData(rows[0],rows[1]);
    const auto saturated=data::AddBytes(UINT64_MAX-5,6);const auto edge=data::AddBytes(UINT64_MAX-5,5);
    const auto remaining=failAfter;failAfter=-1;
    Check(remaining==0 && unchanged && sameBytes==expected && saturated==UINT64_MAX && edge==UINT64_MAX,
        "comparison and saturated accounting never allocate or wrap to an acceptable budget");
}
void OwnedRowsAndCopySource() {
    authors::Track input;input.family=authors::Family::Morph;
    authors::Lane lane;lane.index=0;
    animation_curve::Key a,b;a.frame=-90;a.value=.2;a.outSlope=.01;a.outWeight=.25;
    b.frame=-10;b.value=.8;b.inSlope=-.02;b.inWeight=.3;
    a.tangent=b.tangent=animation_curve::Tangent::Free;lane.track.keys={a,b};input.lanes.push_back(lane);
    auto frozen=authors::Freeze(input);
    std::vector<Key> live(1);live[0].frame=-10;live[0].morph=true;live[0].values[0]=.8;
    live[0].author=authors::Make(authors::Family::Morph,authors::Mode::Authored,{},frozen);
    const auto snapshot=live;
    negative_frames::CopySourceKey copied;copied.frame=live[0].frame;copied.morph=true;copied.values=live[0].values;
    copied.author=live[0].author;
    input.lanes[0].track.keys.back().inSlope=.5;
    live[0].author=authors::Make(authors::Family::Morph,authors::Mode::Authored,{},authors::Freeze(input));
    Check(!data::SameKeyData(live[0],snapshot[0]),"free-curve handle-only edits are visible to actual negative history comparison");
    Check(copied.author==snapshot[0].author && copied.author->Author()->Data().lanes[0].track.keys[1].inSlope==-.02,
        "negative copy source owns the same immutable historical curve, not the new source handles");
    live.clear();frozen.reset();input.lanes.clear();
    Check(copied.author->Author()->Data().lanes[0].track.keys[0].frame==-90 &&
        copied.author->Author()->Data().lanes[0].track.keys[1].inWeight==.3,
        "retained copy data survives producer data destruction with signed frames and Bezier weights intact");
    const auto payload=authors::RetainedBytes(snapshot[0].author);
    Check(data::RetainedKeyBytes(snapshot)==snapshot.capacity()*sizeof(Key)+payload &&
        payload>=sizeof(authors::KeyData)+sizeof(authors::Curves)+2*sizeof(animation_curve::Key),
        "history accounting includes nested immutable curve ownership");
    Check(!negative_frames::CopySourceKey{}.author && !Key{}.author,
        "default storage and copied v1 rows explicitly remain LegacyLinear");
}
}
void NativeNegativeKeyDataChecks() {
    negative_key_data_checks::CompareAndAccount();negative_key_data_checks::OwnedRowsAndCopySource();
}
}
