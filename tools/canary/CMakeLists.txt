set(LLVM_LINK_COMPONENTS
        LLVMAggressiveInstCombine
        LLVMAnalysis
        LLVMAsmParser
        LLVMAsmPrinter
        LLVMBinaryFormat
        LLVMBitReader
        LLVMBitWriter
        LLVMBitstreamReader
        LLVMCodeGen
        LLVMCore
        LLVMCoroutines
        LLVMDemangle
        LLVMFrontendOpenMP
        LLVMIRReader
        LLVMInstCombine
        LLVMInstrumentation
        LLVMLTO
        LLVMLinker
        LLVMMC
        LLVMMCParser
        LLVMMIRParser
        LLVMObject
        LLVMObjectYAML
        LLVMOption
        LLVMPasses
        LLVMProfileData
        LLVMRemarks
        LLVMScalarOpts
        LLVMSupport
        LLVMTarget
        LLVMTransformUtils
        LLVMVectorize
        LLVMipo
)

add_executable(canary canary.cpp)

target_link_libraries(canary PRIVATE
        CanaryMemoryLeak CanaryNullPointer CanaryDyckAA CanaryTransform CanarySupport
        -Wl,--start-group
        ${LLVM_LINK_COMPONENTS}
        -Wl,--end-group
        z ncurses pthread dl
)
