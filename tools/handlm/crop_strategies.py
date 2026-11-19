"""Compare crop strategies starting from an axis-aligned detector-like box (what MXCam has).

usage: python -I crop_strategies.py <float.onnx> <gesture_imgs_dir> <mp_testdata_dir> [scale=1.5] [presence_retry=0.9] [model]

model = float ONNX (default) or a quantized <variant>.native from quantize.py
(reference landmarks always come from the float model / MediaPipe ground truth).

  single      one pass on the axis-aligned square crop
  refine      single, then MediaPipe ROI (rotated) from its landmarks, second pass
  rot3        try 0/+90/-90 deg of the same square, keep the highest presence (3 passes)
  adaptive    single; only if presence < retry threshold also try +90/-90 (1 or 3 passes)
  adaptive+r  adaptive, then a refine pass (2 or 4 passes)
Reports mean point error vs reference landmarks (% of hand size = long side of the
reference landmark bbox), gesture accuracy and the mean number of model passes.
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hlm_common import OrtRunner, detector_like_box, load_sources, make_runner, mediapipe_roi_affine, rect_affine, to_image, warp
from gesture_rules import classify

onnx_path, gdir, mpdir = sys.argv[1:4]
scale = float(sys.argv[4]) if len(sys.argv) > 4 else 1.5
retry = float(sys.argv[5]) if len(sys.argv) > 5 else 0.9
ref_run = OrtRunner(onnx_path)
run = make_runner(sys.argv[6]) if len(sys.argv) > 6 else ref_run


def infer(img, M):
    lm, pres, hand = run(warp(img, M))
    return to_image(lm, M), pres


def strategies(img, ref):
    x0, y0, x1, y1 = detector_like_box(ref)
    cx, cy, side = (x0 + x1) / 2, (y0 + y1) / 2, max(x1 - x0, y1 - y0) * scale
    out = {}
    lm0, p0 = infer(img, rect_affine(cx, cy, side, 0.0))
    out["single"] = (lm0, 1, p0)
    lmr, pr = infer(img, mediapipe_roi_affine(lm0))
    out["refine"] = (lmr, 2, pr)
    cands = [(lm0, p0)] + [infer(img, rect_affine(cx, cy, side, a)) for a in (math.pi / 2, -math.pi / 2)]
    best = max(cands, key=lambda t: t[1])
    out["rot3"] = (best[0], 3, best[1])
    ad, n, pa = (lm0, 1, p0) if p0 >= retry else (best[0], 3, best[1])
    out["adaptive"] = (ad, n, pa)
    lmar, par = infer(img, mediapipe_roi_affine(ad))
    out["adaptive+r"] = (lmar, n + 1, par)
    return out


sources = load_sources(ref_run, gdir, mpdir, verbose=False)
names = ["single", "refine", "rot3", "adaptive", "adaptive+r"]
acc = {k: [] for k in names}
err = {k: [] for k in names}
passes = {k: [] for k in names}
fails = {k: [] for k in names}
for s in sources:
    ref = s["ref"][:, :2]
    hand = max(np.ptp(ref[:, 0]), np.ptp(ref[:, 1]))
    for k, (lm, n, p) in strategies(s["img"], s["ref"]).items():
        e = np.linalg.norm(lm - ref, axis=1).mean() / hand * 100
        g = classify(lm)
        acc[k].append(g == s["label"])
        err[k].append(e)
        passes[k].append(n)
        if g != s["label"]:
            fails[k].append(f"{s['name']}({g})")
print(f"model {sys.argv[6] if len(sys.argv) > 6 else onnx_path}, crop scale {scale}, retry threshold {retry}, {len(sources)} hands")
print("| strategy | mean err % hand | max err % hand | gestures | mean passes | failures |")
print("|---|---|---|---|---|---|")
for k in names:
    print(f"| {k} | {np.mean(err[k]):.1f} | {np.max(err[k]):.1f} | {sum(acc[k])}/{len(acc[k])} | {np.mean(passes[k]):.2f} | {', '.join(fails[k]) or '-'} |")
