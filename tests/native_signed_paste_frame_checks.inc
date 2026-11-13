// Include at file scope after Check(bool,const char*) is defined by the harness.
// Call NativeSignedPasteFrameChecks(); no Fixture, payload, or other checks.inc is needed.
// This checks pure frame conversion and synthetic offset preservation, not command routing.
#include "../src/plugins/native_signed_paste.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include <cstdint>
#include <limits>

namespace {
namespace signed_paste_frame_checks {
using mikumikustudio::plugins::ResolveSignedPasteFrame;
using mikumikustudio::plugins::SignedPasteCommand;
constexpr std::int64_t minimum=mikumikustudio::frame_limits::minimum;
constexpr std::int64_t maximum=mikumikustudio::frame_limits::maximum;
constexpr std::int64_t int64Minimum=(std::numeric_limits<std::int64_t>::min)();
constexpr std::int64_t int64Maximum=(std::numeric_limits<std::int64_t>::max)();
static_assert(minimum==-2147483646LL&&maximum==2147483646LL,
    "signed paste uses the existing editor frame bounds");
static_assert(SignedPasteCommand::Ordinary!=SignedPasteCommand::MirrorBones,
    "ordinary and mirror-bones intents stay distinct");

void Expect(std::int64_t origin,std::int64_t offset,bool accepted,
            std::int32_t expected,const char* what) {
    const std::int32_t initialValues[]={123456789,-123456789};
    for(const auto initial:initialValues) {
        struct Output { std::int32_t before,destination,after; } out{-7654321,initial,7654321};
        static_assert(noexcept(ResolveSignedPasteFrame(origin,offset,out.destination)),
            "signed frame conversion must not throw");
        const bool resolved=ResolveSignedPasteFrame(origin,offset,out.destination);
        Check(resolved==accepted&&out.destination==(accepted?expected:initial)&&
            out.before==-7654321&&out.after==7654321,what);
    }
}

void ExplicitBoundaries() {
    struct Case {
        std::int64_t origin,offset;
        bool accepted;
        std::int32_t expected;
        const char* what;
    };
    const Case cases[]={
        {minimum,0,true,-2147483646,"minimum origin and destination are legal"},
        {maximum,0,true,2147483646,"maximum origin and destination are legal"},
        {0,minimum,true,-2147483646,"signed offset reaches the minimum"},
        {0,maximum,true,2147483646,"signed offset reaches the maximum"},
        {minimum,-1,false,0,"one below the minimum destination is rejected"},
        {maximum,1,false,0,"one above the maximum destination is rejected"},
        {0,minimum-1,false,0,"negative offset one beyond the frame range is rejected"},
        {0,maximum+1,false,0,"positive offset one beyond the frame range is rejected"},
        {minimum,1,true,-2147483645,"one inside the minimum is legal"},
        {maximum,-1,true,2147483645,"one inside the maximum is legal"},
        {minimum,4294967292LL,true,2147483646,"full positive span is not narrowed to int32"},
        {maximum,-4294967292LL,true,-2147483646,"full negative span is not narrowed to int32"},
        {minimum,4294967293LL,false,0,"positive full span plus one is rejected"},
        {maximum,-4294967293LL,false,0,"negative full span minus one is rejected"},
        {-5,4,true,-1,"a destination just below zero stays negative"},
        {-5,5,true,0,"a negative origin can land exactly on zero"},
        {-5,6,true,1,"a negative origin can land above zero"},
        {5,-6,true,-1,"a signed negative offset can cross below zero"},
        {5,-5,true,0,"a positive origin can land exactly on zero"},
        {5,-4,true,1,"a signed negative offset can retain a positive destination"},
        {-5,-10,true,-15,"pure arithmetic accepts a negative offset at a negative origin"},
        {0,0,true,0,"zero origin and zero offset produce zero"},
        {minimum-1,0,false,0,"origin one below the frame range is illegal"},
        {maximum+1,0,false,0,"origin one above the frame range is illegal"},
        {minimum-1,1,false,0,"an offset cannot rescue an illegal low origin"},
        {maximum+1,-1,false,0,"an offset cannot rescue an illegal high origin"},
        {minimum-1,2147483647LL,false,0,"an illegal low origin cannot be rescued to zero"},
        {maximum+1,-2147483647LL,false,0,"an illegal high origin cannot be rescued to zero"},
        {int64Minimum,0,false,0,"INT64_MIN origin is rejected before subtraction"},
        {int64Maximum,0,false,0,"INT64_MAX origin is rejected before subtraction"},
        {int64Minimum,int64Maximum,false,0,"extreme inputs summing to minus one still have an illegal origin"},
        {int64Maximum,-int64Maximum,false,0,"extreme inputs summing to zero still have an illegal origin"},
    };
    for(const auto& c:cases)Expect(c.origin,c.offset,c.accepted,c.expected,c.what);

    const std::int64_t origins[]={minimum,-1,0,1,maximum};
    for(const auto origin:origins) {
        Expect(origin,int64Minimum,false,0,"INT64_MIN offset is rejected without addition or negation");
        Expect(origin,int64Maximum,false,0,"INT64_MAX offset is rejected without addition");
    }
    Expect(int64Minimum,int64Minimum,false,0,"two INT64_MIN inputs cannot overflow admission");
    Expect(int64Maximum,int64Maximum,false,0,"two INT64_MAX inputs cannot overflow admission");
}

void SafeDomainArithmetic() {
    // Independently add first in a deliberately bounded domain: every sum is
    // between -6442450940 and 6442450940, well within int64. Do not reproduce
    // the helper's subtraction-based guard as the expected-result oracle.
    const std::int64_t origins[]={minimum-1,minimum,minimum+1,-11,-1,0,1,11,
        maximum-1,maximum,maximum+1};
    const std::int64_t offsets[]={-4294967293LL,-4294967292LL,-2147483647LL,
        -2147483646LL,-21,-1,0,1,21,2147483646LL,2147483647LL,4294967292LL,4294967293LL};
    for(const auto origin:origins)for(const auto offset:offsets) {
        const std::int64_t sum=origin+offset;
        const bool accepted=origin>=minimum&&origin<=maximum&&sum>=minimum&&sum<=maximum;
        Expect(origin,offset,accepted,accepted?static_cast<std::int32_t>(sum):0,
            "bounded direct arithmetic agrees with conversion and failure preserves output");
    }
}

void SharedOffsetsAndZero() {
    struct Record { std::int64_t sourceFrame,offset; std::int32_t expected; };
    const Record records[]={{-10,0,-5},{0,10,5},{10,20,15}};
    for(const auto& record:records) {
        Check(record.sourceFrame-(-10)==record.offset,
            "minus-ten zero ten share offsets zero ten twenty from one source origin");
        Expect(-5,record.offset,true,record.expected,
            "the complete clipboard at minus five lands at minus five five fifteen");
    }
    const std::int64_t offsets[]={0,5,10};
    const std::int32_t expected[]={-5,0,5};
    unsigned negative=0,nonnegative=0,index=0;
    for(const auto offset:offsets) {
        std::int32_t destination=123456789;
        const bool resolved=ResolveSignedPasteFrame(-5,offset,destination);
        Check(resolved&&destination==expected[index++],"resolve before classifying a signed destination");
        if(destination<0)++negative;else ++nonnegative;
    }
    Check(negative==1&&nonnegative==2,"zero belongs only to the nonnegative partition");
}

void FamilyOffsetsAndMirrorFiltering() {
    // Synthetic timing only: no payload, target mapping, mirror transform, or
    // production 421/422 invocation. Model and global timelines stay separate.
    enum class Family { Bone, Morph, Display, Camera, Light, Shadow, Gravity, Accessory };
    struct Record { Family family; std::int64_t offset; std::int32_t expected; };
    const Record model[]={
        {Family::Morph,0,-5},{Family::Bone,10,5},
        {Family::Display,20,15},{Family::Bone,30,25},
    };
    const SignedPasteCommand commands[]={SignedPasteCommand::Ordinary,SignedPasteCommand::MirrorBones};
    for(const auto command:commands) {
        unsigned visited=0;
        for(const auto& record:model) {
            if(command==SignedPasteCommand::MirrorBones&&record.family!=Family::Bone)continue;
            Expect(-5,record.offset,true,record.expected,
                "model family filtering retains the shared offset instead of rebasing its first record");
            ++visited;
        }
        Check(visited==(command==SignedPasteCommand::Ordinary?4u:2u),
            "synthetic 422 filtering keeps only bones with original offsets ten and thirty");
    }
    const Record global[]={
        {Family::Camera,0,-5},{Family::Light,10,5},{Family::Shadow,20,15},
        {Family::Gravity,30,25},{Family::Accessory,40,35},
    };
    for(const auto& record:global)Expect(-5,record.offset,true,record.expected,
        "each global family's distinct first offset retains the clipboard origin");
}
}
}

void NativeSignedPasteFrameChecks() {
    signed_paste_frame_checks::ExplicitBoundaries();
    signed_paste_frame_checks::SafeDomainArithmetic();
    signed_paste_frame_checks::SharedOffsetsAndZero();
    signed_paste_frame_checks::FamilyOffsetsAndMirrorFiltering();
}
