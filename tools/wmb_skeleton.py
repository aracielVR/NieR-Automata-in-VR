#!/usr/bin/env python3
"""Write src/game/body_skeleton.h: 2B's skeleton and which bone each slot of her GPU bone lists is.

Reads her model (pl/pl0000.dtt -> pl0000.wmb, WMB3) straight from the game's CPK archives.

Each skinned mesh part uploads a bone list (skinningBuffer, VS b11); a list is one of the model's bone
sets, and each set entry goes through the bone map to a bone. Which bone set each of her 15 parts (in
first-bind order, see head_bone.h) uses was found in F11 captures: a bone in two lists has the same
matrix in both. Part 13 (her face) switches between two orderings: her normal face, and her serious face
in combat.

Usage: wmb_skeleton.py <NieRAutomata game folder> [output .h]
"""
import math
import os
import struct
import sys

PART_SETS = [5, 1, 8, 7, 6, 11, 13, 12, 14, 15, 16, 18, 21, 22, 20]
SERIOUS_FACE_SET = 23

# the bones the first-person body works with (bone id: name)
NAMED = {
    0: 'HIPS', 1: 'WAIST', 2: 'SPINE', 3: 'CHEST', 4: 'NECK', 5: 'HEAD',
    6: 'RIGHT_CLAVICLE', 7: 'RIGHT_UPPER_ARM', 8: 'RIGHT_FOREARM', 9: 'RIGHT_HAND',
    10: 'LEFT_CLAVICLE', 11: 'LEFT_UPPER_ARM', 12: 'LEFT_FOREARM', 13: 'LEFT_HAND',
    14: 'PELVIS', 15: 'RIGHT_THIGH', 16: 'RIGHT_SHIN', 17: 'RIGHT_ANKLE', 18: 'RIGHT_TOES',
    19: 'LEFT_THIGH', 20: 'LEFT_SHIN', 21: 'LEFT_ANKLE', 22: 'LEFT_TOES',
}

# the other bones: what they are, or may be (from where they are and what they hang off)
HELPERS = {
    4095: 'A root at hip height: may be her centre for the game\'s movement',
    4094: 'The root on the floor: the model\'s origin',
    2560: 'Right shoulder helper: may be upper arm twist', 2561: 'Left shoulder helper: may be upper arm twist',
    2576: 'Right forearm helper: may be forearm twist', 2592: 'Left forearm helper: may be forearm twist',
    2577: 'Right wrist helper: may be wrist twist', 2593: 'Left wrist helper: may be wrist twist',
    3121: 'On the right forearm: may be where a weapon or bracelet attaches',
    3120: 'On the left forearm: may be where a weapon or bracelet attaches',
    2608: 'Right buttock helper: may keep her hips round when she bends',
    2609: 'Left buttock helper: may keep her hips round when she bends',
    2656: 'Front of the right hip: may keep the hip crease smooth',
    2657: 'Front of the left hip: may keep the hip crease smooth',
    2640: 'Neck helper: may keep her neck smooth when her head turns',
    3136: 'Front of her chest: may be breast movement',
    2816: 'Centre of her face: may be the jaw or the face\'s root',
    2817: 'Left eye', 2818: 'Right eye',
    3152: 'Back of the head, low: may be a hair root',
}
FINGERS = ['index finger', 'middle finger', 'ring finger', 'little finger']
HAIR_STRANDS = {3164: 'left side', 3166: 'back left', 3168: 'back', 3170: 'back right', 3172: 'right side'}


def direction(x, z):
    """Which way from her centre a point is (she faces +z with +y up: her left is +x)."""
    names = ['front', 'front right', 'right', 'back right', 'back', 'back left', 'left', 'front left']
    return names[round(math.atan2(-x, z) / (math.pi / 4)) % 8]


def describe(bone, bones):
    i = bone['id']
    if i in NAMED:
        return NAMED[i].lower().replace('_', ' ').capitalize()
    if i in HELPERS:
        return HELPERS[i]
    for first, hand in ((256, 'Right'), (512, 'Left')):
        if first <= i < first + 3:
            return '%s thumb, joint %d' % (hand, i - first + 1)
        if first + 3 <= i < first + 19:
            k = i - first - 3
            return '%s %s, joint %d' % (hand, FINGERS[k // 4], k % 4 + 1)
    if 3153 <= i <= 3155:
        return 'Hair: fringe strand in front, joint %d' % (i - 3152)
    if 3156 <= i <= 3163:
        return 'Hair: %s fringe strand, joint %d' % ('left' if i < 3160 else 'right', (i - 3156) % 4 + 1)
    if 3164 <= i <= 3173:
        return 'Hair: %s strand, joint %d' % (HAIR_STRANDS[i - (i - 3164) % 2], (i - 3164) % 2 + 1)
    if 3102 <= i <= 3107:
        return 'Dress: a front flap on her right (flap %d, joint %d)' % ((i - 3102) // 3 + 1, (i - 3102) % 3 + 1)
    if 3184 <= i <= 3273:
        top = next(b['pos'] for b in bones if b['id'] == 3184 + (i - 3184) // 6 * 6)
        return 'Skirt: strand %d of 15 (%s), joint %d of 6' % ((i - 3184) // 6 + 1, direction(top[0], top[2]), (i - 3184) % 6 + 1)
    return 'Unknown'


# --- CRI CPK archive ---------------------------------------------------------------------------------
def utf_table(b):
    if b[:4] != b'@UTF':  # masked
        out, m = bytearray(b), 0x655f
        for i in range(len(out)):
            out[i] ^= m & 0xff
            m = (m * 0x4115) & 0xffffffff
        b = bytes(out)
    rows_off, str_off, data_off, _, ncol, row_len, nrows = struct.unpack_from('>HIIIHHI', b, 10)
    base = 8

    def string(o):
        s = base + str_off + o
        return b[s:b.index(b'\0', s)].decode('latin1')

    formats = {0: '>B', 1: '>b', 2: '>H', 3: '>h', 4: '>I', 5: '>i', 6: '>Q', 7: '>q', 8: '>f'}

    def read(kind, p):
        if kind in formats:
            return struct.unpack_from(formats[kind], b, p)[0], p + struct.calcsize(formats[kind])
        if kind == 0xA:
            return string(struct.unpack_from('>I', b, p)[0]), p + 4
        if kind == 0xB:
            o, n = struct.unpack_from('>II', b, p)
            return b[base + data_off + o:base + data_off + o + n], p + 8
        raise ValueError('UTF column type %d' % kind)

    columns, p = [], 32
    for _ in range(ncol):
        flags, name = b[p], string(struct.unpack_from('>I', b, p + 1)[0])
        p += 5
        constant = None
        if flags & 0xf0 == 0x30:
            constant, p = read(flags & 0xf, p)
        columns.append((name, flags, constant))
    rows = []
    for r in range(nrows):
        p, row = base + rows_off + r * row_len, {}
        for name, flags, constant in columns:
            if flags & 0xf0 == 0x50:
                row[name], p = read(flags & 0xf, p)
            else:
                row[name] = constant
        rows.append(row)
    return rows


def cpk_file(cpk, wanted):
    with open(cpk, 'rb') as f:
        head = f.read(16)
        if head[:4] != b'CPK ':
            return None
        header = utf_table(f.read(struct.unpack_from('<Q', head, 8)[0]))[0]
        toc, content = header['TocOffset'], header['ContentOffset']
        if not toc:
            return None
        f.seek(toc)
        head = f.read(16)
        rows = utf_table(f.read(struct.unpack_from('<Q', head, 8)[0]))
        base = min(toc, content) if content else toc
        for r in rows:
            if ((r['DirName'] + '/') if r['DirName'] else '') + r['FileName'] == wanted:
                if r['FileSize'] != r['ExtractSize']:
                    raise ValueError(wanted + ' is compressed (CRILAYLA): not supported')
                f.seek(base + r['FileOffset'])
                return f.read(r['FileSize'])
    return None


# --- PlatinumGames DAT archive -----------------------------------------------------------------------
def dat_file(b, wanted):
    count, offsets, _, names, sizes = struct.unpack_from('<5I', b, 4)
    name_size, = struct.unpack_from('<I', b, names)
    for i in range(count):
        name = b[names + 4 + i * name_size:names + 4 + (i + 1) * name_size].split(b'\0')[0].decode()
        if name == wanted:
            o, = struct.unpack_from('<I', b, offsets + 4 * i)
            s, = struct.unpack_from('<I', b, sizes + 4 * i)
            return b[o:o + s]
    return None


# --- WMB3 model --------------------------------------------------------------------------------------
def skeleton(wmb):
    assert wmb[:4] == b'WMB3'
    h = struct.unpack_from('<24I', wmb, 0x28)
    bones = []
    for i in range(h[1]):
        v = struct.unpack_from('<hh21f', wmb, h[0] + 88 * i)
        bones.append({'id': v[0], 'parent': v[1], 'pos': v[11:14]})  # bind pose, world (metres)
    bonemap = struct.unpack_from('<%dI' % h[13], wmb, h[12])
    sets = []
    for i in range(h[15]):
        o, n = struct.unpack_from('<II', wmb, h[14] + 8 * i)
        sets.append([bonemap[s] for s in struct.unpack_from('<%dH' % n, wmb, o)])
    return bones, sets


def main():
    game = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'src', 'game', 'body_skeleton.h')
    data = os.path.join(game, 'data')
    dtt = None
    for cpk in sorted(os.listdir(data)):
        if cpk.endswith('.cpk'):
            dtt = cpk_file(os.path.join(data, cpk), 'pl/pl0000.dtt') or dtt  # later archives override
    if not dtt:
        sys.exit('pl/pl0000.dtt not found in ' + data)
    bones, sets = skeleton(dat_file(dtt, 'pl0000.wmb'))
    index_of = {b['id']: i for i, b in enumerate(bones)}
    assert all(len(sets[s]) < 256 for s in PART_SETS) and len(bones) < 256

    def floats(v):
        return '{%s}' % ', '.join('%.4ff' % x if x else '0' for x in v)

    def slots(name, s):
        return 'inline constexpr uint8_t %s[] = {%s};' % (name, ', '.join(str(b) for b in sets[s]))

    lines = [
        '// body_skeleton.h - 2B\'s skeleton (pl0000.wmb) and which bone each slot of her bone lists is. Written by tools/wmb_skeleton.py.',
        '#pragma once',
        '#include <iterator>',
        '#include <stdint.h>',
        '#include "core/vecmath.h"',
        '',
        'namespace pure {',
        '',
        '// clang-format off (one line per bone, one per part)',
        '// Her bones: the game\'s bone id, the parent (an index here, -1: none), the bind pose\'s position (metres).',
        'struct SkeletonBone {',
        '\tint16_t id;',
        '\tint16_t parent;',
        '\tV3 bind;',
        '};',
        'inline constexpr SkeletonBone kSkeleton[] = {',
    ]
    lines += ['\t{%d, %d, %s},  // %d: %s' % (b['id'], b['parent'], floats(b['pos']), n, describe(b, bones))
              for n, b in enumerate(bones)]
    lines += ['};', 'inline constexpr int32_t kSkeletonBones = std::size(kSkeleton);', '']
    lines += ['// The bones the first-person body works with (indices into kSkeleton).', 'enum SKELETON_BONE : int32_t {']
    lines += ['\tBONE_%s = %d,' % (NAMED[i], index_of[i]) for i in sorted(NAMED)]
    lines += ['};', '']
    lines += ['// Each of her parts\' bone list (kBodyPartBones order): the bone (kSkeleton index) in each slot.']
    lines += [slots('kPart%dSlots' % p, s) for p, s in enumerate(PART_SETS)]
    lines += ['// Part 13 (her face) with her serious face, in combat: the same bones in another order.']
    lines += [slots('kPart13SeriousSlots', SERIOUS_FACE_SET)]
    lines += ['', 'struct PartSlots {', '\tconst uint8_t* bones;', '\tint32_t count;', '};']
    lines += ['inline constexpr PartSlots kPartSlots[] = {']
    lines += ['\t{kPart%dSlots, std::size(kPart%dSlots)},' % (p, p) for p in range(len(PART_SETS))]
    lines += ['};', '// clang-format on', '', '}  // namespace pure', '']
    with open(out, 'w', newline='\r\n') as f:
        f.write('\n'.join(lines))
    print('wrote %s: %d bones, %d parts' % (out, len(bones), len(PART_SETS)))


if __name__ == '__main__':
    main()
