add_library(gm_base STATIC
    types.h
    log.h
    log.cpp
    math.h
    math.cpp
    hash.h
    hash.cpp
    zlib_util.h
    zlib_util.cpp
    encoding.h
    encoding.cpp
    binary_reader.h
    binary_reader.cpp
)

target_include_directories(gm_base PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gm_base PRIVATE zlibstatic)
gmrunner_apply_warnings(gm_base)
