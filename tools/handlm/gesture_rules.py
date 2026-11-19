"""Rule-based mapping from 21 hand landmarks to MXCam gesture IDs (feature 8).

IDs: 1-5 fingers, 6 like, 7 ok, 8 call, 9 dislike, 0 = hand, no gesture.
Input: (21, 2+) landmarks in *image* pixels, image upright and not mirrored
(MXCam recognises on the real image, ADR-0010). Only geometry, no model.

Landmark indices (MediaPipe): 0 wrist; thumb 1 CMC 2 MCP 3 IP 4 tip;
index 5 MCP 6 PIP 7 DIP 8 tip; middle 9-12; ring 13-16; pinky 17-20.
"""
import math

import numpy as np

FINGERS = {"index": (5, 6, 7, 8), "middle": (9, 10, 11, 12), "ring": (13, 14, 15, 16), "pinky": (17, 18, 19, 20)}

# Thresholds (palm = |wrist - middle MCP|). Chosen from anatomy, then checked on
# the sample set; see docs/research/hand-landmarks-proto-a.md for the caveats.
EXT_RATIO = 1.12      # finger extended: |tip-wrist| > EXT_RATIO * |pip-wrist|
EXT_ANGLE = 120.0     # ... and PIP joint angle (MCP-PIP-TIP) > this, degrees
THUMB_OUT = 0.55      # thumb extended: |thumb tip - index MCP| > THUMB_OUT * palm
THUMB_ANGLE = 130.0   # ... and thumb MCP-IP-TIP angle > this
THUMB_BEYOND = 1.05   # ... and |tip - pinky MCP| > THUMB_BEYOND * |IP - pinky MCP|
OK_TOUCH = 0.40       # ok: |thumb tip - index tip| < OK_TOUCH * palm
THUMB_VERT = 0.45     # like/dislike: |dy| of thumb (MCP->tip) > THUMB_VERT * palm


def _angle(a, b, c):
    v1, v2 = a - b, c - b
    cosv = float(np.dot(v1, v2) / (np.linalg.norm(v1) * np.linalg.norm(v2) + 1e-9))
    return math.degrees(math.acos(max(-1.0, min(1.0, cosv))))


def features(lm):
    p = np.asarray(lm, dtype=np.float64)[:, :2]
    palm = float(np.linalg.norm(p[0] - p[9])) + 1e-9
    f = {"palm": palm}
    for name, (mcp, pip, dip, tip) in FINGERS.items():
        r = np.linalg.norm(p[tip] - p[0]) / (np.linalg.norm(p[pip] - p[0]) + 1e-9)
        ang = _angle(p[mcp], p[pip], p[tip])
        f[name + "_ratio"], f[name + "_angle"] = float(r), ang
        f[name] = bool(r > EXT_RATIO and ang > EXT_ANGLE)
    t_out = float(np.linalg.norm(p[4] - p[5]) / palm)
    t_ang = _angle(p[2], p[3], p[4])
    t_beyond = float(np.linalg.norm(p[4] - p[17]) / (np.linalg.norm(p[3] - p[17]) + 1e-9))
    f.update(thumb_out=t_out, thumb_angle=t_ang, thumb_beyond=t_beyond)
    f["thumb"] = bool(t_out > THUMB_OUT and t_ang > THUMB_ANGLE and t_beyond > THUMB_BEYOND)
    f["ok_touch"] = float(np.linalg.norm(p[4] - p[8]) / palm)
    f["thumb_dy"] = float((p[4, 1] - p[2, 1]) / palm)  # +: tip below MCP (image y down)
    return f


def classify(lm):
    f = features(lm)
    idx, mid, rng, pnk, th = f["index"], f["middle"], f["ring"], f["pinky"], f["thumb"]
    n4 = idx + mid + rng + pnk
    # OK: thumb and index tips touch, at least two of middle/ring/pinky up.
    if f["ok_touch"] < OK_TOUCH and (mid + rng + pnk) >= 2:
        return 7
    # like / dislike: only the thumb, pointing clearly up or down.
    if n4 == 0 and th:
        if f["thumb_dy"] < -THUMB_VERT:
            return 6
        if f["thumb_dy"] > THUMB_VERT:
            return 9
        return 0
    # call: thumb + pinky only.
    if th and pnk and not (idx or mid or rng):
        return 8
    count = n4 + (1 if th else 0)
    return count if 1 <= count <= 5 else 0
