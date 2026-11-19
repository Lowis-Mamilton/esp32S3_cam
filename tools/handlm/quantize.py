"""Quantize hand_landmark_lite ONNX with ESP-PPQ for esp32s3 and export .espdl.

usage: python -I -X utf8 quantize.py <float.onnx> <calib.npy> <eval.npz> <out_dir> <variant> [calib_steps] [notest]
(-X utf8: ESP-PPQ prints Unicode bars; -I ignores PYTHONIOENCODING.)
"notest" exports without embedded test values (that is the size that goes to flash).

variant = base[+modifier...]:
  int8        everything w8a8 (ESPDL_S3_INT8, per-tensor, power-of-two scales)
  int16       everything w16a16 (ESPDL_S3_INT16; reference / fallback)
  mixed16     = int8+heads16:   first Conv + the three FC heads in w16a16
  mixed_a16   = int8+headsa16:  first Conv + the three FC heads in w8a16 (ESPDL_S3_W8A16)
  +dw16       all depthwise Conv layers in w16a16
  +eq         cross-layer equalization (esp-dl tutorial settings)
  +mse        activation calibration 'mse' instead of 'kl'
  +tqt        TQT fine-tuning of weights/scales on the calibration set (blockwise, CPU)
  +autoN      quantize once, run ESP-PPQ layerwise error analysis on the landmark output,
              put the N worst Conv/Gemm layers into w16a16, quantize again
Writes <out_dir>/hand_landmark_lite_<variant>.espdl (+ .info/.json from the exporter)
and <out_dir>/<variant>.layers.txt (which layers were dispatched where).
Then runs ESP-PPQ's simulated quantized inference (TorchExecutor) on every eval crop
and saves <out_dir>/<variant>.preds.npz for evaluate.py, plus <variant>.native (PPQ graph). The exporter's test values
(input/outputs embedded in the .espdl) use the first eval crop, for the on-device check.
"""
import os
import sys

import numpy as np
import onnx
import torch
from torch.utils.data import DataLoader, Dataset

from esp_ppq.api import espdl_quantize_onnx, export_ppq_graph, get_target_platform
from esp_ppq.core import TargetPlatform
from esp_ppq.api.setting import QuantizationSettingFactory
from esp_ppq.executor import TorchExecutor
from esp_ppq.quantization.analyse import layerwise_error_analyse

onnx_path, calib_path, eval_path, out_dir, variant = sys.argv[1:6]
steps = int(sys.argv[6]) if len(sys.argv) > 6 else 0
with_test = not (len(sys.argv) > 7 and sys.argv[7] == "notest")
os.makedirs(out_dir, exist_ok=True)
TARGET = "esp32s3"

calib = np.load(calib_path).astype(np.float32)
steps = steps or len(calib)


class DS(Dataset):
    def __len__(self):
        return len(calib)

    def __getitem__(self, i):
        return torch.from_numpy(calib[i])


loader = DataLoader(DS(), batch_size=1, shuffle=False)


def collate(b):
    return b.to("cpu")


# work on a copy: espdl_quantize_onnx overwrites its input with the simplified model
work = os.path.join(out_dir, f"_{variant}_in.onnx")
model = onnx.load(onnx_path)
onnx.save(model, work)

convs = [n.name for n in model.graph.node if n.op_type == "Conv"]
gemms = [n.name for n in model.graph.node if n.op_type in ("Gemm", "MatMul")]
heads = gemms  # landmark / presence / handedness FC heads (world head was removed)
dwconvs = [n.name for n in model.graph.node if n.op_type == "Conv"
           and any(a.name == "group" and a.i > 1 for a in n.attribute)]

ALIASES = {"mixed16": "int8+heads16", "mixed_a16": "int8+headsa16"}
tokens = ALIASES.get(variant, variant).split("+")
base, mods = tokens[0], tokens[1:]
if base not in ("int8", "int16"):
    raise SystemExit("unknown variant " + variant)
quant_type = "w8a8" if base == "int8" else "w16a16"
P16 = get_target_platform(TARGET, "w16a16")


def make_setting(extra16=()):
    st = QuantizationSettingFactory.espdl_setting(num_of_bits=8 if base == "int8" else 16)
    for m in mods:
        if m in ("heads16", "headsa16"):
            plat = P16 if m == "heads16" else get_target_platform(TARGET, "w8a16")
            for name in [convs[0]] + heads:
                st.dispatching_table.append(name, plat)
        elif m == "dw16":
            for name in dwconvs:
                st.dispatching_table.append(name, P16)
        elif m == "eq":
            st.equalization = True
            st.equalization_setting.iterations = 4
            st.equalization_setting.value_threshold = 0.4
            st.equalization_setting.opt_level = 2
            st.equalization_setting.interested_layers = None
        elif m == "mse":
            st.quantize_activation_setting.calib_algorithm = "mse"
        elif m == "tqt":
            st.tqt_optimization = True
            st.tqt_optimization_setting.steps = 300
            st.tqt_optimization_setting.lr = 1e-6
            st.tqt_optimization_setting.block_size = 4
            st.tqt_optimization_setting.collecting_device = "cpu"
            st.tqt_optimization_setting.int_lambda = 1
            st.tqt_optimization_setting.is_scale_trainable = True
        elif m.startswith("auto"):
            pass
        else:
            raise SystemExit("unknown modifier " + m)
    for name in extra16:
        st.dispatching_table.append(name, P16)
    return st


setting = make_setting()
auto = [int(m[4:]) for m in mods if m.startswith("auto")]
if auto:
    probe = os.path.join(out_dir, f"_{variant}_probe.onnx")
    onnx.save(model, probe)
    g0 = espdl_quantize_onnx(onnx_import_file=probe, espdl_export_file=os.path.join(out_dir, "_probe.espdl"),
                             calib_dataloader=loader, calib_steps=steps, input_shape=[1, 3, 224, 224],
                             inputs=[torch.zeros(1, 3, 224, 224)], target=TARGET, quant_type=quant_type,
                             collate_fn=collate, setting=make_setting(), device="cpu", error_report=False,
                             skip_export=True, verbose=0)
    errs = layerwise_error_analyse(graph=g0, running_device="cpu", collate_fn=collate, dataloader=loader,
                                   interested_outputs=["landmarks"], steps=32, verbose=False)
    ranked = sorted(((v if not isinstance(v, tuple) else v[0], k) for k, v in errs.items()
                     if g0.operations[k].type in ("Conv", "Gemm")), reverse=True)
    pick = [k for _, k in ranked[:auto[0]]]
    with open(os.path.join(out_dir, f"{variant}.auto16.txt"), "w") as f:
        for v, k in ranked:
            f.write("%.5f %5s %s\n" % (v, "INT16" if k in pick else "", k))
    setting = make_setting(pick)

ev = np.load(eval_path)
crops = ev["crops"]  # (N,224,224,3)
test_in = torch.from_numpy(np.ascontiguousarray(crops[0].transpose(2, 0, 1)[None]))
out_file = os.path.join(out_dir, f"hand_landmark_lite_{variant}{'' if with_test else '_notest'}.espdl")
graph = espdl_quantize_onnx(
    onnx_import_file=work,
    espdl_export_file=out_file,
    calib_dataloader=loader,
    calib_steps=steps,
    input_shape=[1, 3, 224, 224],
    inputs=[test_in],
    target=TARGET,
    quant_type=quant_type,
    collate_fn=collate,
    setting=setting,
    device="cpu",
    error_report=True,
    skip_export=False,
    export_test_values=with_test,
    verbose=0,
)
with open(os.path.join(out_dir, f"{variant}.layers.txt"), "w") as f:
    for op in graph.operations.values():
        f.write(f"{op.name}\t{op.type}\t{op.platform.name}\n")
# native PPQ graph (keeps the quantization config) for PpqRunner / crop_strategies.py
export_ppq_graph(graph, TargetPlatform.NATIVE, os.path.join(out_dir, f"{variant}.native"))
ex = TorchExecutor(graph=graph, device="cpu")
names = list(graph.outputs.keys())
res = {n: [] for n in names}
for c in crops:
    outs = ex.forward(inputs=torch.from_numpy(np.ascontiguousarray(c.transpose(2, 0, 1)[None])))
    for n, o in zip(names, outs):
        res[n].append(o.detach().cpu().numpy().reshape(-1))
np.savez_compressed(os.path.join(out_dir, f"{variant}.preds.npz"), **{n: np.stack(v) for n, v in res.items()})
print("exported", out_file, os.path.getsize(out_file), "bytes")
