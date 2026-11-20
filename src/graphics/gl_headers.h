#pragma once

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
#define GMRUNNER_USE_GLES3 1
#include <GLES3/gl3.h>
#else
#define GMRUNNER_USE_GLES3 0
#include <glad/gl.h>
#endif
