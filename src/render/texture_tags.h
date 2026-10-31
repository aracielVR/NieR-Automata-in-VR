// texture_tags.h - which game texture is which, by 3Dmigoto's texture hash, kept on the texture itself (texture_tags.cpp).
#pragma once
#include <d3d11.h>
#include <stdint.h>

// After the game's CreateTexture2D: hashes textures created with data and remembers the hash on the texture.
void TextureTagsOnCreate(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data);
// The texture's 3Dmigoto hash; false if it has none (created without data, or not a texture).
bool TextureTagsHash(ID3D11Resource* resource, uint32_t* hash);
// One of the lens flare's textures (HelixMod's list + [Render] LensFlareTextures).
bool TextureTagsIsLensFlare(ID3D11Resource* resource);
