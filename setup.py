import os
import sys
from pathlib import Path
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext

ROOT = Path(__file__).resolve().parent


class BuildExt(build_ext):
    def build_extensions(self):
        if self.compiler.compiler_type == "unix":
            for ext in self.extensions:
                ext.extra_compile_args.append("-std=c++17")
                ext.extra_compile_args.append("-O3")
        elif self.compiler.compiler_type == "msvc":
            for ext in self.extensions:
                ext.extra_compile_args.append("/std:c++17")
                ext.extra_compile_args.append("/O2")
        super().build_extensions()


def get_pybind_include():
    try:
        import pybind11
        return pybind11.get_include()
    except ImportError:
        return ""


sources = [
    "bindings/py_engine.cpp",
    "engine/core/action_constraint.cpp",
    "engine/search/net_mcts.cpp",
    "engine/search/tail_solver.cpp",
    "engine/infer/onnx_policy_value_evaluator.cpp",
    "engine/runtime/selfplay_runner.cpp",
    "engine/runtime/arena_runner.cpp",
    "engine/runtime/heuristic_runner.cpp",
    "games/tictactoe/tictactoe_state.cpp",
    "games/tictactoe/tictactoe_rules.cpp",
    "games/tictactoe/tictactoe_net_adapter.cpp",
    "games/tictactoe/tictactoe_register.cpp",
    "games/splendor/splendor_state.cpp",
    "games/splendor/splendor_rules.cpp",
    "games/splendor/splendor_net_adapter.cpp",
    "games/splendor/splendor_register.cpp",
    "games/azul/azul_state.cpp",
    "games/azul/azul_rules.cpp",
    "games/azul/azul_net_adapter.cpp",
    "games/azul/azul_register.cpp",
    "games/quoridor/quoridor_state.cpp",
    "games/quoridor/quoridor_rules.cpp",
    "games/quoridor/quoridor_net_adapter.cpp",
    "games/quoridor/quoridor_register.cpp",
    "games/loveletter/loveletter_state.cpp",
    "games/loveletter/loveletter_rules.cpp",
    "games/loveletter/loveletter_net_adapter.cpp",
    "games/loveletter/loveletter_register.cpp",
    "games/coup/coup_state.cpp",
    "games/coup/coup_rules.cpp",
    "games/coup/coup_net_adapter.cpp",
    "games/coup/coup_register.cpp",
]

include_dirs = [
    str(ROOT),
    get_pybind_include(),
]

define_macros = []

onnx_root = os.environ.get("BOARD_AI_ONNXRUNTIME_ROOT", "")
with_onnx = os.environ.get("BOARD_AI_WITH_ONNX", "")
library_dirs = []
libraries = []

if with_onnx == "":
    # Auto-detect ONNX Runtime in priority order:
    #   1. Bundled copy under `third_party/onnxruntime-<platform>-<arch>-<ver>/`
    #      — ships with the repo so Linux deploys only need `git clone`, no
    #      external downloads (and no cloud-firewall interference).
    #   2. Homebrew (Mac dev machines): /opt/homebrew or /usr/local.
    candidates = []
    third_party = ROOT / "third_party"
    if third_party.is_dir():
        for p in sorted(third_party.glob("onnxruntime-*")):
            if (p / "include" / "onnxruntime_c_api.h").exists():
                candidates.append(str(p))
    candidates.extend(["/opt/homebrew", "/usr/local"])
    for candidate in candidates:
        if ((Path(candidate) / "include" / "onnxruntime_c_api.h").exists()
                or (Path(candidate) / "include" / "onnxruntime" / "onnxruntime_c_api.h").exists()):
            onnx_root = onnx_root or candidate
            with_onnx = "1"
            break
    if with_onnx != "1":
        with_onnx = "0"

with_onnx = with_onnx == "1"

define_macros.append(("BOARD_AI_WITH_ONNX", "1" if with_onnx else "0"))
if with_onnx:
    if not onnx_root:
        raise RuntimeError("BOARD_AI_WITH_ONNX=1 requires BOARD_AI_ONNXRUNTIME_ROOT")
    include_dirs.append(str(Path(onnx_root) / "include"))
    onnx_inner = Path(onnx_root) / "include" / "onnxruntime"
    if onnx_inner.is_dir():
        include_dirs.append(str(onnx_inner))
    library_dirs.append(str(Path(onnx_root) / "lib"))
    libraries.append("onnxruntime")
else:
    print("WARNING: Building without ONNX runtime. MCTS will use uniform policy (no neural network guidance).")

# rpath so the compiled extension finds the bundled libonnxruntime.so at
# runtime without requiring LD_LIBRARY_PATH to be set by the caller.
# $ORIGIN expands to the directory of the extension itself; the .so gets
# installed to site-packages/, and the bundled ORT lives at
# <repo>/third_party/onnxruntime-*/lib relative to the build dir — but in
# editable installs the extension is dropped next to setup.py, so
# $ORIGIN/third_party/<dir>/lib is the stable relative path.
extra_link_args = []
if with_onnx and onnx_root and sys.platform.startswith("linux"):
    ort_lib_dir = Path(onnx_root) / "lib"
    # Absolute rpath is simplest and works for editable installs. If the
    # user moves the repo after install they'll need to rebuild; that's
    # fine since pip install -e . is how everyone rebuilds anyway.
    extra_link_args.append(f"-Wl,-rpath,{ort_lib_dir}")

ext = Extension(
    name="dinoboard_engine",
    sources=[str(ROOT / s) for s in sources],
    include_dirs=include_dirs,
    library_dirs=library_dirs,
    libraries=libraries,
    define_macros=define_macros,
    language="c++",
    extra_compile_args=[],
    extra_link_args=extra_link_args,
)

setup(
    name="dinoboard",
    version="0.1.0",
    description="DinoBoard universal board game AI platform",
    ext_modules=[ext],
    cmdclass={"build_ext": BuildExt},
    python_requires=">=3.9",
    install_requires=["pybind11>=2.10"],
)
