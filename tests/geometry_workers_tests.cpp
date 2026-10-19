#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include "mikumikustudio/geometry_workers.hpp"

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;
#ifdef _OPENMP
    // An ordinary startup import reads the environment before our default
    // can be installed. Keep the real runtime delay-load contract covered.
    if (GetModuleHandleW(L"VCOMP140.DLL") != nullptr) return 10;
#endif
    const bool useDefault = std::wcscmp(argv[1], L"default") == 0;
    if (!SetEnvironmentVariableW(L"OMP_WAIT_POLICY", useDefault ? nullptr : argv[1]) ||
        !SetEnvironmentVariableW(L"OMP_NUM_THREADS", argv[2])) return 2;
    const int requested = _wtoi(argv[2]);
    if (requested < 1) return 3;

    // This must be the first OpenMP access: checking max threads before it
    // would initialize VCOMP with the old wait policy.
    const int workers = mikumikustudio::GeometryWorkerCount();
#ifdef _OPENMP
    const int expected = requested < 8 ? requested : 8;
    if (workers != expected) return 4;
    wchar_t policy[32]{};
    if (!GetEnvironmentVariableW(L"OMP_WAIT_POLICY", policy, 32) ||
        std::wcscmp(policy, useDefault ? L"PASSIVE" : argv[1]) != 0) return 5;

    // Repeated geometry work still uses the requested team and produces every
    // element, including after workers have waited between frames.
    omp_set_dynamic(0);
    int team = 0;
#pragma omp parallel num_threads(workers)
    {
#pragma omp single
        team = omp_get_num_threads();
    }
    if (team != workers) return 6;
    int values[32768]{};
    for (int frame = 1; frame <= 10; ++frame) {
#pragma omp parallel for schedule(static) num_threads(workers)
        for (int i = 0; i < 32768; ++i) values[i] += i;
        for (int i = 0; i < 32768; ++i)
            if (values[i] != frame * i) return 7;
        Sleep(16);
    }
    if (mikumikustudio::GeometryWorkerCount() != expected) return 8;
    std::printf("policy=%ls workers=%d repeated_work=ok\n", policy, workers);
#else
    if (workers != 1) return 9;
    std::puts("OpenMP disabled; workers=1");
#endif
    return 0;
}
