#pragma once

#include "gfx/renderer.h"
#include "shape_a.h"

namespace xn {

// Diagnostic, headless capture of a bounded PM4 renderer window. The caller
// enables this factory only when XN_RENDER_TRACE names the destination file.
// The returned renderer owns its Vulkan backend; ordinary rendering does not
// construct this wrapper.
IRenderer* CreateRenderCapture(gfx::VulkanRendererConfig config);

}  // namespace xn
