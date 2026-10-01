#pragma once

#include "device.h"

namespace sgd {

wgpu::ShaderModule createShaderModule(const wgpu::Device& device, CString source, const char* label = nullptr);

constexpr wgpu::BindGroupLayoutEntry bufferBindGroupLayoutEntry( //
	uint32_t binding,											 //
	wgpu::ShaderStage visibility,								 //
	wgpu::BufferBindingType bindingType) {
	// Unused binding layouts default to BindingNotUsed; spelling them as {} would give the invalid Undefined.
	wgpu::BindGroupLayoutEntry entry{};
	entry.binding = binding;
	entry.visibility = visibility;
	entry.buffer.type = bindingType;
	return entry;
}

constexpr wgpu::BindGroupLayoutEntry textureBindGroupLayoutEntry(				//
	uint32_t binding,															//
	wgpu::ShaderStage visibility,												//
	wgpu::TextureViewDimension viewDimension = wgpu::TextureViewDimension::e2D, //
	wgpu::TextureSampleType sampleType = wgpu::TextureSampleType::Float) {
	wgpu::BindGroupLayoutEntry entry{};
	entry.binding = binding;
	entry.visibility = visibility;
	entry.texture.sampleType = sampleType;
	entry.texture.viewDimension = viewDimension;
	return entry;
}

constexpr wgpu::BindGroupLayoutEntry samplerBindGroupLayoutEntry( //
	uint32_t binding,											  //
	wgpu::ShaderStage visibility,								  //
	wgpu::SamplerBindingType bindingType = wgpu::SamplerBindingType::Filtering) {
	wgpu::BindGroupLayoutEntry entry{};
	entry.binding = binding;
	entry.visibility = visibility;
	entry.sampler.type = bindingType;
	return entry;
}

inline wgpu::BindGroupEntry bufferBindGroupEntry(uint32_t binding, wgpu::Buffer buffer) { //
	return wgpu::BindGroupEntry{nullptr, binding, buffer};
}

inline wgpu::BindGroupEntry textureBindGroupEntry(uint32_t binding, wgpu::TextureView textureView) { //
	return wgpu::BindGroupEntry{nullptr, binding, nullptr, 0, WGPU_WHOLE_SIZE, nullptr, textureView};
}

inline wgpu::BindGroupEntry samplerBindGroupEntry(uint32_t binding, wgpu::Sampler sampler) { //
	return wgpu::BindGroupEntry{nullptr, binding, {}, {}, {}, sampler};
}

} // namespace sgd
