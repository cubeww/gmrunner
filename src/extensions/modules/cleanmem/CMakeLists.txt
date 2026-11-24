gmrunner_add_extension(
    NAME cleanmem
    FACTORY create_cleanmem_extension
    SOURCES cleanmem_extension.cpp
    WIN32_LIBS psapi
)
