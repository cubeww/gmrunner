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
    single_byte_codepages.h
    binary_reader.h
    binary_reader.cpp
)

target_include_directories(gm_base PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gm_base PRIVATE zlibstatic)
if(ANDROID)
    target_link_libraries(gm_base PRIVATE log)
endif()
gmrunner_apply_warnings(gm_base)
