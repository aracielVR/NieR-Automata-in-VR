// crt.cpp - the few C runtime symbols the compiler may emit calls to.
// The mod is built without a C runtime so it has no dependency on any VC++ redistributable.
#include <stddef.h>

extern "C" {

void* memcpy(void* dst, const void* src, size_t n) {
    void* d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    // overlapping, copy backwards
    d += n - 1;
    s += n - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
        __asm__ volatile("" ::: "memory");  // keep this a plain loop
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) {
        n++;
        __asm__ volatile("" ::: "memory");
    }
    return n;
}

// Required by the MinGW headers' inline helpers / C++ ABI when no CRT is linked.
int _fltused = 0;
void __cxa_pure_virtual() { __builtin_trap(); }
}

extern "C" int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
extern "C" char* strstr(const char* h, const char* n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) a++, b++;
        if (!*b) return (char*)h;
    }
    return nullptr;
}
