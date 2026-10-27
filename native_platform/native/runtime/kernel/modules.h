#pragma once
#include <string>
#include <rex/image_info.h>

namespace xn {
// Registers native code, without loading its guest data or running DllMain.
void RegisterModuleImage(std::string name, const rex::PPCImageInfo& info);
}
