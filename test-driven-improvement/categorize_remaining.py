#!/usr/bin/env python3
"""Categorize the remaining width instructions in out.ll files."""

import os
import re

tmp_dir = "/home/regehr/llvm-width-optimization/test-driven-improvement/tmp"
width_pat = re.compile(r'= (sext|zext|trunc) (i\d+) (.+?) to (i\d+)')

counts = {}

for fname in os.listdir(tmp_dir):
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
    
    for m in width_pat.finditer(out_content):
        op = m.group(1)
        from_type = m.group(2)
        to_type = m.group(4)
        key = f"{op} {from_type} to {to_type}"
        counts[key] = counts.get(key, 0) + 1

print("Remaining width instruction patterns (sorted by frequency):")
for k, v in sorted(counts.items(), key=lambda x: -x[1]):
    print(f"  {v:8d}  {k}")
