add_library(gm_loader STATIC
    game_data.h
    path.cpp
    upx_unpack.h
    upx_unpack.cpp
    antidecompiler.h
    antidecompiler.cpp
    crypto.h
    crypto.cpp
    exe_loader.h
    exe_loader.cpp
)

target_include_directories(gm_loader PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gm_loader
    PUBLIC
        gm_base
        gm_platform
)

gmrunner_apply_warnings(gm_loader)
