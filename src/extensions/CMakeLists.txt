add_library(gm_extensions STATIC
    buffer_store.h
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
    shim_gm82core.h
    shim_gm82core.cpp
    shim_gm82buf.h
    shim_gm82buf.cpp
    shim_gm82joy.h
    shim_gm82joy.cpp
    shim_gm82drag.h
    shim_gm82drag.cpp
    shim_gm82test.h
    shim_gm82test.cpp
    shim_gm82dx.h
    shim_gm82dx.cpp
)

target_include_directories(gm_extensions PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gm_extensions
    PUBLIC
        gm_base
        gm_platform
        gm_loader
        gm_gml
        gm_graphics
        gm_audio
    PRIVATE
        zlibstatic
        SDL3::SDL3-static
)

if(WIN32)
    target_link_libraries(gm_extensions PRIVATE psapi ws2_32)
endif()

gmrunner_apply_warnings(gm_extensions)
