gmrunner_add_extension(
    NAME gm82core
    FACTORY create_gm82core_extension
    SOURCES gm82core_extension.cpp
    LINK_LIBS SDL3::SDL3
    WIN32_LIBS psapi
)
