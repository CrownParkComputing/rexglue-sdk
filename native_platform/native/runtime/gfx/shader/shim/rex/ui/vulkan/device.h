// xenon-native shim for <rex/ui/vulkan/device.h>: spirv_translator.h declares
// Features(const ui::vulkan::VulkanDevice*), which is never defined here; use
// MakeSpirvTranslatorFeatures() from spirv_translator_features.h.
#pragma once
#include <vulkan/vulkan.h>
namespace rex::ui::vulkan {
class VulkanDevice;
}
