#include "core/pure.h"
#include <cstdio>
#include <cmath>
int main() {
	int f = 0;
	for (float x = -1.55f; x <= 1.55f; x += 0.0005f) {
		float a = pure::ftan(x), b = tanf(x);
		if (fabsf(a - b) > 2e-6f * fmaxf(1.f, fabsf(b))) {
			printf("tan %f: %g vs %g\n", x, a, b);
			f++;
			break;
		}
	}
	for (float x = -1.5f; x <= 1.5f; x += 0.001f) {
		if (fabsf(pure::fatan(pure::ftan(x)) - x) > 3e-5f) {
			printf("roundtrip %f\n", x);
			f++;
			break;
		}
	}
	printf(f ? "tan FAIL\n" : "tan tests passed\n");
	return f;
}
