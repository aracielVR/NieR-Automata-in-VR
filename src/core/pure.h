// pure.h - platform-independent pieces of features (JSON lookup, motion controls, GI pick, upscale sizes, ...).
// No Windows dependencies, so this file is also unit-tested on the build host. The math is in vecmath.h.
#pragma once

#include <iterator>
#include <stddef.h>
#include <stdint.h>
#include "core/vecmath.h"

namespace pure {

// Text
// Finds `"key"` in a JSON text and copies its string value (with JSON escapes decoded) to out.
// Good enough for OpenXR runtime manifests; not a general JSON parser.
inline bool json_find_string(const char* json, size_t jsonLen, const char* key, char* out, size_t outCap) {
	size_t keyLen = 0;
	while (key[keyLen]) keyLen++;
	for (size_t i = 0; i + keyLen + 2 <= jsonLen; i++) {
		if (json[i] != '"') continue;
		bool match = true;
		for (size_t k = 0; k < keyLen; k++)
			if (json[i + 1 + k] != key[k]) {
				match = false;
				break;
			}
		if (!match || json[i + 1 + keyLen] != '"') continue;
		size_t p = i + 2 + keyLen;
		while (p < jsonLen && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
		if (p >= jsonLen || json[p] != ':') continue;
		p++;
		while (p < jsonLen && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
		if (p >= jsonLen || json[p] != '"') continue;
		p++;
		size_t n = 0;
		while (p < jsonLen && json[p] != '"') {
			char c = json[p++];
			if (c == '\\' && p < jsonLen) {
				char e = json[p++];
				switch (e) {
					case 'n': c = '\n'; break;
					case 't': c = '\t'; break;
					case 'r': c = '\r'; break;
					case 'b': c = '\b'; break;
					case 'f': c = '\f'; break;
					default: c = e; break;  // \\ \/ \" (\uXXXX unsupported, rare in paths)
				}
			}
			if (n + 1 < outCap) out[n++] = c;
			else return false;
		}
		if (outCap) out[n] = 0;
		return p < jsonLen;
	}
	return false;
}

// Head-direction culling: a LookAt(eye, target, up) turned the way the headset looks.
// `rel` is the eye's pose relative to the game camera (only its rotation is used, columns = the
// eye's right/up/back in the camera's frame). The camera frame is rebuilt from eye/target/up, so
// the result follows this frame's camera even when `rel` is a frame old. Returns false (and leaves
// the outputs alone) for a degenerate look-at.
inline bool head_lookat(const float eye[3], const float target[3], const float up[3], const M4& rel,
						float outTarget[3], float outUp[3]) {
	V3 f{target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
	float d = fsqrt(f.x * f.x + f.y * f.y + f.z * f.z);
	if (!(d > 1e-6f) || !(d < 1e30f)) return false;
	f = {f.x / d, f.y / d, f.z / d};
	V3 r = cross(f, V3{up[0], up[1], up[2]});
	float rl = fsqrt(r.x * r.x + r.y * r.y + r.z * r.z);
	if (!(rl > 1e-6f)) return false;
	r = {r.x / rl, r.y / rl, r.z / rl};
	V3 u = cross(r, f);
	V3 b{-f.x, -f.y, -f.z};
	V3 lf{-rel.at(0, 2), -rel.at(1, 2), -rel.at(2, 2)}, lu{rel.at(0, 1), rel.at(1, 1), rel.at(2, 1)};
	V3 wf{lf.x * r.x + lf.y * u.x + lf.z * b.x, lf.x * r.y + lf.y * u.y + lf.z * b.y, lf.x * r.z + lf.y * u.z + lf.z * b.z};
	V3 wu{lu.x * r.x + lu.y * u.x + lu.z * b.x, lu.x * r.y + lu.y * u.y + lu.z * b.y, lu.x * r.z + lu.y * u.z + lu.z * b.z};
	float fl = fsqrt(wf.x * wf.x + wf.y * wf.y + wf.z * wf.z), ul = fsqrt(wu.x * wu.x + wu.y * wu.y + wu.z * wu.z);
	if (!(fl > 0.5f && fl < 2) || !(ul > 0.5f && ul < 2)) return false;  // not a rotation
	outTarget[0] = eye[0] + d * wf.x / fl;
	outTarget[1] = eye[1] + d * wf.y / fl;
	outTarget[2] = eye[2] + d * wf.z / fl;
	outUp[0] = wu.x / ul;
	outUp[1] = wu.y / ul;
	outUp[2] = wu.z / ul;
	return true;
}

// Texture registers (t0-t31, as a bit mask) that a DXBC shader samples or loads from as textures:
// sample*/gather*/ld/ld_ms instructions, not resinfo or structured-buffer loads. 0 if unreadable.
inline uint32_t dxbc_sampled_textures(const uint8_t* b, size_t len) {
	auto u32 = [&](size_t o) -> uint32_t {
		return o + 4 <= len ? (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24 : 0;
	};
	if (len < 32 || b[0] != 'D' || b[1] != 'X' || b[2] != 'B' || b[3] != 'C') return 0;
	uint32_t chunks = u32(28);
	for (uint32_t c = 0; c < chunks && c < 64; c++) {
		size_t off = u32(32 + 4 * (size_t)c);
		if (off + 8 > len) return 0;
		bool code = (b[off] == 'S' && b[off + 1] == 'H' && (b[off + 2] == 'E' || b[off + 2] == 'D') &&
					 (b[off + 3] == 'X' || b[off + 3] == 'R'));
		if (!code) continue;
		size_t size = u32(off + 4), base = off + 8, n = size / 4;
		if (base + size > len) return 0;
		uint32_t mask = 0;
		size_t j = 2;  // version, length
		while (j < n) {
			uint32_t t = u32(base + 4 * j), op = t & 0x7ff;
			size_t ln = op == 0x35 ? u32(base + 4 * (j + 1)) : (t >> 24) & 0x7f;
			if (ln == 0 || j + ln > n) break;
			bool sample = op == 45 || op == 46 || (op >= 69 && op <= 74) || op == 109 || (op >= 126 && op <= 128);
			if (sample)
				for (size_t k = j + 1; k + 1 < j + ln; k++) {
					uint32_t o = u32(base + 4 * k);
					if (((o >> 12) & 0xff) == 7 && ((o >> 20) & 3) == 1 && ((o >> 22) & 7) == 0 && !(o >> 31)) {
						uint32_t r = u32(base + 4 * (k + 1));
						if (r < 32) mask |= 1u << r;
						break;
					}
				}
			j += ln;
		}
		return mask;
	}
	return 0;
}

// Perspective projection detection (camera probe)
// m is 16 consecutive floats as found in a constant buffer.
//   row-major  (mul(v, M)):  w comes from column 3:  m[3]=m[7]=m[15]=0, m[11]=+-1
//   column-major (mul(M, v)): w comes from row 3:    m[12]=m[13]=m[15]=0, m[14]=+-1
struct ProjInfo {
	bool rowMajor;
	float sx, sy;  // focal scales: 1/tan(half fov)
	float cx, cy;  // off-centre terms
	float A, B;    // depth mapping: z_ndc = A + B / z_view (w = +-z)
	float wSign;   // +1 left-handed (camera looks down +z), -1 right-handed
	float vfovDeg, aspect;
};

inline bool classify_projection(const float* m, ProjInfo* out) {
	ProjInfo p = {};
	bool row = near0(m[3]) && near0(m[7]) && near0(m[15]) && (near0(m[11] - 1) || near0(m[11] + 1)) && near0(m[1]) &&
			   near0(m[2]) && near0(m[4]) && near0(m[6]) && near0(m[12]) && near0(m[13]);
	bool col = near0(m[12]) && near0(m[13]) && near0(m[15]) && (near0(m[14] - 1) || near0(m[14] + 1)) && near0(m[1]) &&
			   near0(m[3]) && near0(m[4]) && near0(m[7]) && near0(m[8]) && near0(m[9]);
	if (row == col) return false;  // neither, or degenerate (both)
	p.rowMajor = row;
	p.sx = m[0];
	p.sy = m[5];
	// down to ~0.01: CullingFov up to 170 deg (sx = 0.049 at 16:9) must still count as the main camera
	if (!(p.sx > 0.01f && p.sx < 50 && p.sy > 0.01f && p.sy < 50)) return false;
	p.aspect = p.sy / p.sx;
	if (p.aspect < 0.2f || p.aspect > 5) return false;
	p.A = m[10];
	if (row) {
		p.cx = m[8];
		p.cy = m[9];
		p.B = m[14];
		p.wSign = m[11];
	} else {
		p.cx = m[2];
		p.cy = m[6];
		p.B = m[11];
		p.wSign = m[14];
	}
	if (near0(p.B)) return false;  // affine z: not a perspective projection
	p.vfovDeg = 2 * fatan(1 / p.sy) * 57.2957795f;
	*out = p;
	return true;
}

// VR controllers as a gamepad (vr_controllers.cpp): the left menu button. A tap (released within
// 0.5 s, stick not used) gives Start for 120 ms; holding it gives Back after 0.5 s; while it is held
// the left stick is the D-pad (and the stick itself reads neutral). Bits as XInput's.
struct MenuButton {
	uint32_t since = 0, startUntil = 0;
	bool usedForDpad = false;
	enum : uint16_t { UP = 1,
					  DOWN = 2,
					  LEFT = 4,
					  RIGHT = 8,
					  START = 0x10,
					  BACK = 0x20 };
	// returns the buttons to add; clears *lx/*ly while the menu button is held
	uint16_t update(bool down, float* lx, float* ly, uint32_t t) {
		uint16_t b = 0;
		if (down) {
			if (!since) {
				since = t ? t : 1;
				usedForDpad = false;
			}
			const float k = 0.6f;
			uint16_t d = 0;
			if (*ly > k) d |= UP;
			if (*ly < -k) d |= DOWN;
			if (*lx < -k) d |= LEFT;
			if (*lx > k) d |= RIGHT;
			if (d) usedForDpad = true;
			b |= d;
			if (!usedForDpad && t - since > 500) b |= BACK;
			*lx = *ly = 0;
		} else if (since) {
			if (!usedForDpad && t - since <= 500) startUntil = t + 120;
			since = 0;
		}
		if ((int32_t)(startUntil - t) > 0) b |= START;
		return b;
	}
};

// GI volumes (viewfix.cpp). The game's probe-selection pass loops over g_CubeInput (96-byte
// entries = 24 floats: a = position (a.w != 0: slot in use), b/c/d = rows of view -> box space,
// f.xyz = half extents) and writes the chosen *slot numbers* per pixel; later shaders sample
// g_CubeTexArray at those slots, so slots must never move. This picks, from a CPU copy of the
// buffer, which slots stay on: all used ones (keep >= used) or the `keep` nearest to the camera
// (the view-space origin; distance to the box like the game's per-pixel test). Returns the loop
// length the game's pass needs: one past the last slot left on (plus `margin` when nothing is
// dropped, for volumes that appear before the next read-back), at least 1, at most count.
struct GiPick {
	int32_t used, highest, kept, length;  // highest = last used slot (-1: none)
	uint32_t mask[8];                     // bit per slot (256 max): stays on; all ones when nothing is dropped
	bool dropping;
};
inline GiPick gi_pick(const float* e, int count, int keep, int margin) {
	GiPick r{};
	r.highest = -1;
	if (count > 256) count = 256;
	float dist[256];
	for (int i = 0; i < count; i++) {
		const float* v = e + i * 24;
		dist[i] = -1;
		if (v[3] == 0) continue;
		r.used++;
		r.highest = i;
		float q[3] = {v[7], v[11], v[15]}, d2 = 0;
		for (int k = 0; k < 3; k++) {
			float h = v[20 + k] < 0 ? -v[20 + k] : v[20 + k];
			float o = q[k] > h ? q[k] - h : (q[k] < -h ? q[k] + h : 0);
			d2 += o * o;
		}
		dist[i] = d2;
	}
	r.dropping = keep < r.used;
	if (!r.dropping) {
		for (int w = 0; w < 8; w++) r.mask[w] = 0xffffffffu;
		r.kept = r.used;
		r.length = r.highest + 1 + margin;
	} else {
		int last = -1;
		for (int i = 0; i < count; i++) {
			if (dist[i] < 0) continue;
			int rank = 0;  // volumes nearer than this one (ties: lower slot first)
			for (int j = 0; j < count && rank < keep; j++)
				if (dist[j] >= 0 && (dist[j] < dist[i] || (dist[j] == dist[i] && j < i))) rank++;
			if (rank < keep) {
				r.mask[i >> 5] |= 1u << (i & 31);
				r.kept++;
				last = i;
			}
		}
		r.length = last + 1;
	}
	if (r.length < 1) r.length = 1;
	if (r.length > count) r.length = count;
	return r;
}

// Level reference for recentering: heading-only orientation whose forward is the horizontal
// part of the head's forward and whose up is the space's up. OpenXR's LOCAL space has +Y up, but
// Virtual Desktop through SteamVR gave one rolled 180 degrees (head 1.5 m *below* the origin, the
// game picture upside down). When the head's up points down -Y (*flipped stays as it was while the
// head is tilted sideways), the reference is rolled 180 degrees about forward so it is upright for the
// player. Returns false when the head looks straight up or down (keep the previous reference).
inline bool level_reference(Q head, bool* flipped, Q* out) {
	V3 up = rotate(head, {0, 1, 0});
	if (up.y < -0.5f) *flipped = true;
	else if (up.y > 0.5f) *flipped = false;
	Q yaw;
	if (!yaw_from_forward(rotate(head, {0, 0, -1}), &yaw)) return false;
	*out = *flipped ? q_mul(yaw, Q{0, 0, 1, 0}) : yaw;  // 180 deg about the forward (Z) axis
	return true;
}

// Headset-shaped resolutions (extra_resolutions.cpp): 50-100% of the size that gives the headset its full
// detail (fullW x fullH, saved from the last session), rounded to 8 pixels, from 640x480 to Direct3D
// 11's 16384, leaving out sizes the mode list already has. `listed` holds (w << 16 | h). Returns how
// many were written (at most kHeadsetScaleCount).
inline constexpr float kHeadsetScales[] = {0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
inline constexpr int32_t kHeadsetScaleCount = std::size(kHeadsetScales);
inline int headset_sizes(uint32_t fullW, uint32_t fullH, const uint32_t* listed, int nlisted, uint32_t* ow, uint32_t* oh) {
	int n = 0;
	for (float scale : kHeadsetScales) {
		uint32_t w = ((uint32_t)(fullW * scale + 4)) & ~7u, h = ((uint32_t)(fullH * scale + 4)) & ~7u;
		if (w < 640 || h < 480 || w > 16384 || h > 16384) continue;
		uint32_t key = (w & 0xffff) << 16 | (h & 0xffff);
		bool dup = false;
		for (int j = 0; j < nlisted && !dup; j++) dup = listed[j] == key;
		if (dup) continue;
		ow[n] = w;
		oh[n] = h;
		n++;
	}
	return n;
}

// Upscaling (upscale.cpp): the i-th number (i >= 1) of the Halton sequence in base b, in [0, 1).
// Bases 2 and 3 together spread the sub-pixel offsets evenly over a pixel.
inline float halton(uint32_t i, uint32_t b) {
	float f = 1, r = 0;
	for (; i > 0; i /= b) {
		f /= (float)b;
		r += f * (float)(i % b);
	}
	return r;
}

// How much each upscaler mode enlarges the image: DLAA, Quality, Balanced, Performance, Ultra
// performance (NVIDIA's ratios).
inline float upscale_ratio(int mode) {
	static constexpr float kR[5] = {1.0f, 1.5f, 1.7241f, 2.0f, 3.0f};
	return kR[mode < 0 ? 0 : (mode > 4 ? 4 : mode)];
}

// The mode a game image enlarged `ratio` times is in (the nearest of the ratios above, by their
// geometric middles): what games call the mode for that render size.
inline int upscale_mode(float ratio) {
	if (ratio < 1.22f) return 0;  // up to ~sqrt(1.0 * 1.5)
	if (ratio < 1.61f) return 1;  // sqrt(1.5 * 1.72)
	if (ratio < 1.86f) return 2;  // sqrt(1.72 * 2)
	if (ratio < 2.45f) return 3;  // sqrt(2 * 3)
	return 4;
}

// Output size, as games do it: the target size (the headset's own detail x OutputScale, the game's
// shape) stays put and the game's resolution is the input. Enlarges 1x to 3x (a game image larger
// than the target is only smoothed, DLAA), even pixels, at most 8192. False if too small to upscale.
inline bool upscale_output(uint32_t w, uint32_t h, uint32_t targetW, uint32_t targetH, uint32_t* ow, uint32_t* oh) {
	if (w < 64 || h < 64) return false;
	float r = 1;
	if (targetW && targetH) {
		float rw = (float)targetW / w, rh = (float)targetH / h;
		r = rw > rh ? rw : rh;
	}
	// Below the Quality band the upscaler's own mode is DLAA, which means "do not resize": asking for
	// it with an output larger than the input is a configuration NGX does not have, so the picture is
	// left at its own size and only cleaned up (2026-10-27: the mismatch reached the driver).
	if (r < 1.22f) r = 1;
	if (r > 3) r = 3;
	float big = (float)(w > h ? w : h) * r;
	if (big > 8192) r *= 8192 / big;
	if (r <= 1) {
		*ow = w, *oh = h;
		return true;
	}
	*ow = ((uint32_t)(w * r + 1)) & ~1u;
	*oh = ((uint32_t)(h * r + 1)) & ~1u;
	if (*ow < w) *ow = w;
	if (*oh < h) *oh = h;
	return true;
}

// The game resolution that gives `mode` for an output of ow x oh (rounded to 8 pixels).
inline void upscale_input_for(uint32_t ow, uint32_t oh, int mode, uint32_t* w, uint32_t* h) {
	float r = upscale_ratio(mode);
	*w = ((uint32_t)(ow / r + 4)) & ~7u;
	*h = ((uint32_t)(oh / r + 4)) & ~7u;
}

// Number of different jitter offsets for an enlargement (NVIDIA: 8 x ratio squared), 8 to 72.
inline uint32_t jitter_phases(float ratio) {
	float n = 8 * ratio * ratio;
	return n < 8 ? 8u : (n > 72 ? 72u : (uint32_t)(n + 0.5f));
}

// Texture identity (texture_tags.cpp): 3Dmigoto's texture hash, so the TextureOverride hashes of a 3Dmigoto /
// HelixMod fix name the same textures here. CRC32C (Castagnoli), appending to `crc` like 3Dmigoto's crc32c_hw.
inline uint32_t crc32c(uint32_t crc, const uint8_t* p, size_t n) {
	crc = ~crc;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0x82F63B78u & (0u - (crc & 1)));
	}
	return ~crc;
}
typedef uint32_t (*Crc32cFn)(uint32_t crc, const uint8_t* p, size_t n);

// Bytes per 4x4 block of a block-compressed DXGI format (BC1-BC7), 0 for any other format.
inline uint32_t bc_block_bytes(uint32_t dxgiFormat) {
	if ((dxgiFormat >= 70 && dxgiFormat <= 72) || (dxgiFormat >= 79 && dxgiFormat <= 81)) return 8;  // BC1, BC4
	if ((dxgiFormat >= 73 && dxgiFormat <= 78) || (dxgiFormat >= 82 && dxgiFormat <= 84)) return 16;  // BC2, BC3, BC5
	if (dxgiFormat >= 94 && dxgiFormat <= 99) return 16;                                               // BC6H, BC7
	return 0;
}

// 3Dmigoto's hash of a Texture2D created with initial data (ResourceHash.cpp, the default texture_hash=0):
// the first array slice's top mip, then the 44-byte D3D11_TEXTURE2D_DESC as the game passed it. Its v1.2.1 length
// (width x height x array size bytes) is used when it fits inside the top mip; otherwise each row of 4x4 blocks
// without the pitch padding (block-compressed only). False for the one case not reproduced here: an uncompressed
// texture array. (3Dmigoto also renames screen-sized widths and heights first; the textures this is for are not.)
inline bool migoto_texture_hash(const uint8_t desc[44], uint32_t width, uint32_t height, uint32_t arraySize, uint32_t format,
								const uint8_t* data, uint32_t rowPitch, Crc32cFn crc, uint32_t* out) {
	uint32_t hash = 0;
	uint32_t block = bc_block_bytes(format);
	size_t v12 = (size_t)width * height * arraySize;
	size_t rowBytes = block ? (size_t)((width + 3) / 4) * block : 0;
	size_t rows = block ? (height + 3) / 4 : height;
	size_t length = block ? rowBytes * rows : (size_t)rowPitch * height;
	if (v12 <= length) hash = crc(0, data, v12);
	else if (!block) return false;
	else
		for (size_t r = 0; r < rows; r++) hash = crc(hash, data + r * rowPitch, rowBytes < rowPitch ? rowBytes : rowPitch);
	*out = crc(hash, desc, 44);
	return true;
}

}  // namespace pure
