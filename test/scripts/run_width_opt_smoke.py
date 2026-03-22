#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple


DEFAULT_OPT = Path("/Users/regehr/llvm-project/for-alive/bin/opt")


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
        "assertion failed",
    ]
    return any(marker in combined_output for marker in crash_markers)


def format_failure(kind: str, path: Path, proc: subprocess.CompletedProcess[str]) -> str:
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


def collect_tests(repo_root: Path, directories: Sequence[str]) -> List[Path]:
    tests: List[Path] = []
    for directory in directories:
        tests.extend(sorted((repo_root / directory).glob("*.ll")))
    return tests


def optimize_file(opt_bin: Path, plugin: Path, src: Path, dst: Path) -> subprocess.CompletedProcess[str]:
    return run(
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


def make_output_path(tmpdir: Path, repo_root: Path, test: Path, index: int) -> Path:
    relative = test.relative_to(repo_root)
    safe_stem = "__".join(relative.with_suffix("").parts)
    return tmpdir / f"{index:04d}-{safe_stem}.opt.ll"


def default_jobs() -> int:
    return max(1, os.cpu_count() or 1)


def run_test(
    opt_bin: Path, plugin: Path, repo_root: Path, tmpdir: Path, index: int, test: Path
) -> Tuple[Path, subprocess.CompletedProcess[str]]:
    output = make_output_path(tmpdir, repo_root, test, index)
    return test, optimize_file(opt_bin, plugin, test, output)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    default_plugin = find_default_plugin(repo_root)

    parser = argparse.ArgumentParser(
        description="Run width-opt on every .ll file in the requested test directories."
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=repo_root,
        help="Repository root containing the test directories.",
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
        help="Path to libWidthOpt shared library.",
    )
    parser.add_argument(
        "--dir",
        dest="directories",
        action="append",
        help="Test directory to scan for .ll files. Defaults to test.",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=default_jobs(),
        help="Number of files to optimize concurrently.",
    )
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    opt_bin = args.opt_bin.resolve()
    plugin = args.plugin.resolve() if args.plugin is not None else None
    directories = args.directories or ["test"]

    if not opt_bin.exists():
        print(f"error: opt not found: {opt_bin}", file=sys.stderr)
        return 1

    if plugin is None or not plugin.exists():
        print(
            "error: plugin not found; pass --plugin /path/to/libWidthOpt.dylib",
            file=sys.stderr,
        )
        return 1

    if args.jobs < 1:
        print("error: --jobs must be at least 1", file=sys.stderr)
        return 1

    tests = collect_tests(repo_root, directories)
    if not tests:
        print("error: no .ll tests found", file=sys.stderr)
        return 1

    failures = 0
    with tempfile.TemporaryDirectory(prefix="width-opt-smoke-") as tmp:
        tmpdir = Path(tmp)
        workers = min(args.jobs, len(tests))
        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = [
                executor.submit(run_test, opt_bin, plugin, repo_root, tmpdir, index, test)
                for index, test in enumerate(tests)
            ]
            for future in as_completed(futures):
                test, proc = future.result()
                relative = test.relative_to(repo_root)

                if is_crash(proc):
                    print(f"CRASH {relative}")
                    print(format_failure("width-opt crash", test, proc))
                    failures += 1
                    continue
                if proc.returncode != 0:
                    print(f"FAIL {relative}")
                    print(format_failure("width-opt", test, proc))
                    failures += 1
                    continue

                print(f"PASS {relative}")

    if failures:
        print(f"\n{failures} file(s) failed", file=sys.stderr)
        return 1

    print(f"\nOptimized {len(tests)} file(s) with width-opt.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
