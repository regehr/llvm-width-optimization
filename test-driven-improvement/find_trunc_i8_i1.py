#!/usr/bin/env python3
"""Analyze trunc i8 to i1 patterns in out.ll files with exactly 1 remaining instruction."""

import os
import re

tmp_dir = "/home/regehr/llvm-width-optimization/test-driven-improvement/tmp"
count_pat = re.compile(r'= (sext|zext|trunc) ')

pattern_counts = {}

for fname in sorted(os.listdir(tmp_dir)):
    if not fname.endswith('.out.ll'):
        continue
    if any(x in fname for x in ['.manual.', '.opt.', '.current-opt.']):
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
    if '= trunc i8' not in out_content or 'to i1' not in out_content:
        continue
    
    lines = out_content.split('\n')
    for i, line in enumerate(lines):
        if '= trunc i8' in line and 'to i1' in line:
            m = re.search(r'(%\S+) = trunc i8 (%\S+) to i1', line)
            if not m:
                continue
            trunc_name = m.group(1)
            src_name = m.group(2)
            # Find context: what does src_name come from?
            src_line = next((l.strip() for l in lines if l.strip().startswith(src_name + ' ')), '')
            # Find uses of trunc_name
            uses = [l.strip() for l in lines if trunc_name in l and l.strip() != line.strip()]
            use_summary = '; '.join(uses[:3])
            
            # Classify by source pattern
            if 'load i8' in src_line:
                key = 'load i8 → trunc to i1'
            elif 'and i8' in src_line:
                key = 'and i8 → trunc to i1'
            elif 'call' in src_line:
                key = f'call → trunc i8 to i1 (use: {use_summary[:40]})'
            elif 'phi i8' in src_line:
                key = 'phi i8 → trunc to i1'
            elif 'select i8' in src_line or 'select i1' in src_line:
                key = 'select → trunc i8 to i1'
            elif 'zext i1' in src_line:
                key = 'zext i1 to i8 → trunc to i1'
            else:
                key = f'other src: {src_line[:60]}'
            pattern_counts[key] = pattern_counts.get(key, 0) + 1

print("trunc i8 to i1 source patterns (1-remaining files):")
for k, v in sorted(pattern_counts.items(), key=lambda x: -x[1]):
    print(f"  {v:5d}  {k}")
