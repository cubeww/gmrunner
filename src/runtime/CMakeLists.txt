add_library(gm_runtime STATIC
    instance.h
    collision.h
    collision.cpp
    data_structures.h
    data_structures.cpp
    particles.h
    particles.cpp
    runner.h
    runner.cpp
    save_load.cpp
    builtins_core.cpp
    builtins_gameplay.cpp
    builtins_graphics.cpp
    builtins_actions.cpp
    builtins_misc.cpp
)

target_include_directories(gm_runtime PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gm_runtime
    PUBLIC
        gm_base
        gm_platform
        gm_loader
        gm_gml
        gm_graphics
        gm_audio
        gm_extensions
)

gmrunner_apply_warnings(gm_runtime)
