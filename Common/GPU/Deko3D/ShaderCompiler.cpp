// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "Common/GPU/Deko3D/ShaderCompiler.h"

#include <cstdlib>
#include <mutex>
#include <vector>

#include "Common/GPU/Deko3D/UamBridge.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "ext/SPIRV-Cross/spirv_glsl.hpp"

bool CompileDeko3DShader(ShaderStage stage, const char *source, std::string *binary, uint32_t *arrayTextures, uint32_t *textures,
						 std::string *error, uint32_t textureBindingBase) {
	// glslang and UAM share process-wide compiler state.
	static std::mutex compilerMutex;
	std::lock_guard<std::mutex> lock(compilerMutex);
	*arrayTextures = *textures = 0;
	VkShaderStageFlagBits vkStage;
	int uamStage;
	switch (stage) {
	case ShaderStage::Vertex:
		vkStage = VK_SHADER_STAGE_VERTEX_BIT;
		uamStage = UamStage_Vertex;
		break;
	case ShaderStage::Fragment:
		vkStage = VK_SHADER_STAGE_FRAGMENT_BIT;
		uamStage = UamStage_Fragment;
		break;
	case ShaderStage::Compute:
		vkStage = VK_SHADER_STAGE_COMPUTE_BIT;
		uamStage = UamStage_Compute;
		break;
	default:
		*error = "Unsupported deko3d shader stage";
		return false;
	}
	std::vector<uint32_t> spirv;
	if (!GLSLtoSPV(vkStage, source, GLSLVariant::VULKAN, spirv, error)) {
		return false;
	}
	std::string glsl;
	try {
		spirv_cross::CompilerGLSL compiler(std::move(spirv));
		const auto resources = compiler.get_shader_resources();
		for (const auto &resource : resources.sampled_images) {
			const unsigned originalBinding = compiler.get_decoration(resource.id, spv::DecorationBinding);
			if (originalBinding < textureBindingBase || originalBinding - textureBindingBase >= 32) {
				*error = "Deko3D texture binding is outside the native slot range";
				return false;
			}
			const unsigned binding = originalBinding - textureBindingBase;
			compiler.set_decoration(resource.id, spv::DecorationBinding, binding);
			if (binding < 32) {
				*textures |= 1u << binding;
				if (compiler.get_type(resource.type_id).image.arrayed) {
					*arrayTextures |= 1u << binding;
				}
			}
			compiler.unset_decoration(resource.id, spv::DecorationDescriptorSet);
		}
		for (const auto &resource : resources.uniform_buffers) {
			compiler.unset_decoration(resource.id, spv::DecorationDescriptorSet);
		}
		for (const auto &resource : resources.storage_buffers) {
			compiler.unset_decoration(resource.id, spv::DecorationDescriptorSet);
		}
		for (const auto &resource : resources.storage_images) {
			compiler.unset_decoration(resource.id, spv::DecorationDescriptorSet);
		}
		for (const auto &resource : resources.push_constant_buffers) {
			compiler.set_decoration(resource.id, spv::DecorationBinding, stage == ShaderStage::Compute ? 5 : 7);
		}
		spirv_cross::CompilerGLSL::Options options;
		options.emit_push_constant_as_uniform_buffer = true;
		options.version = 450;
		options.es = false;
		options.vulkan_semantics = false;
		options.separate_shader_objects = true;
		options.enable_420pack_extension = true;
		compiler.set_common_options(options);
		glsl = compiler.compile();
	} catch (const std::exception &exception) {
		*error = exception.what();
		return false;
	}
	size_t size = 0;
	char *log = nullptr;
	void *compiled = UamCompileGlsl(glsl.c_str(), uamStage, &size, &log);
	if (log) {
		*error = log;
		free(log);
	}
	if (!compiled) {
		return false;
	}
	binary->assign((const char *)compiled, size);
	free(compiled);
	return true;
}
