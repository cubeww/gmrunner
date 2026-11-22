if(ANDROID)
    # SDLActivity loads libSDL3.so followed by libmain.so and invokes SDL_main.
    add_library(main SHARED
        cli.h
        cli.cpp
        main.cpp
    )
    set(_app_target main)
elseif(APPLE AND GMRUNNER_MACOS_BUNDLE)
    add_executable(gmrunner MACOSX_BUNDLE
        cli.h
        cli.cpp
        main.cpp
    )
    set_target_properties(gmrunner PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "gmrunner"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.gmrunner.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
    set(_app_target gmrunner)
else()
    add_executable(gmrunner
        cli.h
        cli.cpp
        main.cpp
    )
    set(_app_target gmrunner)
endif()

target_include_directories(${_app_target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(${_app_target}
    PRIVATE
        gm_base
        gm_platform
        gm_loader
        gm_gml
        gm_graphics
        gm_audio
        gm_runtime
        SDL3::SDL3
)

if(EMSCRIPTEN)
    set_target_properties(${_app_target} PROPERTIES SUFFIX ".html")
    set_property(TARGET ${_app_target} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_SOURCE_DIR}/web/shell.html"
        "${CMAKE_SOURCE_DIR}/web/gmrunner_web.js"
    )

    target_link_options(${_app_target} PRIVATE
        "-sUSE_WEBGL2=1"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sALLOW_MEMORY_GROWTH=1"
        # Nested GML calls and native extension handlers share the C++ stack.
        "-sSTACK_SIZE=4194304"
        "-sINITIAL_MEMORY=134217728"
        "-sMAXIMUM_MEMORY=2147483648"
        "-sFORCE_FILESYSTEM=1"
        "-lidbfs.js"
        "-sEXPORTED_FUNCTIONS=_main,_gmrunner_web_launch,_gmrunner_web_stop,_gmrunner_web_set_touch_overlay,_gmrunner_web_set_touch_edit_mode,_gmrunner_web_is_touch_editing,_gmrunner_web_set_muted"
        "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,ccall,cwrap,UTF8ToString,stringToNewUTF8,HEAPU8"
        "--shell-file" "${CMAKE_SOURCE_DIR}/web/shell.html"
    )

    if(GMRUNNER_WASM_ASYNC_MODE STREQUAL "JSPI")
        target_link_options(${_app_target} PRIVATE
            "-sJSPI=1"
            "-sJSPI_EXPORTS=gmrunner_web_launch,gmrunner_web_stop,gmrunner_web_frame"
        )
    else()
        target_link_options(${_app_target} PRIVATE
            "-sASYNCIFY=1"
            "-sASYNCIFY_STACK_SIZE=131072"
        )
    endif()

    add_custom_command(TARGET ${_app_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/web/gmrunner_web.js"
            "$<TARGET_FILE_DIR:${_app_target}>/gmrunner_web.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/web/gmrunner_web.css"
            "$<TARGET_FILE_DIR:${_app_target}>/gmrunner_web.css"
        COMMENT "Copying WebAssembly frontend assets alongside gmrunner.html"
    )
endif()

if(NOT ANDROID AND NOT EMSCRIPTEN)
    if(APPLE AND GMRUNNER_MACOS_BUNDLE)
        install(TARGETS ${_app_target} BUNDLE DESTINATION .)
    else()
        install(TARGETS ${_app_target} RUNTIME DESTINATION bin)
    endif()
endif()

gmrunner_apply_warnings(${_app_target})
