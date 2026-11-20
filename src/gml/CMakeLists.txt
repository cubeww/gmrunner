add_library(gm_gml STATIC
    builtins_catalog.h
    builtins_catalog.cpp
    value.h
    value.cpp
    symbol_table.h
    symbol_table.cpp
    dnd_lowerer.h
    dnd_lowerer.cpp
    lexer.h
    lexer.cpp
    bytecode.h
    bytecode.cpp
    compiler.h
    compiler.cpp
    vm.h
    vm.cpp
)

target_include_directories(gm_gml PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gm_gml
    PUBLIC
        gm_base
        gm_loader
)

gmrunner_apply_warnings(gm_gml)
