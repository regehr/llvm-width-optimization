#!/usr/bin/env python3
"""Analyze context around 'zext i1 to i64' in out.ll files with exactly 1 remaining instruction."""

import os
import re

tmp_dir = "/home/regehr/llvm-width-optimization/test-driven-improvement/tmp"
count_pat = re.compile(r'= (sext|zext|trunc) ')

pattern_counts = {}

files_checked = 0
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
    
    # Find context around the zext i1 to i64
    lines = out_content.split('\n')
    for i, line in enumerate(lines):
        if '= zext i1' in line and 'to i64' in line:
            # Extract the SSA name
            m = re.search(r'(%\S+) = zext i1 (%\S+) to i64', line)
            if not m:
                continue
            zext_name = m.group(1)
            # Find uses of zext_name
            uses = [l.strip() for l in lines if zext_name in l and l.strip() != line.strip()]
            use_summary = '; '.join(uses[:3])
            # Classify by use pattern
            if '@llvm.expect.i64' in use_summary:
                key = 'used_in_llvm.expect.i64'
            elif 'call' in use_summary:
                key = 'used_in_call'
            elif 'store' in use_summary:
                key = 'used_in_store'
            elif 'add\|sub\|mul' in use_summary:
                key = 'used_in_arith'
            elif 'icmp' in use_summary:
                key = 'used_in_icmp'
            elif 'select' in use_summary:
                key = 'used_in_select'
            elif 'ret' in use_summary:
                key = 'used_in_ret'
            else:
                key = f'other: {use_summary[:60]}'
            pattern_counts[key] = pattern_counts.get(key, 0) + 1
            files_checked += 1

print(f"Files checked: {files_checked}")
for k, v in sorted(pattern_counts.items(), key=lambda x: -x[1]):
    print(f"  {v:5d}  {k}")
