#!/usr/bin/env python3
# Flash a large raw image to a fastboot partition that fastboot itself cannot
# flash whole.  `fastboot flash userdata big.img` fails with "Failed reading
# from userdata" once the image is over ~2 GiB (fastboot mmaps the file and a
# size/offset overflows), and the Android-side `dd` fast path is unusable when
# /data is FBE-encrypted f2fs (writeback overwrites a raw dd).  This splits the
# raw image into <2 GiB Android sparse (.simg) windows: each window is one
# sparse file with DONT_CARE outside its block range and RAW (all-zero windows
# skipped) inside, so `fastboot flash PART win.simg` writes only that window.
# The bootloader writes the raw partition directly (no f2fs/FBE), so nothing
# corrupts it.  ~130 MB/s, ~2 min for a 16 GiB image.  Run the image from a
# LOCAL disk (fastboot also struggles to read a multi-GiB file off a slow/
# external volume).  Usage:  sparseflash.py IMG PARTITION
import os, struct, subprocess, sys
IMG = sys.argv[1]; PART = sys.argv[2]
BLK = 4096
WIN_BLKS = (1024 * 1024 * 1024) // BLK      # 1 GiB window (< fastboot's ~2 GiB limit)
total = os.path.getsize(IMG)
total_blks = (total + BLK - 1) // BLK
MAGIC = 0xed26ff3a
def hdr(total_chunks):
    return struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BLK, total_blks, total_chunks, 0)
RAW, DONT = 0xCAC1, 0xCAC3
def chunk_dont(nblk):
    return struct.pack('<HHII', DONT, 0, nblk, 12)
def chunk_raw_hdr(nblk, nbytes):
    return struct.pack('<HHII', RAW, 0, nblk, 12 + nbytes)
f = open(IMG, 'rb')
win = 0; nwin = (total_blks + WIN_BLKS - 1) // WIN_BLKS
for wstart in range(0, total_blks, WIN_BLKS):
    wend = min(wstart + WIN_BLKS, total_blks)
    f.seek(wstart * BLK)
    data = f.read((wend - wstart) * BLK)
    if len(data) < (wend - wstart) * BLK:
        data += b'\0' * ((wend - wstart) * BLK - len(data))
    if not any(data):
        win += 1; continue  # all-zero window: skip entirely
    # build chunks: DONT_CARE [0,wstart), RAW [wstart,wend), DONT_CARE [wend,total)
    chunks = []; nchunks = 0
    if wstart > 0:
        chunks.append(chunk_dont(wstart)); nchunks += 1
    chunks.append(chunk_raw_hdr(wend - wstart, len(data))); chunks.append(data); nchunks += 1
    if wend < total_blks:
        chunks.append(chunk_dont(total_blks - wend)); nchunks += 1
    out = '/tmp/win.simg'
    with open(out, 'wb') as o:
        o.write(hdr(nchunks))
        for c in chunks: o.write(c)
    print(f'[win {win+1}/{nwin}] blocks {wstart}-{wend} ({len(data)//1024//1024} MiB) flashing...', flush=True)
    r = subprocess.run(['fastboot', 'flash', PART, out], capture_output=True, text=True)
    tail = (r.stdout + r.stderr).strip().replace('\r', '\n').splitlines()
    print('   ' + (tail[-1] if tail else ''), flush=True)
    if r.returncode != 0 or 'Failed' in (r.stdout + r.stderr):
        print('   !! FAILED, aborting', flush=True); sys.exit(1)
    win += 1
os.path.exists('/tmp/win.simg') and os.remove('/tmp/win.simg')
print('ALL WINDOWS FLASHED', flush=True)
