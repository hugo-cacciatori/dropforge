#include <dsp_core/SmoothChaos.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <vector>

namespace {
bool forbidAllocation = false;
void require(bool ok, const char* message) { if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); } }
}
void* operator new(std::size_t n) { if (forbidAllocation) std::abort(); if (auto* p=std::malloc(n ? n : 1)) return p; throw std::bad_alloc{}; }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main()
{
    double worstLongCorrelation = 0.0;
    for (const auto rate : {44100,48000,88200,96000,176400,192000})
    {
        dsp_core::SmoothChaos a,b,other;
        a.reset(); b.reset(); other.reset(0.731293);
        std::vector<double> audio(static_cast<std::size_t>(rate));
        double mean=0.0, energy=0.0, difference=0.0;
        forbidAllocation = true;
        for (int i=0; i<rate; ++i)
        {
            const auto x=a.next(1733.0,rate), y=b.next(1733.0,rate);
            require(x==y && std::isfinite(x) && std::abs(x)<=1.0, "bounded exact reset/replay at six rates");
            audio[static_cast<std::size_t>(i)]=x; mean+=x; energy+=x*x;
            difference+=std::abs(x-other.next(1733.0,rate));
        }
        forbidAllocation = false;
        mean/=rate;
        require(std::sqrt(energy/rate)>0.25 && std::sqrt(energy/rate)<0.9, "chaos sustains nonzero bounded texture");
        require(difference/rate>0.1, "different seeds produce independent trajectories");
        const int lag=rate*3/100;
        double ab=0.0,aa=0.0,bb=0.0;
        for (int i=lag; i<rate; ++i) { const auto x=audio[i]-mean,y=audio[i-lag]-mean; ab+=x*y; aa+=x*x; bb+=y*y; }
        worstLongCorrelation=std::max(worstLongCorrelation,std::abs(ab/std::sqrt(aa*bb)));
        a.reset(); b.reset();
        forbidAllocation=true;
        for (int i=0; i<4096; ++i) require(a.next(7000,rate)==b.next(7000,rate), "reset restores state at another clock");
        forbidAllocation=false;
    }
    require(worstLongCorrelation<0.25, "chaotic control stream does not lock into a short repeating tone");
    dsp_core::SmoothChaos invalid;
    invalid.reset(std::numeric_limits<double>::quiet_NaN());
    for (int i=0; i<10000; ++i) require(std::isfinite(invalid.next(std::numeric_limits<double>::infinity(),0)), "invalid rate/clock containment");
    std::cout << "smooth_chaos_rates=6 replay=exact allocations=0 worst_30ms_correlation=" << worstLongCorrelation << '\n';
}
