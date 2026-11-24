# Helper macro for declaring self-contained gmrunner extension modules under src/extensions/modules/<name>/.
#
# Example:
#   gmrunner_add_extension(
#       NAME cleanmem
#       FACTORY create_cleanmem_extension
#       SOURCES cleanmem_extension.cpp
#       WIN32_LIBS psapi
#   )
#
# Optional arguments:
#   OPTION <VAR_NAME>        - Adds a CMake option() toggle for this extension (default: always enabled)
#   DEFAULT <ON|OFF>         - Default value when OPTION is specified (default: ON)
#   PRIORITY <int>           - Registration order (default: 0; higher registers later and wins alias conflicts)
#   THIRD_PARTY_SOURCES ...  - Sources compiled in an isolated static target with warnings suppressed
#   PRIVATE_INCLUDE_DIRS ... - Extra private include paths for this extension
#   LINK_LIBS ...            - Cross-platform private link libraries (e.g. SDL3::SDL3, zlibstatic)
#   WIN32_LIBS / UNIX_LIBS / APPLE_LIBS ... - Platform-specific private link libraries
#   COMPILE_DEFINITIONS ...  - Private preprocessor definitions

function(gmrunner_add_extension)
    cmake_parse_arguments(EXT
        ""
        "NAME;FACTORY;OPTION;DEFAULT;PRIORITY"
        "SOURCES;THIRD_PARTY_SOURCES;PRIVATE_INCLUDE_DIRS;LINK_LIBS;WIN32_LIBS;UNIX_LIBS;APPLE_LIBS;COMPILE_DEFINITIONS"
        ${ARGN}
    )

    if(NOT EXT_NAME)
        message(FATAL_ERROR "gmrunner_add_extension: NAME is required")
    endif()
    if(NOT EXT_SOURCES)
        message(FATAL_ERROR "gmrunner_add_extension(${EXT_NAME}): SOURCES is required")
    endif()
    if(NOT EXT_FACTORY)
        set(EXT_FACTORY "create_${EXT_NAME}_extension")
    endif()
    if(NOT DEFINED EXT_PRIORITY)
        set(EXT_PRIORITY 0)
    endif()

    if(EXT_OPTION)
        if(NOT DEFINED EXT_DEFAULT)
            set(EXT_DEFAULT ON)
        endif()
        option(${EXT_OPTION} "Enable ${EXT_NAME} extension module" ${EXT_DEFAULT})
        if(NOT ${EXT_OPTION})
            return()
        endif()
    endif()

    set(_target "gm_ext_${EXT_NAME}")
    add_library(${_target} STATIC ${EXT_SOURCES})
    target_link_libraries(${_target} PRIVATE gm_extensions_core)

    if(EXT_PRIVATE_INCLUDE_DIRS)
        target_include_directories(${_target} PRIVATE ${EXT_PRIVATE_INCLUDE_DIRS})
    endif()
    if(EXT_COMPILE_DEFINITIONS)
        target_compile_definitions(${_target} PRIVATE ${EXT_COMPILE_DEFINITIONS})
    endif()
    if(EXT_LINK_LIBS)
        target_link_libraries(${_target} PRIVATE ${EXT_LINK_LIBS})
    endif()
    if(WIN32 AND EXT_WIN32_LIBS)
        target_link_libraries(${_target} PRIVATE ${EXT_WIN32_LIBS})
    endif()
    if(UNIX AND NOT APPLE AND EXT_UNIX_LIBS)
        target_link_libraries(${_target} PRIVATE ${EXT_UNIX_LIBS})
    endif()
    if(APPLE AND EXT_APPLE_LIBS)
        target_link_libraries(${_target} PRIVATE ${EXT_APPLE_LIBS})
    endif()

    gmrunner_apply_warnings(${_target})

    if(EXT_THIRD_PARTY_SOURCES)
        set(_tp_target "gm_ext_${EXT_NAME}_third_party")
        add_library(${_tp_target} STATIC ${EXT_THIRD_PARTY_SOURCES})
        if(EXT_PRIVATE_INCLUDE_DIRS)
            target_include_directories(${_tp_target} PRIVATE ${EXT_PRIVATE_INCLUDE_DIRS})
        endif()
        if(EXT_COMPILE_DEFINITIONS)
            target_compile_definitions(${_tp_target} PRIVATE ${EXT_COMPILE_DEFINITIONS})
        endif()
        if(EXT_LINK_LIBS)
            target_link_libraries(${_tp_target} PRIVATE ${EXT_LINK_LIBS})
        endif()
        if(MSVC)
            target_compile_options(${_tp_target} PRIVATE /w)
        else()
            target_compile_options(${_tp_target} PRIVATE -w)
        endif()
        target_link_libraries(${_target} PRIVATE ${_tp_target})
    endif()

    # Zero-pad shifted priority so lexicographic sort orders by (priority ASC, name ASC).
    math(EXPR _prio_shifted "${EXT_PRIORITY} + 100000")
    string(LENGTH "${_prio_shifted}" _prio_len)
    while(_prio_len LESS 6)
        set(_prio_shifted "0${_prio_shifted}")
        string(LENGTH "${_prio_shifted}" _prio_len)
    endwhile()

    set_property(GLOBAL APPEND PROPERTY GMRUNNER_EXTENSION_TARGETS "${_target}")
    set_property(GLOBAL APPEND PROPERTY GMRUNNER_EXTENSION_ENTRIES "${_prio_shifted}:${EXT_NAME}:${EXT_FACTORY}")
endfunction()
