#include <rex/graphics/pipeline/shader/spirv_translator.h>

namespace rex::graphics {

spv::Id SpirvShaderTranslator::CreateConstantBuffer(uint32_t binding, spv::Id type,
                                                  const char* name) {
  if (!gpu_draw_constants_) {
    const auto variable = builder_->createVariable(spv::NoPrecision, spv::StorageClassUniform,
                                                  type, name);
    builder_->addDecoration(variable, spv::DecorationDescriptorSet, int(kDescriptorSetConstants));
    builder_->addDecoration(variable, spv::DecorationBinding, int(binding));
    if (features_.spirv_version >= spv::Spv_1_4) main_interface_.push_back(variable);
    return variable;
  }
  const auto pointer = builder_->makePointer(spv::StorageClassPhysicalStorageBuffer, type);
  const auto variable = builder_->createVariable(spv::NoPrecision, spv::StorageClassPrivate,
                                                pointer, name);
  builder_->addDecoration(variable, spv::DecorationAliasedPointer);
  gpu_constant_variables_[binding] = variable;
  gpu_constant_pointer_types_[binding] = pointer;
  if (features_.spirv_version >= spv::Spv_1_4) main_interface_.push_back(variable);
  return variable;
}

void SpirvShaderTranslator::CreateGpuDrawInterface() {
  if (!gpu_draw_constants_) return;
  const auto type_u64 = builder_->makeUintType(64);
  const auto array = builder_->makeRuntimeArray(type_u64);
  builder_->addDecoration(array, spv::DecorationArrayStride, 8);
  const auto table = builder_->makeStructType({array}, "XnDrawTable");
  builder_->addMemberDecoration(table, 0, spv::DecorationOffset, 0);
  builder_->addDecoration(table, spv::DecorationBlock);
  gpu_draw_table_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassStorageBuffer,
                                            table, "xn_draw_table");
  builder_->addDecoration(gpu_draw_table_, spv::DecorationDescriptorSet, int(kDescriptorSetConstants));
  builder_->addDecoration(gpu_draw_table_, spv::DecorationBinding, 0);
  if (features_.spirv_version >= spv::Spv_1_4) main_interface_.push_back(gpu_draw_table_);

  // Guest rendering has one host instance. Its firstInstance selects an
  // immutable 64-byte draw record; the guest vertex index is left intact.
  // Existing packed interpolators/point parameters occupy locations 0..17.
  gpu_draw_input_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassInput,
      is_pixel_shader() ? type_uint_ : type_int_, "xn_draw_id_in");
  if (is_pixel_shader()) {
    builder_->addDecoration(gpu_draw_input_, spv::DecorationLocation, 18);
    builder_->addDecoration(gpu_draw_input_, spv::DecorationFlat);
  } else {
    builder_->addDecoration(gpu_draw_input_, spv::DecorationBuiltIn, spv::BuiltInInstanceIndex);
    gpu_draw_output_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                               type_uint_, "xn_draw_id_out");
    builder_->addDecoration(gpu_draw_output_, spv::DecorationLocation, 18);
    builder_->addDecoration(gpu_draw_output_, spv::DecorationFlat);
    main_interface_.push_back(gpu_draw_output_);
  }
  main_interface_.push_back(gpu_draw_input_);
}

void SpirvShaderTranslator::LoadGpuDrawConstants() {
  if (!gpu_draw_constants_) return;
  auto draw_id = builder_->createLoad(gpu_draw_input_, spv::NoPrecision);
  if (!is_pixel_shader()) {
    draw_id = builder_->createUnaryOp(spv::OpBitcast, type_uint_, draw_id);
    builder_->createStore(draw_id, gpu_draw_output_);
  }
  const auto base = builder_->createBinOp(spv::OpIMul, type_uint_, draw_id,
                                         builder_->makeUintConstant(8));
  for (uint32_t binding = 0; binding < kConstantBufferCount; ++binding) {
    if (!gpu_constant_variables_[binding]) continue;
    const auto index = builder_->createBinOp(spv::OpIAdd, type_uint_, base,
                                            builder_->makeUintConstant(binding));
    const auto address_pointer = builder_->createAccessChain(spv::StorageClassStorageBuffer,
        gpu_draw_table_, {builder_->makeIntConstant(0), index});
    const auto address = builder_->createLoad(address_pointer, spv::NoPrecision);
    const auto pointer = builder_->createUnaryOp(spv::OpConvertUToPtr,
                                                 gpu_constant_pointer_types_[binding], address);
    builder_->createStore(pointer, gpu_constant_variables_[binding]);
  }
}

}  // namespace rex::graphics
