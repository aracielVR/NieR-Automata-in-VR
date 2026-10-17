// pure.h - platform-independent helpers (string formatting, parsing, small vector math).
// No CRT and no Windows dependencies, so this file is also unit-tested on the build host.
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace pure {

// ---------------------------------------------------------------------------------------------
// Minimal printf-style formatter.
// Supports: %d %i %u %x %X %p %s %S(wide) %c %f %%   flags: - 0   width   .precision
// length modifiers: l ll z I64 (all integers are read as 64-bit when ll/z/I64 is given)
// ---------------------------------------------------------------------------------------------
struct Out {
    char* buf;
    size_t cap;
    size_t len;
    void put(char c) {
        if (len + 1 < cap) buf[len] = c;
        len++;
    }
};

inline void put_padded(Out& o, const char* s, size_t n, int width, bool left, char pad) {
    int fill = width > (int)n ? width - (int)n : 0;
    if (!left) {
        // keep a leading '-' in front of zero padding
        if (pad == '0' && n > 0 && s[0] == '-') {
            o.put('-');
            s++;
            n--;
        }
        for (int i = 0; i < fill; i++) o.put(pad);
    }
    for (size_t i = 0; i < n; i++) o.put(s[i]);
    if (left)
        for (int i = 0; i < fill; i++) o.put(' ');
}

inline size_t utoa_rev(uint64_t v, unsigned base, bool upper, char* tmp) {
    const char* digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t n = 0;
    do {
        tmp[n++] = digs[v % base];
        v /= base;
    } while (v);
    return n;
}

inline void fmt_uint(Out& o, uint64_t v, unsigned base, bool upper, bool neg, int width, bool left, char pad) {
    char tmp[32];
    char s[34];
    size_t n = utoa_rev(v, base, upper, tmp);
    size_t k = 0;
    if (neg) s[k++] = '-';
    while (n) s[k++] = tmp[--n];
    put_padded(o, s, k, width, left, pad);
}

inline void fmt_double(Out& o, double v, int prec, int width, bool left, char pad) {
    char s[64];
    size_t k = 0;
    if (v != v) {
        put_padded(o, "nan", 3, width, left, ' ');
        return;
    }
    if (v < 0) {
        s[k++] = '-';
        v = -v;
    }
    if (v > 1e15) {
        const char* inf = (v > 1.7e308) ? "inf" : "1e15+";
        for (const char* p = inf; *p; p++) s[k++] = *p;
        put_padded(o, s, k, width, left, ' ');
        return;
    }
    if (prec < 0) prec = 4;
    if (prec > 9) prec = 9;
    uint64_t scale = 1;
    for (int i = 0; i < prec; i++) scale *= 10;
    uint64_t ip = (uint64_t)v;
    double frac = v - (double)ip;
    uint64_t fp = (uint64_t)(frac * (double)scale + 0.5);
    if (fp >= scale) {
        ip += 1;
        fp -= scale;
    }
    char tmp[32];
    size_t n = utoa_rev(ip, 10, false, tmp);
    while (n) s[k++] = tmp[--n];
    if (prec > 0) {
        s[k++] = '.';
        n = utoa_rev(fp, 10, false, tmp);
        for (int i = (int)n; i < prec; i++) s[k++] = '0';
        while (n) s[k++] = tmp[--n];
    }
    put_padded(o, s, k, width, left, pad);
}

// Encodes one UTF-16 (or UTF-32 on the host) code unit sequence into UTF-8.
template <typename WChar>
inline void put_wide(Out& o, const WChar* w, int width, bool left) {
    char tmp[1024];
    size_t k = 0;
    for (; *w && k + 4 < sizeof(tmp); w++) {
        uint32_t c = (uint32_t)*w;
        if (sizeof(WChar) == 2 && c >= 0xD800 && c <= 0xDBFF && w[1] >= 0xDC00 && w[1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + ((uint32_t)w[1] - 0xDC00);
            w++;
        }
        if (c < 0x80) {
            tmp[k++] = (char)c;
        } else if (c < 0x800) {
            tmp[k++] = (char)(0xC0 | (c >> 6));
            tmp[k++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            tmp[k++] = (char)(0xE0 | (c >> 12));
            tmp[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            tmp[k++] = (char)(0xF0 | (c >> 18));
            tmp[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
            tmp[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    put_padded(o, tmp, k, width, left, ' ');
}

inline int vformat(char* buf, size_t cap, const char* f, va_list ap) {
    Out o{buf, cap, 0};
    for (; *f; f++) {
        if (*f != '%') {
            o.put(*f);
            continue;
        }
        f++;
        if (!*f) break;
        bool left = false;
        char pad = ' ';
        for (;; f++) {
            if (*f == '-') left = true;
            else if (*f == '0') pad = '0';
            else break;
        }
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int prec = -1;
        if (*f == '.') {
            f++;
            prec = 0;
            while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        int lng = 0;  // 0 = int, 1 = long, 2 = 64-bit
        for (;;) {
            if (*f == 'l') {
                lng++;
                f++;
            } else if (*f == 'z') {
                lng = 2;
                f++;
            } else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') {
                lng = 2;
                f += 3;
            } else
                break;
        }
        if (left) pad = ' ';
        switch (*f) {
            case 'd':
            case 'i': {
                int64_t v = lng >= 2 ? va_arg(ap, long long) : (lng == 1 ? (int64_t)va_arg(ap, long) : (int64_t)va_arg(ap, int));
                bool neg = v < 0;
                uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
                fmt_uint(o, u, 10, false, neg, width, left, pad);
                break;
            }
            case 'u':
            case 'x':
            case 'X': {
                uint64_t v = lng >= 2 ? va_arg(ap, unsigned long long)
                                      : (lng == 1 ? (uint64_t)va_arg(ap, unsigned long) : (uint64_t)va_arg(ap, unsigned int));
                fmt_uint(o, v, *f == 'u' ? 10 : 16, *f == 'X', false, width, left, pad);
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void*);
                o.put('0');
                o.put('x');
                fmt_uint(o, v, 16, true, false, 16, false, '0');
                break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                size_t n = 0;
                while (s[n] && (prec < 0 || (int)n < prec)) n++;
                put_padded(o, s, n, width, left, ' ');
                break;
            }
            case 'S': {
                const wchar_t* w = va_arg(ap, const wchar_t*);
                if (!w) w = L"(null)";
                put_wide(o, w, width, left);
                break;
            }
            case 'c':
                o.put((char)va_arg(ap, int));
                break;
            case 'f':
                fmt_double(o, va_arg(ap, double), prec, width, left, pad);
                break;
            case '%':
                o.put('%');
                break;
            default:
                o.put('%');
                o.put(*f);
                break;
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

inline int format(char* buf, size_t cap, const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vformat(buf, cap, f, ap);
    va_end(ap);
    return r;
}

// ---------------------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------------------
template <typename Ch>
inline bool parse_float(const Ch* s, float* out) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    bool neg = false;
    if (*s == '-' || *s == '+') neg = (*s++ == '-');
    double v = 0;
    bool any = false;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        any = true;
    }
    if (*s == '.') {
        s++;
        double sc = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (*s++ - '0') * sc;
            sc *= 0.1;
            any = true;
        }
    }
    if (!any) return false;
    *out = (float)(neg ? -v : v);
    return true;
}

template <typename Ch>
inline bool parse_int(const Ch* s, int* out) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    bool neg = false;
    if (*s == '-' || *s == '+') neg = (*s++ == '-');
    int v = 0;
    bool any = false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (;; s++) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = v * 16 + d;
            any = true;
        }
    } else {
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            any = true;
        }
    }
    if (!any) return false;
    *out = neg ? -v : v;
    return true;
}

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

// ---------------------------------------------------------------------------------------------
// Math (OpenXR conventions: right-handed, +Y up, -Z forward)
// ---------------------------------------------------------------------------------------------
struct V3 {
    float x, y, z;
};
struct Q {
    float x, y, z, w;
};

inline bool near0(float v, float eps = 1e-4f) { return v > -eps && v < eps; }
inline float fsqrt(float v) { return v <= 0 ? 0 : __builtin_sqrtf(v); }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 rotate(Q q, V3 v) {
    V3 u{q.x, q.y, q.z};
    V3 t = cross(u, v);
    t = {t.x + q.w * v.x, t.y + q.w * v.y, t.z + q.w * v.z};
    V3 c = cross(u, t);
    return {v.x + 2 * c.x, v.y + 2 * c.y, v.z + 2 * c.z};
}

// Yaw-only rotation (about +Y) whose forward (-Z) points along the horizontal part of `fwd`.
// Returns false if `fwd` is (nearly) vertical.
inline bool yaw_from_forward(V3 fwd, Q* out) {
    float h = fsqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    if (h < 1e-4f) return false;
    float c = -fwd.z / h;  // cos(theta)
    float s = -fwd.x / h;  // sin(theta)
    float hw = fsqrt((1 + c) * 0.5f);
    float hy = fsqrt((1 - c) * 0.5f);
    if (s < 0) hy = -hy;
    *out = {0, hy, 0, hw};
    return true;
}

// ---------------------------------------------------------------------------------------------
// 4x4 matrices in NieR's convention: column vectors (p' = M p), stored row by row
// (m[r*4+c]); translation in m[3], m[7], m[11]. Right-handed, +Y up, camera looks down -Z,
// which matches OpenXR's conventions.
// ---------------------------------------------------------------------------------------------
struct M4 {
    float m[16];
    float& at(int r, int c) { return m[r * 4 + c]; }
    float at(int r, int c) const { return m[r * 4 + c]; }
};

inline M4 m4_identity() {
    M4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}

inline M4 m4_mul(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.at(i, k) * b.at(k, j);
            r.at(i, j) = s;
        }
    return r;
}

// Rotation + translation from a pose (OpenXR quaternion x,y,z,w).
inline M4 m4_from_pose(Q q, V3 p) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    M4 r = m4_identity();
    r.at(0, 0) = 1 - 2 * (yy + zz);
    r.at(0, 1) = 2 * (xy - wz);
    r.at(0, 2) = 2 * (xz + wy);
    r.at(1, 0) = 2 * (xy + wz);
    r.at(1, 1) = 1 - 2 * (xx + zz);
    r.at(1, 2) = 2 * (yz - wx);
    r.at(2, 0) = 2 * (xz - wy);
    r.at(2, 1) = 2 * (yz + wx);
    r.at(2, 2) = 1 - 2 * (xx + yy);
    r.at(0, 3) = p.x;
    r.at(1, 3) = p.y;
    r.at(2, 3) = p.z;
    return r;
}

// Unit quaternion of the rotation part of m (Shepperd's method; m need not be exactly orthonormal).
inline Q q_from_m4(const M4& m) {
    float t = m.at(0, 0) + m.at(1, 1) + m.at(2, 2);
    Q q;
    if (t > 0) {
        float s = fsqrt(t + 1) * 2;
        q = {(m.at(2, 1) - m.at(1, 2)) / s, (m.at(0, 2) - m.at(2, 0)) / s, (m.at(1, 0) - m.at(0, 1)) / s, s / 4};
    } else if (m.at(0, 0) > m.at(1, 1) && m.at(0, 0) > m.at(2, 2)) {
        float s = fsqrt(1 + m.at(0, 0) - m.at(1, 1) - m.at(2, 2)) * 2;
        q = {s / 4, (m.at(0, 1) + m.at(1, 0)) / s, (m.at(0, 2) + m.at(2, 0)) / s, (m.at(2, 1) - m.at(1, 2)) / s};
    } else if (m.at(1, 1) > m.at(2, 2)) {
        float s = fsqrt(1 + m.at(1, 1) - m.at(0, 0) - m.at(2, 2)) * 2;
        q = {(m.at(0, 1) + m.at(1, 0)) / s, s / 4, (m.at(1, 2) + m.at(2, 1)) / s, (m.at(0, 2) - m.at(2, 0)) / s};
    } else {
        float s = fsqrt(1 + m.at(2, 2) - m.at(0, 0) - m.at(1, 1)) * 2;
        q = {(m.at(0, 2) + m.at(2, 0)) / s, (m.at(1, 2) + m.at(2, 1)) / s, s / 4, (m.at(1, 0) - m.at(0, 1)) / s};
    }
    float n = fsqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 0) q = {q.x / n, q.y / n, q.z / n, q.w / n};
    return q;
}

// Inverse of a rotation+translation matrix.
inline M4 m4_rigid_inverse(const M4& a) {
    M4 r = m4_identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.at(i, j) = a.at(j, i);
    for (int i = 0; i < 3; i++) r.at(i, 3) = -(r.at(i, 0) * a.at(0, 3) + r.at(i, 1) * a.at(1, 3) + r.at(i, 2) * a.at(2, 3));
    return r;
}

inline bool m4_is_rigid(const M4& a, float eps = 2e-3f) {
    if (!near0(a.at(3, 0), eps) || !near0(a.at(3, 1), eps) || !near0(a.at(3, 2), eps) || !near0(a.at(3, 3) - 1, eps))
        return false;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float d = a.at(0, i) * a.at(0, j) + a.at(1, i) * a.at(1, j) + a.at(2, i) * a.at(2, j);
            if (!near0(d - (i == j ? 1.f : 0.f), eps)) return false;
        }
    return true;
}

// Camera-to-world with pitch and roll removed (yaw kept, position kept): a level horizon.
inline M4 m4_level_camera(const M4& cam) {
    V3 fwd{-cam.at(0, 2), -cam.at(1, 2), -cam.at(2, 2)};
    Q yaw;
    if (!yaw_from_forward(fwd, &yaw)) {
        // looking straight up/down: derive yaw from the camera's up vector instead
        V3 up{cam.at(0, 1), cam.at(1, 1), cam.at(2, 1)};
        V3 f2 = fwd.y < 0 ? up : V3{-up.x, -up.y, -up.z};
        if (!yaw_from_forward(f2, &yaw)) yaw = Q{0, 0, 0, 1};
    }
    return m4_from_pose(yaw, {cam.at(0, 3), cam.at(1, 3), cam.at(2, 3)});
}

// Symmetric perspective with the given half-extent tangents, keeping the game's depth mapping
// (row 2) and right-handed w = -z (row 3).
inline M4 m4_symmetric_projection(const M4& game, float tanHalfW, float tanHalfH) {
    M4 r = {};
    r.at(0, 0) = 1 / tanHalfW;
    r.at(1, 1) = 1 / tanHalfH;
    r.at(2, 2) = game.at(2, 2);
    r.at(2, 3) = game.at(2, 3);
    r.at(3, 2) = game.at(3, 2);
    return r;
}

// Pose of `eye` relative to `ref` (both in the same space): inverse(ref) * eye.
inline M4 m4_relative(const M4& ref, const M4& eye) { return m4_mul(m4_rigid_inverse(ref), eye); }

// Head-direction culling (0.3.22): a LookAt(eye, target, up) turned the way the headset looks.
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

// Smallest symmetric tangent box with aspect `aspect` (= width/height) that contains the
// (possibly asymmetric, canted-display) eye field of view given as tangents l<0<r, d<0<u.
inline void cover_fov(float l, float r, float u, float d, float aspect, float* halfW, float* halfH) {
    float hw = -l > r ? -l : r;
    float hh = u > -d ? u : -d;
    if (hw / hh < aspect)
        hw = hh * aspect;
    else
        hh = hw / aspect;
    *halfW = hw;
    *halfH = hh;
}

// tan for |x| < pi/2 via Taylor series of sin and cos (|x| <= 1.57: rel. error < 1e-6).
inline float ftan(float xf) {
    double x = xf, x2 = x * x;
    double s = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156 * (1 - x2 / 210)))))));
    double c = 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132 * (1 - x2 / 182 * (1 - x2 / 240)))))));
    return (float)(s / c);
}

// atan approximation (max error ~1e-5 rad), avoids needing a math library.
inline float fatan(float x) {
    bool neg = x < 0;
    if (neg) x = -x;
    bool inv = x > 1;
    if (inv) x = 1 / x;
    // minimax polynomial on [0,1]
    float x2 = x * x;
    float r = x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f + x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
    if (inv) r = 1.5707963f - r;
    return neg ? -r : r;
}

// sin and cos for |x| <= pi/2 (Taylor series, rel. error < 1e-6).
inline void fsincos(float xf, float* sn, float* cs) {
    double x = xf, x2 = x * x;
    *sn = (float)(x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156)))))));
    *cs = (float)(1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132 * (1 - x2 / 182)))))));
}

inline Q q_mul(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Eye sync (0.3.25). Alternate-eye rendering: the older eye's image was rendered with the game
// camera as it was a game frame earlier. qwOld/qwNew map tracking (LOCAL) orientations to the game
// world for the two frames (world = qw * tracking). Shown with the pose returned here, the older
// image lines up with the newer camera's orientation: the runtime's reprojection turns it by the
// camera's turn since. Returns false (pose unchanged) when the turn is larger than maxDeg (a cut).
inline bool eye_sync_rotation(Q qwOld, Q qwNew, Q eyeOld, float maxDeg, Q* out) {
    Q inv{-qwNew.x, -qwNew.y, -qwNew.z, qwNew.w};
    Q d = q_mul(inv, qwOld);
    float w = d.w < 0 ? -d.w : d.w;
    if (w > 1) w = 1;
    float sinHalf, cosHalf;
    fsincos(maxDeg * 0.5f * 0.0174533f, &sinHalf, &cosHalf);
    if (w < cosHalf) return false;
    *out = q_mul(d, eyeOld);
    return true;
}
// Angle (degrees) of the rotation between two unit quaternions.
inline float q_angle_deg(Q a, Q b) {
    float w = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    w = w < 0 ? -w : w;
    if (w >= 1) return 0;
    if (w < 1e-6f) return 180;
    return 2 * fatan(fsqrt(1 - w * w) / w) * 57.29578f;  
}

// Camera levelling that adapts to steep views. Pitch and roll are removed while the camera looks
// roughly horizontally (|pitch| <= lo degrees); above that more and more of the pitch is kept, all
// of it from hi degrees (top-down sections: the arena then lies in front of you instead of at your
// feet). The heading comes from forward and up together, so it stays stable even when the camera
// looks straight down. *pitchDeg / *kept report the camera pitch and the fraction kept.
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

// Pitch of a camera-to-world matrix's line of sight (-z), degrees: negative = looking down.
inline float camera_pitch_deg(const M4& cam) {
    float fx = -cam.at(0, 2), fy = -cam.at(1, 2), fz = -cam.at(2, 2);
    float h = fsqrt(fx * fx + fz * fz);
    float pitch = h > 1e-6f ? fatan(fy / h) : (fy > 0 ? 1.5707963f : -1.5707963f);
    return pitch * 57.29578f;
}

inline M4 m4_level_camera_adaptive(const M4& cam, float lo, float hi, float* pitchDeg, float* kept) {
    V3 fwd{-cam.at(0, 2), -cam.at(1, 2), -cam.at(2, 2)};
    V3 up{cam.at(0, 1), cam.at(1, 1), cam.at(2, 1)};
    float h = fsqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    float pitch = h > 1e-6f ? fatan(fwd.y / h) : (fwd.y > 0 ? 1.5707963f : -1.5707963f);
    float sgn = pitch >= 0 ? 1.0f : -1.0f;
    V3 dir{fwd.x - sgn * up.x, fwd.y - sgn * up.y, fwd.z - sgn * up.z};
    Q yaw;
    if (!yaw_from_forward(dir, &yaw) && !yaw_from_forward(fwd, &yaw)) yaw = Q{0, 0, 0, 1};
    float deg = pitch * 57.29578f, a = deg < 0 ? -deg : deg;
    float t = hi > lo ? (a - lo) / (hi - lo) : (a >= hi ? 1.0f : 0.0f);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float k = t * t * (3 - 2 * t);  // smoothstep
    float sn, cs;
    fsincos(pitch * k * 0.5f, &sn, &cs);
    Q q = q_mul(yaw, Q{sn, 0, 0, cs});
    if (pitchDeg) *pitchDeg = deg;
    if (kept) *kept = k;
    return m4_from_pose(q, {cam.at(0, 3), cam.at(1, 3), cam.at(2, 3)});
}

// ---------------------------------------------------------------------------------------------
// Perspective projection detection (camera probe)
// m is 16 consecutive floats as found in a constant buffer.
//   row-major  (mul(v, M)):  w comes from column 3:  m[3]=m[7]=m[15]=0, m[11]=+-1
//   column-major (mul(M, v)): w comes from row 3:    m[12]=m[13]=m[15]=0, m[14]=+-1
// ---------------------------------------------------------------------------------------------
struct ProjInfo {
    bool rowMajor;
    float sx, sy;       // focal scales: 1/tan(half fov)
    float cx, cy;       // off-centre terms
    float A, B;         // depth mapping: z_ndc = A + B / z_view (w = +-z)
    float wSign;        // +1 left-handed (camera looks down +z), -1 right-handed
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

// VR controllers as a gamepad (vrinput.cpp, 0.4.0): the left menu button. A tap (released within
// 0.5 s, stick not used) gives Start for 120 ms; holding it gives Back after 0.5 s; while it is held
// the left stick is the D-pad (and the stick itself reads neutral). Bits as XInput's.
struct MenuButton {
    uint32_t since = 0, startUntil = 0;
    bool usedForDpad = false;
    enum : uint16_t { UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8, START = 0x10, BACK = 0x20 };
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

// GI volumes (viewfix.cpp, 0.4.3). The game's probe-selection pass loops over g_CubeInput (96-byte
// entries = 24 floats: a = position (a.w != 0: slot in use), b/c/d = rows of view -> box space,
// f.xyz = half extents) and writes the chosen *slot numbers* per pixel; later shaders sample
// g_CubeTexArray at those slots, so slots must never move. This picks, from a CPU copy of the
// buffer, which slots stay on: all used ones (keep >= used) or the `keep` nearest to the camera
// (the view-space origin; distance to the box like the game's per-pixel test). Returns the loop
// length the game's pass needs: one past the last slot left on (plus `margin` when nothing is
// dropped, for volumes that appear before the next read-back), at least 1, at most count.
struct GiPick {
    int used, highest, kept, length;  // highest = last used slot (-1: none)
    uint32_t mask[8];                 // bit per slot (256 max): stays on; all ones when nothing is dropped
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

// Level reference for recentering (0.5.2): heading-only orientation whose forward is the horizontal
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

// Extra resolutions (display.cpp, 0.5.4): 125-300% of the monitor's desktop size, rounded to 8
// pixels, within Direct3D 11's 16384 limit, leaving out sizes the mode list already has (NVIDIA
// DSR/DLDSR modes). `listed` holds (w << 16 | h). Returns how many sizes were written (max 6).
inline int supersample_sizes(uint32_t bw, uint32_t bh, const uint32_t* listed, int nlisted, uint32_t* ow, uint32_t* oh) {
    static const float kF[6] = {1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
    int n = 0;
    for (int k = 0; k < 6; k++) {
        uint32_t w = ((uint32_t)(bw * kF[k] + 4)) & ~7u, h = ((uint32_t)(bh * kF[k] + 4)) & ~7u;
        if (w > 16384 || h > 16384) break;
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

}  // namespace pure
