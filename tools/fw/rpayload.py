#!/usr/bin/env python3
"""Extract single partitions from a remote Android OTA zip (payload.bin) via HTTP ranges.

usage: rpayload.py URL list
       rpayload.py URL get PART OUTFILE
"""
import bz2, lzma, struct, subprocess, sys, zlib

URL = sys.argv[1]


def get(off, n):
    r = subprocess.run(["curl", "-sS", "--fail", "--retry", "3", "-r", f"{off}-{off + n - 1}", URL],
                       capture_output=True, check=True)
    assert len(r.stdout) == n, (off, n, len(r.stdout))
    return r.stdout


def total_size():
    h = subprocess.run(["curl", "-sSI", "-r", "0-0", URL], capture_output=True, text=True, check=True).stdout
    for l in h.splitlines():
        if l.lower().startswith("content-range"):
            return int(l.split("/")[-1])
    raise SystemExit("no content-range")


def find_payload():
    size = total_size()
    tail = get(size - 65557, 65557) if size > 65557 else get(0, size)
    i = tail.rfind(b"PK\x05\x06")
    _, _, _, _, n, cd_size, cd_off = struct.unpack_from("<IHHHHII", tail, i)
    if cd_off == 0xFFFFFFFF or n == 0xFFFF:  # zip64
        j = tail.rfind(b"PK\x06\x07", 0, i)
        (eocd64,) = struct.unpack_from("<Q", tail, j + 8)
        e = get(eocd64, 56)
        n, cd_size, cd_off = struct.unpack_from("<QQQ", e, 32)
    cd = get(cd_off, cd_size)
    p = 0
    while p < len(cd):
        (sig, _, _, _, method, _, _, _, csize, usize, fnl, exl, cml, _, _, _, lho) = struct.unpack_from(
            "<IHHHHHHIIIHHHHHII", cd, p)
        name = cd[p + 46:p + 46 + fnl].decode()
        extra = cd[p + 46 + fnl:p + 46 + fnl + exl]
        q = 0
        while q < len(extra):
            hid, hl = struct.unpack_from("<HH", extra, q)
            if hid == 1:
                vals = list(struct.unpack_from("<" + "Q" * (hl // 8), extra, q + 4))
                if usize == 0xFFFFFFFF: usize = vals.pop(0)
                if csize == 0xFFFFFFFF: csize = vals.pop(0)
                if lho == 0xFFFFFFFF: lho = vals.pop(0)
            q += 4 + hl
        if name == "payload.bin":
            assert method == 0, "payload.bin is compressed"
            lh = get(lho, 30)
            fl, el = struct.unpack_from("<HH", lh, 26)
            return lho + 30 + fl + el, usize
        p += 46 + fnl + exl + cml
    raise SystemExit("payload.bin not found")


def varint(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1
        v |= (c & 0x7F) << s; s += 7
        if c < 0x80:
            return v, i


def fields(b):
    i = 0
    while i < len(b):
        k, i = varint(b, i)
        f, t = k >> 3, k & 7
        if t == 0:
            v, i = varint(b, i)
        elif t == 2:
            l, i = varint(b, i); v = b[i:i + l]; i += l
        elif t == 1:
            v = b[i:i + 8]; i += 8
        elif t == 5:
            v = b[i:i + 4]; i += 4
        else:
            raise ValueError(t)
        yield f, v


def manifest(base):
    h = get(base, 24)
    assert h[:4] == b"CrAU", h[:4]
    ver, msize = struct.unpack(">QQ", h[4:20])
    sigsize = struct.unpack(">I", h[20:24])[0] if ver >= 2 else 0
    hdr = 24 if ver >= 2 else 20
    m = get(base + hdr, msize)
    data_base = base + hdr + msize + sigsize
    bs, parts = 4096, []
    for f, v in fields(m):
        if f == 3: bs = v
        if f == 13:
            name, ops, size = None, [], None
            for g, w in fields(v):
                if g == 1: name = w.decode()
                if g == 7:  # new_partition_info
                    for h2, x in fields(w):
                        if h2 == 1: size = x
                if g == 8:
                    op = {"type": 0, "off": 0, "len": 0, "dst": []}
                    for o, y in fields(w):
                        if o == 1: op["type"] = y
                        elif o == 2: op["off"] = y
                        elif o == 3: op["len"] = y
                        elif o == 6:
                            e = dict(fields(y)); op["dst"].append((e.get(1, 0), e.get(2, 0)))
                    ops.append(op)
            parts.append((name, size, ops))
    return data_base, bs, parts


def main():
    base, _ = find_payload()
    data_base, bs, parts = manifest(base)
    if sys.argv[2] == "list":
        for name, size, ops in parts:
            print(f"{name:24s} {size or 0:>12d}  ops={len(ops)} types={sorted({o['type'] for o in ops})}")
        return
    want, out = sys.argv[3], sys.argv[4]
    for name, size, ops in parts:
        if name != want:
            continue
        img = bytearray(size)
        for op in ops:
            blob = get(data_base + op["off"], op["len"]) if op["len"] else b""
            t = op["type"]
            if t == 0: d = blob
            elif t == 1: d = bz2.decompress(blob)
            elif t == 8: d = lzma.decompress(blob)
            elif t == 6: d = b""
            elif t == 14:
                d = subprocess.run(["zstd", "-dc"], input=blob, capture_output=True, check=True).stdout
            else:
                raise SystemExit(f"unsupported op type {t}")
            pos = 0
            for start, nb in op["dst"]:
                chunk = d[pos:pos + nb * bs] if d else bytes(nb * bs)
                img[start * bs:start * bs + len(chunk)] = chunk
                pos += nb * bs
        open(out, "wb").write(img)
        print(f"wrote {out} ({size} bytes, {len(ops)} ops)")
        return
    raise SystemExit(f"no partition {want}")


main()
