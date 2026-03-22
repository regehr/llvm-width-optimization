#!/usr/bin/env python3
"""Count remaining width instructions across all .ll files using updated pass."""

import os
import subprocess
import re

tmp_dir = "/home/regehr/llvm-width-optimization/test-driven-improvement/tmp"
opt = "/home/regehr/llvm-project/for-alive/bin/opt"
plugin = "/home/regehr/llvm-width-optimization/build/lib/libWidthOpt.so"
count_pat = re.compile(r'= (sext|zext|trunc) ')

total_files = 0
files_with_remaining = 0
total_remaining = 0

# Sample a subset for speed
import random
random.seed(42)
all_ll = [f for f in os.listdir(tmp_dir) 
          if f.endswith('.ll') and not any(x in f for x in ['.out.', '.opt.', '.manual.', '.current-opt.'])]
sample = random.sample(all_ll, min(1000, len(all_ll)))

for fname in sample:
    in_path = os.path.join(tmp_dir, fname)
    result = subprocess.run(
        [opt, f"-load-pass-plugin={plugin}", "-passes=width-opt", "-S", in_path],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        continue
    out = result.stdout
    remaining = len(count_pat.findall(out))
    total_files += 1
    if remaining > 0:
        files_with_remaining += 1
        total_remaining += remaining

print(f"Sample size: {total_files}")
print(f"Files with remaining instructions: {files_with_remaining} ({100*files_with_remaining/total_files:.1f}%)")
print(f"Total remaining instructions: {total_remaining}")
print(f"Average remaining per file: {total_remaining/total_files:.2f}")
