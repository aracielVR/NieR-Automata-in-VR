// texture_tags.cpp - which game texture is which, by 3Dmigoto's texture hash, kept on the texture itself.
// The hash is the one 3Dmigoto fixes use in their [TextureOverride*] sections, so a HelixMod fix's texture lists
// name the same textures here. It is stored as private data on the texture, so a texture pointer the game frees
// and reuses can never carry an old texture's hash.
#include "render/texture_tags.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include "core/pure.h"
#include <atomic>
#include <cpuid.h>
#include <nmmintrin.h>
#include <string.h>

namespace {
// {6A1F4C2E-2B7D-4E8A-9C1D-5F3E7B9A0D41}
constexpr GUID kHashTag = {0x6a1f4c2e, 0x2b7d, 0x4e8a, {0x9c, 0x1d, 0x5f, 0x3e, 0x7b, 0x9a, 0x0d, 0x41}};

// The lens flare's textures, from HelixMod's NieR:Automata 3D fix (d3dx.ini, [TextureOverrideLensFlares1-17]).
// Its flare switch filters these at t0 of the vertex shader 3Dmigoto calls e51a09c27e5185f8 = our 0981c9c0fc22cb33,
// the effect shader that draws the flare's 23 quads in JJ's capture (NOTES A11).
constexpr uint32_t kLensFlareTextures[] = {0x0e6282d3, 0x9ba50cbf, 0x6a62c90e, 0x08876e6b, 0x6ded1b3b, 0x28de46d6,
										   0x8b9fdbac, 0x9cf9dbf0, 0xb5968729, 0xd553371d, 0xb4752996, 0x183752f0,
										   0x43fabd65, 0xf9c5fc4a, 0x61d6fcd3, 0x6d19168f, 0x04633434};
constexpr int kBuiltIn = (int)(sizeof(kLensFlareTextures) / sizeof(kLensFlareTextures[0]));

std::atomic<uint64_t> s_flareSeen;  // bit per list entry (built-in, then the ini's): created at least once
std::atomic<uint32_t> s_hashed;

// Index in the flare list (built-in first, then [Render] LensFlareTextures), or -1.
int FlareIndex(uint32_t hash) {
	for (int i = 0; i < kBuiltIn; i++)
		if (kLensFlareTextures[i] == hash) return i;
	for (int i = 0; i < g_cfg.lensFlareExtraCount; i++)
		if (g_cfg.lensFlareExtra[i] == hash) return kBuiltIn + i;
	return -1;
}

// CRC32C with the CPU's own instruction (SSE 4.2), as 3Dmigoto does: textures are hashed as the game loads them.
__attribute__((target("sse4.2"))) uint32_t Crc32cHardware(uint32_t crc, const uint8_t* p, size_t n) {
	uint64_t c = ~crc;
	for (; n >= 8; p += 8, n -= 8) {
		uint64_t word;
		memcpy(&word, p, 8);
		c = _mm_crc32_u64(c, word);
	}
	for (; n; p++, n--) c = _mm_crc32_u8((uint32_t)c, *p);
	return ~(uint32_t)c;
}

pure::Crc32cFn Crc32c() {
	static pure::Crc32cFn fn = [] {
		unsigned a, b, c, d;
		bool sse42 = __get_cpuid(1, &a, &b, &c, &d) && (c & bit_SSE4_2);
		return sse42 ? &Crc32cHardware : &pure::crc32c;
	}();
	return fn;
}
}  // namespace

void TextureTagsOnCreate(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data) {
	if (!texture || !desc || !data || !data->pSysMem) return;
	static_assert(sizeof(D3D11_TEXTURE2D_DESC) == 44, "3Dmigoto hashes the 44-byte description");
	uint32_t hash;
	if (!pure::migoto_texture_hash((const uint8_t*)desc, desc->Width, desc->Height, desc->ArraySize, desc->Format,
								   (const uint8_t*)data->pSysMem, data->SysMemPitch, Crc32c(), &hash))
		return;
	texture->SetPrivateData(kHashTag, sizeof(hash), &hash);
	if (!s_hashed++) Log("texture tags: the game's textures are named by 3Dmigoto's hash as they load");
	int flare = FlareIndex(hash);
	if (flare < 0) return;
	uint64_t bit = 1ull << flare;
	uint64_t before = s_flareSeen.fetch_or(bit);
	if (!(before & bit))  // the first time this one loads
		Log("lens flare: texture %08x loaded (%ux%u, format %u) - %d of the %d known flare textures seen so far", (unsigned)hash,
			desc->Width, desc->Height, (unsigned)desc->Format, __builtin_popcountll(before | bit),
			kBuiltIn + g_cfg.lensFlareExtraCount);
}

bool TextureTagsHash(ID3D11Resource* resource, uint32_t* hash) {
	if (!resource) return false;
	UINT size = sizeof(*hash);
	return SUCCEEDED(resource->GetPrivateData(kHashTag, &size, hash)) && size == sizeof(*hash);
}

bool TextureTagsIsLensFlare(ID3D11Resource* resource) {
	uint32_t hash;
	return TextureTagsHash(resource, &hash) && FlareIndex(hash) >= 0;
}
