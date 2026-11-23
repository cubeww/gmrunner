add_library(gm_graphics STATIC
    rhi_types.h
    graphics_device.h
    gl_headers.h
    gl_graphics_device.h
    gl_graphics_device.cpp
    dx9_shader_transpiler.h
    dx9_shader_transpiler.cpp
    dx9_pipeline.h
    dx9_pipeline.cpp
    model3d.h
    model3d.cpp
    renderer2d.h
    renderer2d.cpp
    builtin_font.h
    message_dialog.h
    message_dialog.cpp
    touch_overlay.h
    touch_overlay.cpp
)

target_include_directories(gm_graphics PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(gm_graphics SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
target_link_libraries(gm_graphics
    PUBLIC
        gm_base
        gm_loader
        gm_platform
)

if(ANDROID)
    target_link_libraries(gm_graphics PRIVATE GLESv3)
elseif(EMSCRIPTEN)
    # WebGL 2.0 headers and symbols are provided by Emscripten.
else()
    target_link_libraries(gm_graphics PRIVATE glad_gl_core_33)
endif()

gmrunner_apply_warnings(gm_graphics)
