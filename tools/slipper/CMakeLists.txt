execute_process(COMMAND ${LLVMCONFIG} --libs OUTPUT_VARIABLE LLVM_LINK_COMPONENTS OUTPUT_STRIP_TRAILING_WHITESPACE)
add_executable(slipper slipper.cpp)
if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    target_link_libraries(slipper PRIVATE
        CanaryDyckAA CanarySupport
        -Wl,--start-group
        ${LLVM_LINK_COMPONENTS}
        -Wl,--end-group
        z ncurses pthread dl
    )
else()
    target_link_libraries(slipper PRIVATE
        CanaryDyckAA CanarySupport
        ${LLVM_LINK_COMPONENTS}
        z ncurses pthread dl
    )
endif()