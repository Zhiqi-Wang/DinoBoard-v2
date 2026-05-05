"""Fix games/splendor/model/splendor_2p.onnx which was accidentally built
to ingest our 295-dim feature vector by *slicing off the last dim* and
feeding the first 294 dims to a first-layer Gemm whose weights were
trained assuming the reference-project DinoBoard feature layout.

Our encoder's order is a *permutation* of the reference's order plus
one extra bit — not "ref order padded at the end" — so the slice
version feeds garbage into the network:

    our 0..209   = ref 0..209        (bank + stats + nobles + tableau)  OK
    our 210..248 = ref opp_reserved → trained weights expect my_reserved
    our 249..254 = ref metadata     → weights expect opp_reserved
    our 255      = first_player bit → weights expect opp_reserved
    our 256..293 = 38/39 of own_reserved (slice dropped the 39th dim)
                                    → weights expect metadata

Fix: apply the correct column permutation to backbone.0.weight (so the
network receives features in the order it was trained on), pad one zero
column for the new first_player bit, remove the Slice node entirely,
and widen the Gemm input to 295 dims.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import onnx
from onnx import helper, numpy_helper


MODEL_PATH = Path("games/splendor/model/splendor_2p.onnx")


def permutation() -> np.ndarray:
    """Our index j -> reference index. -1 means "zero column"."""
    perm = np.empty(295, dtype=np.int64)
    perm[0:210] = np.arange(0, 210)          # bank + stats + nobles + tableau (identity)
    perm[210:249] = np.arange(249, 288)      # our opp_reserved -> ref opp_reserved
    perm[249:255] = np.arange(288, 294)      # our 6-bit metadata -> ref metadata
    perm[255] = -1                            # new first_player bit -> zero column
    perm[256:295] = np.arange(210, 249)      # our own_reserved -> ref my_reserved
    return perm


def main() -> int:
    if not MODEL_PATH.exists():
        print(f"model missing: {MODEL_PATH}", file=sys.stderr)
        return 1

    model = onnx.load(str(MODEL_PATH))
    g = model.graph

    # Locate the Slice that drops the last dim of 'features' and the Gemm
    # that consumes its output.
    slice_node = next((n for n in g.node if n.op_type == "Slice" and "features" in n.input), None)
    if slice_node is None:
        print("no Slice node consuming 'features' found — is this already fixed?", file=sys.stderr)
        return 1
    sliced_output = slice_node.output[0]

    gemm_node = next((n for n in g.node if n.op_type == "Gemm" and sliced_output in n.input), None)
    if gemm_node is None:
        print(f"no Gemm consuming {sliced_output} found", file=sys.stderr)
        return 1

    weight_name = gemm_node.input[1]
    w_ini = next((t for t in g.initializer if t.name == weight_name), None)
    if w_ini is None:
        print(f"initializer {weight_name} not found", file=sys.stderr)
        return 1

    W = numpy_helper.to_array(w_ini)
    if W.shape != (512, 294):
        print(f"unexpected weight shape {W.shape}, expected (512, 294)", file=sys.stderr)
        return 1

    # Build permuted weight [512, 295].
    perm = permutation()
    W_new = np.zeros((512, 295), dtype=W.dtype)
    for j, src in enumerate(perm):
        if src >= 0:
            W_new[:, j] = W[:, src]

    new_w_ini = numpy_helper.from_array(W_new, name=weight_name)
    idx = list(g.initializer).index(w_ini)
    g.initializer.remove(w_ini)
    g.initializer.insert(idx, new_w_ini)

    # Rewire the Gemm to consume 'features' directly, removing the Slice.
    new_gemm_inputs = list(gemm_node.input)
    new_gemm_inputs[0] = "features"
    gemm_node.ClearField("input")
    gemm_node.input.extend(new_gemm_inputs)
    g.node.remove(slice_node)

    # Drop the Slice's obsolete initializers if present.
    for unused in ("_slice_starts", "_slice_ends", "_slice_axes", "_slice_steps"):
        ini = next((t for t in g.initializer if t.name == unused), None)
        if ini is not None:
            g.initializer.remove(ini)

    # 'features' input stays at 295 (was already 295 because the Slice
    # was the adapter). No input shape change needed.

    onnx.checker.check_model(model)
    onnx.save(model, str(MODEL_PATH))
    print(f"wrote fixed model to {MODEL_PATH}")
    print("  Slice removed, backbone.0.weight permuted from [512, 294] -> [512, 295]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
