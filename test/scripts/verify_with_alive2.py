#!/usr/bin/env python3

import argparse
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import List, Optional, Tuple


DEFAULT_OPT = Path("/Users/regehr/llvm-project/for-alive/bin/opt")
DEFAULT_ALIVE_TV = Path("/Users/regehr/alive2-regehr/build/alive-tv")
DEFAULT_PLUGIN = Path("/tmp/llvm-width-optimization-build-fixpoint/lib/libWidthOpt.dylib")
NO_ALIVE_TAG = "no-alive"


def run(cmd: List[str], *, cwd: Optional[Path] = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        capture_output=True,
    )


def optimize_file(opt_bin: Path, plugin: Path, src: Path, dst: Path) -> subprocess.CompletedProcess:
    cmd = [
        str(opt_bin),
        "-load-pass-plugin",
        str(plugin),
        "-passes=width-opt",
        "-S",
        str(src),
        "-o",
        str(dst),
    ]
    return run(cmd)


def verify_file(alive_tv: Path, src: Path, tgt: Path) -> subprocess.CompletedProcess:
    return run(
        [str(alive_tv), "--disable-undef-input", str(src), str(tgt)]
    )


def print_ir_pair(repo_root: Path, src: Path, tgt: Path) -> None:
    try:
        src_text = src.read_text()
        tgt_text = tgt.read_text()
    except OSError as exc:
        print(f"warning: failed to read IR for {src.name}: {exc}", file=sys.stderr)
        return

    print(f"=== {src.relative_to(repo_root)} : src ===")
    print(src_text.rstrip())
    print(f"=== {src.relative_to(repo_root)} : tgt ===")
    print(tgt_text.rstrip())


def collect_tests(repo_root: Path, directories: List[str]) -> List[Path]:
    tests: List[Path] = []
    for directory in directories:
        test_dir = repo_root / directory
        tests.extend(sorted(test_dir.glob("*.ll")))
    return tests


def has_no_alive_tag(path: Path) -> bool:
    try:
        for line in path.read_text().splitlines():
            if NO_ALIVE_TAG in line:
                return True
    except OSError as exc:
        print(f"warning: failed to read {path}: {exc}", file=sys.stderr)
    return False


def print_failure(kind: str, path: Path, proc: subprocess.CompletedProcess[str]) -> None:
    print(f"FAIL {kind} {path.name}")
    if proc.stdout:
        print("stdout:")
        print(proc.stdout.rstrip())
    if proc.stderr:
        print("stderr:")
        print(proc.stderr.rstrip())


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]

    parser = argparse.ArgumentParser(
        description="Optimize LLVM IR tests with width-opt and validate them with Alive2."
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=repo_root,
        help="Repository root containing test/ and tests/.",
    )
    parser.add_argument(
        "--opt-bin",
        type=Path,
        default=DEFAULT_OPT,
        help=f"Path to opt (default: {DEFAULT_OPT}).",
    )
    parser.add_argument(
        "--alive-tv",
        type=Path,
        default=DEFAULT_ALIVE_TV,
        help=f"Path to alive-tv (default: {DEFAULT_ALIVE_TV}).",
    )
    parser.add_argument(
        "--plugin",
        type=Path,
        default=DEFAULT_PLUGIN,
        help=f"Path to libWidthOpt.dylib (default: {DEFAULT_PLUGIN}).",
    )
    parser.add_argument(
        "--dir",
        dest="directories",
        action="append",
        choices=["test", "tests"],
        help="Restrict verification to one or more test directories.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print the source and optimized IR text for each checked file.",
    )
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    opt_bin = args.opt_bin.resolve()
    alive_tv = args.alive_tv.resolve()
    plugin = args.plugin.resolve()
    directories = args.directories or ["test", "tests"]

    missing = [str(path) for path in [opt_bin, alive_tv, plugin] if not path.exists()]
    if missing:
        for path in missing:
            print(f"error: required file not found: {path}", file=sys.stderr)
        return 1

    tests = collect_tests(repo_root, directories)
    if not tests:
        print("error: no .ll tests found", file=sys.stderr)
        return 1

    failures = 0
    skipped = 0

    def process_test(test: Path, tmpdir: Path) -> Tuple[str, Path, Optional[subprocess.CompletedProcess], Optional[subprocess.CompletedProcess]]:
        """Returns (status, test, opt_proc, alive_proc) where status is 'skip'/'pass'/'fail-opt'/'fail-alive'."""
        if has_no_alive_tag(test):
            return ("skip", test, None, None)
        optimized = tmpdir / f"{test.stem}.opt.ll"
        opt_proc = optimize_file(opt_bin, plugin, test, optimized)
        if opt_proc.returncode != 0:
            return ("fail-opt", test, opt_proc, None)
        alive_proc = verify_file(alive_tv, test, optimized)
        if alive_proc.returncode != 0:
            return ("fail-alive", test, opt_proc, alive_proc)
        return ("pass", test, opt_proc, alive_proc)

    with tempfile.TemporaryDirectory(prefix="width-opt-alive2-") as tmp:
        tmpdir = Path(tmp)
        with ThreadPoolExecutor() as executor:
            future_to_test = {executor.submit(process_test, t, tmpdir): t for t in tests}
            results = []
            for future in as_completed(future_to_test):
                results.append(future.result())

        # Sort results by test path for deterministic output; print inside tmpdir context
        # so verbose mode can still read the optimized IR files.
        results.sort(key=lambda r: r[1])

        for status, test, opt_proc, alive_proc in results:
            rel = test.relative_to(repo_root)
            if status == "skip":
                print(f"SKIP {rel}: {NO_ALIVE_TAG}")
                skipped += 1
            elif status == "fail-opt":
                print_failure("opt", test, opt_proc)
                failures += 1
            elif status == "fail-alive":
                print_failure("alive-tv", test, alive_proc)
                failures += 1
            else:
                if args.verbose:
                    optimized = tmpdir / f"{test.stem}.opt.ll"
                    print_ir_pair(repo_root, test, optimized)
                summary = alive_proc.stdout.strip().splitlines()
                tail = summary[-1] if summary else "ok"
                print(f"PASS {rel}: {tail}")

    if failures:
        print(f"\n{failures} file(s) failed", file=sys.stderr)
        return 1

    print(f"\nVerified {len(tests) - skipped} file(s), skipped {skipped}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
