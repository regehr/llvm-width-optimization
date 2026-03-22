#!/usr/bin/env python3
"""Analyze zext i1 to i8 patterns in out.ll files with exactly 1 remaining instruction."""

import os
import re

tmp_dir = "/home/regehr/llvm-width-optimization/test-driven-improvement/tmp"
count_pat = re.compile(r'= (sext|zext|trunc) ')

pattern_counts = {}
examples = {}

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
    if '= zext i1' not in out_content or 'to i8' not in out_content:
        continue
    
    lines = out_content.split('\n')
    for i, line in enumerate(lines):
        if '= zext i1' in line and 'to i8' in line:
            m = re.search(r'(%\S+) = zext i1 (%\S+) to i8', line)
            if not m:
                continue
            zext_name = m.group(1)
            src_name = m.group(2)
            # Find uses of zext_name
            uses = [l.strip() for l in lines if zext_name in l and l.strip() != line.strip()]
            use_summary = '; '.join(u for u in uses[:3])
            
            if 'store i8' in use_summary and len(uses) == 1:
                key = 'zext i1 to i8 → store only'
            elif 'ret i8' in use_summary and len(uses) == 1:
                key = 'zext i1 to i8 → ret only'
            elif 'call' in use_summary:
                key = 'zext i1 to i8 → call'
            elif 'switch' in use_summary:
                key = 'zext i1 to i8 → switch'
            elif 'store' in use_summary:
                key = f'zext i1 to i8 → store+other ({use_summary[:40]})'
            else:
                key = f'other use: {use_summary[:60]}'
            
            pattern_counts[key] = pattern_counts.get(key, 0) + 1
            if key not in examples:
                examples[key] = (base, line.strip(), uses[:3])

print("zext i1 to i8 use patterns (1-remaining files):")
for k, v in sorted(pattern_counts.items(), key=lambda x: -x[1]):
    print(f"  {v:5d}  {k}")
    if k in examples:
        base, instr, uses = examples[k]
        print(f"          e.g. {base}: {instr}")
        for u in uses[:2]:
            print(f"               uses: {u}")
