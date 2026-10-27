#include "mikumikustudio/evaluation_time.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

using mikumikustudio::EvaluationTime;
namespace {
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
constexpr std::uint64_t maximum=mikumikustudio::frame_limits::maximum;
bool Same(const EvaluationTime& a,const EvaluationTime& b){return a.Frame()==b.Frame()&&a.DiscreteFrame()==b.DiscreteFrame();}
void FrameInputs(){
    EvaluationTime value;CHECK(value.Frame()==0&&value.DiscreteFrame()==0);
    for(double frame:{0.0,-0.0,.125,.5,7.5,10.75,double(maximum)}){
        CHECK(EvaluationTime::FromFrame(frame,value));CHECK(value.Frame()==frame&&value.DiscreteFrame()==static_cast<uint32_t>(frame));
    }
    CHECK(EvaluationTime::FromFrame(7.5,value));const auto before=value;
    const double bad[]{-1,-.125,std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),double(maximum)+1,std::nextafter(double(maximum),std::numeric_limits<double>::infinity())};
    for(double frame:bad)CHECK(!EvaluationTime::FromFrame(frame,value)&&Same(value,before));
    CHECK(EvaluationTime::FromFrame(std::nextafter(10.,0.),value)&&value.DiscreteFrame()==9&&value.Frame()<10);
    CHECK(EvaluationTime::FromFrame(std::nextafter(10.,11.),value)&&value.DiscreteFrame()==10&&value.Frame()>10);
}
void RationalBoundaries(){
    struct Case{std::int64_t numerator;std::uint64_t denominator;std::uint32_t discrete;double rounded;};
    // Expected discrete results follow integer identities. For example,
    // 30*9007199254740992 = 270215977642229760 exactly. Changing the
    // denominator by one crosses frame 1 without changing its rounded double.
    const Case cases[]{
        {0,1,0,0},{0,UINT64_MAX,0,0},{1,60,0,.5},{1,4,7,7.5},{7,20,10,10.5},
        {9007199254740992ll,270215977642229761ull,0,1},{9007199254740992ll,270215977642229760ull,1,1},
        {9007199254740992ll,270215977642229759ull,1,1},
        {9007199254740993ll,27021597764222980ull,9,10},{9007199254740993ll,27021597764222979ull,10,10},
        {9007199254740993ll,27021597764222978ull,10,10},
        {499999999999999999ll,3000000000000000000ull,4,5},
        {1999999999999999999ll,3000000000000000000ull,19,20},
        {2000000000000000000ll,3000000000000000000ull,20,20},
        {2000000000000000001ll,3000000000000000000ull,20,20},
        {4499999999999999999ll,3000000000000000000ull,44,45},
        {99999999999999999ll,3000000000ull,999999999,1000000000},
        {100000000000000000ll,3000000000ull,1000000000,1000000000},
        {100000000000000001ll,3000000000ull,1000000000,1000000000},
        {2147483645999999999ll,30000000000ull,2147483645u,2147483646.0},
        {2147483646000000000ll,30000000000ull,2147483646u,2147483646.0},
        {INT64_MAX,UINT64_MAX,14,15},{INT64_MAX,18446744073709551614ull,15,15},
        {INT64_MAX,18446744073709551613ull,15,15},{INT64_MAX,9223372036854775807ull,30,30}
    };
    for(const auto& item:cases){EvaluationTime value;CHECK(EvaluationTime::FromSeconds(item.numerator,item.denominator,value));
        CHECK(value.DiscreteFrame()==item.discrete&&value.Frame()==item.rounded);}
    EvaluationTime tiny;CHECK(EvaluationTime::FromSeconds(1,UINT64_MAX,tiny)&&tiny.DiscreteFrame()==0&&tiny.Frame()>0&&tiny.Frame()<1e-17);
    EvaluationTime beforeKey;CHECK(EvaluationTime::FromSeconds(9007199254740992ll,270215977642229761ull,beforeKey));
    EvaluationTime fromRounded;CHECK(EvaluationTime::FromFrame(beforeKey.Frame(),fromRounded));
    CHECK(beforeKey.Frame()==fromRounded.Frame()&&beforeKey.DiscreteFrame()==0&&fromRounded.DiscreteFrame()==1);
    // Fractions just above the maximum are invalid even if their doubles round
    // back down to the maximum. Failure preserves BOTH representations.
    const auto sentinel=beforeKey;
    const Case invalid[]{
        {-1,1,0,0},{INT64_MIN,UINT64_MAX,0,0},{0,0,0,0},{1,0,0,0},
        {2147483646000000001ll,30000000000ull,0,0},{2147483647,30,0,0},
        {INT64_MAX,1,0,0},{INT64_MAX,30,0,0}
    };
    for(const auto& item:invalid)CHECK(!EvaluationTime::FromSeconds(item.numerator,item.denominator,beforeKey)&&Same(beforeKey,sentinel));
    const Case equivalent[]{
        {1,3,10,10},{3,9,10,10},{9007199254740993ll,27021597764222979ull,10,10},
        {3002399751580331ll,9007199254740993ull,10,10},
        {27021597764222976ll,810647932926689283ull,0,1}
    };
    for(const auto& item:equivalent){EvaluationTime value;CHECK(EvaluationTime::FromSeconds(item.numerator,item.denominator,value));CHECK(value.DiscreteFrame()==item.discrete&&value.Frame()==item.rounded);}
    EvaluationTime small,scaledSmall;
    CHECK(EvaluationTime::FromSeconds(1,9007199254740993ull,small));
    CHECK(EvaluationTime::FromSeconds(3,27021597764222979ull,scaledSmall));
    CHECK(Same(small,scaledSmall)&&small.Frame()>0&&small.DiscreteFrame()==0);
}
std::uint64_t Random(std::uint64_t& seed){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
void IndependentIntegerOracle(){
    // This oracle uses one ordinary uint64 multiplication/division. Generated
    // numerators guarantee n*30 fits, so it does not duplicate the helper's
    // carry algorithm or require intrinsics, a new ISA, or an extra dependency.
    std::uint64_t seed=0x97a352b4c801d6efull;unsigned accepted=0,rejected=0;
    for(unsigned i=0;i<100000;++i){
        const auto n=Random(seed)%(UINT64_MAX/30+1);
        const std::uint64_t d=i%3==0?(Random(seed)%1000000+1):(Random(seed)|1);
        const auto product=n*30,quotient=product/d,remainder=product%d;
        const bool valid=quotient<maximum||(quotient==maximum&&remainder==0);
        EvaluationTime value;CHECK(EvaluationTime::FromFrame(123.25,value));const auto before=value;
        CHECK(EvaluationTime::FromSeconds(static_cast<std::int64_t>(n),d,value)==valid);
        if(!valid){++rejected;CHECK(Same(value,before));continue;}
        ++accepted;CHECK(value.DiscreteFrame()==quotient&&std::isfinite(value.Frame())&&value.Frame()>=0&&value.Frame()<=maximum);
        const double reference=static_cast<double>(product)/static_cast<double>(d);
        const double ulp=std::nextafter(reference,std::numeric_limits<double>::infinity())-reference;
        CHECK(std::abs(value.Frame()-reference)<=4*ulp);
        if(remainder==0)CHECK(value.Frame()==quotient);
        // Scaling both sides by small integers is another independent invariant.
        if(n<=std::uint64_t(INT64_MAX)/3&&d<=UINT64_MAX/3){EvaluationTime scaled;
            CHECK(EvaluationTime::FromSeconds(static_cast<std::int64_t>(n*3),d*3,scaled));
            CHECK(Same(scaled,value));
        }
    }
    CHECK(accepted>1000&&rejected>1000);
}
}
int main(){FrameInputs();RationalBoundaries();IndependentIntegerOracle();std::printf("PASS %u evaluation time checks\n",checks);return 0;}
