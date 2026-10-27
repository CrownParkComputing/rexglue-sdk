// XAM music messages and the music frame delivered on the guest audio thread.
#pragma once
#include <cstdint>
namespace xn {
uint32_t XmpMessage(uint32_t message, uint32_t buffer, uint32_t length);
struct XmpCapture { uint32_t callback = 0, context = 0, buffer = 0; };
// The audio pump delivers the returned capture on its guest-capable thread.
XmpCapture XmpPrepareFrame();
void XmpMixFrame(float* stereo, uint32_t frames);
void XmpShutdown();
}
