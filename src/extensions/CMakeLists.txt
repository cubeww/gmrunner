add_library(gm_extensions STATIC
    extension_host.h
    dll_registry.h
    dll_registry.cpp
    shim_cleanmem.h
    shim_cleanmem.cpp
    shim_supersound.h
    shim_supersound.cpp
    shim_maizemusic.h
    shim_maizemusic.cpp
    shim_gmfmod.h
    shim_gmfmod.cpp
)

target_include_directories(gm_extensions PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gm_extensions
    PUBLIC
        gm_base
        gm_platform
        gm_loader
        gm_gml
        gm_audio
)

if(WIN32)
    target_link_libraries(gm_extensions PRIVATE psapi)
endif()

gmrunner_apply_warnings(gm_extensions)
