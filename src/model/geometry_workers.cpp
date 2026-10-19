#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "mikumikustudio/geometry_workers.hpp"

namespace mikumikustudio {

void InitializeGeometryWorkers() {
#ifdef _OPENMP
    static const bool initialized = [] {
        // VCOMP is delay-loaded and reads this setting when it loads. Its active wait
        // outlasts a capped frame, keeping the team busy between frames even
        // while the main thread sleeps. Keep the same team and work; let idle
        // workers block instead. This changes only this process's environment.
        if (GetEnvironmentVariableW(L"OMP_WAIT_POLICY", nullptr, 0) == 0 &&
            GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return SetEnvironmentVariableW(L"OMP_WAIT_POLICY", L"PASSIVE") != FALSE;
        }
        return true;
    }();
    (void)initialized;
#endif
}

} // namespace mikumikustudio
