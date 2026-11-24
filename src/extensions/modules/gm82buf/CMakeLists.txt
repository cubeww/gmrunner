gmrunner_add_extension(
    NAME gm82buf
    FACTORY create_gm82buf_extension
    SOURCES gm82buf_extension.cpp
    WIN32_LIBS ws2_32
)
