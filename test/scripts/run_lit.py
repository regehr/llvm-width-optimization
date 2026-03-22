#!/usr/bin/env python3

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Sequence


def run(cmd: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(cmd),
        text=True,
        capture_output=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Run llvm-lit on the requested path.")
    parser.add_argument("--llvm-lit", type=Path, required=True, help="Path to llvm-lit.")
    parser.add_argument("path", type=Path, help="Lit test directory or suite entry point.")
    args = parser.parse_args()

    proc = run([str(args.llvm_lit), "-sv", str(args.path)])
    if proc.stdout:
        print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
