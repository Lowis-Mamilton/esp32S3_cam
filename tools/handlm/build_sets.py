"""Build the sample hands, evaluation crops and calibration crops.

usage: python -I build_sets.py <float.onnx> <gesture_imgs_dir> <mp_testdata_dir> <out_dir>

Sources (each also mirrored left<->right):
  * 9 tiles of esp-dl examples/hand_gesture_recognition/gestures.png (labels 1-9)
  * esp-dl examples/hand_gesture_recognition/gesture.jpg (person showing "one")
  * MediaPipe test assets thumb_up / pointing_up / pointing_up_rotated / victory / fist
    (with MediaPipe's reference landmarks)
Reference landmarks for sources without ground truth = float model on the ideal
(MediaPipe-style rotated) crop, iterated from a rough box (pseudo ground truth).

Outputs in <out_dir>: sources.json, eval.npz (crops, affines, meta), calib_all.npy,
calib_holdout.npy (holdout = without the 9 grid tiles, to test generalisation).
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hlm_common import OrtRunner, axis_aligned_from_box, detector_like_box, load_sources, mediapipe_roi_affine, warp, S

onnx_path, gdir, mpdir, out = sys.argv[1:5]
os.makedirs(out, exist_ok=True)
run = OrtRunner(onnx_path)

sources = load_sources(run, gdir, mpdir)

# ------------------------------------------------------------ eval crops
crops, Ms, meta = [], [], []
for si, s in enumerate(sources):
    variants = {"rot": mediapipe_roi_affine(s["ref"])}
    box = detector_like_box(s["ref"])
    for k in (1.3, 1.5, 2.0):
        variants[f"aa{int(k * 10)}"] = axis_aligned_from_box(*box, k)
    for v, M in variants.items():
        crops.append(warp(s["img"], M))
        Ms.append(M)
        meta.append(dict(src=si, name=s["name"], crop=v, label=s["label"], grid=s["grid"], ref_kind=s["ref_kind"]))
np.savez_compressed(os.path.join(out, "eval.npz"), crops=np.stack(crops).astype(np.float32), M=np.stack(Ms),
                    meta=json.dumps(meta), refs=np.stack([s["ref"] for s in sources]))
json.dump([{k: (v.tolist() if isinstance(v, np.ndarray) else v) for k, v in s.items() if k != "img"} for s in sources],
          open(os.path.join(out, "sources.json"), "w"), indent=1)
print("eval crops:", len(crops))

# ------------------------------------------------------------ calibration crops


def augment(rng, s):
    if rng.random() < 0.5:
        M = axis_aligned_from_box(*detector_like_box(s["ref"]), rng.uniform(1.15, 1.9))
    else:
        M = mediapipe_roi_affine(s["ref"])
        # scale the rect around its centre
        sc = rng.uniform(0.85, 1.25)
        M = np.vstack([M, [0, 0, 1]])
        C = np.array([[1 / sc, 0, S / 2 * (1 - 1 / sc)], [0, 1 / sc, S / 2 * (1 - 1 / sc)], [0, 0, 1]])
        M = (C @ M)[:2]
    # extra rotation + shift in crop space
    ang = np.radians(rng.uniform(-30, 30))
    ca, sa = np.cos(ang), np.sin(ang)
    R = np.array([[ca, sa, 0.0], [-sa, ca, 0.0]])
    R[:, 2] = S / 2 - R[:, :2] @ np.array([S / 2, S / 2]) + rng.normal(0, 0.07 * S, 2)
    M = (np.vstack([R, [0, 0, 1]]) @ np.vstack([M, [0, 0, 1]]))[:2]
    c = warp(s["img"], M)
    if rng.random() < 0.2:
        c = np.asarray(Image.fromarray((c * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.0)),
                       dtype=np.float32) / 255.0
    c = c * rng.uniform(0.55, 1.35) * rng.uniform(0.9, 1.1, 3)
    m = c.mean()
    c = (c - m) * rng.uniform(0.7, 1.3) + m
    c = c + rng.normal(0, rng.uniform(0, 0.02), c.shape)
    return np.clip(c, 0, 1).astype(np.float32)


rng = np.random.default_rng(1234)
for tag, pool, per in [("all", sources, 10), ("holdout", [s for s in sources if not s["grid"]], 25)]:
    arr = np.stack([augment(rng, s) for s in pool for _ in range(per)]).transpose(0, 3, 1, 2)
    np.save(os.path.join(out, f"calib_{tag}.npy"), np.ascontiguousarray(arr))
    print(f"calib_{tag}: {arr.shape}")
# contact sheet of a few calibration crops for eyeballing
cal = np.load(os.path.join(out, "calib_all.npy"))
sheet = np.concatenate([np.concatenate(list(cal[r * 10:(r + 1) * 10].transpose(0, 2, 3, 1)), 1) for r in range(0, 30, 6)], 0)
Image.fromarray((sheet * 255).astype(np.uint8)).save(os.path.join(out, "calib_sheet.jpg"))
