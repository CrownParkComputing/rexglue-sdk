/**
 ******************************************************************************
 * ReXGlue - rex::shader-translator standalone draw_util constants
 ******************************************************************************
 *
 * spirv_translator_rb.cpp reads the D3D10 standard sample positions when it
 * builds the fragment-shader-interlock depth/stencil path. They are defined in
 * util/draw.cpp, which pulls in the texture cache, the memexport copy helpers
 * and the plugin cvars - none of which belong in the standalone translator. So
 * the two tables are repeated here, byte for byte. Keep in sync with
 * src/graphics/util/draw.cpp.
 */

#include <cstdint>

#include <rex/graphics/util/draw.h>

namespace rex::graphics::draw_util {

const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
const int8_t kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};

}  // namespace rex::graphics::draw_util
