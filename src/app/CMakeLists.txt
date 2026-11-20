add_executable(gmrunner
    cli.h
    cli.cpp
    main.cpp
)

target_include_directories(gmrunner PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gmrunner
    PRIVATE
        gm_base
        gm_platform
        gm_loader
        gm_gml
        gm_graphics
        gm_audio
        gm_runtime
)

if(EMSCRIPTEN)
    set_target_properties(gmrunner PROPERTIES SUFFIX ".html")
    target_link_options(gmrunner PRIVATE
        "-sUSE_WEBGL2=1"
        "-sFULL_ES3=1"
        "-sALLOW_MEMORY_GROWTH=1"
    )
endif()

gmrunner_apply_warnings(gmrunner)
