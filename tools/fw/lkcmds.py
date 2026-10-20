#!/usr/bin/env python3
"""List fastboot commands registered by a MediaTek lk.img.

usage: lkcmds.py lk.img WORKDIR
Finds the fastboot_register(name, handler, w2, w3) function through the call
that passes "getvar:", then prints every direct call, the tail calls, and
the tables walked by a loop that calls it (entries {u32, u32, name*, fn*}).
"""
import re, struct, subprocess, sys, os

img, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
d = open(img, 'rb').read()
assert struct.unpack_from('<I', d, 0)[0] == 0x58881688 and d[8:10] == b'lk'
size = struct.unpack_from('<I', d, 4)[0]
body = d[0x200:0x200 + size]
binp = os.path.join(work, 'lk_main.bin')
open(binp, 'wb').write(body)
dis = subprocess.run(['aarch64-elf-objdump', '-D', '-b', 'binary', '-m', 'aarch64', binp],
                     capture_output=True, text=True).stdout
ins = []
for l in dis.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)', l)
    if m:
        ins.append((int(m.group(1), 16), m.group(2), m.group(3)))


def cstr(o):
    if not 0 <= o < len(body):
        return None
    e = body.find(b'\0', o)
    s = body[o:e]
    return s.decode() if s and all(32 <= c < 127 for c in s) else None


def regs_before(i, n=12):
    r, adrp = {}, {}
    for j in range(max(0, i - n), i):
        _, op, a = ins[j]
        m = re.match(r'(x\d+|w\d+),\s*(.*)', a)
        if not m:
            continue
        rd, rest = m.groups()
        if op == 'adrp':
            adrp[rd] = int(rest.split(',')[0], 16); r[rd] = None
        elif op == 'add':
            mm = re.match(r'(x\d+),\s*#0x([0-9a-f]+)', rest)
            r[rd] = adrp[mm.group(1)] + int(mm.group(2), 16) if mm and mm.group(1) in adrp else None
        elif op == 'adr':
            r[rd] = int(rest.split()[0], 16)
        elif op == 'mov':
            r[rd] = 0 if rest in ('wzr', 'xzr') else (int(rest.split()[0][1:], 0) if rest.startswith('#') else None)
        else:
            r[rd] = None
    return r


getvar = body.find(b'\0getvar:\0') + 1
reg_fn = None
for i, (a, op, args) in enumerate(ins):
    if op == 'bl':
        r = regs_before(i)
        if r.get('x0') == getvar:
            reg_fn = args.split()[0]
            break
assert reg_fn, 'register function not found'
print(f'fastboot_register = lk+{reg_fn}')
names = []
for i, (a, op, args) in enumerate(ins):
    if op in ('bl', 'b') and args.split()[0] == reg_fn:
        r = regs_before(i)
        n = cstr(r['x0']) if isinstance(r.get('x0'), int) else None
        if n:
            names.append((n, r.get('w2'), r.get('w3'), f'{op} @{a:#x}'))
        else:
            # table loop: look back for two GOT loads (start/end) via adrp+ldr
            win = ins[max(0, i - 40):i]
            got = []
            adrp = {}
            for _, o2, a2 in win:
                m = re.match(r'(x\d+),\s*0x([0-9a-f]+)$', a2) if o2 == 'adrp' else None
                if m: adrp[m.group(1)] = int(m.group(2), 16)
                m = re.match(r'(x\d+),\s*\[(x\d+),\s*#(\d+)\]', a2) if o2 == 'ldr' else None
                if m and m.group(2) in adrp:
                    got.append(adrp[m.group(2)] + int(m.group(3)))
            ptrs = sorted({struct.unpack_from('<Q', body, g)[0] for g in got if g + 8 <= len(body)})
            ptrs = [p for p in ptrs if p >> 48 == 0xffff]
            if len(ptrs) >= 2:
                base = None
                lo, hi = ptrs[0], ptrs[-1]
                # base: page such that table lies in the image and entries' names resolve
                for cand in range((lo & ~0xfffff) - 0x400000, (lo & ~0xfffff) + 0x400000, 0x1000):
                    o = lo - cand
                    if 0 <= o < len(body) and hi - cand <= len(body):
                        n0 = struct.unpack_from('<Q', body, o + 8)[0] - cand
                        if cstr(n0):
                            base = cand
                            break
                if base is None:
                    print(f'  table loop @{a:#x}: base not found, ptrs {[hex(p) for p in ptrs]}')
                    continue
                for o in range(lo - base, hi - base, 0x18):
                    w2, w3, nm, h = struct.unpack_from('<IIQQ', body, o)
                    names.append((cstr(nm - base), w2, w3, f'table @lk+{o:#x}'))
for n, w2, w3, where in names:
    print(f'  {n!r:32s} w2={w2} w3(forbid when locked)={w3}  [{where}]')
print('TOTAL', len(names), ' has "boot":', any(n == 'boot' for n, *_ in names))
