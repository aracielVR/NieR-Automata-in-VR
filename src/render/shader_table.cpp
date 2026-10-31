// shader_table.cpp - every shader the game creates, with a copy of its bytecode and which pass it belongs to.
#include "render/shader_table.h"
#include "core/locks.h"
#include "core/pure.h"
#include <string.h>

namespace {
struct ShaderEntry {
	void* shader;
	uint64_t hash;
	uint8_t* blob;
	uint32_t len;
	ShaderStage stage;
	bool usedInCapture;
	bool ui;          // vertex shader with the UIParam / cbUIParam cbuffers
	bool sao;         // ambient occlusion: "SaoGlobalParams"
	bool shadowMask;  // screen-space shadow pass: reads "ShadowView_Buffer", not "ScatterParam"
	bool scatter;     // light scattering (fog, light shafts): "ScatterParam" (also reads the shadow map)
	bool vignette;    // tone mapping: "g_VignettoParam"
	bool effect;      // effect system: particles, screen filters: "EffectParameterIndex"
	bool glare;       // the glare (bloom) around the sun: "g_GlareParam" (its only parameters)
	ComputeShaderClass computeClass;
	uint32_t sampled;  // texture registers it samples (bit mask)
};
constexpr uint32_t kSlots = 1 << 16;
ShaderEntry* s_shaders;
SRWLOCK s_tableLock = SRWLOCK_INIT;
uint32_t s_count;

uint32_t SlotFor(void* shader) {
	uint64_t mixed = (uint64_t)(uintptr_t)shader * 0x9E3779B97F4A7C15ull;
	return (uint32_t)(mixed >> 48) & (kSlots - 1);
}

uint64_t Fnv1a(const uint8_t* data, size_t size) {
	uint64_t hash = 1469598103934665603ull;
	for (size_t i = 0; i < size; i++) hash = (hash ^ data[i]) * 1099511628211ull;
	return hash;
}

bool Contains(const uint8_t* blob, uint32_t len, const char* name) {
	size_t nameLen = strlen(name);
	for (uint32_t k = 0; k + nameLen <= len; k++)
		if (memcmp(blob + k, name, nameLen) == 0) return true;
	return false;
}

ShaderEntry* Find(void* shader) {
	if (!s_shaders || !shader) return nullptr;
	uint32_t slot = SlotFor(shader);
	for (uint32_t probe = 0; probe < kSlots; probe++, slot = (slot + 1) & (kSlots - 1)) {
		if (s_shaders[slot].shader == shader) return &s_shaders[slot];
		if (!s_shaders[slot].shader) return nullptr;
	}
	return nullptr;
}
}  // namespace

void ShaderTableInit() {
	if (!s_shaders) s_shaders = (ShaderEntry*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ShaderEntry) * kSlots);
}

void ShaderTableRemember(void* shader, const void* bytecode, SIZE_T len, ShaderStage stage) {
	if (!s_shaders || !shader || !bytecode || len == 0 || len > (1u << 24)) return;
	uint8_t* copy = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, len);
	if (!copy) return;
	memcpy(copy, bytecode, len);
	ExclusiveLock guard(&s_tableLock);
	uint32_t slot = SlotFor(shader);
	for (uint32_t probe = 0; probe < kSlots; probe++, slot = (slot + 1) & (kSlots - 1)) {
		ShaderEntry& entry = s_shaders[slot];
		if (entry.shader != shader && entry.shader != nullptr) continue;
		if (entry.blob) HeapFree(GetProcessHeap(), 0, entry.blob);  // pointer reused by a new shader
		if (!entry.shader) s_count++;
		entry = {};
		entry.shader = shader;
		entry.hash = Fnv1a(copy, len);
		entry.blob = copy;
		entry.len = (uint32_t)len;
		entry.stage = stage;
		uint32_t size = entry.len;
		if (stage == ST_VS) entry.ui = Contains(copy, size, "UIParam");
		if (stage == ST_CS)
			for (uint32_t k = 0; k + 11 <= size && entry.computeClass == CS_OTHER; k++) {  // whichever name comes first
				if (memcmp(copy + k, "g_LightAry", 10) == 0) entry.computeClass = CS_LIGHT_CULLING;
				else if (memcmp(copy + k, "g_CubeInput", 11) == 0) entry.computeClass = CS_PROBE_SELECTION;
			}
		if (stage == ST_PS) {
			entry.sampled = pure::dxbc_sampled_textures(copy, len);
			entry.sao = Contains(copy, size, "SaoGlobalParams");
			// The light-scattering shader reads ShadowView_Buffer too, but it is not the shadow mask:
			// "Shadows off" on it turned into dense grey fog.
			entry.scatter = Contains(copy, size, "ScatterParam");
			entry.shadowMask = !entry.scatter && Contains(copy, size, "ShadowView_Buffer");
			entry.vignette = Contains(copy, size, "g_VignettoParam");
			entry.effect = Contains(copy, size, "EffectParameterIndex");
			// a pass of its own, with nothing in it but the flare: g_GlareColor, g_GlareParam, one texture
			entry.glare = Contains(copy, size, "g_GlareParam");
		}
		return;
	}
	HeapFree(GetProcessHeap(), 0, copy);  // table full
}

bool ShaderTableIsUi(void* vertexShader) {
	AcquireSRWLockShared(&s_tableLock);
	ShaderEntry* entry = Find(vertexShader);
	bool isUi = entry && entry->ui;
	ReleaseSRWLockShared(&s_tableLock);
	return isUi;
}

PixelShaderClass ShaderTableClassifyPixel(void* pixelShader, uint32_t* sampled, uint64_t* hash) {
	AcquireSRWLockShared(&s_tableLock);
	ShaderEntry* entry = Find(pixelShader);
	PixelShaderClass kind = PS_OTHER;
	if (entry) {
		if (entry->sao) kind = PS_AMBIENT_OCCLUSION;
		else if (entry->shadowMask) kind = PS_SHADOW_MASK;
		else if (entry->vignette) kind = PS_TONE_MAPPING;
		else if (entry->effect) kind = PS_EFFECT;
		else if (entry->scatter) kind = PS_LIGHT_SCATTERING;
		else if (entry->glare) kind = PS_GLARE;
	}
	*sampled = entry ? entry->sampled : 0;
	*hash = entry ? entry->hash : 0;
	ReleaseSRWLockShared(&s_tableLock);
	return kind;
}

ComputeShaderClass ShaderTableClassifyCompute(void* computeShader) {
	AcquireSRWLockShared(&s_tableLock);
	ShaderEntry* entry = Find(computeShader);
	ComputeShaderClass kind = entry ? entry->computeClass : CS_OTHER;
	ReleaseSRWLockShared(&s_tableLock);
	return kind;
}

void ShaderTableMarkUsed(void* shader) {
	SharedLock guard(&s_tableLock);
	ShaderEntry* entry = Find(shader);
	if (entry) entry->usedInCapture = true;
}

int ShaderTableTakeUsed(ShaderBlobVisitor visit) {
	if (!s_shaders) return 0;
	int count = 0;
	{
		ExclusiveLock guard(&s_tableLock);
		for (uint32_t i = 0; i < kSlots; i++) {
			ShaderEntry& entry = s_shaders[i];
			if (!entry.shader || !entry.usedInCapture) continue;
			entry.usedInCapture = false;
			visit(entry.stage, entry.shader, entry.hash, entry.blob, entry.len);
			count++;
		}
	}
	return count;
}

uint32_t ShaderTableCount() { return s_count; }
