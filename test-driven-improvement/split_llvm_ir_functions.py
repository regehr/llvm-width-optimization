#!/usr/bin/env python3

import argparse
import concurrent.futures
import os
import re
import shutil
import string
import subprocess
import sys
import tempfile
from pathlib import Path


LLVM_BIN = Path("/home/regehr/llvm-project-regehr/build/bin")
LLVM_AS = LLVM_BIN / "llvm-as"
LLVM_DIS = LLVM_BIN / "llvm-dis"
LLVM_EXTRACT = LLVM_BIN / "llvm-extract"
OPT = LLVM_BIN / "opt"
ALIVE_TV = "alive-tv"
ALIVE_TV_TIMEOUT_SECONDS = 5

DEFINE_RE = re.compile(
    r'^\s*define\b.*?@(?P<name>"(?:[^"\\]|\\[0-9A-Fa-f]{2}|\\.)*"|[-a-zA-Z$._0-9]+)\s*\('
)
AVAILABLE_EXTERNALLY_RE = re.compile(r"^\s*define\b(?:(?!@).)*\bavailable_externally\b")
TARGET_CAST_INSTR_RE = re.compile(
    r'^\s*(?:%[-a-zA-Z$._0-9]+|%"(?:[^"\\]|\\[0-9A-Fa-f]{2}|\\.)*")\s*=\s*(?:sext|zext|trunc|select)\b'
)


def run_tool(args: list[str]) -> None:
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        stderr = result.stderr.strip()
        raise RuntimeError(
            f"command failed: {' '.join(args)}" + (f"\n{stderr}" if stderr else "")
        )


def get_tool_stdout(args: list[str]) -> str:
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        stderr = result.stderr.strip()
        raise RuntimeError(
            f"command failed: {' '.join(args)}" + (f"\n{stderr}" if stderr else "")
        )
    return result.stdout + result.stderr


def decode_quoted_name(token: str) -> str:
    assert token.startswith('"') and token.endswith('"')
    body = token[1:-1]
    out: list[str] = []
    i = 0
    while i < len(body):
        if body[i] != "\\":
            out.append(body[i])
            i += 1
            continue
        if i + 2 < len(body) and all(c in string.hexdigits for c in body[i + 1 : i + 3]):
            out.append(chr(int(body[i + 1 : i + 3], 16)))
            i += 3
            continue
        if i + 1 >= len(body):
            raise RuntimeError(f"malformed quoted LLVM name: {token}")
        out.append(body[i + 1])
        i += 2
    return "".join(out)


def list_defined_function_records(disassembled_ll: Path) -> list[tuple[str, bool]]:
    functions: list[tuple[str, bool]] = []
    for line in disassembled_ll.read_text(encoding="utf-8").splitlines():
        match = DEFINE_RE.match(line)
        if not match:
            continue
        token = match.group("name")
        extractable = not AVAILABLE_EXTERNALLY_RE.search(line)
        if token.startswith('"'):
            functions.append((decode_quoted_name(token), extractable))
        else:
            functions.append((token, extractable))
    return functions


def reserve_output_path(output_dir: Path) -> Path:
    fd, output_name = tempfile.mkstemp(prefix="split-", suffix=".ll", dir=output_dir)
    os.close(fd)
    return Path(output_name)


def normalize_to_bitcode(input_path: Path, workdir: Path) -> Path:
    module_bc = workdir / "module.bc"
    suffix = input_path.suffix.lower()
    if suffix == ".ll":
        run_tool([str(LLVM_AS), str(input_path), "-o", str(module_bc)])
        return module_bc
    if suffix == ".bc":
        return input_path
    raise RuntimeError("input must end in .ll or .bc")


def optimize_module(module_bc: Path, workdir: Path) -> Path:
    optimized_bc = workdir / "module.optimized.bc"
    run_tool([str(OPT), "-passes=sroa,adce", str(module_bc), "-o", str(optimized_bc)])
    return optimized_bc


def disassemble_module(module_bc: Path, workdir: Path) -> Path:
    module_ll = workdir / "module.ll"
    run_tool([str(LLVM_DIS), str(module_bc), "-o", str(module_ll)])
    return module_ll


def verify_module(module_ll: Path) -> None:
    run_tool([str(OPT), "-passes=verify", "-disable-output", str(module_ll)])


def alive_tv_accepts_module(module_ll: Path) -> bool:
    try:
        result = subprocess.run(
            [ALIVE_TV, "--disable-undef-input", str(module_ll)],
            capture_output=True,
            text=True,
            timeout=ALIVE_TV_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        return False
    return result.returncode == 0


def count_defined_functions(disassembled_ll: Path) -> int:
    count = 0
    for line in disassembled_ll.read_text(encoding="utf-8").splitlines():
        if DEFINE_RE.match(line):
            count += 1
    return count


def module_has_multiple_target_cast_instructions(disassembled_ll: Path) -> bool:
    in_function = False
    target_cast_count = 0
    for line in disassembled_ll.read_text(encoding="utf-8").splitlines():
        if not in_function:
            if DEFINE_RE.match(line):
                in_function = True
            continue
        if line.lstrip().startswith("}"):
            in_function = False
            continue
        if TARGET_CAST_INSTR_RE.match(line):
            target_cast_count += 1
            if target_cast_count >= 2:
                return True
    return False


def process_function(module_bc: Path, output_dir: Path, function_name: str) -> Path | None:
    output_path = reserve_output_path(output_dir)
    try:
        run_tool(
            [
                str(LLVM_EXTRACT),
                f"--func={function_name}",
                "--keep-const-init",
                "-S",
                str(module_bc),
                "-o",
                str(output_path),
            ]
        )
        if count_defined_functions(output_path) != 1:
            output_path.unlink(missing_ok=True)
            return None
        if not module_has_multiple_target_cast_instructions(output_path):
            output_path.unlink(missing_ok=True)
            return None
        verify_module(output_path)
        if not alive_tv_accepts_module(output_path):
            output_path.unlink(missing_ok=True)
            return None
        return output_path
    except Exception:
        output_path.unlink(missing_ok=True)
        raise


def list_function_instruction_counts(module_ll: Path, extractable_mask: list[bool]) -> list[int]:
    output = get_tool_stdout(
        [str(OPT), "-passes=print<func-properties>", "-disable-output", str(module_ll)]
    )
    counts: list[int] = []
    in_function_block = False
    function_index = -1
    keep_current_count = False
    for line in output.splitlines():
        if line.startswith("Printing analysis results of CFA for function '") and line.endswith("':"):
            function_index += 1
            if function_index >= len(extractable_mask):
                raise RuntimeError("LLVM reported more functions than were parsed from the module")
            keep_current_count = extractable_mask[function_index]
            in_function_block = True
            continue
        if in_function_block and line.startswith("TotalInstructionCount: "):
            if keep_current_count:
                counts.append(int(line.removeprefix("TotalInstructionCount: ")))
            in_function_block = False
    if function_index + 1 != len(extractable_mask):
        raise RuntimeError("LLVM reported fewer functions than were parsed from the module")
    return counts


def ensure_tools_exist() -> None:
    missing = [str(path) for path in (LLVM_AS, LLVM_DIS, LLVM_EXTRACT, OPT) if not path.is_file()]
    if missing:
        raise RuntimeError("missing LLVM tools:\n" + "\n".join(missing))
    if shutil.which(ALIVE_TV) is None:
        raise RuntimeError(f"missing required tool in PATH: {ALIVE_TV}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Split an LLVM IR module into one verifier-clean .ll file per defined function."
    )
    parser.add_argument(
        "--min-instructions",
        type=int,
        default=5,
        help="skip functions whose LLVM instruction count is less than this value",
    )
    parser.add_argument(
        "--max-instructions",
        type=int,
        default=50,
        help="skip functions whose LLVM instruction count is greater than this value",
    )
    parser.add_argument("output_dir", type=Path, help="directory for emitted .ll files")
    parser.add_argument("input", type=Path, help="input LLVM IR file (.ll or .bc)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_path = args.input.resolve()
    output_dir = args.output_dir.resolve()

    try:
        ensure_tools_exist()
        if not input_path.is_file():
            raise RuntimeError(f"input file does not exist: {input_path}")

        output_dir.mkdir(parents=True, exist_ok=True)

        with tempfile.TemporaryDirectory(prefix="split-llvm-ir-") as tmp:
            workdir = Path(tmp)
            module_bc = normalize_to_bitcode(input_path, workdir)
            module_bc = optimize_module(module_bc, workdir)
            module_ll = disassemble_module(module_bc, workdir)
            function_records = list_defined_function_records(module_ll)
            functions = [name for name, extractable in function_records if extractable]
            if args.min_instructions < 0:
                raise RuntimeError("--min-instructions must be non-negative")
            if args.max_instructions < 0:
                raise RuntimeError("--max-instructions must be non-negative")
            if args.min_instructions > args.max_instructions:
                raise RuntimeError("--min-instructions must be less than or equal to --max-instructions")
            instruction_counts = list_function_instruction_counts(
                module_ll,
                [extractable for _, extractable in function_records],
            )
            if len(instruction_counts) != len(functions):
                raise RuntimeError(
                    "failed to match LLVM instruction counts to defined functions"
                )

            candidate_functions = [
                function_name
                for index, function_name in enumerate(functions)
                if args.min_instructions <= instruction_counts[index] <= args.max_instructions
            ]
            max_workers = min(len(candidate_functions), os.cpu_count() or 1)
            if max_workers == 0:
                print(f"{input_path.name}: kept 0 functions")
                return 0

            kept_count = 0
            with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
                futures = [
                    executor.submit(process_function, module_bc, output_dir, function_name)
                    for function_name in candidate_functions
                ]
                for future in futures:
                    output_path = future.result()
                    if output_path is not None:
                        kept_count += 1
            print(f"{input_path.name}: kept {kept_count} functions")

    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
