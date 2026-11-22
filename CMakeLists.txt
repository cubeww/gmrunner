cmake_minimum_required(VERSION 3.24)

project(gmrunner
    VERSION 0.1.0
    DESCRIPTION "Cross-platform GameMaker 8.0 / 8.1 / 8.2 Runner"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(GMRUNNER_MACOS_BUNDLE "Build gmrunner as a macOS .app bundle" OFF)
set(GMRUNNER_WASM_ASYNC_MODE "ASYNCIFY" CACHE STRING "WebAssembly async execution mode (ASYNCIFY or JSPI)")
set_property(CACHE GMRUNNER_WASM_ASYNC_MODE PROPERTY STRINGS ASYNCIFY JSPI)

function(gmrunner_apply_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-unused-parameter
        )
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Third-party submodules
# ---------------------------------------------------------------------------

# zlib (static only)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC  ON  CACHE BOOL "" FORCE)
set(ZLIB_INSTALL       OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/zlib EXCLUDE_FROM_ALL)

# SDL3 (shared on Android for SDLActivity loader; static everywhere else)
if(ANDROID)
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
endif()
set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
set(SDL_TESTS         OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL       OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_CAMERA        OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR        OFF CACHE BOOL "" FORCE)
if(APPLE)
    # The built-in SDL converter only covers Unicode, ASCII, and Latin-1.
    set(SDL_SYSTEM_ICONV ON CACHE BOOL "Use system iconv for GM8 legacy codepages" FORCE)
endif()
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

# glad2 OpenGL 3.3 Core loader (Desktop only; Android & Emscripten use native GLES3/WebGL2)
if(NOT ANDROID AND NOT EMSCRIPTEN)
    set(Python_FIND_STRATEGY LOCATION)
    add_subdirectory(third_party/glad/cmake glad_cmake EXCLUDE_FROM_ALL)
    glad_add_library(glad_gl_core_33 STATIC REPRODUCIBLE API gl:core=3.3)
    get_target_property(_glad_inc glad_gl_core_33 INTERFACE_INCLUDE_DIRECTORIES)
    if(_glad_inc)
        set_target_properties(glad_gl_core_33 PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_glad_inc}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Engine libraries & executable
# ---------------------------------------------------------------------------
add_subdirectory(src/base)
add_subdirectory(src/platform)
add_subdirectory(src/loader)
add_subdirectory(src/gml)
add_subdirectory(src/graphics)
add_subdirectory(src/audio)
add_subdirectory(src/extensions)
add_subdirectory(src/runtime)
add_subdirectory(src/app)
