// pure::crc32c and pure::migoto_texture_hash (3Dmigoto's texture hash, texture_tags.cpp).
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
int main() {
	// the CRC-32C check value
	check(pure::crc32c(0, (const uint8_t*)"123456789", 9) == 0xE3069283u, "crc32c check value");
	// appending in two parts = one pass
	check(pure::crc32c(pure::crc32c(0, (const uint8_t*)"1234", 4), (const uint8_t*)"56789", 5) == 0xE3069283u, "crc32c append");

	check(pure::bc_block_bytes(71) == 8 && pure::bc_block_bytes(78) == 16 && pure::bc_block_bytes(98) == 16, "BC block sizes");
	check(pure::bc_block_bytes(28) == 0, "RGBA8 is not block-compressed");

	uint8_t desc[44];
	for (int i = 0; i < 44; i++) desc[i] = (uint8_t)i;
	std::vector<uint8_t> data(8 * 8 * 4 * 2);
	for (size_t i = 0; i < data.size(); i++) data[i] = (uint8_t)(i * 7 + 3);
	uint32_t h = 0;

	// BC3 8x8: 4 blocks of 16 bytes = 64 bytes = width x height, so the v1.2.1 length is the whole top mip
	check(pure::migoto_texture_hash(desc, 8, 8, 1, 77, data.data(), 32, pure::crc32c, &h), "BC3 hashed");
	check(h == pure::crc32c(pure::crc32c(0, data.data(), 64), desc, 44), "BC3: top mip, then the description");

	// BC1 8x8 (8-byte blocks): rows of 16 bytes; with a padded pitch of 24 only the 16 bytes of each row count
	check(pure::migoto_texture_hash(desc, 8, 8, 1, 71, data.data(), 24, pure::crc32c, &h), "BC1 hashed");
	uint32_t rows = pure::crc32c(pure::crc32c(0, data.data(), 16), data.data() + 24, 16);
	check(h == pure::crc32c(rows, desc, 44), "BC1: each row of blocks without its padding");

	// RGBA8 8x8, pitch 32: the v1.2.1 length is 64 bytes, not the 256 of the image
	check(pure::migoto_texture_hash(desc, 8, 8, 1, 28, data.data(), 32, pure::crc32c, &h), "RGBA8 hashed");
	check(h == pure::crc32c(pure::crc32c(0, data.data(), 64), desc, 44), "RGBA8: width x height bytes");

	// an uncompressed array longer than its first slice is the one case left out
	check(!pure::migoto_texture_hash(desc, 8, 8, 8, 28, data.data(), 32, pure::crc32c, &h), "RGBA8 array not reproduced");

	if (!fails) std::printf("texhash: all passed\n");
	return fails ? 1 : 0;
}
