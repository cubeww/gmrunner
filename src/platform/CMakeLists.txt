add_library(gm_platform STATIC
    clock.h
    clock.cpp
    platform.h
    platform.cpp
    registry.cpp
    vfs.h
    vfs.cpp
    window.h
    window.cpp
)

target_include_directories(gm_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gm_platform
    PUBLIC gm_base
    PRIVATE SDL3::SDL3-static
)

if(WIN32)
    target_link_libraries(gm_platform PRIVATE winmm)
endif()

gmrunner_apply_warnings(gm_platform)
