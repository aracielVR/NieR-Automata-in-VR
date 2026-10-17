"""Builds a synthetic capture with a known camera to test tools/analyze_capture.py."""
import math, struct, sys
def rec(t, payload): return struct.pack("<HHI", t, 0, len(payload)) + payload
def rdef_blob(cb_name, var_names_offsets, bind_point):
    # RDEF payload (SM 5.0)
    strings = b""; def_s = {}
    def s(x):
        nonlocal strings
        if x not in def_s: def_s[x] = len(strings); strings += x.encode() + b"\0"
        return def_s[x]
    for n in [cb_name] + [v for v, _, _ in var_names_offsets]: s(n)
    hdr = 28 + 32
    cb_off = hdr; var_off = cb_off + 24; bind_off = var_off + 40 * len(var_names_offsets); str_off = bind_off + 32
    r = struct.pack("<IIII", 1, cb_off, 1, bind_off) + bytes([0, 5]) + struct.pack("<HII", 0xFFFE, 0, 0) + b"RD11" + bytes(28)
    r += struct.pack("<IIIIII", str_off + def_s[cb_name], len(var_names_offsets), var_off, 256, 0, 0)
    for v, start, size in var_names_offsets:
        r += struct.pack("<IIIIIIIIII", str_off + def_s[v], start, size, 2, 0, 0, 0, 0, 0, 0)
    r += struct.pack("<IIIIIIII", str_off + def_s[cb_name], 0, 0, 0, 0, bind_point, 1, 0)
    r += strings
    chunk = b"RDEF" + struct.pack("<I", len(r)) + r
    total = 32 + 4 + len(chunk)
    return b"DXBC" + bytes(16) + struct.pack("<III", 1, total, 1) + struct.pack("<I", 36) + chunk
# camera: yaw 30 deg, at (10, 2, -5); D3DX LH, row-major
yaw = math.radians(30); cy, sy_ = math.cos(yaw), math.sin(yaw)
Rw = [[cy, 0, -sy_, 0], [0, 1, 0, 0], [sy_, 0, cy, 0], [10, 2, -5, 1]]   # camera world (row-vector)
def inv_rigid(M):
    R = [[M[j][i] for j in range(3)] for i in range(3)]
    t = M[3][:3]; it = [-sum(t[k] * R[k][j] for k in range(3)) for j in range(3)]
    return [R[0] + [0], R[1] + [0], R[2] + [0], it + [1]]
V = inv_rigid(Rw)
fov, asp, n, f = math.radians(70), 16 / 9, 0.1, 3000
syv = 1 / math.tan(fov / 2); sx = syv / asp
P = [[sx, 0, 0, 0], [0, syv, 0, 0], [0, 0, f / (f - n), 1], [0, 0, -n * f / (f - n), 0]]
mm = lambda a, b: [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
VP = mm(V, P)
flat = lambda M: struct.pack("<16f", *[x for r in M for x in r])
tr = lambda M: [[M[j][i] for j in range(4)] for i in range(4)]
cb = flat(tr(VP)) + flat(tr(V)) + flat(tr(P)) + bytes(256 - 192)   # column-major upload, like HLSL default
BUF, SH = 0x1000, 0x2000
out = b"NVRCAP01"
out += rec(1, struct.pack("<III", 1, 0, 0))
out += rec(6, struct.pack("<IIIIQQ", 1, 3840, 2160, 28, 0x9, 0x8))
out += rec(4, bytes([0]) + bytes(7) + struct.pack("<Q", SH))
out += rec(2, bytes([0, 0, 0, 0]) + struct.pack("<II", 0, 1) + struct.pack("<Q", BUF))
out += rec(3, struct.pack("<QIB", BUF, 256, 0) + bytes(3) + cb)
for i in range(10): out += rec(5, bytes([1, 0, 0, 0]) + struct.pack("<III", 36, 0, 0))
blob = rdef_blob("CameraCB", [("g_ViewProj", 0, 64), ("g_View", 64, 64), ("g_Proj", 128, 64)], 0)
out += rec(8, bytes([0]) + bytes(7) + struct.pack("<QQ", SH, 0xabc) + blob)
out += rec(12, struct.pack("<III", 1, 0, 0))
open(sys.argv[1], "wb").write(out)
