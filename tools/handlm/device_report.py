"""Summarise a phase-B bench log (firmware/main/handlm_bench.cpp, TTL log).

usage: python -I -X utf8 device_report.py <log> <float.onnx> <eval.npz> <quant_dir>

Prints: loads / memory, timing per cycle, device output on the fixed crop vs the
ESP-PPQ simulation and the float model, the per-module profile grouped by op
type and precision, and the end-to-end runs on the test frames (gesture rules
on the device landmarks).
"""
import collections
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gesture_rules import classify  # noqa: E402
from hlm_common import OrtRunner  # noqa: E402

log_path, onnx_path, eval_path, qdir = sys.argv[1:5]
VARIANT = {"dw16h16": "int8+dw16+heads16", "dw16": "int8+dw16", "auto8": "int8+auto8", "int8": "int8", "int16": "int16"}
# Test frames (batch1 + batch2, tools: gesture-frames make_frames.py); hands left -> right.
EXPECT = {0: [1], 1: [1, 2], 2: [3, 4], 3: [5, 6], 4: [7, 8], 5: [9], 6: None, 7: None}
GNAME = {0: "none", 1: "one", 2: "two", 3: "three", 4: "four", 5: "five", 6: "like", 7: "ok", 8: "call", 9: "dislike"}

text = open(log_path, encoding="utf-8", errors="replace").read()
text = re.sub(r"\x1b\[[0-9;]*m", "", text)
lines = text.splitlines()

names = {}
for m in re.finditer(r"storage #(\d+) 0x\w+ (\d+) B (\w+) (\S+)", text):
    names[int(m.group(1))] = m.group(4)


def base(idx):
    return names.get(idx, "?").split(":")[0]


print("## loads")
for l in lines:
    if re.search(r"hlm: (load|unload|mem #|bench start|cycle|before boot|test c|input )", l) or "boot done" in l \
            or "gesture ready" in l or "previous run crashed" in l:
        print(l.split(") ", 1)[-1] if ") hlm:" in l or ") mxcam:" in l else l)

print("\n## runs")
for l in lines:
    if "hlm: run c" in l or "hlm: runs c" in l:
        print(l.split(") ", 1)[-1])

ev = np.load(eval_path)
ort = OrtRunner(onnx_path)
flm, fpres, fhd = ort(ev["crops"][0])
print("\n## fixed crop (eval crop 0 = grid_one/rot) vs ESP-PPQ simulation and float (mean / max point error, px of 224)")
print("| cycle | model | vs PPQ sim | vs float | presence dev / sim / float | handedness dev / sim |")
print("|---|---|---|---|---|---|")
for m in re.finditer(r"out c(\d+) #(\d+) p=([-\d.]+) hd=([-\d.]+):((?: [-\d.]+,[-\d.]+){21})", text):
    cyc, idx = int(m.group(1)), int(m.group(2))
    pts = np.array([[float(a) for a in p.split(",")] for p in m.group(5).split()])
    v = VARIANT.get(base(idx))
    q = np.load(os.path.join(qdir, f"{v}.preds.npz"))
    qlm = q["landmarks"][0].reshape(21, 3)[:, :2]
    ds = np.linalg.norm(pts - qlm, axis=1)
    df = np.linalg.norm(pts - flm[:, :2], axis=1)
    print(f"| {cyc} | #{idx} {names.get(idx)} | {ds.mean():.3f} / {ds.max():.3f} | {df.mean():.2f} / {df.max():.2f} "
          f"| {float(m.group(3)):.4f} / {float(q['presence'][0][0]):.4f} / {fpres:.4f} "
          f"| {float(m.group(4)):.4f} / {float(q['handedness'][0][0]):.4f} |")

print("\n## profile (first cycle), grouped by op type and precision")
prof = collections.defaultdict(list)
for m in re.finditer(r"prof #(\d+)\|([^|]*)\|([^|]*)\|(\d+)", text):
    prof[int(m.group(1))].append((m.group(2), m.group(3), int(m.group(4))))
for idx, rows in prof.items():
    v = VARIANT.get(base(idx))
    plat = {}
    lp = os.path.join(qdir, f"{v}.layers.txt")
    if os.path.exists(lp):
        for l in open(lp, encoding="utf-8"):
            p = l.rstrip("\n").split("\t")
            if len(p) == 3:
                plat[p[0]] = "16" if "INT16" in p[2] else "8"
    total = sum(r[2] for r in rows if r[0] == "total")
    agg = collections.Counter()
    cnt = collections.Counter()
    for name, typ, us in rows:
        if name == "total":
            continue
        key = typ + ("/int" + plat[name] if name in plat and typ in ("Conv", "Gemm") else "")
        agg[key] += us
        cnt[key] += 1
    print(f"\n#{idx} {names.get(idx)}: total {total / 1000:.1f} ms, {sum(cnt.values())} modules")
    print("| op | n | ms | % |")
    print("|---|---|---|---|")
    for k, us in agg.most_common():
        print(f"| {k} | {cnt[k]} | {us / 1000:.1f} | {us / total * 100:.1f} |")

print("\n## end to end (test frames f0-f7, live = last)")
e2e = re.compile(r"e2e c(\d+) #(\d+) f(\d+) h(\d+) box=(-?\d+),(-?\d+) (\d+)x(\d+) .*?acq passes=(\d+) (\d+) ms \(run (\d+), warp (\d+)\) "
                 r"p0=([-\d.]+) p90=([-\d.]+) pm90=([-\d.]+) best=(\d) pA=([-\d.]+) \| track (\d+) ms \(run (\d+), warp (\d+)\) pT=([-\d.]+)")
lmre = re.compile(r"(lmA|lmT) c(\d+) #(\d+) f(\d+) h(\d+) p=([-\d.]+) hd=([-\d.]+):((?: [-\d.]+,[-\d.]+){21})")
lms = {}
for m in lmre.finditer(text):
    pts = np.array([[float(a) for a in p.split(",")] for p in m.group(8).split()])
    lms[(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))] = pts
rows = collections.defaultdict(list)
for m in e2e.finditer(text):
    cyc, idx, f, h = (int(m.group(i)) for i in (1, 2, 3, 4))
    rows[(cyc, idx)].append(dict(f=f, h=h, x=int(m.group(5)), passes=int(m.group(9)), acq=int(m.group(10)),
                                 acq_run=int(m.group(11)), acq_warp=int(m.group(12)), p0=float(m.group(13)),
                                 pA=float(m.group(17)), trk=int(m.group(18)), trk_run=int(m.group(19)),
                                 trk_warp=int(m.group(20)), pT=float(m.group(21)),
                                 gA=classify(lms[("lmA", cyc, idx, f, h)]) if ("lmA", cyc, idx, f, h) in lms else None,
                                 gT=classify(lms[("lmT", cyc, idx, f, h)]) if ("lmT", cyc, idx, f, h) in lms else None))
for (cyc, idx), rs in sorted(rows.items()):
    print(f"\ncycle {cyc} #{idx} {names.get(idx)}")
    print("| frame | hand (x) | expected | passes | acq ms (run/warp) | p0 | pA | rule acq | track ms (run/warp) | pT | rule track |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    ok_a = ok_t = n = 0
    byf = collections.defaultdict(list)
    for r in rs:
        byf[r["f"]].append(r)
    for f in sorted(byf):
        hs = sorted(byf[f], key=lambda r: r["x"])
        exp = EXPECT.get(f)
        for j, r in enumerate(hs):
            e = exp[j] if exp and j < len(exp) and len(hs) == len(exp) else None
            if e is not None:
                n += 1
                ok_a += r["gA"] == e
                ok_t += r["gT"] == e
            print(f"| f{f} | h{r['h']} ({r['x']}) | {GNAME.get(e, '?') if e is not None else '-'} | {r['passes']} | "
                  f"{r['acq']} ({r['acq_run']}/{r['acq_warp']}) | {r['p0']:.3f} | {r['pA']:.3f} | {GNAME.get(r['gA'])} | "
                  f"{r['trk']} ({r['trk_run']}/{r['trk_warp']}) | {r['pT']:.3f} | {GNAME.get(r['gT'])} |")
    if n:
        print(f"rules: acquired {ok_a}/{n}, tracking {ok_t}/{n}; mean passes {np.mean([r['passes'] for r in rs]):.2f}")
for l in lines:
    if "hlm: det c" in l:
        print(l.split(") ", 1)[-1])
