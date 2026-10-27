// xenon-native shim for <rex/graphics/flags.h>: only the knobs the vendored
// translator and draw_util sources read. Defined in ../../src/xn_flags.cpp.
#pragma once
#include <string>
#include <rex/cvar.h>
REXCVAR_DECLARE(std::string, dump_shaders);
REXCVAR_DECLARE(bool, gpu_vertex_nan_kill_debug);
REXCVAR_DECLARE(bool, half_pixel_offset);
