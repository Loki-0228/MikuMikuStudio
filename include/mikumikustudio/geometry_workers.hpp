#pragma once
#ifdef _OPENMP
#include <omp.h>
#endif

namespace mikumikustudio {
// Configure the process-local wait policy before the first OpenMP call.
// Explicit launcher settings are preserved.
void InitializeGeometryWorkers();

// Use the same bounded team for skinning and sorting. Alternating a full
// machine team with a smaller team costs more than these short jobs save.
inline int GeometryWorkerCount() {
#ifdef _OPENMP
    InitializeGeometryWorkers();
    const int requested = omp_get_max_threads();
    return requested < 8 ? requested : 8;
#else
    return 1;
#endif
}
}
