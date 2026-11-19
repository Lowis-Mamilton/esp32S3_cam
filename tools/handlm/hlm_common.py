"""Shared helpers for the MXCam hand-landmark prototype (phase A, PC only).

Crops are 224x224 RGB float32 in [0, 1] (what hand_landmark_lite expects).
Every crop carries a 2x3 affine `M` that maps image pixels -> crop pixels, so
landmarks predicted in crop pixels can be mapped back to the image.
"""
import math
import os
import re

import numpy as np
from PIL import Image

S = 224  # model input side

# Gesture IDs used by MXCam feature 8 (docs/SPEC.md, ADR-0019).
GESTURES = {1: "one", 2: "two", 3: "three", 4: "four", 5: "five",
            6: "like", 7: "ok", 8: "call", 9: "dislike"}
GESTURE_ID = {v: k for k, v in GESTURES.items()}

# Tiles of examples_hand_gesture_recognition_gestures.png (esp-dl example image),
# same boxes as the M8a bench frame generator.
GRID_TILES = {
    "one": (58, 84, 322, 348), "two": (342, 84, 604, 345), "three": (614, 82, 881, 349),
    "four": (894, 82, 1162, 348), "five": (1184, 80, 1451, 348),
    "like": (156, 460, 428, 732), "ok": (459, 458, 733, 732), "call": (762, 455, 1040, 732),
    "dislike": (1065, 455, 1343, 732),
}


def imread_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"))


def invert_affine(M):
    A = np.linalg.inv(np.vstack([M, [0, 0, 1]]))
    return A[:2]


def read_mp_landmarks(path, w, h):
    """MediaPipe test-data *_landmarks.pbtxt -> (21,3) array in image pixels (z scaled by w)."""
    txt = open(path, encoding="utf-8").read()
    body = txt[txt.index("landmarks {"):]
    xs = [float(v) for v in re.findall(r"\bx:\s*([-0-9.e]+)", body)][:21]
    ys = [float(v) for v in re.findall(r"\by:\s*([-0-9.e]+)", body)][:21]
    zs = [float(v) for v in re.findall(r"\bz:\s*([-0-9.e]+)", body)][:21]
    lab = re.search(r'label:\s*"(\w+)"', txt)
    return np.stack([np.array(xs) * w, np.array(ys) * h, np.array(zs) * w], 1), (lab.group(1) if lab else None)


# ---------------------------------------------------------------- crops

def rect_affine(cx, cy, side, angle_rad=0.0):
    """Affine image->crop for a square of `side` px centred at (cx, cy), rotated by
    angle (rad, image rotated so that the rect's 'up' becomes crop 'up')."""
    s = S / side
    c, si = math.cos(angle_rad), math.sin(angle_rad)
    # crop = s * R(-angle) * (p - centre) + S/2
    M = np.array([[s * c, s * si, 0.0], [-s * si, s * c, 0.0]], dtype=np.float64)
    M[:, 2] = np.array([S / 2, S / 2]) - M[:, :2] @ np.array([cx, cy])
    return M


def warp(img, M, border=(0, 0, 0)):
    """Bilinear affine crop (image -> SxS), constant border; returns float32 [0,1]."""
    Minv = invert_affine(M)
    im = Image.fromarray(np.ascontiguousarray(img))
    out = im.transform((S, S), Image.Transform.AFFINE, data=tuple(Minv.ravel()),
                       resample=Image.Resampling.BILINEAR, fillcolor=border)
    return np.asarray(out, dtype=np.float32) / 255.0


def to_image(pts_crop, M):
    """(N,2+) crop pixels -> image pixels."""
    Minv = invert_affine(M)
    p = pts_crop[:, :2]
    return p @ Minv[:, :2].T + Minv[:, 2]


def axis_aligned_from_box(x0, y0, x1, y1, scale):
    """What MXCam can do with a hand_detect box: square, not rotated, side = long edge * scale."""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    side = max(x1 - x0, y1 - y0) * scale
    return rect_affine(cx, cy, side, 0.0)


def detector_like_box(lm_img, pad=0.10):
    """Proxy for a hand_detect box: bbox of the 21 landmarks padded by `pad` of each side
    (landmarks sit inside the hand silhouette)."""
    x0, y0 = lm_img[:, 0].min(), lm_img[:, 1].min()
    x1, y1 = lm_img[:, 0].max(), lm_img[:, 1].max()
    w, h = x1 - x0, y1 - y0
    return x0 - pad * w, y0 - pad * h, x1 + pad * w, y1 + pad * h


# MediaPipe hand_landmark_landmarks_to_roi.pbtxt: partial landmarks, rotation 0->9
# (target 90 deg), RectTransformation scale 2.0, shift_y -0.1, square_long.
ROI_IDX = [0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18]


def mediapipe_roi_from_landmarks(lm_img):
    p = lm_img[ROI_IDX, :2]
    x0, y0 = lm_img[0, :2]
    x1, y1 = lm_img[9, :2]
    rot = math.pi / 2 - math.atan2(-(y1 - y0), x1 - x0)
    rot = rot - 2 * math.pi * math.floor((rot + math.pi) / (2 * math.pi))
    c, s = math.cos(rot), math.sin(rot)
    # rotate points around (x0, y0) by -rot to get them in the hand frame
    d = p - np.array([x0, y0])
    rx = d[:, 0] * c + d[:, 1] * s
    ry = -d[:, 0] * s + d[:, 1] * c
    cxr, cyr = (rx.min() + rx.max()) / 2, (ry.min() + ry.max()) / 2
    w, h = rx.max() - rx.min(), ry.max() - ry.min()
    # back to image frame
    cx = x0 + cxr * c - cyr * s
    cy = y0 + cxr * s + cyr * c
    # RectTransformation: shift_y -0.1 (in rect frame), scale 2.0, square long
    shift = -0.1 * h
    cx += -shift * s  # dx = -h*shift_y*sin(rot)
    cy += shift * c   # dy =  h*shift_y*cos(rot)
    side = max(w, h) * 2.0
    return cx, cy, side, rot


def mediapipe_roi_affine(lm_img):
    cx, cy, side, rot = mediapipe_roi_from_landmarks(lm_img)
    return rect_affine(cx, cy, side, rot)


# ---------------------------------------------------------------- metrics

def kp_err_pct(a_crop, b_crop):
    """Mean 2-D distance between two (21,2+) landmark sets in crop px, as % of crop side."""
    d = np.linalg.norm(a_crop[:, :2] - b_crop[:, :2], axis=1)
    return float(d.mean() / S * 100.0)


def palm_size(lm):
    return float(np.linalg.norm(lm[0, :2] - lm[9, :2]))


def here(*p):
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), *p)


# ---------------------------------------------------------------- runners
# Every runner maps a (224,224,3) float crop -> (lm (21,3) crop px, presence, handedness).

class OrtRunner:
    def __init__(self, onnx_path):
        import onnxruntime as ort
        self.s = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
        self.inp = self.s.get_inputs()[0].name
        self.outs = [o.name for o in self.s.get_outputs()]

    def __call__(self, crop):
        x = np.ascontiguousarray(crop.transpose(2, 0, 1)[None])
        r = dict(zip(self.outs, self.s.run(None, {self.inp: x})))
        return r["landmarks"].reshape(21, 3), float(r["presence"].ravel()[0]), float(r["handedness"].ravel()[0])


class TfliteRunner:
    def __init__(self, tflite_path):
        from ai_edge_litert.interpreter import Interpreter
        self.it = Interpreter(model_path=tflite_path)
        self.it.allocate_tensors()
        self.inp = self.it.get_input_details()[0]["index"]
        self.od = {d["name"]: d["index"] for d in self.it.get_output_details()}

    def __call__(self, crop):
        self.it.set_tensor(self.inp, crop[None].astype(np.float32))
        self.it.invoke()
        g = lambda n: self.it.get_tensor(self.od[n])
        return g("Identity").reshape(21, 3), float(g("Identity_1").ravel()[0]), float(g("Identity_2").ravel()[0])


# ---------------------------------------------------------------- sample hands

def load_sources(run, gdir, mpdir, verbose=True):
    """Sample hands (each also mirrored). run = float model runner (for pseudo ground truth).
    Returns dicts: name, img, label, ref (21,3 image px), ref_kind gt|pseudo, grid, boot_presence."""
    raw = []  # (name, image, label, init_box or None, gt_landmarks or None)
    grid = imread_rgb(os.path.join(gdir, "examples_hand_gesture_recognition_gestures.png"))
    for name, box in GRID_TILES.items():
        raw.append(("grid_" + name, grid, GESTURE_ID[name], box, None))
    raw.append(("gesturejpg_one", imread_rgb(os.path.join(gdir, "examples_hand_gesture_recognition_gestures_gesture.jpg")),
                1, (55, 260, 235, 440), None))
    for fn, lab in [("thumb_up", 6), ("pointing_up", 1), ("pointing_up_rotated", 1), ("victory", 2), ("fist", 0)]:
        img = imread_rgb(os.path.join(mpdir, fn + ".jpg"))
        gt, _ = read_mp_landmarks(os.path.join(mpdir, fn + "_landmarks.pbtxt"), img.shape[1], img.shape[0])
        raw.append(("mp_" + fn, img, lab, None, gt))

    def bootstrap(img, box):
        x0, y0, x1, y1 = box
        M = rect_affine((x0 + x1) / 2, (y0 + y1) / 2, max(x1 - x0, y1 - y0), 0.0)
        for _ in range(4):
            lm, pres, _ = run(warp(img, M))
            M = mediapipe_roi_affine(to_image(lm, M))
        lm, pres, hand = run(warp(img, M))
        return np.concatenate([to_image(lm, M), lm[:, 2:]], 1), pres

    sources = []
    for name, img, lab, box, gt in raw:
        for mirror in (False, True):
            im = np.ascontiguousarray(img[:, ::-1]) if mirror else img
            w = im.shape[1]
            if gt is not None:
                ref = gt.copy()
                if mirror:
                    ref[:, 0] = w - ref[:, 0]
                pres, kind = 1.0, "gt"
            else:
                b = box
                if mirror:
                    b = (w - box[2], box[1], w - box[0], box[3])
                ref, pres = bootstrap(im, b)
                kind = "pseudo"
            sources.append(dict(name=name + ("_mir" if mirror else ""), img=im, label=lab, ref=ref, ref_kind=kind,
                                grid=name.startswith("grid_"), boot_presence=float(pres)))
            if verbose:
                print(f"{sources[-1]['name']:28s} label={lab} ref={kind} presence={pres:.3f}")
    return sources


class PpqRunner:
    """ESP-PPQ simulated quantized inference from a .native graph written by quantize.py."""

    def __init__(self, native_path):
        import torch
        from esp_ppq.api import load_native_graph
        from esp_ppq.executor import TorchExecutor
        self.torch = torch
        g = load_native_graph(native_path)
        self.ex = TorchExecutor(graph=g, device="cpu")
        self.names = list(g.outputs.keys())

    def __call__(self, crop):
        x = self.torch.from_numpy(np.ascontiguousarray(crop.transpose(2, 0, 1)[None]))
        r = {n: o.detach().cpu().numpy().ravel() for n, o in zip(self.names, self.ex.forward(inputs=x))}
        return r["landmarks"].reshape(21, 3), float(r["presence"][0]), float(r["handedness"][0])


def make_runner(path):
    return PpqRunner(path) if path.endswith(".native") else OrtRunner(path)
