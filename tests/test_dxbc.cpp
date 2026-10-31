// pure::dxbc_sampled_textures on a hand-built shader container.
#include <cstdio>
#include <cstring>
#include <vector>
#include "core/pure.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
static uint32_t opnd(uint32_t type, uint32_t ncomp = 2, uint32_t mode = 1) {
	return ncomp | mode << 2 | (mode == 1 ? 0xE4u << 4 : 0xFu << 4) | type << 12 | 1u << 20;  // 1-D, imm32 index
}
static std::vector<uint8_t> container(const std::vector<uint32_t>& code) {
	std::vector<uint32_t> shex = {0x00050050u, 0};  // ps_5_0, length
	shex.insert(shex.end(), code.begin(), code.end());
	shex[1] = (uint32_t)shex.size();
	std::vector<uint8_t> b(36 + 8 + shex.size() * 4, 0);
	std::memcpy(&b[0], "DXBC", 4);
	uint32_t one = 1, total = (uint32_t)b.size(), chunks = 1, off = 36, size = (uint32_t)shex.size() * 4;
	std::memcpy(&b[20], &one, 4);
	std::memcpy(&b[24], &total, 4);
	std::memcpy(&b[28], &chunks, 4);
	std::memcpy(&b[32], &off, 4);
	std::memcpy(&b[36], "SHEX", 4);
	std::memcpy(&b[40], &size, 4);
	std::memcpy(&b[44], shex.data(), size);
	return b;
}
int main() {
	auto inst = [](uint32_t op, std::vector<uint32_t> ops) {
		std::vector<uint32_t> v = {op | (uint32_t)(ops.size() + 1) << 24};
		v.insert(v.end(), ops.begin(), ops.end());
		return v;
	};
	std::vector<uint32_t> code;
	auto add = [&](std::vector<uint32_t> v) { code.insert(code.end(), v.begin(), v.end()); };
	// sample r0, v1, t10, s0
	add(inst(69, {opnd(0, 2, 0), 0, opnd(1), 1, opnd(7), 10, opnd(6), 0}));
	// ld_structured r1, l(0), l(0), t30  (a buffer: not counted)
	add(inst(167, {opnd(0, 2, 0), 1, opnd(7), 0, opnd(7), 0, opnd(7), 30}));
	// resinfo r2, l(0), t0 (size query: not counted)
	add(inst(61, {opnd(0, 2, 0), 2, opnd(7), 0}));
	// sample_l r3, v1, t2, s1, r0.x
	add(inst(72, {opnd(0, 2, 0), 3, opnd(1), 1, opnd(7), 2, opnd(6), 1, opnd(0, 2, 2), 0}));
	add({0x0100003eu});  // ret
	auto b = container(code);
	uint32_t m = pure::dxbc_sampled_textures(b.data(), b.size());
	check(m == ((1u << 10) | (1u << 2)), "samples t10 and t2 only");
	check(pure::dxbc_sampled_textures(b.data(), 20) == 0, "truncated blob");
	std::printf(fails ? "dxbc tests FAILED\n" : "dxbc tests passed\n");
	return fails != 0;
}
