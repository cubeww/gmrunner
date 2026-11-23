add_library(gm_platform STATIC
    clock.h
    clock.cpp
    font_loader.h
    font_loader.cpp
    platform.h
    platform.cpp
    registry.cpp
    vfs_source.h
    vfs.h
    vfs.cpp
    zip_archive.h
    zip_archive.cpp
    web_bridge.h
    web_bridge.cpp
    window.h
    window.cpp
)

target_include_directories(gm_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gm_platform
    PUBLIC gm_base
    PRIVATE SDL3::SDL3
)

if(WIN32)
    target_link_libraries(gm_platform PRIVATE winmm user32 gdi32 comctl32)
endif()

gmrunner_apply_warnings(gm_platform)
