add_library(gm_graphics STATIC
    rhi_types.h
    graphics_device.h
    gl_headers.h
    gl_graphics_device.h
    gl_graphics_device.cpp
)

target_include_directories(gm_graphics PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gm_graphics PUBLIC gm_base)

if(ANDROID)
    target_link_libraries(gm_graphics PRIVATE GLESv3)
elseif(EMSCRIPTEN)
    # WebGL 2.0 headers and symbols are provided by Emscripten.
else()
    target_link_libraries(gm_graphics PRIVATE glad_gl_core_33)
endif()

gmrunner_apply_warnings(gm_graphics)
