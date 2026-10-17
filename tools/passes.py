"""Pass-by-pass listing of a capture: draws/dispatches with shaders, render targets, SRV inputs and
UAV outputs, so data flow between passes can be followed. Usage: passes.py capture.bin [first last]"""
import sys
import analyze_capture as A


def main():
    events, blobs = A.parse(open(sys.argv[1], 'rb').read())
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9
    sh = {}
    srv = {1: {}, 3: {}}
    uav = {}
    rt = None
    n = 0
    for e in events:
        a = e.args
        if e.kind == 'setshader':
            sh[a['stage']] = a['shader']
        elif e.kind == 'setsrv':
            for i, v in enumerate(a['views']):
                srv.setdefault(a['stage'], {})[a['start'] + i] = v
        elif e.kind == 'setuav':
            for i, v in enumerate(a['views']):
                uav[a['start'] + i] = v
        elif e.kind == 'setrt':
            rt = a
        elif e.kind in ('draw', 'dispatch'):
            n += 1
            if not lo <= n <= hi:
                continue
            fmt = lambda v: '%x:%dx%d/f%d' % (v[0] & 0xfffff, v[1], v[2], v[3]) if v[0] else '-'
            if e.kind == 'dispatch':
                s = sh.get(3)
                ins = {k: fmt(v) for k, v in sorted(srv.get(3, {}).items()) if v[0]}
                outs = {k: fmt(v) for k, v in sorted(uav.items()) if v[0]}
                print(n, 'DISPATCH', a['xyz'], 'cs', hex(s or 0)[-5:], 'in', ins, 'out', outs)
            else:
                s = sh.get(1)
                ins = {k: fmt(v) for k, v in sorted(srv.get(1, {}).items()) if v[0]}
                print(n, 'draw ps', hex(s or 0)[-5:], 'rt %dx%d f%d res %x depth %x' % (rt['w'], rt['h'], rt['fmt'], rt['rtv0'] & 0xfffff, rt['dsv'] & 0xfffff) if rt else '', 'in', ins)


if __name__ == '__main__':
    main()
