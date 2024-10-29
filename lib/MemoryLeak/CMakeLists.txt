
add_library(CanaryMemoryLeak STATIC
    MLDVFG.cpp
    MLDValueFlowAnalysis.cpp
    VFGReachable.cpp
    MLDAllocationAnalysis.cpp
    MLDInstrumentation.cpp
)