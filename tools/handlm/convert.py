"""hand_landmark_lite.tflite -> ONNX (NCHW input) for ESP-PPQ.

usage: python -I convert.py <hand_landmark_lite.tflite> <out.onnx>

tflite2onnx 0.4.1 converts the graph (fp16 weights are DEQUANTIZEd to fp32; it warns
"float16 not supported/tested" -- the result is checked numerically against TFLite in
evaluate.py) and already exposes an NCHW input [1,3,224,224] (what ESP-PPQ / esp-dl
expect). We drop the world-landmark head, rename outputs and run onnx-simplifier.
The NHWC branch below is only a fallback for other converter versions.
"""
import sys

import numpy as np
import onnx
from onnx import helper, TensorProto
from onnxsim import simplify
import tflite2onnx

src, dst = sys.argv[1], sys.argv[2]
tmp = dst + ".raw.onnx"
tflite2onnx.convert(src, tmp)
m = onnx.load(tmp)
g = m.graph
print("raw inputs:", [(i.name, [d.dim_value for d in i.type.tensor_type.shape.dim]) for i in g.input])
print("raw outputs:", [(o.name, [d.dim_value for d in o.type.tensor_type.shape.dim]) for o in g.output])

# --- NCHW input: new input 'input' [1,3,224,224] -> Transpose(0,2,3,1) -> old NHWC input name
old_in = g.input[0]
dims = [d.dim_value for d in old_in.type.tensor_type.shape.dim]
if dims == [1, 224, 224, 3]:
    new_in = helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, 224, 224])
    tr = helper.make_node("Transpose", ["input"], [old_in.name], perm=[0, 2, 3, 1], name="input_nchw_to_nhwc")
    g.node.insert(0, tr)
    g.input.remove(old_in)
    g.input.insert(0, new_in)

# --- outputs: landmarks [1,63], presence [1,1], handedness [1,1]; drop world landmarks.
# Identify by name order from the tflite (Identity, Identity_1, Identity_2, Identity_3).
outs = list(g.output)
keep_names = {"Identity", "Identity_1", "Identity_2"}
named = {o.name: o for o in outs}
if keep_names <= set(named):
    for o in outs:
        if o.name not in keep_names:
            g.output.remove(o)
else:
    print("WARN: unexpected output names, keeping all:", list(named))

# rename outputs to readable names
rename = {"Identity": "landmarks", "Identity_1": "presence", "Identity_2": "handedness"}
for n in g.node:
    n.output[:] = [rename.get(x, x) for x in n.output]
    n.input[:] = [rename.get(x, x) for x in n.input]
for o in g.output:
    o.name = rename.get(o.name, o.name)

m.opset_import[0].version = max(m.opset_import[0].version, 13)
onnx.checker.check_model(m)
ms, ok = simplify(m)
assert ok, "onnxsim check failed"
onnx.save(ms, dst)
ops = {}
for n in ms.graph.node:
    ops[n.op_type] = ops.get(n.op_type, 0) + 1
print("saved", dst, "ops:", ops)
print("inputs:", [(i.name, [d.dim_value for d in i.type.tensor_type.shape.dim]) for i in ms.graph.input])
print("outputs:", [(o.name, [d.dim_value for d in o.type.tensor_type.shape.dim]) for o in ms.graph.output])
