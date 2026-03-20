#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


DEFAULT_OPT = Path("/Users/regehr/llvm-project/for-alive/bin/opt")
DEFAULT_LLVM_PIPELINE = (
    "function(instcombine,aggressive-instcombine,correlated-propagation,"
    "bdce,adce,instcombine)"
)

FUNCTION_RE = re.compile(
    r'^\s*define\b.*@(?P<name>"(?:[^"\\]|\\.)*"|[-$._A-Za-z][-$._A-Za-z0-9]*)\s*\('
)
CAST_RE = re.compile(r"=\s*(sext|zext|trunc)\b")


def run(cmd: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(cmd),
        text=True,
        capture_output=True,
    )


def is_crash(proc: subprocess.CompletedProcess[str]) -> bool:
    if proc.returncode < 0:
        return True

    combined_output = "\n".join(part for part in [proc.stdout, proc.stderr] if part).lower()
    crash_markers = [
        "stack dump",
        "please submit a bug report",
        "segmentation fault",
        "signal",
    ]
    return any(marker in combined_output for marker in crash_markers)


def format_failure(
    kind: str, path: Path, proc: subprocess.CompletedProcess[str]
) -> str:
    lines = [f"{kind} failed for {path}"]
    if proc.stdout:
        lines.append("stdout:")
        lines.append(proc.stdout.rstrip())
    if proc.stderr:
        lines.append("stderr:")
        lines.append(proc.stderr.rstrip())
    return "\n".join(lines)


def candidate_plugins(repo_root: Path) -> Iterable[Path]:
    env_plugin = os.environ.get("WIDTH_OPT_PLUGIN")
    if env_plugin:
        yield Path(env_plugin)

    for suffix in ("so", "dylib", "dll"):
        yield repo_root / "build" / "lib" / f"libWidthOpt.{suffix}"
        yield repo_root.parent / "tmp" / "llvm-width-optimization-build" / "lib" / f"libWidthOpt.{suffix}"
        yield Path("/tmp/llvm-width-optimization-build") / "lib" / f"libWidthOpt.{suffix}"
        yield Path("/tmp/llvm-width-optimization-build-fixpoint") / "lib" / f"libWidthOpt.{suffix}"
        yield Path("/Users/regehr/tmp/llvm-width-optimization-build") / "lib" / f"libWidthOpt.{suffix}"
        yield Path("/Users/regehr/tmp/llvm-width-optimization-build-fixpoint") / "lib" / f"libWidthOpt.{suffix}"


def find_default_plugin(repo_root: Path) -> Optional[Path]:
    for candidate in candidate_plugins(repo_root):
        if candidate.exists():
            return candidate.resolve()
    return None


def emit_module_ir(
    opt_bin: Path,
    src: Path,
    dst: Path,
    *,
    passes: Optional[str] = None,
    plugin: Optional[Path] = None,
) -> None:
    cmd: List[str] = [str(opt_bin)]
    if plugin is not None:
        cmd.extend(["-load-pass-plugin", str(plugin)])
    if passes is not None:
        cmd.append(f"-passes={passes}")
    cmd.extend(["-S", str(src), "-o", str(dst)])

    proc = run(cmd)
    kind = "opt"
    if plugin is not None and passes is not None:
        kind = "width-opt"
    elif passes is not None:
        kind = "llvm-opt"

    if is_crash(proc):
        raise RuntimeError(format_failure(f"{kind} crash", src, proc))
    if proc.returncode != 0:
        raise RuntimeError(format_failure(kind, src, proc))


def parse_function_cast_counts(ir_text: str) -> Dict[str, int]:
    counts: Dict[str, int] = {}
    current_function: Optional[str] = None

    for line in ir_text.splitlines():
        if current_function is None:
            match = FUNCTION_RE.match(line)
            if match:
                current_function = match.group("name")
                counts.setdefault(current_function, 0)
            continue

        if line.lstrip().startswith("}"):
            current_function = None
            continue

        if CAST_RE.search(line):
            counts[current_function] += 1

    return counts


def load_function_cast_counts(path: Path) -> Dict[str, int]:
    return parse_function_cast_counts(path.read_text())


def format_row(name: str, original: int, ours: int, llvm: int) -> str:
    return (
        f"  {name}: original={original} width-opt={ours} llvm={llvm} "
        f"width-opt-removed={original - ours} llvm-removed={original - llvm}"
    )


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    default_plugin = find_default_plugin(repo_root)

    parser = argparse.ArgumentParser(
        description=(
            "Compare width-opt against a focused LLVM width-change cleanup "
            "pipeline on one LLVM module, counting only sext/zext/trunc per function."
        )
    )
    parser.add_argument(
        "--opt-bin",
        type=Path,
        default=DEFAULT_OPT,
        help=f"Path to opt (default: {DEFAULT_OPT}).",
    )
    parser.add_argument(
        "--plugin",
        type=Path,
        default=default_plugin,
        help=(
            "Path to libWidthOpt shared library. If omitted, the script tries "
            "common local build locations."
        ),
    )
    parser.add_argument(
        "--llvm-passes",
        default=DEFAULT_LLVM_PIPELINE,
        help=(
            "Focused LLVM new-pass-manager pipeline used as the comparison "
            f"baseline (default: {DEFAULT_LLVM_PIPELINE})."
        ),
    )
    parser.add_argument(
        "module",
        type=Path,
        help="LLVM module (.ll or bitcode) to compare.",
    )
    args = parser.parse_args()

    opt_bin = args.opt_bin.resolve()
    plugin = args.plugin.resolve() if args.plugin is not None else None
    module = args.module.resolve()

    if not module.exists():
        print(f"error: input module not found: {module}", file=sys.stderr)
        return 1

    if not opt_bin.exists():
        print(f"error: opt not found: {opt_bin}", file=sys.stderr)
        return 1

    if plugin is None or not plugin.exists():
        print(
            "error: plugin not found; pass --plugin /path/to/libWidthOpt.dylib",
            file=sys.stderr,
        )
        return 1

    with tempfile.TemporaryDirectory(prefix="width-precision-") as tmp:
        tmpdir = Path(tmp)
        original_ir = tmpdir / "original.ll"
        ours_ir = tmpdir / "ours.ll"
        llvm_ir = tmpdir / "llvm.ll"

        try:
            emit_module_ir(opt_bin, module, original_ir)
            emit_module_ir(opt_bin, module, ours_ir, passes="width-opt", plugin=plugin)
            emit_module_ir(opt_bin, module, llvm_ir, passes=args.llvm_passes)
        except RuntimeError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1

        original_counts = load_function_cast_counts(original_ir)
        ours_counts = load_function_cast_counts(ours_ir)
        llvm_counts = load_function_cast_counts(llvm_ir)

    missing_in_ours = sorted(set(original_counts) - set(ours_counts))
    missing_in_llvm = sorted(set(original_counts) - set(llvm_counts))
    if missing_in_ours:
        print(
            "warning: functions missing from width-opt output: "
            + ", ".join(missing_in_ours),
            file=sys.stderr,
        )
    if missing_in_llvm:
        print(
            "warning: functions missing from LLVM output: "
            + ", ".join(missing_in_llvm),
            file=sys.stderr,
        )

    comparable_functions = sorted(
        name
        for name, original in original_counts.items()
        if original > 0 and name in ours_counts and name in llvm_counts
    )

    width_opt_better: List[Tuple[str, int, int, int]] = []
    llvm_better: List[Tuple[str, int, int, int]] = []

    for name in comparable_functions:
        original = original_counts[name]
        ours = ours_counts[name]
        llvm = llvm_counts[name]
        if ours < llvm:
            width_opt_better.append((name, original, ours, llvm))
        elif llvm < ours:
            llvm_better.append((name, original, ours, llvm))

    width_opt_better.sort(key=lambda item: (item[2] - item[3], item[0]))
    llvm_better.sort(key=lambda item: (item[3] - item[2], item[0]))

    total_original = sum(original_counts[name] for name in comparable_functions)
    total_ours = sum(ours_counts[name] for name in comparable_functions)
    total_llvm = sum(llvm_counts[name] for name in comparable_functions)

    print(f"module: {module}")
    print(f"llvm pipeline: {args.llvm_passes}")
    print(
        f"comparable functions with original sext/zext/trunc: {len(comparable_functions)}"
    )
    print(
        "totals: "
        f"original={total_original} width-opt={total_ours} llvm={total_llvm} "
        f"width-opt-removed={total_original - total_ours} "
        f"llvm-removed={total_original - total_llvm}"
    )

    print("\nFunctions where width-opt did better:")
    if width_opt_better:
        for name, original, ours, llvm in width_opt_better:
            print(format_row(name, original, ours, llvm))
    else:
        print("  none")

    print("\nFunctions where LLVM did better:")
    if llvm_better:
        for name, original, ours, llvm in llvm_better:
            print(format_row(name, original, ours, llvm))
    else:
        print("  none")

    return 0


if __name__ == "__main__":
    sys.exit(main())
