"""Contact sheet of eval crops with predicted landmarks (float ONNX, optionally a quantized variant).

usage: python -I viz.py <float.onnx> <data_dir> <out.jpg> [crop_type=rot] [quant_preds.npz]
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hlm_common import OrtRunner
from gesture_rules import classify
from hlm_common import GESTURES, to_image

onnx_path, data_dir, out = sys.argv[1:4]
ct = sys.argv[4] if len(sys.argv) > 4 else "rot"
qp = np.load(sys.argv[5]) if len(sys.argv) > 5 else None
ev = np.load(os.path.join(data_dir, "eval.npz"))
meta = json.loads(str(ev["meta"]))
run = OrtRunner(onnx_path)
BONES = [(0, 1), (1, 2), (2, 3), (3, 4), (0, 5), (5, 6), (6, 7), (7, 8), (5, 9), (9, 10), (10, 11), (11, 12),
         (9, 13), (13, 14), (14, 15), (15, 16), (13, 17), (0, 17), (17, 18), (18, 19), (19, 20)]
tiles = []
for i, m in enumerate(meta):
    if m["crop"] != ct:
        continue
    c = ev["crops"][i]
    lm, pres, hand = run(c)
    im = Image.fromarray((c * 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    sets = [(lm, (0, 255, 0))]
    if qp is not None:
        sets.append((qp["landmarks"][i].reshape(21, 3), (255, 0, 255)))
    for pts, col in sets:
        for a, b in BONES:
            d.line([tuple(pts[a, :2]), tuple(pts[b, :2])], fill=col, width=1)
    g = classify(to_image(lm, ev["M"][i]))
    d.text((2, 2), f"{m['name'][:20]}", fill=(255, 255, 0))
    d.text((2, 12), f"exp {GESTURES.get(m['label'], 'none')} got {GESTURES.get(g, 'none')} p={pres:.2f}", fill=(255, 255, 0))
    tiles.append(np.asarray(im))
cols = 6
while len(tiles) % cols:
    tiles.append(np.zeros_like(tiles[0]))
rows = [np.concatenate(tiles[r:r + cols], 1) for r in range(0, len(tiles), cols)]
Image.fromarray(np.concatenate(rows, 0)).save(out)
print("saved", out)
