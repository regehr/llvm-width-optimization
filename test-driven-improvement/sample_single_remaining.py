#!/usr/bin/env python3
"""Sample out.ll files with exactly 1 remaining width instruction, where pass made progress."""

import os
import re

tmp_dir = "/home/regehr/llvm-width-optimization/test-driven-improvement/tmp"
width_pat = re.compile(r'= (sext|zext|trunc) (i\d+) (.+?) to (i\d+)')
count_pat = re.compile(r'= (sext|zext|trunc) ')

by_type = {}

for fname in sorted(os.listdir(tmp_dir)):
    if not fname.endswith('.out.ll'):
        continue
    if '.manual.' in fname or '.opt.' in fname or '.current-opt.' in fname:
        continue
    
    base = fname[:-len('.out.ll')]
    orig_path = os.path.join(tmp_dir, base + '.ll')
    out_path = os.path.join(tmp_dir, fname)
    
    if not os.path.exists(orig_path):
        continue
    
    with open(out_path) as f:
        out_content = f.read()
    with open(orig_path) as f:
        orig_content = f.read()
    
    orig_count = len(count_pat.findall(orig_content))
    out_count = len(count_pat.findall(out_content))
    
    if orig_count <= out_count or out_count != 1:
        continue
    
    # Find the remaining instruction type
    m = width_pat.search(out_content)
    if not m:
        continue
    
    key = f"{m.group(1)} {m.group(2)} to {m.group(4)}"
    if key not in by_type:
        by_type[key] = []
    by_type[key].append(base)

print("Types with 1 remaining instruction (pass made progress):")
for k, v in sorted(by_type.items(), key=lambda x: -len(x[1])):
    print(f"  {len(v):5d}  {k}  -- e.g. {v[0]}")
