gmrunner_add_extension(
    NAME gm82joy
    FACTORY create_gm82joy_extension
    SOURCES gm82joy_extension.cpp
    LINK_LIBS SDL3::SDL3
)
