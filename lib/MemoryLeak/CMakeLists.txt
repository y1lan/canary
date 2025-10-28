
add_library(CanaryMemoryLeak STATIC
    MLDVFG.cpp
    MLDValueFlowAnalysis.cpp
    VFGReachable.cpp
    MLDAllocationAnalysis.cpp
    MLDInstrumentation.cpp
    MLDDeallocationAnalysis.cpp
)

target_link_libraries(CanaryMemoryLeak PRIVATE z3)