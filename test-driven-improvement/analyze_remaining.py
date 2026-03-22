#!/usr/bin/env python3
"""Find out.ll files with few remaining width-changing instructions where pass made progress."""

import os
import re
import sys

tmp_dir = "/home/regehr/llvm-width-optimization/test-driven-improvement/tmp"
pattern = re.compile(r'= (sext|zext|trunc) ')

results = []

for fname in os.listdir(tmp_dir):
    # Only look at primary out.ll files (not manual/opt/current-opt variants)
    if not fname.endswith('.out.ll'):
        continue
    if '.manual.' in fname or '.opt.' in fname or '.current-opt.' in fname:
        continue
    
    base = fname[:-len('.out.ll')]
    orig_path = os.path.join(tmp_dir, base + '.ll')
    out_path = os.path.join(tmp_dir, fname)
    
    if not os.path.exists(orig_path):
        continue
    
    # Count width instructions in each
    with open(orig_path) as f:
        orig_content = f.read()
    with open(out_path) as f:
        out_content = f.read()
    
    orig_count = len(pattern.findall(orig_content))
    out_count = len(pattern.findall(out_content))
    
    # Only care about cases where pass made progress AND there are remaining instructions
    if orig_count > out_count and out_count > 0:
        results.append((out_count, orig_count - out_count, base, out_path))

results.sort()
print(f"Total: {len(results)} files with remaining instructions after pass made progress")
print("\nFiles with fewest remaining (top 50):")
for out_count, removed, base, path in results[:50]:
    print(f"  {out_count} remaining ({removed} removed): {base}")
