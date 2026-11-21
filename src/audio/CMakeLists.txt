add_library(gm_audio_third_party STATIC
    third_party_audio.c
)
if(MSVC)
    target_compile_options(gm_audio_third_party PRIVATE /O2)
else()
    target_compile_options(gm_audio_third_party PRIVATE -O2)
endif()
target_include_directories(gm_audio_third_party SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/miniaudio
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/TinySoundFont
)

add_library(gm_audio STATIC
    audio_crypto.h
    audio_crypto.cpp
    dsp_effects.h
    dsp_effects.cpp
    tracker_decoder.h
    tracker_decoder.cpp
    midi_decoder.h
    midi_decoder.cpp
    audio_engine.h
    audio_engine.cpp
)

target_include_directories(gm_audio PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(gm_audio SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/miniaudio
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/TinySoundFont
)
target_link_libraries(gm_audio
    PUBLIC
        gm_base
        gm_loader
    PRIVATE
        gm_audio_third_party
)

if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)
    target_link_libraries(gm_audio PRIVATE pthread m dl)
endif()

gmrunner_apply_warnings(gm_audio)
