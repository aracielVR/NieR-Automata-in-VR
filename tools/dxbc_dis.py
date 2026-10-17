"""Minimal SM4/SM5 DXBC disassembler (enough to follow how a shader uses its cbuffers).

Usage from Python: dis(blob) -> list of text lines.
"""
import struct

OPS = {0: 'add', 1: 'and', 2: 'break', 3: 'breakc', 7: 'continue', 13: 'discard', 14: 'div', 15: 'dp2', 16: 'dp3',
       17: 'dp4', 18: 'else', 21: 'endif', 22: 'endloop', 24: 'eq', 25: 'exp', 26: 'frc', 27: 'ftoi', 28: 'ftou',
       29: 'ge', 30: 'iadd', 31: 'if', 32: 'ieq', 33: 'ige', 34: 'ilt', 35: 'imad', 36: 'imax', 37: 'imin',
       38: 'imul', 39: 'ine', 40: 'ineg', 41: 'ishl', 42: 'ishr', 43: 'itof', 45: 'ld', 47: 'log', 48: 'loop',
       49: 'lt', 50: 'mad', 51: 'min', 52: 'max', 54: 'mov', 55: 'movc', 56: 'mul', 57: 'ne', 59: 'not', 60: 'or',
       61: 'resinfo', 62: 'ret', 64: 'round_ne', 65: 'round_ni', 66: 'round_pi', 67: 'round_z', 68: 'rsq',
       69: 'sample', 70: 'sample_c', 71: 'sample_c_lz', 72: 'sample_l', 73: 'sample_d', 74: 'sample_b', 75: 'sqrt',
       77: 'sincos', 78: 'udiv', 79: 'ult', 80: 'uge', 81: 'umul', 83: 'umax', 84: 'umin', 85: 'ushr', 86: 'utof',
       87: 'xor', 121: 'bufinfo', 167: 'ld_structured', 168: 'ld_structured_indexable'}
# declarations (not printed): 88-106, 143-166, 169-182, 183+ dcl_*
DCL = set(range(88, 107)) | set(range(143, 167)) | set(range(169, 183)) | set(range(183, 200))
TYPES = {0: 'r', 1: 'v', 2: 'o', 3: 'x', 4: 'l', 6: 'sampler', 7: 't', 8: 'cb', 9: 'icb'}


def _operand(toks, i):
    t = toks[i]
    i += 1
    ncomp = t & 3
    sel = (t >> 2) & 3
    otype = (t >> 12) & 0xff
    dim = (t >> 20) & 3
    ext = t >> 31
    mod = 0
    if ext:
        et = toks[i]
        if (et & 0x3f) == 1:
            mod = (et >> 6) & 0xff
        i += 1
    comp = ''
    if ncomp == 2:
        if sel == 0:
            m = (t >> 4) & 0xf
            comp = '.' + ''.join('xyzw'[k] for k in range(4) if m >> k & 1)
        elif sel == 1:
            comp = '.' + ''.join('xyzw'[(t >> (4 + 2 * k)) & 3] for k in range(4))
        else:
            comp = '.' + 'xyzw'[(t >> 4) & 3]
    if otype == 4:  # immediate32
        n = 4 if ncomp == 2 else 1
        vals = toks[i:i + n]
        i += n
        fl = [struct.unpack('<f', struct.pack('<I', v))[0] for v in vals]
        return 'l(' + ', '.join(('%g' % f) if abs(f) < 1e7 and (f != 0 or v == 0) else hex(v)
                                  for f, v in zip(fl, vals)) + ')', i
    idx = []
    for d in range(dim):
        rep = (t >> (22 + 3 * d)) & 7
        if rep == 0:
            idx.append(str(toks[i]))
            i += 1
        elif rep == 2:  # relative
            sub, i = _operand(toks, i)
            idx.append(sub)
        elif rep == 3:
            v = toks[i]
            i += 1
            sub, i = _operand(toks, i)
            idx.append('%s+%d' % (sub, v))
        else:
            idx.append('?')
            i += 1
    name = TYPES.get(otype, 'T%d' % otype)
    if otype == 8 and len(idx) == 2:
        txt = 'cb%s[%s]%s' % (idx[0], idx[1], comp)
    else:
        txt = name + ''.join('[%s]' % x if k else x for k, x in enumerate(idx)) + comp
    return {0: txt, 1: '-' + txt, 2: '|%s|' % txt, 3: '-|%s|' % txt}.get(mod, txt), i


def dis(blob):
    count = struct.unpack_from('<I', blob, 28)[0]
    for c in struct.unpack_from(f'<{count}I', blob, 32):
        if blob[c:c + 4] not in (b'SHDR', b'SHEX'):
            continue
        size = struct.unpack_from('<I', blob, c + 4)[0]
        toks = struct.unpack_from(f'<{size // 4}I', blob[c + 8:c + 8 + size])
        i = 2
        out = []
        while i < len(toks):
            t = toks[i]
            op = t & 0x7ff
            ln = (t >> 24) & 0x7f
            if op == 0x35:
                ln = toks[i + 1]
            if ln == 0:
                break
            if op in OPS:
                j = i + 1
                if t >> 31:
                    j += 1
                ops = []
                while j < i + ln:
                    try:
                        s, j = _operand(toks, j)
                    except IndexError:
                        break
                    ops.append(s)
                sat = '_sat' if (t >> 13) & 1 else ''
                out.append(OPS[op] + sat + ' ' + ', '.join(ops))
            elif op not in DCL:
                out.append('op%d (len %d)' % (op, ln))
            i += ln
        return out
    return []
