"""Evaluate float (TFLite vs ONNX), quantized (ESP-PPQ simulation) and rule gestures.

usage: python -I evaluate.py <hand_landmark_lite.tflite> <float.onnx> <data_dir> <quant_dir> <variant>...

Reads <data_dir>/eval.npz and <quant_dir>/<variant>.preds.npz, prints tables and
writes <quant_dir>/results.json.
"""
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hlm_common import GESTURES, OrtRunner, S, TfliteRunner, kp_err_pct, to_image
from gesture_rules import classify

tfl_path, onnx_path, data_dir, qdir = sys.argv[1:5]
variants = sys.argv[5:]
ev = np.load(os.path.join(data_dir, "eval.npz"))
crops, Ms, refs = ev["crops"], ev["M"], ev["refs"]
meta = json.loads(str(ev["meta"]))
N = len(crops)
results = {}


def ref_in_crop(i):
    r = refs[meta[i]["src"]][:, :2]
    M = Ms[i]
    return r @ M[:, :2].T + M[:, 2]


def hand_side_px(i):
    """long side of the reference landmark bbox, in crop px"""
    r = ref_in_crop(i)
    return float(max(np.ptp(r[:, 0]), np.ptp(r[:, 1])))


# ---------------------------------------------------------------- float: TFLite vs ONNX
tfl, ort_ = TfliteRunner(tfl_path), OrtRunner(onnx_path)
P = {"tflite": [], "float": []}
for c in crops:
    P["tflite"].append(tfl(c))
    P["float"].append(ort_(c))
d_lm = max(float(np.abs(a[0] - b[0]).max()) for a, b in zip(P["tflite"], P["float"]))
d_pr = max(abs(a[1] - b[1]) for a, b in zip(P["tflite"], P["float"]))
d_hd = max(abs(a[2] - b[2]) for a, b in zip(P["tflite"], P["float"]))
print(f"\n## TFLite vs ONNX float ({N} crops): max |dlandmark| {d_lm:.4g} px (of 224), max |dpresence| {d_pr:.3g}, max |dhandedness| {d_hd:.3g}")
results["tflite_vs_onnx"] = dict(n=N, max_lm_px=d_lm, max_presence=d_pr, max_handedness=d_hd)

# ---------------------------------------------------------------- quantized predictions
for v in variants:
    q = np.load(os.path.join(qdir, f"{v}.preds.npz"))
    P[v] = [(q["landmarks"][i].reshape(21, 3), float(q["presence"][i][0]), float(q["handedness"][i][0])) for i in range(N)]

models = ["float"] + variants
crop_types = sorted({m["crop"] for m in meta}, key=lambda s: (s != "rot", s))

# ---------------------------------------------------------------- float accuracy vs reference by crop type
print("\n## Float model vs reference landmarks (mean point error)")
print("| crop | sources | ref | % crop side | % hand size | presence (min/median) |")
print("|---|---|---|---|---|---|")
results["float_vs_ref"] = {}
for kind in ("gt", "pseudo"):
    for ct in crop_types:
        idx = [i for i in range(N) if meta[i]["crop"] == ct and meta[i]["ref_kind"] == kind]
        if kind == "pseudo" and ct == "rot":
            continue  # reference was defined on this crop
        e = [kp_err_pct(P["float"][i][0], ref_in_crop(i)) for i in idx]
        eh = [np.linalg.norm(P["float"][i][0][:, :2] - ref_in_crop(i), axis=1).mean() / hand_side_px(i) * 100 for i in idx]
        pr = [P["float"][i][1] for i in idx]
        print(f"| {ct} | {len(idx)} | {kind} | {np.mean(e):.2f} | {np.mean(eh):.2f} | {min(pr):.3f} / {np.median(pr):.3f} |")
        results["float_vs_ref"][f"{kind}_{ct}"] = dict(n=len(idx), pct_crop=float(np.mean(e)), pct_hand=float(np.mean(eh)),
                                                      presence_min=float(min(pr)))

# ---------------------------------------------------------------- quantized vs float
print("\n## Quantized (ESP-PPQ simulation) vs float, mean point error as % of crop side")
print("| variant | all crops mean | median | p90 | max | rot | rot max | aa13 | aa15 | aa20 | float-confident crops (presence>0.9) mean / max | max |dpresence| (rot) | handedness flips |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
results["quant_vs_float"] = {}
for v in variants:
    e = np.array([kp_err_pct(P[v][i][0], P["float"][i][0]) for i in range(N)])
    by = {ct: float(np.mean([e[i] for i in range(N) if meta[i]["crop"] == ct])) for ct in crop_types}
    rot_idx = [i for i in range(N) if meta[i]["crop"] == "rot"]
    conf = [i for i in range(N) if P["float"][i][1] > 0.9]
    dp = max(abs(P[v][i][1] - P["float"][i][1]) for i in rot_idx)
    flips = sum((P[v][i][2] > 0.5) != (P["float"][i][2] > 0.5) for i in range(N))
    print(f"| {v} | {e.mean():.2f} | {np.median(e):.2f} | {np.percentile(e, 90):.2f} | {e.max():.2f} | {by['rot']:.2f} | "
          f"{max(e[i] for i in rot_idx):.2f} | " + " | ".join(f"{by.get(ct, float('nan')):.2f}" for ct in ["aa13", "aa15", "aa20"])
          + f" | {np.mean(e[conf]):.2f} / {np.max(e[conf]):.2f} (n={len(conf)}) | {dp:.3f} | {flips}/{N} |")
    results["quant_vs_float"][v] = dict(mean=float(e.mean()), median=float(np.median(e)), p90=float(np.percentile(e, 90)),
                                        max=float(e.max()), by_crop=by, rot_max=float(max(e[i] for i in rot_idx)), confident_mean=float(np.mean(e[conf])),
                                        confident_max=float(np.max(e[conf])), n_confident=len(conf),
                                        max_dpresence_rot=float(dp), handedness_flips=int(flips))

# ---------------------------------------------------------------- presence / handedness sanity
print("\n## Presence / handedness (float and quantized)")
print("| model | presence min (all) | presence median (all) | presence min (rot) | handedness>0.5 original / mirrored (rot) | mirror pairs with opposite handedness (rot) | agrees with float (rot) |")
print("|---|---|---|---|---|---|---|")
for m in models:
    pr = [P[m][i][1] for i in range(N)]
    rot = [i for i in range(N) if meta[i]["crop"] == "rot"]
    orig = [P[m][i][2] > 0.5 for i in rot if not meta[i]["name"].endswith("_mir")]
    mir = [P[m][i][2] > 0.5 for i in rot if meta[i]["name"].endswith("_mir")]
    byname = {meta[i]["name"]: P[m][i][2] for i in rot}
    pairs = [n for n in byname if not n.endswith("_mir")]
    opp = sum((byname[n] > 0.5) != (byname[n + "_mir"] > 0.5) for n in pairs)
    agree = sum((P[m][i][2] > 0.5) == (P["float"][i][2] > 0.5) for i in rot)
    print(f"| {m} | {min(pr):.3f} | {np.median(pr):.3f} | {min(P[m][i][1] for i in rot):.3f} | {sum(orig)}/{len(orig)} , {sum(mir)}/{len(mir)} | "
          f"{opp}/{len(pairs)} | {agree}/{len(rot)} |")

# ---------------------------------------------------------------- rule-based gestures
print("\n## Rule-based gestures: accuracy (labelled hands; fist = expect 0)")
print("| model | " + " | ".join(crop_types) + " |")
print("|---|" + "---|" * len(crop_types))
results["gestures"] = {}
fails = {}
for m in models:
    row = []
    for ct in crop_types:
        idx = [i for i in range(N) if meta[i]["crop"] == ct]
        ok = 0
        for i in idx:
            lm_img = to_image(P[m][i][0], Ms[i])
            g = classify(lm_img)
            if g == meta[i]["label"]:
                ok += 1
            else:
                fails.setdefault((m, ct), []).append(f"{meta[i]['name']}: {GESTURES.get(meta[i]['label'], 'none')}->{GESTURES.get(g, 'none')}")
        row.append(f"{ok}/{len(idx)}")
        results["gestures"][f"{m}_{ct}"] = [ok, len(idx)]
    print(f"| {m} | " + " | ".join(row) + " |")
# reference landmarks themselves (upper bound of the rules)
idx = [i for i in range(N) if meta[i]["crop"] == "rot"]
ok = 0
for i in idx:
    g = classify(refs[meta[i]["src"]])
    ok += g == meta[i]["label"]
    if g != meta[i]["label"]:
        fails.setdefault(("reference", "-"), []).append(f"{meta[i]['name']}: {GESTURES.get(meta[i]['label'], 'none')}->{GESTURES.get(g, 'none')}")
print(f"| reference landmarks | {ok}/{len(idx)} |")
results["gestures"]["reference"] = [int(ok), len(idx)]
print("\nfailures:")
for k, v in fails.items():
    print(" ", k, "; ".join(v))
results["gesture_failures"] = {f"{a}/{b}": v for (a, b), v in fails.items()}
json.dump(results, open(os.path.join(qdir, "results.json"), "w"), indent=1)
