#!/usr/bin/env python3
"""Find zext i1 to i64 used in switch statements - a potentially eliminatable pattern."""

import os
import re

tmp_dir = "/home/regehr/llvm-width-optimization/test-driven-improvement/tmp"
count_pat = re.compile(r'= (sext|zext|trunc) ')

examples = []

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
    if '= zext i1' not in out_content or 'to i64' not in out_content:
        continue
    
    lines = out_content.split('\n')
    for i, line in enumerate(lines):
        if '= zext i1' in line and 'to i64' in line:
            m = re.search(r'(%\S+) = zext i1 (%\S+) to i64', line)
            if not m:
                continue
            zext_name = m.group(1)
            # Find uses
            uses = [l.strip() for l in lines if zext_name in l and l.strip() != line.strip()]
            if any('switch' in u for u in uses):
                examples.append((base, line.strip(), uses[:5]))

print(f"Found {len(examples)} examples of zext i1 to i64 used in switch:")
for base, instr, uses in examples[:5]:
    print(f"\n  File: {base}")
    print(f"  Instr: {instr}")
    for u in uses:
        print(f"  Use: {u}")
