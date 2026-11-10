// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "Common/GPU/Deko3D/ShaderCompiler.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <vector>

#include "Common/GPU/Deko3D/UamBridge.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "ext/SPIRV-Cross/spirv_glsl.hpp"

void Deko3DCountDistances(const char *source, uint32_t *clipDistances, uint32_t *cullDistances) {
	const std::string_view text(source);
	auto count = [&](std::string_view name) {
		uint32_t result = 0;
		for (size_t pos = text.find(name); pos != std::string_view::npos; pos = text.find(name, pos + 1)) {
			size_t at = pos + name.size();
			if (at >= text.size() || text[at] != '[') {
				continue;
			}
			uint32_t value = 0;
			bool digits = false;
			while (++at < text.size() && text[at] >= '0' && text[at] <= '9') {
				value = value * 10 + (text[at] - '0');
				digits = true;
			}
			if (!digits || at >= text.size() || text[at] != ']') {
				continue;
			}
			// "float gl_ClipDistance[3]" declares a size; anything else is an index.
			size_t before = pos;
			while (before > 0 && (text[before - 1] == ' ' || text[before - 1] == '\t')) {
				before--;
			}
			const bool declaration = before >= 5 && text.substr(before - 5, 5) == "float";
			result = std::max(result, declaration ? value : value + 1);
		}
		return result;
	};
	*clipDistances = count("gl_ClipDistance");
	*cullDistances = count("gl_CullDistance");
}

uint32_t Deko3DClipDistanceMode(uint32_t clipDistances, uint32_t cullDistances) {
	uint32_t mode = 0;
	for (uint32_t i = 0; i < cullDistances && clipDistances + i < 8; ++i) {
		mode |= 1u << ((clipDistances + i) * 4);
	}
	return mode;
}

bool CompileDeko3DShader(ShaderStage stage, const char *source, std::string *binary, uint32_t *arrayTextures, uint32_t *textures,
						 std::string *error, uint32_t textureBindingBase, uint32_t *clipDistances, uint32_t *cullDistances) {
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
	unsigned clip = 0, cull = 0;
	void *compiled = UamCompileGlslDistances(glsl.c_str(), uamStage, &size, &log, &clip, &cull);
	if (clipDistances) {
		*clipDistances = clip;
	}
	if (cullDistances) {
		*cullDistances = cull;
	}
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
