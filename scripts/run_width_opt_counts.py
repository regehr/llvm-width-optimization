#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable, List, Optional, Tuple


DEFAULT_OPT = Path("/home/regehr/llvm-project-regehr/build/bin/opt")
INSTRUCTION_COUNT_RE = re.compile(r"TotalInstructionCount:\s+(\d+)")


def run(cmd: List[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
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


def candidate_plugins(repo_root: Path) -> Iterable[Path]:
    env_plugin = os.environ.get("WIDTH_OPT_PLUGIN")
    if env_plugin:
        yield Path(env_plugin)

    for suffix in ("so", "dylib", "dll"):
        yield repo_root / "build" / "lib" / f"libWidthOpt.{suffix}"
        yield repo_root.parent / "tmp" / "llvm-width-optimization-build" / "lib" / f"libWidthOpt.{suffix}"
        yield Path("/tmp/llvm-width-optimization-build") / "lib" / f"libWidthOpt.{suffix}"
        yield Path("/tmp/llvm-width-optimization-build-check") / "lib" / f"libWidthOpt.{suffix}"
        yield Path("/home/regehr/tmp/llvm-width-optimization-build") / "lib" / f"libWidthOpt.{suffix}"


def find_default_plugin(repo_root: Path) -> Optional[Path]:
    for candidate in candidate_plugins(repo_root):
        if candidate.exists():
            return candidate.resolve()
    return None


def count_instructions(opt_bin: Path, ir_file: Path) -> int:
    proc = run(
        [
            str(opt_bin),
            "-passes=print<func-properties>",
            "-disable-output",
            str(ir_file),
        ]
    )
    if is_crash(proc):
        raise SystemExit(format_failure("count crash", ir_file, proc))
    if proc.returncode != 0:
        raise RuntimeError(format_failure("count", ir_file, proc))

    total = 0
    combined_output = "\n".join(part for part in [proc.stdout, proc.stderr] if part)
    for match in INSTRUCTION_COUNT_RE.finditer(combined_output):
        total += int(match.group(1))

    if total == 0 and "TotalInstructionCount:" not in combined_output:
        raise RuntimeError(
            f"failed to parse instruction count for {ir_file}\n{combined_output.rstrip()}"
        )

    return total


def optimize_file(opt_bin: Path, plugin: Path, src: Path, dst: Path) -> None:
    proc = run(
        [
            str(opt_bin),
            "-load-pass-plugin",
            str(plugin),
            "-passes=width-opt",
            "-S",
            str(src),
            "-o",
            str(dst),
        ]
    )
    if is_crash(proc):
        raise SystemExit(format_failure("opt crash", src, proc))
    if proc.returncode != 0:
        raise RuntimeError(format_failure("opt", src, proc))


def format_failure(kind: str, path: Path, proc: subprocess.CompletedProcess[str]) -> str:
    lines = [f"{kind} failed for {path}"]
    if proc.stdout:
        lines.append("stdout:")
        lines.append(proc.stdout.rstrip())
    if proc.stderr:
        lines.append("stderr:")
        lines.append(proc.stderr.rstrip())
    return "\n".join(lines)


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    default_plugin = find_default_plugin(repo_root)

    parser = argparse.ArgumentParser(
        description="Run width-opt on LLVM IR files and report instruction counts before and after."
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
        help="Path to libWidthOpt shared library. If omitted, the script tries a few common local build locations.",
    )
    parser.add_argument(
        "files",
        nargs="+",
        type=Path,
        help="LLVM IR (.ll) files to optimize.",
    )
    args = parser.parse_args()

    opt_bin = args.opt_bin.resolve()
    plugin = args.plugin.resolve() if args.plugin is not None else None
    files = [path.resolve() for path in args.files]

    missing_inputs = [str(path) for path in files if not path.exists()]
    if missing_inputs:
        for path in missing_inputs:
            print(f"error: input file not found: {path}", file=sys.stderr)
        return 1

    if not opt_bin.exists():
        print(f"error: opt not found: {opt_bin}", file=sys.stderr)
        return 1

    if plugin is None or not plugin.exists():
        print(
            "error: plugin not found; pass --plugin /path/to/libWidthOpt.so",
            file=sys.stderr,
        )
        return 1

    failures = 0
    results: List[Tuple[Path, int, int, int]] = []
    with tempfile.TemporaryDirectory(prefix="width-opt-counts-") as tmp:
        tmpdir = Path(tmp)
        for src in files:
            dst = tmpdir / f"{src.stem}.opt.ll"

            try:
                before = count_instructions(opt_bin, src)
                optimize_file(opt_bin, plugin, src, dst)
                after = count_instructions(opt_bin, dst)
            except RuntimeError as exc:
                print(f"FAIL {src}: {exc}", file=sys.stderr)
                failures += 1
                continue

            delta = after - before
            results.append((src, before, after, delta))
            print(f"{src}: before={before} after={after} delta={delta:+d}")

    if results:
        total_before = sum(before for _, before, _, _ in results)
        total_after = sum(after for _, _, after, _ in results)
        total_delta = sum(delta for _, _, _, delta in results)
        print(
            f"\nTOTAL: files={len(results)} before={total_before} "
            f"after={total_after} delta={total_delta:+d}"
        )

        increases = [(src, before, after, delta) for src, before, after, delta in results if delta > 0]
        if increases:
            print("\nFILES WITH INSTRUCTION INCREASES:")
            for src, before, after, delta in increases:
                print(f"{src}: before={before} after={after} delta={delta:+d}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
