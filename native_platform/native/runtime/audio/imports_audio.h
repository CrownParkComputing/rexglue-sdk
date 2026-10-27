// xenon-native: registration list for the audio kernel imports defined in
// imports_audio.cpp. A kernel that fills its dispatch table from a list
// (rather than by symbol) can iterate kAudioImports.
#pragma once
#include <cstddef>

#include <rex/ppc/func.h>

namespace xn::audio {
struct AudioImport {
  const char* name;  // import name without the __imp__ prefix
  PPCFunc* function;
};
extern const AudioImport kAudioImports[];
extern const size_t kAudioImportCount;
// Stops the pump and decoder threads; call from KernelShutdown.
void AudioShutdown();

// The XMA hardware behind these imports, for a title that also drives it
// through the register block (runtime/audio/xma_registers.h). Both reach the
// same engine and the same context table - calling this is what makes the two
// paths one decoder rather than two. Forces audio setup if it has not happened
// yet, so it is safe to call before the title has created a context.
class XmaEngine;
XmaEngine& Engine();
// Guest (virtual) address of the context table; the caller translates it with
// the kernel's MmGetPhysicalAddress, which is the number the guest compares
// its own context pointers against.
uint32_t ContextTableAddress();
}  // namespace xn::audio
