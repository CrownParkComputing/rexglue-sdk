/**
 ******************************************************************************
 * xenon-native - RB_COPY_DEST_INFO.copy_dest_exp_bias at resolve
 ******************************************************************************
 * WHAT THE HARDWARE DOES
 *
 * A Xenos resolve does not just copy EDRAM into a texture: it multiplies the
 * colour by 2^copy_dest_exp_bias on the way, then converts to the destination
 * texture format. That is how a 360 title gets an HDR range through a
 * fixed-point texture. Geometry Wars 2 renders its bloom chain into
 * k_2_10_10_10_FLOAT ("7e3") EDRAM, whose range is [0, 32), and resolves it
 * into k_16_16_16_16 and k_2_10_10_10 UNORM textures with exp_bias = -5,
 * i.e. x 1/32, which lands [0, 32) exactly on [0, 1].
 *
 * Measured over a boot of each title (every resolve, source and destination
 * format and bias from the register):
 *
 *   Geometry Wars 2   108 resolves  srcfmt=3 (7e3) -> destfmt=26  exp_bias=-5
 *                       8 resolves  srcfmt=3 (7e3) -> destfmt=7   exp_bias=-5
 *                       2 resolves  srcfmt=2        -> destfmt=7   exp_bias=0
 *   Geometry Wars 1     6 resolves  srcfmt=0        -> destfmt=6   exp_bias=0
 *
 * Dropping the bias meant every value above 1.0 saturated white in the
 * destination texture instead of every value above 32.0 - a factor of 32 on
 * the input to a bloom chain, which is why the second title's picture came out
 * near-white with its structure intact.
 *
 * THE CLAMP, AND WHY THIS IS EXACT RATHER THAN AN APPROXIMATION
 *
 * The console saturates at 32 at every blend step, because that is the top of
 * the 7e3 range and every write goes through the format. We accumulate in
 * VK_FORMAT_R16G16B16A16_SFLOAT, which does not saturate until ~65504, and
 * then clamp once here when the scaled result is converted into the UNORM
 * destination. Those two are the same result, but only because:
 *
 *   - the accumulation is NON-NEGATIVE, and
 *   - the blend is SATURATING rather than wrapping, and
 *   - no intermediate value above the ceiling is ever read back at full range.
 *
 * The emulator's shader backend does it PER BLEND: it wraps every blend in a
 * clamp whose range comes from the render target format - [0, 1] for plain
 * 8-bit surfaces, and [0, 31.875] for this float format. That per-blend clamp
 * is the behaviour the single clamp here stands in for, and our host float
 * attachment clamps at nothing at all.
 *
 * Under those conditions clamping once at the end equals clamping at each
 * step: once a channel reaches the ceiling it stays there, and nothing below
 * the ceiling was altered on the way. Both blend modes this title uses on a
 * 7e3 target are non-negative - ONE ADD ZERO and ONE ADD ONE - so it holds.
 *
 * IT STOPS BEING EXACT the moment a title does any of these:
 *
 *   - subtracts, or uses any blend that can produce a negative intermediate
 *     (REVERSE_SUBTRACT, a negative blend factor, a shader emitting negatives);
 *   - relies on an intermediate above the ceiling being visible later, e.g.
 *     accumulating past 32 and then scaling back DOWN before the resolve;
 *   - reads the EDRAM surface at full range through some path other than a
 *     resolve carrying this bias.
 *
 * In any of those cases the difference between "saturate at every step" and
 * "saturate once" is real and this pass is no longer a faithful model. The fix
 * then is to clamp the render target itself at each write, which means packing
 * 7e3 in the pixel shader - the accuracy path this renderer does not have.
 * If you are here because a third title looks wrong, that is the question to
 * ask first.
 *
 * ALPHA
 *
 * The bias is applied to RGB only. 7e3's alpha is 2-bit UNORM, not float, so
 * an exponent bias on it is not meaningful, and scaling it by 1/32 would crush
 * it to nothing. This is untestable in either Geometry Wars title: measured
 * across every draw in both, no blend factor reads destination alpha
 * (no DST_ALPHA or INV_DST_ALPHA anywhere - the INV_SRC_ALPHA the first title
 * uses reads SOURCE alpha from the shader), and no pixel shader's colour
 * output depends on the alpha channel of a texture fetch. So nothing here can
 * distinguish the choices. A title that resolves a k_16_16_16_16_FLOAT target
 * with a meaningful float alpha would, and would need this revisited.
 */
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vulkan/vulkan.h>

#include "resolve_bias_shaders.h"
#include "depth_pack_shader.h"

namespace xn::gfx {

// A full-screen pass that copies one rectangle of a colour image into another
// image, multiplying RGB by a constant. Self-contained: it owns its layout,
// pipelines (one per destination format) and sampler. Descriptor sets come
// from the renderer's fence-owned batch pool.
// A separate instance initialized with pack_depth selects depth_pack.frag
// and a second, integer stencil sampler to reconstruct raw guest D24S8 bytes.
struct ResolveBiasPass {
  VkDevice device = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkShaderModule vert = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  bool packs_depth = false;
  std::unordered_map<uint32_t, VkPipeline> pipelines;       // by VkFormat

  struct Push {
    int32_t src_offset[2];
    int32_t pad[2];
    float scale[4];
  };
  static_assert(sizeof(Push) == 32, "push constant layout");

  bool Init(VkDevice d, bool pack_depth = false) {
    if (device) return true;
    device = d;
    packs_depth = pack_depth;
    VkDescriptorSetLayoutBinding b[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      b[i].binding = i;
      b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      b[i].descriptorCount = 1;
      b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = packs_depth ? 2 : 1;
    lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(device, &lci, nullptr, &set_layout) != VK_SUCCESS) return Fail();

    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &pipeline_layout) != VK_SUCCESS) {
      return Fail();
    }

    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &sci, nullptr, &sampler) != VK_SUCCESS) return Fail();

    vert = MakeModule(kResolveBiasVert, sizeof(kResolveBiasVert));
    frag = packs_depth ? MakeModule(kDepthPackFrag, sizeof(kDepthPackFrag))
                       : MakeModule(kResolveBiasFrag, sizeof(kResolveBiasFrag));
    if (!vert || !frag) return Fail();
    return true;
  }

  void Destroy() {
    if (!device) return;
    for (auto& p : pipelines) vkDestroyPipeline(device, p.second, nullptr);
    pipelines.clear();
    if (vert) vkDestroyShaderModule(device, vert, nullptr);
    if (frag) vkDestroyShaderModule(device, frag, nullptr);
    if (sampler) vkDestroySampler(device, sampler, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    vert = frag = VK_NULL_HANDLE;
    sampler = VK_NULL_HANDLE;
    pipeline_layout = VK_NULL_HANDLE;
    set_layout = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
  }

  // `src_view` must be in SHADER_READ_ONLY_OPTIMAL and `dest_view` in
  // COLOR_ATTACHMENT_OPTIMAL; the caller owns those transitions, and must not
  // have a render pass open. Returns false if the pass could not be built, in
  // which case the caller should fall back to its plain copy.
  bool Apply(VkCommandBuffer cb, VkDescriptorPool pool, VkImageView src_view, VkImageView dest_view,
             VkFormat dest_format, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w,
             uint32_t h, float scale, VkImageView stencil_view = VK_NULL_HANDLE,
             bool float_depth = false, bool swap_green_blue = false,
             uint32_t fixed16_number = 0) {
    if (!device || !w || !h) return false;
    VkPipeline pipeline = GetPipeline(dest_format);
    VkDescriptorSet set = GetSet(pool, src_view, stencil_view);
    if (!pipeline || !set) return false;

    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = dest_view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{dx, dy}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport vp = {float(dx), float(dy), float(w), float(h), 0.0f, 1.0f};
    VkRect2D sc = {{dx, dy}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 0,
                            nullptr);
    // gl_FragCoord is the destination texel; the source texel is that plus the
    // difference of the two rectangle origins. The rectangles are the same
    // size, so this is a 1:1 fetch with no filtering.
    Push push = {};
    push.src_offset[0] = sx - dx;
    push.src_offset[1] = sy - dy;
    push.pad[0] = packs_depth ? (float_depth ? 1 : 0) : int32_t(fixed16_number);
    push.pad[1] = swap_green_blue ? 1 : 0;
    push.scale[0] = push.scale[1] = push.scale[2] = scale;
    push.scale[3] = 1.0f;  // see ALPHA above
    vkCmdPushConstants(cb, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    return true;
  }

 private:

  bool Fail() {
    Destroy();
    return false;
  }

  VkShaderModule MakeModule(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
  }

  VkDescriptorSet GetSet(VkDescriptorPool pool, VkImageView src_view, VkImageView stencil_view) {
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[2] = {
        {sampler, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler, stencil_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wr[2] = {};
    for (uint32_t i = 0; i < (packs_depth ? 2u : 1u); ++i) {
      wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      wr[i].dstSet = set;
      wr[i].dstBinding = i;
      wr[i].descriptorCount = 1;
      wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(device, packs_depth ? 2 : 1, wr, 0, nullptr);
    return set;
  }

  VkPipeline GetPipeline(VkFormat dest_format) {
    auto it = pipelines.find(uint32_t(dest_format));
    if (it != pipelines.end()) return it->second;
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vs.viewportCount = 1;
    vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPipelineRenderingCreateInfo prci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    prci.colorAttachmentCount = 1;
    prci.pColorAttachmentFormats = &dest_format;

    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &prci;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vs;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = pipeline_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline) !=
        VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    pipelines[uint32_t(dest_format)] = pipeline;
    return pipeline;
  }
};

}  // namespace xn::gfx
