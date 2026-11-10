#include "ppsspp_config.h"
#include <algorithm>

#include "Common/StringUtils.h"

#include "GPU/Common/ShaderId.h"
#include "GPU/Common/ShaderCommon.h"
#include "GPU/Common/GPUStateUtils.h"
#include "Common/Data/Random/Rng.h"

#include "GPU/Vulkan/VulkanContext.h"

#include "GPU/Common/FragmentShaderGenerator.h"
#include "GPU/Common/VertexShaderGenerator.h"
#include "GPU/Common/ReinterpretFramebuffer.h"
#include "GPU/Common/StencilCommon.h"
#include "GPU/Common/DepalettizeShaderCommon.h"

#include "UnitTest.h"
#ifdef PPSSPP_DEKO3D_SHADER_TESTS
#include "Common/GPU/Deko3D/ShaderCompiler.h"
#include "Common/GPU/thin3d.h"
#include "Common/File/FileUtil.h"
#include "GPU/Common/TextureShaderCommon.h"
#endif

#if PPSSPP_PLATFORM(WINDOWS)
#include <wrl/client.h>
#include "GPU/D3D11/D3D11Util.h"
#include "GPU/D3D11/D3D11Loader.h"
#endif

static constexpr size_t CODE_BUFFER_SIZE = 32768;

bool GenerateFShader(FShaderID id, char *buffer, ShaderLanguage lang, Draw::Bugs bugs, std::string *errorString) {
	buffer[0] = '\0';

	FragmentShaderFlags flags;

	uint64_t uniformMask;
	switch (lang) {
	case ShaderLanguage::GLSL_VULKAN:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_VULKAN);
		return GenerateFragmentShader(id, buffer, compat, bugs, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::GLSL_1xx:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_1xx);
		return GenerateFragmentShader(id, buffer, compat, bugs, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::GLSL_3xx:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_3xx);
		return GenerateFragmentShader(id, buffer, compat, bugs, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::HLSL_D3D11:
	{
		ShaderLanguageDesc compat(ShaderLanguage::HLSL_D3D11);
		return GenerateFragmentShader(id, buffer, compat, bugs, &uniformMask, &flags, errorString);
	}
	default:
		return false;
	}
}

bool GenerateVShader(VShaderID id, char *buffer, ShaderLanguage lang, Draw::Bugs bugs, std::string *errorString) {
	buffer[0] = '\0';

	VertexShaderFlags flags;

	uint32_t attrMask;
	uint64_t uniformMask;
	switch (lang) {
	case ShaderLanguage::GLSL_VULKAN:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_VULKAN);
		return GenerateVertexShader(id, buffer, compat, bugs, &attrMask, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::GLSL_1xx:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_1xx);
		return GenerateVertexShader(id, buffer, compat, bugs, &attrMask, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::GLSL_3xx:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_3xx);
		return GenerateVertexShader(id, buffer, compat, bugs, &attrMask, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::HLSL_D3D11:
	{
		ShaderLanguageDesc compat(ShaderLanguage::HLSL_D3D11);
		return GenerateVertexShader(id, buffer, compat, bugs, &attrMask, &uniformMask, &flags, errorString);
	}
	default:
		return false;
	}
}

static VkShaderStageFlagBits StageToVulkan(ShaderStage stage) {
	switch (stage) {
	case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
	case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
	case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	return VK_SHADER_STAGE_FRAGMENT_BIT;
}

bool TestCompileShader(const char *buffer, ShaderLanguage lang, ShaderStage stage, std::string *errorMessage) {
	std::vector<uint32_t> spirv;
	switch (lang) {
#if PPSSPP_PLATFORM(WINDOWS)
	case ShaderLanguage::HLSL_D3D11:
	{
		const char *programType = nullptr;
		switch (stage) {
		case ShaderStage::Vertex: programType = "vs_4_0"; break;
		case ShaderStage::Fragment: programType = "ps_4_0"; break;
		default:
			*errorMessage = "Unknown shader stage";
			return false;
		}
		auto output = CompileShaderToBytecodeD3D11(buffer, strlen(buffer), programType, 0, errorMessage);
		if (output.empty() && errorMessage->empty()) {
			*errorMessage = "Error compiling HLSL shader: bytecode empty";
		}
		return !output.empty();
	}
#endif

	case ShaderLanguage::GLSL_VULKAN:
#ifdef PPSSPP_DEKO3D_SHADER_TESTS
	{
		std::string binary;
		uint32_t arrays, textures;
		return CompileDeko3DShader(stage, buffer, &binary, &arrays, &textures, errorMessage);
	}
#endif
		return GLSLtoSPV(StageToVulkan(stage), buffer, GLSLVariant::VULKAN, spirv, errorMessage);
	case ShaderLanguage::GLSL_1xx:
		return GLSLtoSPV(StageToVulkan(stage), buffer, GLSLVariant::GL140, spirv, errorMessage);
	case ShaderLanguage::GLSL_3xx:
		return GLSLtoSPV(StageToVulkan(stage), buffer, GLSLVariant::GLES300, spirv, errorMessage);
	default:
		*errorMessage = "Unknown shader language";
		return false;
	}
}

void PrintDiff(const char *a, const char *b) {
	// Stupidest diff ever: Just print both lines, and a few around it, when we find a mismatch.
	std::vector<std::string> a_lines;
	std::vector<std::string> b_lines;
	SplitString(a, '\n', a_lines);
	SplitString(b, '\n', b_lines);
	for (size_t i = 0; i < a_lines.size() && i < b_lines.size(); i++) {
		if (a_lines[i] != b_lines[i]) {
			// Print some context
			for (size_t j = std::max((int)i - 4, 0); j < i; j++) {
				printf("%s\n", a_lines[j].c_str());
			}
			printf("DIFF found at line %d:\n", (int)i);
			printf("a: %s\n", a_lines[i].c_str());
			printf("b: %s\n", b_lines[i].c_str());
			printf("...continues...\n");
			for (size_t j = i + 1; j < i + 5 && j < a_lines.size() && j < b_lines.size(); j++) {
				printf("a: %s\n", a_lines[j].c_str());
				printf("b: %s\n", b_lines[j].c_str());
			}
			printf("==================\n");
			return;
		}
	}
}

const char *ShaderLanguageToString(ShaderLanguage lang) {
	switch (lang) {
	case HLSL_D3D11: return "HLSL_D3D11";
	case GLSL_VULKAN: return "GLSL_VULKAN";
	case GLSL_1xx: return "GLSL_1xx";
	case GLSL_3xx: return "GLSL_3xx";
	default: return "N/A";
	}
}

bool TestReinterpretShaders() {
	Draw::Bugs bugs;

	ShaderLanguage languages[] = {
#if PPSSPP_PLATFORM(WINDOWS)
		ShaderLanguage::HLSL_D3D11,
#endif
		ShaderLanguage::GLSL_VULKAN,
		ShaderLanguage::GLSL_3xx,
	};
	GEBufferFormat fmts[3] = {
		GE_FORMAT_565,
		GE_FORMAT_5551,
		GE_FORMAT_4444,
	};
	char *buffer = new char[65536];

	// Generate all despite failures - it's only 6.
	bool failed = false;

	for (int k = 0; k < ARRAY_SIZE(languages); k++) {
		if (g_testLog) {
			printf("=== %s ===\n\n", ShaderLanguageToString(languages[k]));
		}

		ShaderLanguageDesc desc(languages[k]);

		std::string errorMessage;

		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				if (i == j)
					continue;  // useless shader!
				ShaderWriter writer(buffer, desc, ShaderStage::Fragment);
				GenerateReinterpretFragmentShader(writer, fmts[i], fmts[j]);
				if (strlen(buffer) >= 8192) {
					printf("Reinterpret fragment shader %d exceeded buffer:\n\n%s\n", (int)j, LineNumberString(buffer).c_str());
					failed = true;
				}
				if (!TestCompileShader(buffer, languages[k], ShaderStage::Fragment, &errorMessage)) {
					printf("Error compiling reinterpret fragment shader %d:\n\n%s\n\n%s\n", (int)j, LineNumberString(buffer).c_str(), errorMessage.c_str());
					failed = true;
				} else {
					if (g_testLog) {
						printf("===\n%s\n===\n", buffer);
					}
				}
			}
		}
	}

	delete[] buffer;
	return !failed;
}

bool TestStencilShaders() {
	Draw::Bugs bugs;

	ShaderLanguage languages[] = {
#if PPSSPP_PLATFORM(WINDOWS)
		ShaderLanguage::HLSL_D3D11,
#endif
		ShaderLanguage::GLSL_VULKAN,
		ShaderLanguage::GLSL_3xx,
	};

	char *buffer = new char[65536];

	bool failed = false;

	for (int k = 0; k < ARRAY_SIZE(languages); k++) {
		if (g_testLog) {
			printf("=== %s ===\n\n", ShaderLanguageToString(languages[k]));
		}

		ShaderLanguageDesc desc(languages[k]);
		std::string errorMessage;

		// Generate all despite failures - it's only a few.
		// Only use export on Vulkan, because GLSL_3xx is ES which doesn't support stencil export.
		bool allowUseExport = languages[k] == ShaderLanguage::GLSL_VULKAN;
		for (int useExport = 0; useExport <= (allowUseExport ? 1 : 0); ++useExport) {
			GenerateStencilFs(buffer, desc, bugs, useExport == 1);
			if (strlen(buffer) >= 8192) {
				printf("Stencil fragment shader (useExport=%d) exceeded buffer:\n\n%s\n", useExport, LineNumberString(buffer).c_str());
				failed = true;
			}
			if (!TestCompileShader(buffer, languages[k], ShaderStage::Fragment, &errorMessage)) {
				printf("Error compiling stencil shader (useExport=%d):\n\n%s\n\n%s\n", useExport, LineNumberString(buffer).c_str(), errorMessage.c_str());
				failed = true;
			} else {
				if (g_testLog) {
					printf("===\n%s\n===\n", buffer);
				}
			}
		}

		GenerateStencilVs(buffer, desc);
		if (strlen(buffer) >= 8192) {
			printf("Stencil vertex shader exceeded buffer:\n\n%s\n", LineNumberString(buffer).c_str());
			failed = true;
		}
		if (!TestCompileShader(buffer, languages[k], ShaderStage::Vertex, &errorMessage)) {
			printf("Error compiling stencil shader:\n\n%s\n\n%s\n", LineNumberString(buffer).c_str(), errorMessage.c_str());
			failed = true;
		} else {
			if (g_testLog) {
				printf("===\n%s\n===\n", buffer);
			}
		}
	}

	delete[] buffer;
	return !failed;
}

bool TestDepalShaders() {
	Draw::Bugs bugs;

	ShaderLanguage languages[] = {
#if PPSSPP_PLATFORM(WINDOWS)
		ShaderLanguage::HLSL_D3D11,
#endif
		ShaderLanguage::GLSL_VULKAN,
		ShaderLanguage::GLSL_3xx,
		ShaderLanguage::GLSL_1xx,
	};

	char *buffer = new char[65536];

	for (int k = 0; k < ARRAY_SIZE(languages); k++) {
		if (g_testLog) {
			printf("=== %s ===\n\n", ShaderLanguageToString(languages[k]));
		}

		ShaderLanguageDesc desc(languages[k]);
		std::string errorMessage;

		// TODO: Try more configurations of the fragment shader.
		DepalConfig configs[2]{};
		configs[0].clutFormat = GE_CMODE_16BIT_ABGR4444;
		configs[0].shift = 8;
		configs[0].startPos = 64;
		configs[0].mask = 0xFF;
		configs[0].bufferFormat = GE_FORMAT_8888;
		configs[0].textureFormat = GE_TFMT_CLUT32;
		configs[0].depthUpperBits = 0;
		// A depth read through the X swizzle, stretched into 16-bit halves (Cars Race-O-Rama).
		configs[1].clutFormat = GE_CMODE_16BIT_BGR5650;
		configs[1].shift = 1;
		configs[1].mask = 0xFF;
		configs[1].bufferFormat = GE_FORMAT_DEPTH16;
		configs[1].textureFormat = GE_TFMT_CLUT16;
		configs[1].depthUpperBits = 2;
		configs[1].depthIntoHalves = true;

		for (const DepalConfig &config : configs) {
			ShaderWriter writer(buffer, desc, ShaderStage::Fragment);
			GenerateDepalFs(writer, config);
			if (strlen(buffer) >= 8192) {
				printf("Depal shader exceeded buffer:\n\n%s\n", LineNumberString(buffer).c_str());
				delete[] buffer;
				return false;
			}
			if (!TestCompileShader(buffer, languages[k], ShaderStage::Fragment, &errorMessage)) {
				printf("Error compiling depal shader:\n\n%s\n\n%s\n", LineNumberString(buffer).c_str(), errorMessage.c_str());
				delete[] buffer;
				return false;
			} else {
				if (g_testLog) {
					printf("===\n%s\n===\n", buffer);
				}
			}
		}
	}

	delete[] buffer;
	return true;
}

const ShaderLanguage languages[] = {
#if PPSSPP_PLATFORM(WINDOWS)
	ShaderLanguage::HLSL_D3D11,
#endif
	ShaderLanguage::GLSL_VULKAN,
	ShaderLanguage::GLSL_1xx,
	ShaderLanguage::GLSL_3xx,
};
const int numLanguages = ARRAY_SIZE(languages);

// vr: GPU_USE_VIRTUAL_REALITY is set, which only the GLSL backends (OpenGL, Vulkan) use.
bool TestVertexShaders(bool vr) {
	char *buffer[numLanguages];

	for (int i = 0; i < numLanguages; i++) {
		buffer[i] = new char[65536];
	}
	GMRng rng;
	int successes = 0;
	int count = 700;

	Draw::Bugs bugs;

	// Generate a bunch of random vertex shader IDs, try to generate shader source.
	// Then compile it and check that it's ok.
	for (int i = 0; i < count; i++) {
		uint64_t id64 = rng.R64();
		VShaderID id;
		id.FromUint64(id64);
#ifdef PPSSPP_DEKO3D_SHADER_TESTS
		id.SetBit(VS_BIT_SIMPLE_STEREO, false);
#endif

		// The generated bits need some adjustment:

		// If mode is through, we won't do hardware transform.
		if (id.Bit(VS_BIT_IS_THROUGH)) {
			id.SetBit(VS_BIT_USE_HW_TRANSFORM, 0);
		}
		if (id.Bit(VS_BIT_VERTEX_RANGE_CULLING)) {
			continue;
		}

		bool generateSuccess[numLanguages]{};
		std::string genErrorString[numLanguages];

		for (int j = 0; j < numLanguages; j++) {
			if (vr && languages[j] == ShaderLanguage::HLSL_D3D11) {
				buffer[j][0] = '\0';
				continue;
			}
			generateSuccess[j] = GenerateVShader(id, buffer[j], languages[j], bugs, &genErrorString[j]);
			if (!genErrorString[j].empty()) {
				if (g_testLog) {
					printf("%s\n", genErrorString[j].c_str());
				}
			}
		}

		for (int j = 0; j < numLanguages; j++) {
			if (strlen(buffer[j]) >= CODE_BUFFER_SIZE) {
				printf("Vertex shader exceeded buffer:\n\n%s\n", LineNumberString(buffer[j]).c_str());
				for (int i = 0; i < numLanguages; i++) {
					delete[] buffer[i];
				}
				return false;
			}
		}

		// Now that we have the strings ready for easy comparison (buffer,4 in the watch window),
		// let's try to compile them.
		for (int j = 0; j < numLanguages; j++) {
			if (generateSuccess[j]) {
				std::string errorMessage;
				if (!TestCompileShader(buffer[j], languages[j], ShaderStage::Vertex, &errorMessage)) {
					printf("Error compiling vertex shader %d:\n\n%s\n\nERROR: %s\n\n(end of error)\n\n", (int)j, LineNumberString(buffer[j]).c_str(), errorMessage.c_str());
					for (int i = 0; i < numLanguages; i++) {
						delete[] buffer[i];
					}
					return false;
				}
				successes++;
			}
		}
	}

	if (g_testLog) {
		printf("%d/%d vertex shaders generated (it's normal that it's not all, there are invalid bit combos)\n", successes, count * numLanguages);
	}

	for (int i = 0; i < numLanguages; i++) {
		delete[] buffer[i];
	}
	return true;
}

bool TestFragmentShaders() {
	char *buffer[numLanguages];

	for (int i = 0; i < numLanguages; i++) {
		buffer[i] = new char[65536];
	}
	GMRng rng;
	int successes = 0;
	int count = 300;

	Draw::Bugs bugs;

	// Generate a bunch of random fragment shader IDs, try to generate shader source.
	// Then compile it and check that it's ok.
	for (int i = 0; i < count; i++) {
		uint64_t id64 = rng.R64();
		FShaderID id;
		id.FromUint64(id64);
#ifdef PPSSPP_DEKO3D_SHADER_TESTS
		id.SetBit(FS_BIT_STEREO, false);
		id.SetBit(FS_BIT_USE_FRAMEBUFFER_FETCH, false);
#endif

		// bits we don't need to test because they are irrelevant on d3d11
		id.SetBit(FS_BIT_NO_DEPTH_CANNOT_DISCARD_STENCIL, false);

		// DX9 disabling:
		if (static_cast<ReplaceAlphaType>(id.Bits(FS_BIT_STENCIL_TO_ALPHA, 2)) == ReplaceAlphaType::REPLACE_ALPHA_DUALSOURCE)
			continue;

		bool generateSuccess[numLanguages]{};
		std::string genErrorString[numLanguages];

		for (int j = 0; j < numLanguages; j++) {
			generateSuccess[j] = GenerateFShader(id, buffer[j], languages[j], bugs, &genErrorString[j]);
			if (!genErrorString[j].empty()) {
				if (g_testLog) {
					printf("%s\n", genErrorString[j].c_str());
				}
			}
			// We ignore the contents of the error string here, not even gonna try to compile if it errors.
		}

		for (int j = 0; j < numLanguages; j++) {
			if (strlen(buffer[j]) >= CODE_BUFFER_SIZE) {
				printf("Fragment shader exceeded buffer:\n\n%s\n", LineNumberString(buffer[j]).c_str());
				for (int i = 0; i < numLanguages; i++) {
					delete[] buffer[i];
				}
				return false;
			}
		}

		// Now that we have the strings ready for easy comparison (buffer,4 in the watch window),
		// let's try to compile them.
		for (int j = 0; j < numLanguages; j++) {
			if (generateSuccess[j]) {
				std::string errorMessage;
				if (!TestCompileShader(buffer[j], languages[j], ShaderStage::Fragment, &errorMessage)) {
					printf("Error compiling fragment shader %d:\n\n%s\n\nERROR: %s\n\n(end of error)\n\n", (int)j, LineNumberString(buffer[j]).c_str(), errorMessage.c_str());
					for (int i = 0; i < numLanguages; i++) {
						delete[] buffer[i];
					}
					return false;
				}
				successes++;
			}
		}
	}

	if (g_testLog) {
		printf("%d/%d fragment shaders generated (it's normal that it's not all, there are invalid bit combos)\n", successes, count * numLanguages);
	}

	for (int i = 0; i < numLanguages; i++) {
		delete[] buffer[i];
	}
	return true;
}

#ifdef PPSSPP_DEKO3D_SHADER_TESTS
static bool TestDeko3DClipDistanceHeaders() {
	// TGSI stores clip distances in vec4s. Padding components must not become clip planes.
	for (uint32_t count = 0; count <= 8; ++count) {
		std::string source = "#version 450\nlayout(location=0) in vec4 position;\n";
		if (count) {
			source += StringFromFormat("out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[%u]; };\n", count);
		}
		source += "void main() { gl_Position=position;\n";
		for (uint32_t i = 0; i < count; ++i) {
			source += StringFromFormat("gl_ClipDistance[%u] = position.x + %u.0;\n", i, i);
		}
		source += "}\n";
		std::string binary, error;
		uint32_t arrays = 0, textures = 0;
		if (!CompileDeko3DShader(ShaderStage::Vertex, source.c_str(), &binary, &arrays, &textures, &error)) {
			printf("Deko3D clip-distance shader (%u planes): %s\n", count, error.c_str());
			return false;
		}
		uint32_t controlSize, programsOffset, entrypoint;
		EXPECT_TRUE(binary.size() >= 24);
		memcpy(&controlSize, binary.data() + 8, sizeof(controlSize));
		memcpy(&programsOffset, binary.data() + 16, sizeof(programsOffset));
		EXPECT_TRUE(programsOffset + 8 <= binary.size());
		memcpy(&entrypoint, binary.data() + programsOffset + 4, sizeof(entrypoint));
		// DKSH's graphics entry point starts with Maxwell's 80-byte header. OMAP C is at byte 72.
		const size_t header = (size_t)controlSize + entrypoint;
		EXPECT_TRUE(header + 80 <= binary.size());
		const uint8_t clipMask = (uint8_t)binary[header + 72];
		EXPECT_EQ_INT(clipMask, (1u << count) - 1);
	}
	return true;
}

// The Deko3D backend counts a cached vertex shader's clip and cull distances from its source, and sets the
// hardware's cull mode for the slots after the clip distances. The counts have to match UAM's.
static bool TestDeko3DCullDistances() {
	uint32_t clip = 0, cull = 0;
	Deko3DCountDistances("out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[3]; };\ngl_ClipDistance[1] = 1.0;", &clip, &cull);
	EXPECT_EQ_INT(clip, 3);
	EXPECT_EQ_INT(cull, 0);
	Deko3DCountDistances("gl_ClipDistance[0] = a;\ngl_ClipDistance[1] = b;\ngl_CullDistance[1] = c;\n", &clip, &cull);
	EXPECT_EQ_INT(clip, 2);
	EXPECT_EQ_INT(cull, 2);
	EXPECT_EQ_INT(Deko3DClipDistanceMode(0, 0), 0);
	EXPECT_EQ_INT(Deko3DClipDistanceMode(2, 2), 0x1100);
	EXPECT_EQ_INT(Deko3DClipDistanceMode(3, 2), 0x11000);
	EXPECT_EQ_INT(Deko3DClipDistanceMode(7, 2), 0x10000000);

	const u32 oldFlags = gstate_c.UseFlags();
	gstate_c.SetUseFlags(oldFlags | GPU_USE_CLIP_DISTANCE | GPU_USE_CULL_DISTANCE);
	Draw::Bugs bugs;
	GMRng rng;
	std::vector<char> buffer(128 * 1024);
	int compiled = 0, withCull = 0;
	bool ok = true;
	for (int i = 0; i < 300 && ok; i++) {
		const uint64_t id64 = rng.R64();
		VShaderID id;
		id.FromUint64(id64);
		id.SetBit(VS_BIT_SIMPLE_STEREO, false);
		id.SetBit(VS_BIT_IS_THROUGH, false);
		id.SetBit(VS_BIT_USE_HW_TRANSFORM, true);
		id.SetBit(VS_BIT_VERTEX_RANGE_CULLING, i % 2 == 0);
		std::string error;
		if (!GenerateVShader(id, buffer.data(), GLSL_VULKAN, bugs, &error)) {
			continue;  // Not every random ID is a valid combination.
		}
		std::string binary;
		uint32_t arrays = 0, textures = 0, compiledClip = 0, compiledCull = 0;
		if (!CompileDeko3DShader(ShaderStage::Vertex, buffer.data(), &binary, &arrays, &textures, &error, 0, &compiledClip, &compiledCull)) {
			printf("Deko3D vertex shader %016llx: %s\n", (unsigned long long)id64, error.c_str());
			ok = false;
			break;
		}
		Deko3DCountDistances(buffer.data(), &clip, &cull);
		if (clip != compiledClip || cull != compiledCull) {
			printf("Deko3D distances of %016llx: source clip=%u cull=%u, compiled clip=%u cull=%u\n", (unsigned long long)id64, clip, cull, compiledClip, compiledCull);
			ok = false;
		}
		// The header exports the clip distances and then the cull distances (see TestDeko3DClipDistanceHeaders).
		uint32_t controlSize, programsOffset, entrypoint;
		memcpy(&controlSize, binary.data() + 8, sizeof(controlSize));
		memcpy(&programsOffset, binary.data() + 16, sizeof(programsOffset));
		memcpy(&entrypoint, binary.data() + programsOffset + 4, sizeof(entrypoint));
		const uint8_t distanceMask = (uint8_t)binary[(size_t)controlSize + entrypoint + 72];
		if (distanceMask != (1u << (clip + cull)) - 1) {
			printf("Deko3D distance mask of %016llx: %02x for clip=%u cull=%u\n", (unsigned long long)id64, distanceMask, clip, cull);
			ok = false;
		}
		compiled++;
		withCull += cull > 0;
	}
	gstate_c.SetUseFlags(oldFlags);
	printf("Deko3D cull distances: %d vertex shaders compiled, %d with cull distances\n", compiled, withCull);
	EXPECT_TRUE(ok);
	EXPECT_TRUE(withCull >= 50);
	return true;
}

static bool TestDeko3DThin3DShaders() {
	const ShaderLanguageDesc language(GLSL_VULKAN);
	for (int i = 0; i < Draw::VS_MAX_PRESET; ++i) {
		for (bool tint : {false, true}) {
			const std::string source = Draw::GenerateVertexShaderPreset(language, (Draw::VertexShaderPreset)i, tint);
			std::string binary, error;
			uint32_t arrays = 0, textures = 0;
			EXPECT_TRUE(CompileDeko3DShader(ShaderStage::Vertex, source.c_str(), &binary, &arrays, &textures, &error, 1));
			EXPECT_EQ_INT(textures, 0);
		}
	}
	for (int i = 0; i < Draw::FS_MAX_PRESET; ++i) {
		const std::string source = Draw::GenerateFragmentShaderPreset(language, (Draw::FragmentShaderPreset)i);
		std::string binary, error;
		uint32_t arrays = 0, textures = 0;
		EXPECT_TRUE(CompileDeko3DShader(ShaderStage::Fragment, source.c_str(), &binary, &arrays, &textures, &error, 1));
		EXPECT_EQ_INT(textures, 1);
		EXPECT_EQ_INT(arrays, 0);
		// The emulation path must retain Vulkan binding 1 as native slot 1.
		EXPECT_TRUE(CompileDeko3DShader(ShaderStage::Fragment, source.c_str(), &binary, &arrays, &textures, &error));
		EXPECT_EQ_INT(textures, 2);
	}
	const char *invalid = "#version 450\nlayout(binding=0) uniform sampler2D tex;\nlayout(location=0) out vec4 color;\nvoid main() { "
						  "color=texture(tex,vec2(0)); }";
	std::string binary, error;
	uint32_t arrays = 0, textures = 0;
	EXPECT_FALSE(CompileDeko3DShader(ShaderStage::Fragment, invalid, &binary, &arrays, &textures, &error, 1));
	EXPECT_TRUE(!error.empty());
	return true;
}

static bool TestDeko3DScalingShaders() {
	const char *files[] = {
		"tex_2xbrz.csh", "tex_4xbrz.csh", "tex_nnedi3_2x_single.csh", "tex_nnedi3_4x_single.csh",
		"tex_nnedi3_mp_nns16_pass1_buffer.csh", "tex_nnedi3_mp_nns16_pass1_image.csh", "tex_nnedi3_mp_nns16_pass2.csh",
		"tex_nnedi3_mp_cshift_2x.csh", "tex_nnedi3_mp_cshift_4x.csh",
	};
	for (size_t i = 0; i < ARRAY_SIZE(files); ++i) {
		std::string source, error, binary;
		if (!File::ReadTextFileToString(Path("assets/shaders") / files[i], &source)) { printf("Could not read %s\n", files[i]); return false; }
		if (i < 4) { source = StringFromFormat(textureScaleUploadShader, source.c_str()); }
		uint32_t arrays = 0, textures = 0;
		if (!CompileDeko3DShader(ShaderStage::Compute, source.c_str(), &binary, &arrays, &textures, &error)) {
			printf("deko3d scaling shader %s failed: %s\n", files[i], error.c_str()); return false;
		}
	}
	return true;
}
#endif

bool TestShaderGenerators() {
#if PPSSPP_PLATFORM(WINDOWS)
	LoadD3D11();
	init_glslang();
#else
	init_glslang();
#endif

#ifdef PPSSPP_DEKO3D_SHADER_TESTS
	if (!TestDeko3DClipDistanceHeaders()) {
		return false;
	}
	if (!TestDeko3DCullDistances()) {
		return false;
	}
	if (!TestDeko3DThin3DShaders()) {
		return false;
	}
	if (!TestDeko3DScalingShaders()) { return false; }
#endif

	if (!TestStencilShaders()) {
		return false;
	}

	if (!TestReinterpretShaders()) {
		return false;
	}

	if (!TestDepalShaders()) {
		return false;
	}

	if (!TestFragmentShaders()) {
		return false;
	}

	if (!TestVertexShaders(false)) {
		return false;
	}

	// Again with the VR uniforms and the per-eye view (with VS_BIT_SIMPLE_STEREO on Vulkan).
	const u32 oldFlags = gstate_c.GetUseFlags();
	gstate_c.SetUseFlags(oldFlags | GPU_USE_VIRTUAL_REALITY);
	const bool vrResult = TestVertexShaders(true);
	gstate_c.SetUseFlags(oldFlags);
	if (!vrResult) {
		return false;
	}

	return true;
} 
