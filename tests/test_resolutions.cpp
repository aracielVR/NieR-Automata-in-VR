// Extra resolutions (pure::supersample_sizes): based on the desktop size, no duplicates, D3D11 limit.
#include <cstdio>
#include "pure.h"
static int fails;
static void check(bool c, const char* w) { if (!c) { std::printf("FAIL %s\n", w); fails++; } }
static uint32_t key(uint32_t w, uint32_t h) { return w << 16 | h; }
int main() {
    uint32_t w[6], h[6];
    // 4K monitor, no DSR: 125..300%
    uint32_t plain[] = {key(1920, 1080), key(2560, 1440), key(3840, 2160)};
    int n = pure::supersample_sizes(3840, 2160, plain, 3, w, h);
    check(n == 6 && w[0] == 4800 && h[0] == 2704 && w[3] == 7680 && w[5] == 11520 && h[5] == 6480, "4K: 4800x2704 .. 11520x6480");
    // 4K monitor with DSR (the 0.5.3 user log): the list has 5120x2880 and 5760x3240; 4800x2704 must be offered
    uint32_t dsr[] = {key(3840, 2160), key(5120, 2880), key(5760, 3240)};
    n = pure::supersample_sizes(3840, 2160, dsr, 3, w, h);
    check(n == 5 && w[0] == 4800 && h[0] == 2704, "4K + DSR: 4800x2704 is back");
    bool dup = false;
    for (int i = 0; i < n; i++) dup |= w[i] == 5760 && h[i] == 3240;
    check(!dup, "4K + DSR: 5760x3240 (already listed by DSR) not added twice");
    // 1080p monitor: up to 5760x3240
    uint32_t p1080[] = {key(1280, 720), key(1920, 1080)};
    n = pure::supersample_sizes(1920, 1080, p1080, 2, w, h);
    check(n == 6 && w[5] == 5760 && h[5] == 3240, "1080p: up to 5760x3240");
    n = pure::supersample_sizes(1920, 1080, plain, 3, w, h);
    check(n == 5, "1080p: 3840x2160 (200%) already listed is not added twice");
    // very large desktop: sizes above 16384 left out
    n = pure::supersample_sizes(7680, 4320, plain, 3, w, h);
    check(n == 4 && w[3] == 15360, "8K: only sizes within 16384");
    if (!fails) std::printf("resolution tests passed\n");
    return fails ? 1 : 0;
}
