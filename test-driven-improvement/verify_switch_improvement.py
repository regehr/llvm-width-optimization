#!/usr/bin/env python3
"""Count how many existing out.ll files are improved by running the new pass."""

import os
import subprocess
import re

tmp_dir = "/home/regehr/llvm-width-optimization/test-driven-improvement/tmp"
opt = "/home/regehr/llvm-project/for-alive/bin/opt"
plugin = "/home/regehr/llvm-width-optimization/build/lib/libWidthOpt.so"
count_pat = re.compile(r'= (sext|zext|trunc) ')

improved = 0
checked = 0
total_reduced = 0

# Check out.ll files that have zext i1 to iN (likely switch candidates)
for fname in sorted(os.listdir(tmp_dir)):
    if not fname.endswith('.out.ll'):
        continue
    if any(x in fname for x in ['.manual.', '.opt.', '.current-opt.']):
        continue
    
    out_path = os.path.join(tmp_dir, fname)
    base = fname[:-len('.out.ll')]
    in_path = os.path.join(tmp_dir, base + '.ll')
    
    if not os.path.exists(in_path):
        continue
    
    with open(out_path) as f:
        old_content = f.read()
    
    old_count = len(count_pat.findall(old_content))
    if old_count == 0:
        continue
    
    # Only check files with zext i1 patterns
    if '= zext i1' not in old_content:
        continue
    
    checked += 1
    if checked > 5000:  # Sample limit
        break
    
    result = subprocess.run(
        [opt, f"-load-pass-plugin={plugin}", "-passes=width-opt", "-S", in_path],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        continue
    
    new_count = len(count_pat.findall(result.stdout))
    if new_count < old_count:
        improved += 1
        total_reduced += (old_count - new_count)

print(f"Files checked (with zext i1): {checked}")
print(f"Files improved by new pass: {improved}")
print(f"Total instructions reduced: {total_reduced}")
