#include "core/pure.h"
#include <cstdio>
#include <cmath>
#include <initializer_list>
using namespace pure;
int fails = 0;
#define EXPECT(c)                                 \
	do {                                          \
		if (!(c)) {                               \
			printf("FAIL %d %s\n", __LINE__, #c); \
			fails++;                              \
		}                                         \
	} while (0)
int main() {
	for (float x = -50; x <= 50; x += 0.01f) {
		if (fabsf(fatan(x) - atanf(x)) > 2e-5f) {
			printf("atan %f %g\n", x, fatan(x) - atanf(x));
			fails++;
			break;
		}
	}
	// D3DX LH perspective, fov 60 deg, aspect 16:9, n=0.1 f=1000, row-major
	float fov = 60 * 3.14159265f / 180, asp = 16.f / 9, n = 0.1f, f = 1000;
	float sy = 1 / tanf(fov / 2), sx = sy / asp, A = f / (f - n), B = -n * f / (f - n);
	float rm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, A, 1, 0, 0, B, 0};
	ProjInfo p = {};
	EXPECT(classify_projection(rm, &p));
	EXPECT(p.rowMajor);
	EXPECT(fabsf(p.vfovDeg - 60) < 0.01f);
	EXPECT(fabsf(p.aspect - asp) < 1e-4f);
	EXPECT(p.wSign == 1);
	EXPECT(fabsf(-p.B / p.A - n) < 1e-4f);
	// transposed (column-major memory), RH reversed-Z infinite
	float cm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, 0, 0.1f, 0, 0, -1, 0};
	EXPECT(classify_projection(cm, &p));
	EXPECT(!p.rowMajor);
	EXPECT(p.wSign == -1);
	EXPECT(p.B == 0.1f);
	// identity and view matrices must not match
	float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	EXPECT(!classify_projection(id, &p));
	float ortho[16] = {0.01f, 0, 0, 0, 0, 0.02f, 0, 0, 0, 0, 0.001f, 0, 0, 0, 0.5f, 1};
	EXPECT(!classify_projection(ortho, &p));
	float zero[16] = {0};
	EXPECT(!classify_projection(zero, &p));
	// NieR's main camera widened for culling (0.3.20 broke stereo at 170 deg: sx 0.049 was rejected)
	for (float deg : {120.f, 160.f, 170.f}) {
		float wy = 1 / tanf(deg * 3.14159265f / 360), wx = wy / asp, a = -1.00001f, b = -0.100001f;
		float nm[16] = {wx, 0, 0, 0, 0, wy, 0, 0, 0, 0, a, -1, 0, 0, b, 0};
		EXPECT(classify_projection(nm, &p));
		EXPECT(fabsf(p.vfovDeg - deg) < 0.05f);
		EXPECT(fabsf(p.aspect - asp) < 1e-3f);
		EXPECT(p.wSign == -1);
	}
	printf(fails ? "%d FAILURES\n" : "projection tests passed\n", fails);
	return fails != 0;
}
