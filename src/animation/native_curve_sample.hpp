#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace mikudancestudio::native_curve_sample {
struct Sample { double value=0,slope=0; };
inline double Cubic(double a,double b,double u){
    const double v=1-u;return 3*v*v*u*a+3*v*u*u*b+u*u*u;
}
inline double Parameter(double x1,double x2,double time){
    if(time<=0)return 0;if(time>=1)return 1;
    double low=0,high=1;
    for(int i=0;i<48;++i){const double middle=(low+high)*.5;
        if(Cubic(x1,x2,middle)<time)low=middle;else high=middle;}
    return (low+high)*.5;
}
// Native playback uses a float, fixed-step inverse. Differencing nearby
// playback samples yields zero for long intervals because both map to one
// quantization cell. Speed is the underlying Bezier's dy/dx, independently of
// that value sampler. Raw 0..127 controls, including crossed X, stay intact.
inline Sample Evaluate(const std::array<double,4>& controls,double time){
    const double x1=controls[0]/127.0,y1=controls[1]/127.0;
    const double x2=controls[2]/127.0,y2=controls[3]/127.0;
    time=std::clamp(time,0.0,1.0);
    const double u=Parameter(x1,x2,time),v=1-u;
    const double dx=3*v*v*x1+6*v*u*(x2-x1)+3*u*u*(1-x2);
    const double dy=3*v*v*y1+6*v*u*(y2-y1)+3*u*u*(1-y2);
    double slope=0;
    if(dx>1e-10)slope=dy/dx;
    else {
        // A vertical tangent has no finite point derivative. Display a local
        // secant rather than injecting NaN/infinity into fitting and GDI.
        const double low=std::max(0.0,time-1e-6),high=std::min(1.0,time+1e-6);
        slope=(Cubic(y1,y2,Parameter(x1,x2,high))-Cubic(y1,y2,Parameter(x1,x2,low)))/(high-low);
    }
    return {Cubic(y1,y2,u),slope};
}
}
