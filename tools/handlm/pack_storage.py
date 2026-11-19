"""Pack hand-landmark models + test data into one image for the `storage` partition
(phase B on-device bench, firmware/main/handlm_bench.cpp; nothing here ships).

usage: python -I pack_storage.py <out.bin> <eval.npz> <frames_dir> name=path.espdl [name=path.espdl ...]

Layout (offsets relative to the partition start, every item 64 KiB aligned so it
can be esp_partition_mmap'ed on its own):
  0x0      first model (so esp-dl's MODEL_LOCATION_IN_FLASH_PARTITION path, which
           always reads offset 0, loads it)
  ...      the other models, one after the other
  ...      "HLIN" input crop: eval crop 0 (grid_one/rot, the crop embedded as the
           test input of the *_test .espdl files), 224x224x3 HWC int16, exponent -15
  ...      "HLFR" test frames: every *.rgb565be in frames_dir (320x240 RGB565 BE)
  size-64K "HLDX" directory: u32 magic, u32 count, then count x {u32 offset,
           u32 bytes, char name[40]}
Write it with: esptool --chip esp32s3 -p COMx write_flash 0x410000 <out.bin>
(0x410000 = storage offset in firmware/partitions.csv).
"""
import glob
import os
import struct
import sys

import numpy as np

PART_SIZE = 0xBF0000
ALIGN = 0x10000
DIR_OFF = PART_SIZE - ALIGN


def align(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


out_path, eval_path, frames_dir = sys.argv[1:4]
models = [a.split("=", 1) for a in sys.argv[4:]]
items = []  # (name, bytes)
for name, path in models:
    items.append((name, open(path, "rb").read()))

crop = np.load(eval_path)["crops"][0]  # (224,224,3) float32 0..1
q = np.clip(np.round(crop.astype(np.float64) * 32768.0), -32768, 32767).astype("<i2")
items.append(("input", struct.pack("<4sIIIi", b"HLIN", 224, 224, 3, -15) + q.tobytes()))

frames = b""
names = []
for f in sorted(glob.glob(os.path.join(frames_dir, "*.rgb565be"))):
    data = open(f, "rb").read()
    assert len(data) % (320 * 240 * 2) == 0, f
    frames += data
    names.append(f"{os.path.basename(f)}:{len(data) // (320 * 240 * 2)}")
items.append(("frames", struct.pack("<4sIII", b"HLFR", len(frames) // (320 * 240 * 2), 320, 240) + frames))

blob = bytearray()
entries = []
for name, data in items:
    off = len(blob)
    entries.append((off, len(data), name))
    blob += data
    blob += b"\xff" * (align(len(blob)) - len(blob))
if len(blob) > DIR_OFF:
    raise SystemExit(f"too big: {len(blob)} > {DIR_OFF}")
blob += b"\xff" * (DIR_OFF - len(blob))
d = struct.pack("<4sI", b"HLDX", len(entries))
for off, n, name in entries:
    d += struct.pack("<II40s", off, n, name.encode()[:39])
blob += d
open(out_path, "wb").write(blob)
for i, (off, n, name) in enumerate(entries):
    print(f"#{i} 0x{off:06x} {n:9d} {name}")
print("frames:", ", ".join(names))
print(f"{out_path}: {len(blob)} bytes (directory at 0x{DIR_OFF:06x})")
