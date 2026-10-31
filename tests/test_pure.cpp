#include "core/pure.h"
#include <cstdio>
#include <cstring>
#include <cmath>
using namespace pure;
static int fails = 0;
#define EXPECT_STR(got, want)                                                         \
	do {                                                                              \
		if (strcmp(got, want)) {                                                      \
			printf("FAIL %s:%d got [%s] want [%s]\n", __FILE__, __LINE__, got, want); \
			fails++;                                                                  \
		}                                                                             \
	} while (0)
#define EXPECT(c)                                              \
	do {                                                       \
		if (!(c)) {                                            \
			printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
			fails++;                                           \
		}                                                      \
	} while (0)
int main() {
	const char* js = "{\n \"file_format_version\": \"1.0.0\",\n \"runtime\" : {\n  \"name\":\"SteamVR\",\n  \"library_path\" : \"..\\\\bin\\\\win64\\\\vrclient_x64.dll\" ,\"functions\":{\"xrNegotiateLoaderRuntimeInterface\":\"myNeg\"}}}";
	char o[260];
	EXPECT(json_find_string(js, strlen(js), "library_path", o, sizeof o));
	EXPECT_STR(o, "..\\bin\\win64\\vrclient_x64.dll");
	EXPECT(json_find_string(js, strlen(js), "xrNegotiateLoaderRuntimeInterface", o, sizeof o));
	EXPECT_STR(o, "myNeg");
	EXPECT(!json_find_string(js, strlen(js), "nope", o, sizeof o));
	// yaw: for a set of angles, check yaw_from_forward reproduces forward direction
	for (int k = -17; k <= 18; k++) {
		float th = k * 0.1745329f * 1.0f;
		V3 fwd{-sinf(th) * 0.8f, 0.6f, -cosf(th) * 0.8f};
		Q q = {};
		EXPECT(yaw_from_forward(fwd, &q));
		V3 r = rotate(q, {0, 0, -1});
		float h = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
		EXPECT(fabsf(r.x - fwd.x / h) < 1e-4f && fabsf(r.z - fwd.z / h) < 1e-4f && fabsf(r.y) < 1e-5f);
		EXPECT(fabsf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1) < 1e-4f);
	}
	Q q = {};
	EXPECT(!yaw_from_forward({0, -1, 0}, &q));
	// rotate sanity: 90deg about Y maps -Z to -X
	Q r90{0, sinf(0.785398f), 0, cosf(0.785398f)};
	V3 v = rotate(r90, {0, 0, -1});
	EXPECT(fabsf(v.x + 1) < 1e-5f && fabsf(v.z) < 1e-5f);
	printf(fails ? "%d FAILURES\n" : "all pure tests passed\n", fails);
	return fails != 0;
}
