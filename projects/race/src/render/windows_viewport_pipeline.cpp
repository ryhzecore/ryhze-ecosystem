#include "windows_viewport_internal.hpp"
namespace race::render {
Result<void> WindowsViewport::Impl::initialize_root(std::span<const ResourceBinding> bindings,
                                                    ComPtr<ID3DBlob> &serialized, ComPtr<ID3DBlob> &errors) {
  std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {bindings[0].register_index, bindings[0].space, bindings[0].constant_bytes / 4};
  parameters[0].ShaderVisibility = native_visibility(bindings[0].visibility);
  const D3D12_DESCRIPTOR_RANGE texture_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, bindings[1].count,
                                             bindings[1].register_index, bindings[1].space, 0};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {1, &texture_range};
  parameters[1].ShaderVisibility = native_visibility(bindings[1].visibility);
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.MaxLOD = static_cast<float>(viewport_texture_max_mips - 1);
  sampler.ShaderRegister = bindings[2].register_index;
  sampler.RegisterSpace = bindings[2].space;
  sampler.ShaderVisibility =
      native_visibility(bindings[2].visibility); // Legacy color sampling still fixes LOD=0.
  D3D12_ROOT_SIGNATURE_DESC signature{};
  signature.NumParameters = static_cast<UINT>(parameters.size());
  signature.pParameters = parameters.data();
  signature.NumStaticSamplers = 1;
  signature.pStaticSamplers = &sampler;
  signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  auto result = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
  if (FAILED(result))
    return native_error(result, "viewport root signature serialization failed");
  result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&root));
  if (FAILED(result))
    return native_error(result, "viewport root signature creation failed");

  return {};
}
Result<void> WindowsViewport::Impl::initialize_pipelines(const Program &retained,
                                                         D3D12_GRAPHICS_PIPELINE_STATE_DESC &pipeline,
                                                         const D3D12_INPUT_ELEMENT_DESC *layout) {
  pipeline.pRootSignature = root.Get();
  pipeline.VS = {retained.vertex.data(), retained.vertex.size()};
  pipeline.PS = {retained.pixel.data(), retained.pixel.size()};
  pipeline.InputLayout = {layout, 3};
  pipeline.SampleMask = UINT_MAX;
  auto &blend = pipeline.BlendState.RenderTarget[0];
  blend.SrcBlend = D3D12_BLEND_ONE;
  blend.DestBlend = D3D12_BLEND_ZERO;
  blend.BlendOp = D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlendAlpha = D3D12_BLEND_ZERO;
  blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.LogicOp = D3D12_LOGIC_OP_NOOP;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipeline.RasterizerState.DepthClipEnable = TRUE;
  pipeline.DepthStencilState.DepthEnable = TRUE;
  pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  pipeline.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
  pipeline.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
  pipeline.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                          D3D12_COMPARISON_FUNC_ALWAYS};
  pipeline.DepthStencilState.BackFace = pipeline.DepthStencilState.FrontFace;
  pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipeline.NumRenderTargets = 1;
  pipeline.RTVFormats[0] = color_format;
  pipeline.DSVFormat = depth_format;
  pipeline.SampleDesc.Count = 1;
  auto result = device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&triangles));
  if (FAILED(result))
    return native_error(result, "DXIL cube pipeline creation failed");
  // PhysicalDiffuseV1 opaque stage: same vertex interface and raster state into the FP16 target.
  pipeline.PS = {retained.physical_pixel.data(), retained.physical_pixel.size()};
  pipeline.RTVFormats[0] = hdr_format;
  result = device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&physical_triangles));
  if (FAILED(result))
    return native_error(result, "DXIL physical cube pipeline creation failed");
  pipeline.PS = {retained.pixel.data(), retained.pixel.size()};
  pipeline.RTVFormats[0] = color_format;
  pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
  pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  result = device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&lines));
  if (FAILED(result))
    return native_error(result, "DXIL grid/selection pipeline creation failed");
  pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipeline.DepthStencilState.DepthEnable = FALSE;
  pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  result = device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&overlay_triangles));
  if (FAILED(result))
    return native_error(result, "DXIL viewport overlay pipeline creation failed");

  return {};
}
Result<void> WindowsViewport::Impl::initialize_output_root(const Program &retained,
                                                           ComPtr<ID3DBlob> &output_serialized,
                                                           ComPtr<ID3DBlob> &output_errors) {
  // OutputTransform: its own root signature from the admitted output ABI (b0 32 bytes and
  // the FP16 source at t0, pixel stage only), no input layout, no depth, display format.
  const auto output_bindings = retained.light_output.output_bindings();
  std::array<D3D12_ROOT_PARAMETER, 2> output_parameters{};
  output_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  output_parameters[0].Constants = {output_bindings[0].register_index, output_bindings[0].space,
                                    output_bindings[0].constant_bytes / 4};
  output_parameters[0].ShaderVisibility = native_visibility(output_bindings[0].visibility);
  const D3D12_DESCRIPTOR_RANGE hdr_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, output_bindings[1].count,
                                         output_bindings[1].register_index, output_bindings[1].space, 0};
  output_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  output_parameters[1].DescriptorTable = {1, &hdr_range};
  output_parameters[1].ShaderVisibility = native_visibility(output_bindings[1].visibility);
  D3D12_ROOT_SIGNATURE_DESC output_signature{};
  output_signature.NumParameters = static_cast<UINT>(output_parameters.size());
  output_signature.pParameters = output_parameters.data();
  output_signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  auto result = D3D12SerializeRootSignature(&output_signature, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &output_serialized, &output_errors);
  if (FAILED(result))
    return native_error(result, "output transform root signature serialization failed");
  result = device->CreateRootSignature(0, output_serialized->GetBufferPointer(),
                                       output_serialized->GetBufferSize(), IID_PPV_ARGS(&output_root));
  if (FAILED(result))
    return native_error(result, "output transform root signature creation failed");

  return {};
}
Result<void>
WindowsViewport::Impl::initialize_output_pipeline(const Program &retained,
                                                  const D3D12_GRAPHICS_PIPELINE_STATE_DESC &pipeline) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC output_pipeline{};
  output_pipeline.pRootSignature = output_root.Get();
  output_pipeline.VS = {retained.output_vertex.data(), retained.output_vertex.size()};
  output_pipeline.PS = {retained.output_pixel.data(), retained.output_pixel.size()};
  output_pipeline.SampleMask = UINT_MAX;
  output_pipeline.BlendState = pipeline.BlendState;
  output_pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  output_pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  output_pipeline.RasterizerState.DepthClipEnable = TRUE;
  output_pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  output_pipeline.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
  output_pipeline.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
  output_pipeline.DepthStencilState.FrontFace = pipeline.DepthStencilState.FrontFace;
  output_pipeline.DepthStencilState.BackFace = pipeline.DepthStencilState.FrontFace;
  output_pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  output_pipeline.NumRenderTargets = 1;
  output_pipeline.RTVFormats[0] = color_format;
  output_pipeline.SampleDesc.Count = 1;
  auto result = device->CreateGraphicsPipelineState(&output_pipeline, IID_PPV_ARGS(&output_transform));
  if (FAILED(result))
    return native_error(result, "DXIL output transform pipeline creation failed");

  return {};
}

} // namespace race::render
