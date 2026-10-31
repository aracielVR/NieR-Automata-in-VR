// shader_table.h - every shader the game creates, with a copy of its bytecode and what kind of pass it is.
#pragma once
#include <d3d11.h>
#include <stdint.h>

// Values are part of the capture file format (tools/analyze_capture.py).
enum ShaderStage : uint8_t { ST_VS = 0,
							 ST_PS = 1,
							 ST_GS = 2,
							 ST_CS = 3 };

// Pixel shader passes the mod acts on.
enum PixelShaderClass {
	PS_OTHER = 0,
	PS_AMBIENT_OCCLUSION = 1,
	PS_SHADOW_MASK = 2,
	PS_TONE_MAPPING = 3,  // holds the vignette parameters
	PS_EFFECT = 4,        // NieR's effect system: particles, screen filters
	PS_LIGHT_SCATTERING = 5,
	PS_GLARE = 6,  // the glare (bloom) around the sun: "g_GlareParam" - not the lens flare
	PS_CLASS_COUNT  // keep last: tables indexed by the class are this long
};

// Compute shader passes the mod acts on (viewfix.cpp).
enum ComputeShaderClass {
	CS_OTHER = 0,
	CS_LIGHT_CULLING = 1,
	CS_PROBE_SELECTION = 2,
};

void ShaderTableInit();
void ShaderTableRemember(void* shader, const void* bytecode, SIZE_T len, ShaderStage stage);
bool ShaderTableIsUi(void* vertexShader);  // a vertex shader of NieR's 2D UI
// `sampled`: the texture registers it samples; `hash`: its bytecode hash (stable between runs, for the log).
PixelShaderClass ShaderTableClassifyPixel(void* pixelShader, uint32_t* sampled, uint64_t* hash);
ComputeShaderClass ShaderTableClassifyCompute(void* computeShader);

// For the frame capture: shaders bound during it, and their bytecode once it ends.
void ShaderTableMarkUsed(void* shader);
typedef void (*ShaderBlobVisitor)(ShaderStage stage, void* shader, uint64_t hash, const uint8_t* blob, uint32_t len);
int ShaderTableTakeUsed(ShaderBlobVisitor visit);  // visits and unmarks each used shader; returns how many
uint32_t ShaderTableCount();
