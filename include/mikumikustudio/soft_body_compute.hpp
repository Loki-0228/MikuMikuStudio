#pragma once
#include "mikumikustudio/processor_settings.hpp"
#include <memory>
#include <string>
class btSoftBody;
template<class T> class btAlignedObjectArray;
namespace mikumikustudio::plugins::resource_budget { class Collector; }
namespace mikumikustudio::soft_compute {
enum class Result { Idle,Cpu,Small,MeasuredCpu,Unsupported,Unavailable,Gpu };
struct Status {
    Result result=Result::Idle;
    std::wstring adapter;
    std::string detail;
    double cpuMilliseconds=0,gpuMilliseconds=0;
    unsigned long long cpuSteps=0,gpuSteps=0;
    unsigned long long clusterGpuSteps=0,clusterCpuSteps=0;
    bool clusterIterationsCpu=false;
};
const Status& GetStatus();
// Owns prepare/iterations/cleanup when accepted; false leaves the world alone.
bool SolveClusters(btAlignedObjectArray<btSoftBody*>&,const processor::Selection&);
class State {
public:
    State();~State();
    // False leaves the Bullet body untouched and requests its CPU solver.
    // Automatic calibration may execute that CPU solve itself and return true.
    bool Solve(btSoftBody&,const processor::Selection&);
    void Measure(plugins::resource_budget::Collector&) const;
private:
    struct Impl;std::unique_ptr<Impl> impl;
};
}
