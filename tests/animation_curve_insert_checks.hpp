#pragma once
#include "mikumikustudio/animation_curve.hpp"
#include <cstdio>

inline int RunSplitChecks() {
    namespace ac=mikumikustudio::animation_curve;
    int failures=0,cases=0,samples=0;double valueError=0,speedError=0,curvatureError=0;
    auto check=[&](bool good,const char* name){if(!good){++failures;printf("Subdivision failure: %s\n",name);}};
    auto encoded=[](const ac::Track& track){std::vector<std::uint8_t> bytes;std::string error;ac::Encode({{"curve",track}},bytes,error);return bytes;};
    ac::Track automatic;
    const int frames[]={0,10,30,60};const double values[]={0,20,-10,5};
    for(int i=0;i<4;++i){ac::Key key;key.frame=frames[i];key.value=values[i];automatic.keys.push_back(key);}
    ac::UpdateAutomatic(automatic);
    ac::Track weighted=automatic;
    for(auto& key:weighted.keys){key.tangent=ac::Tangent::Free;key.inWeight=.41;key.outWeight=.11;key.inSlope=-2.2;key.outSlope=3.7;}
    ac::Track linear=weighted,step=weighted,equal=weighted,discrete=weighted;
    for(auto& key:linear.keys)key.interpolation=ac::Interpolation::Linear;
    for(auto& key:step.keys)key.interpolation=ac::Interpolation::Step;
    for(auto& key:equal.keys)key.value=5;
    discrete.discrete=true;
    for(const auto& original:{automatic,weighted,linear,step,equal,discrete})for(int frame:{1,7,11,15,29,31,47,59}){
        ac::Track split=original;std::size_t index=0;std::string error;
        const auto result=ac::InsertPreservingShape(split,frame,index,error);
        check(result==ac::InsertResult::Inserted,"supported scalar interval splits");
        if(result!=ac::InsertResult::Inserted)continue;
        check(ac::Validate(split,error)&&split.keys[index].frame==frame,"valid ordered key");
        // A later automatic-tangent pass must not undo the explicit split.
        ac::UpdateAutomatic(split);
        for(int n=0;n<=2400;++n){
            const double f=n*.025;
            const auto a=ac::Evaluate(original,f),b=ac::Evaluate(split,f);
            valueError=std::max(valueError,std::abs(a.value-b.value));
            speedError=std::max(speedError,std::abs(a.derivative-b.derivative));
            const double h=.001;
            const double before=(ac::Evaluate(original,f+h).derivative-ac::Evaluate(original,f-h).derivative)/(2*h);
            const double after=(ac::Evaluate(split,f+h).derivative-ac::Evaluate(split,f-h).derivative)/(2*h);
            curvatureError=std::max(curvatureError,std::abs(before-after));++samples;
        }
        ++cases;
    }
    check(valueError<1e-9&&speedError<1e-9&&curvatureError<1e-6,"value, slope and second derivative unchanged");
    for(int frame:{0,10,30,60}){
        auto unchanged=automatic;const auto before=encoded(unchanged);std::size_t index=0;std::string error;
        check(ac::InsertPreservingShape(unchanged,frame,index,error)==ac::InsertResult::Existing&&before==encoded(unchanged),"existing frames are no-ops");
    }
    for(int frame:{3,75}){
        auto original=automatic;for(auto& key:original.keys)key.frame+=10;
        auto split=original;std::size_t index=0;std::string error;
        check(ac::InsertPreservingShape(split,frame,index,error)==ac::InsertResult::Inserted,"constant extrapolation inserts");
        for(int n=0;n<=800;++n)check(std::abs(ac::Evaluate(original,n*.1).value-ac::Evaluate(split,n*.1).value)<1e-9,"extrapolation preserves values");
    }
    ac::Track constrained=weighted;constrained.keys[0].outWeight=.0001;constrained.keys[1].inWeight=.1;
    const auto before=encoded(constrained);std::size_t index=0;std::string error;
    check(ac::InsertPreservingShape(constrained,9,index,error)==ac::InsertResult::Rejected&&before==encoded(constrained),"unrepresentable split is rejected atomically");
    printf("Subdivision check: %d intervals, %d samples; max value %.3g, slope %.3g, second derivative %.3g; %d failures\n",cases,samples,valueError,speedError,curvatureError,failures);
    return failures;
}
