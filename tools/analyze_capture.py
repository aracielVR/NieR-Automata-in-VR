#!/usr/bin/env python3
"""Analyse a NieRVR camera-probe capture (NieRVR_capture_N.bin).

Finds the camera: perspective projection matrices, view matrices (rigid transforms) and
view-projection products in the frame's constant buffers, which shaders/draws use them, and,
when the shaders carry reflection data, the names the game gives those cbuffer variables.

Usage: analyze_capture.py capture.bin [--draws N] [--dump-buffer PTR]
"""
import argparse
import collections
import math
import struct
import sys

R_FRAME_BEGIN, R_SET_CB, R_CB_DATA, R_SET_SHADER, R_DRAW, R_SET_RT, R_VIEWPORT, R_SHADER_BLOB, R_DISPATCH, \
    R_EXEC_CMDLIST, R_FRAME_END, R_SET_SRV, R_SET_UAV, R_TEX_HASH, R_BUF_DATA = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16
STAGES = ["VS", "PS", "GS", "CS"]
DRAW_KINDS = ["Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced"]
VIA = ["map", "update", "readback"]


# ---------------------------------------------------------------------------------------------
# parsing
# ---------------------------------------------------------------------------------------------
def records(data):
    if data[:8] != b"NVRCAP01":
        sys.exit("not a NieRVR capture")
    off = 8
    while off + 8 <= len(data):
        typ, _pad, size = struct.unpack_from("<HHI", data, off)
        off += 8
        if off + size > len(data):
            print(f"warning: truncated record at {off}")
            return
        yield typ, data[off:off + size]
        off += size


class Event:
    __slots__ = ("kind", "args")

    def __init__(self, kind, **args):
        self.kind, self.args = kind, args


def parse(data):
    events, blobs = [], {}
    for typ, p in records(data):
        if typ == R_SET_CB:
            stage, start, count = p[0], *struct.unpack_from("<II", p, 4)
            bufs = struct.unpack_from(f"<{count}Q", p, 12)
            events.append(Event("setcb", stage=stage, start=start, bufs=bufs))
        elif typ == R_CB_DATA:
            buf, width, via = struct.unpack_from("<QIB", p, 0)
            events.append(Event("cbdata", buf=buf, width=width, via=via, data=bytes(p[16:])))
        elif typ == R_SET_SHADER:
            events.append(Event("setshader", stage=p[0], shader=struct.unpack_from("<Q", p, 8)[0]))
        elif typ == R_DRAW:
            kind, a, b, c = p[0], *struct.unpack_from("<III", p, 4)
            events.append(Event("draw", dkind=kind, a=a, b=b, c=c))
        elif typ == R_SET_RT:
            n, w, h, fmt, dsv, rtv0 = struct.unpack_from("<IIIIQQ", p, 0)
            events.append(Event("setrt", n=n, w=w, h=h, fmt=fmt, dsv=dsv, rtv0=rtv0))
        elif typ == R_VIEWPORT:
            n = struct.unpack_from("<I", p, 0)[0]
            vps = [struct.unpack_from("<6f", p, 4 + 24 * i) for i in range(n)]
            events.append(Event("viewport", vps=vps))
        elif typ == R_SHADER_BLOB:
            stage = p[0]
            sh, h = struct.unpack_from("<QQ", p, 8)
            blobs[sh] = (stage, h, bytes(p[24:]))
        elif typ == R_DISPATCH:
            events.append(Event("dispatch", xyz=struct.unpack_from("<III", p, 0)))
        elif typ == R_EXEC_CMDLIST:
            events.append(Event("execcl"))
        elif typ in (R_SET_SRV, R_SET_UAV):
            stage, start, count = p[0], *struct.unpack_from("<II", p, 4)
            views = [struct.unpack_from("<QIIII", p, 12 + 24 * i) for i in range(count)]
            events.append(Event("setsrv" if typ == R_SET_SRV else "setuav", stage=stage, start=start, views=views))
        elif typ == R_BUF_DATA:  # part of a shader-resource buffer written by UpdateSubresource
            buf, offset, length = struct.unpack_from("<QII", p, 0)
            events.append(Event("bufdata", buf=buf, offset=offset, data=bytes(p[16:16 + length])))
        elif typ == R_TEX_HASH:  # 3Dmigoto's texture hash of a texture just bound (render/texture_tags.cpp)
            res, h = struct.unpack_from("<QI", p, 0)
            events.append(Event("texhash", res=res, hash=h))
    return events, blobs


# ---------------------------------------------------------------------------------------------
# DXBC reflection (RDEF chunk): cbuffer and variable names
# ---------------------------------------------------------------------------------------------
def cstr(b, off):
    end = b.find(b"\0", off)
    return b[off:end if end >= 0 else len(b)].decode("latin1", "replace")


def parse_rdef(blob):
    """Returns {cbuffer_name: {"size":n, "vars":[(name, offset, size)]}, "_bind": {cbuffer_name: slot}}."""
    if len(blob) < 32 or blob[:4] != b"DXBC":
        return None
    count = struct.unpack_from("<I", blob, 28)[0]
    chunks = struct.unpack_from(f"<{count}I", blob, 32)
    for c in chunks:
        if blob[c:c + 4] != b"RDEF":
            continue
        size = struct.unpack_from("<I", blob, c + 4)[0]
        r = blob[c + 8:c + 8 + size]
        cb_count, cb_off, bind_count, bind_off = struct.unpack_from("<IIII", r, 0)
        minor, major = r[16], r[17]
        var_size = 40 if major >= 5 else 24
        bind_size = 40 if major >= 5 and minor >= 1 else 32
        out = {"_bind": {}}
        for i in range(cb_count):
            name_off, var_count, var_off, cb_size = struct.unpack_from("<IIII", r, cb_off + 24 * i)
            vars_ = []
            for v in range(var_count):
                vo = var_off + var_size * v
                vname_off, start, vsize = struct.unpack_from("<III", r, vo)
                vars_.append((cstr(r, vname_off), start, vsize))
            out[cstr(r, name_off)] = {"size": cb_size, "vars": vars_}
        for i in range(bind_count):
            bo = bind_off + bind_size * i
            name_off, btype, _ret, _dim, _samples, bind_point = struct.unpack_from("<IIIIII", r, bo)
            if btype == 0:  # D3D_SIT_CBUFFER
                out["_bind"][cstr(r, name_off)] = bind_point
        return out
    return None


# ---------------------------------------------------------------------------------------------
# matrix maths
# ---------------------------------------------------------------------------------------------
def mats_in(data):
    for off in range(0, len(data) - 63, 16):
        m = struct.unpack_from("<16f", data, off)
        if all(math.isfinite(x) for x in m):
            yield off, m


def rows(m):
    return [list(m[0:4]), list(m[4:8]), list(m[8:12]), list(m[12:16])]


def transpose(r):
    return [[r[j][i] for j in range(4)] for i in range(4)]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inverse(r):
    m = [row[:] + [1.0 if i == j else 0.0 for j in range(4)] for i, row in enumerate(r)]
    for col in range(4):
        piv = max(range(col, 4), key=lambda i: abs(m[i][col]))
        if abs(m[piv][col]) < 1e-12:
            return None
        m[col], m[piv] = m[piv], m[col]
        p = m[col][col]
        m[col] = [x / p for x in m[col]]
        for i in range(4):
            if i != col:
                f = m[i][col]
                m[i] = [x - f * y for x, y in zip(m[i], m[col])]
    return [row[4:] for row in m]


def near(a, b, eps=1e-3):
    return abs(a - b) < eps


def projection_info(m):
    """Perspective projection in either memory layout. Returns dict or None."""
    r = rows(m)
    for layout, R in (("row", r), ("column", transpose(r))):
        # in row-vector form (mul(v, M)): w = column 3 = (0, 0, +-1, 0)
        if not (near(R[0][3], 0) and near(R[1][3], 0) and near(R[3][3], 0) and near(abs(R[2][3]), 1)):
            continue
        if not (near(R[0][1], 0) and near(R[0][2], 0) and near(R[1][0], 0) and near(R[1][2], 0)
                and near(R[3][0], 0) and near(R[3][1], 0)):
            continue
        sx, sy, A, B, s = R[0][0], R[1][1], R[2][2], R[3][2], R[2][3]
        if not (0.05 < sx < 50 and 0.05 < sy < 50) or abs(B) < 1e-6:
            continue
        info = dict(layout=layout, sx=sx, sy=sy, cx=R[2][0], cy=R[2][1], A=A, B=B, wsign=s,
                    vfov=math.degrees(2 * math.atan(1 / sy)), hfov=math.degrees(2 * math.atan(1 / sx)),
                    aspect=sy / sx)
        # depth range. Row-vector form: clip.z = A*z + B, clip.w = s*z; view depth d = s*z > 0,
        # so z = s*d and z_ndc(d) = s*A + B/d. Normalise to the left-handed form by folding s into A.
        A = A * s
        if near(A, 0, 1e-6):
            info["depth"] = f"reversed-Z, infinite far, near={B:.4f}"
        elif near(A, 1, 1e-6):
            info["depth"] = f"infinite far, near={-B:.4f}"
        else:
            n_std, f_std = -B / A, B / (1 - A)
            n_rev, f_rev = B / (1 - A), -B / A
            if 0 < n_std < f_std:
                info["depth"] = f"standard Z, near={n_std:.4f} far={f_std:.1f}"
            elif 0 < n_rev < f_rev:
                info["depth"] = f"reversed-Z, near={n_rev:.4f} far={f_rev:.1f}"
            else:
                info["depth"] = f"A={A:.6f} B={B:.6f}"
        info["R"] = R
        return info
    return None


def is_rigid(R, eps=2e-3):
    """Row-vector rigid transform: 3x3 orthonormal, last column (0,0,0,1)."""
    if not (near(R[0][3], 0) and near(R[1][3], 0) and near(R[2][3], 0) and near(R[3][3], 1)):
        return False
    for i in range(3):
        for j in range(3):
            d = sum(R[i][k] * R[j][k] for k in range(3))
            if not near(d, 1.0 if i == j else 0.0, eps):
                return False
    return True


def view_info(m):
    r = rows(m)
    for layout, R in (("row", r), ("column", transpose(r))):
        if is_rigid(R):
            if all(near(R[i][j], 1.0 if i == j else 0.0) for i in range(4) for j in range(4)):
                return None  # identity: not interesting
            inv = inverse(R)
            cam = inv[3][:3] if inv else None  # camera position in world
            return dict(layout=layout, R=R, cam=cam)
    return None


# ---------------------------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------------------------
def fmt_vec(v):
    return "(" + ", ".join(f"{x:.3f}" for x in v) + ")"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--draws", type=int, default=40, help="draws to list in the pass listing")
    ap.add_argument("--dump-buffer", help="hex pointer of a cbuffer to dump as floats")
    args = ap.parse_args()
    events, blobs = parse(open(args.capture, "rb").read())

    shader_hash = {sh: h for sh, (_st, h, _b) in blobs.items()}
    reflection = {sh: parse_rdef(b) for sh, (_st, _h, b) in blobs.items()}
    n_refl = sum(1 for v in reflection.values() if v)

    # Walk the frame keeping bound state; remember the latest content of each buffer.
    bound = [[0] * 14 for _ in range(4)]
    shader = [0] * 4
    content = {}       # buf -> bytes (latest)
    first_content = {}  # buf -> bytes (first seen this frame)
    width = {}
    uses = collections.defaultdict(lambda: collections.Counter())  # buf -> Counter((stage, slot, shaderhash))
    draws_per_buf = collections.Counter()
    passes = []  # (rt w, h, fmt, n draws)
    cur_pass = None
    n_draw = n_disp = n_upd = 0
    for e in events:
        a = e.args
        if e.kind == "setcb":
            for i, b in enumerate(a["bufs"]):
                if a["stage"] < 4 and a["start"] + i < 14:
                    bound[a["stage"]][a["start"] + i] = b
        elif e.kind == "cbdata":
            n_upd += a["via"] != 2
            content[a["buf"]] = a["data"]
            first_content.setdefault(a["buf"], a["data"])
            width[a["buf"]] = a["width"]
        elif e.kind == "setshader":
            shader[a["stage"]] = a["shader"]
        elif e.kind == "setrt":
            cur_pass = [a["w"], a["h"], a["fmt"], a["n"], 0]
            passes.append(cur_pass)
        elif e.kind in ("draw", "dispatch"):
            if e.kind == "draw":
                n_draw += 1
                if cur_pass:
                    cur_pass[4] += 1
                stages = (0, 1, 2)
            else:
                n_disp += 1
                stages = (3,)
            seen = set()
            for st in stages:
                for slot, b in enumerate(bound[st]):
                    if b:
                        uses[b][(st, slot, shader_hash.get(shader[st] if st != 2 else 0, 0))] += 1
                        seen.add(b)
            for b in seen:
                draws_per_buf[b] += 1

    print(f"== frame: {n_draw} draws, {n_disp} dispatches, {len(passes)} render-target binds, "
          f"{n_upd} cbuffer uploads, {len(content)} cbuffers with data, {len(blobs)} shaders "
          f"({n_refl} with reflection data)")
    if any(e.kind == "execcl" for e in events):
        print("   note: the game executes command lists (deferred contexts) - their contents are not captured")

    print("\n== render passes (render target size / format / draws)")
    merged = []
    for p in passes:
        if merged and merged[-1][:4] == p[:4]:
            merged[-1][4] += p[4]
        else:
            merged.append(list(p))
    for w, h, fmt, n, d in merged:
        if d:
            print(f"   {w:5d}x{h:<5d} fmt {fmt:3d}  rtvs {n}  draws {d}")

    def names_for(buf, offset):
        """Variable names for a buffer offset, from any shader that binds it."""
        found = set()
        for (st, slot, h), _n in uses[buf].items():
            for sh, refl in reflection.items():
                if not refl or shader_hash.get(sh) != h:
                    continue
                for cbname, bp in refl["_bind"].items():
                    if bp == slot and cbname in refl:
                        for vname, start, size in refl[cbname]["vars"]:
                            if start <= offset < start + size:
                                found.add(f"{cbname}.{vname}" + (f"+{offset - start}" if offset != start else ""))
        return sorted(found)

    def describe_uses(buf):
        c = uses[buf]
        slots = collections.Counter((STAGES[st], slot) for (st, slot, _h) in c.elements())
        return ", ".join(f"{s}:b{k} x{n}" for (s, k), n in slots.most_common(6))

    print("\n== projection matrices")
    projs = []
    for buf, data in content.items():
        for off, m in mats_in(data):
            info = projection_info(m)
            if info:
                projs.append((buf, off, info))
                changed = first_content.get(buf) != data
                print(f"   cbuffer {buf:#x} (+{off}, {width[buf]} bytes){' [changed during frame]' if changed else ''}")
                print(f"      {info['layout']}-major, vfov {info['vfov']:.2f} deg, hfov {info['hfov']:.2f} deg, "
                      f"aspect {info['aspect']:.4f}, off-centre ({info['cx']:.4f}, {info['cy']:.4f}), "
                      f"{'left' if info['wsign'] > 0 else 'right'}-handed, {info['depth']}")
                print(f"      used by {draws_per_buf[buf]} draws/dispatches: {describe_uses(buf)}")
                for n in names_for(buf, off):
                    print(f"      name: {n}")
    if not projs:
        print("   none found")

    print("\n== view matrices (rigid transforms) in buffers used by >= 5 draws")
    views = []
    for buf, data in content.items():
        if draws_per_buf[buf] < 5:
            continue
        for off, m in mats_in(data):
            v = view_info(m)
            if v:
                views.append((buf, off, v))
                print(f"   cbuffer {buf:#x} +{off}: {v['layout']}-major, camera position {fmt_vec(v['cam'] or [])}"
                      f", used by {draws_per_buf[buf]} draws ({describe_uses(buf)})")
                for n in names_for(buf, off):
                    print(f"      name: {n}")

    print("\n== view-projection products (M = V * P for a projection P found above)")
    seen_p = []
    for _b, _o, pi in projs:
        key = tuple(round(x, 4) for row in pi["R"] for x in row)
        if key not in seen_p:
            seen_p.append(key)
    for key in seen_p:
        P = [list(key[0:4]), list(key[4:8]), list(key[8:12]), list(key[12:16])]
        Pinv = inverse(P)
        if not Pinv:
            continue
        for buf, data in content.items():
            for off, m in mats_in(data):
                r = rows(m)
                for layout, R in (("row", r), ("column", transpose(r))):
                    V = matmul(R, Pinv)
                    if is_rigid(V, 5e-3) and not all(near(V[i][j], float(i == j)) for i in range(4) for j in range(4)):
                        inv = inverse(V)
                        print(f"   cbuffer {buf:#x} +{off}: {layout}-major viewproj, camera position "
                              f"{fmt_vec(inv[3][:3]) if inv else '?'}, used by {draws_per_buf[buf]} draws "
                              f"({describe_uses(buf)})")
                        for n in names_for(buf, off):
                            print(f"      name: {n}")

    print("\n== reflection: cbuffer variables that look camera-related")
    keys = ("view", "proj", "camera", "cam", "eye", "vp", "wvp", "mvp", "fov", "inv")
    shown = set()
    for sh, refl in reflection.items():
        if not refl:
            continue
        for cbname, cb in refl.items():
            if cbname == "_bind":
                continue
            for vname, start, size in cb["vars"]:
                if any(k in vname.lower() for k in keys):
                    key = (cbname, vname, start)
                    if key not in shown:
                        shown.add(key)
                        print(f"   {cbname} (b{refl['_bind'].get(cbname, '?')}, {cb['size']} B): {vname} @ {start} "
                              f"({size} B)")
    if not shown:
        print("   (no reflection names: the shaders are stripped)" if not n_refl else "   none")

    if args.dump_buffer:
        buf = int(args.dump_buffer, 16)
        data = content.get(buf)
        print(f"\n== dump of {buf:#x}")
        if data:
            for off in range(0, len(data), 16):
                print(f"   +{off:5d}: " + " ".join(f"{x:12.5f}" for x in struct.unpack_from("<4f", data, off)))


if __name__ == "__main__":
    main()
