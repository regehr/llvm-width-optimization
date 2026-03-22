#!/usr/bin/env python3
"""Analyze zext i32 to i64 patterns in out.ll files with exactly 1 remaining instruction."""

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
    if '= zext i32' not in out_content or 'to i64' not in out_content:
        continue
    
    lines = out_content.split('\n')
    for i, line in enumerate(lines):
        if '= zext i32' in line and 'to i64' in line:
            m = re.search(r'(%\S+) = zext i32 (%\S+) to i64', line)
            if not m:
                continue
            zext_name = m.group(1)
            # Find uses of zext_name
            uses = [l.strip() for l in lines if zext_name in l and l.strip() != line.strip()]
            
            # Classify by use
            if len(uses) == 1:
                u = uses[0]
                if 'getelementptr' in u or 'GEP' in u:
                    key = 'gep only'
                elif 'call' in u:
                    key = f'call: {u[:50]}'
                elif 'ret i64' in u:
                    key = 'ret i64'
                elif 'store i64' in u:
                    key = 'store i64'
                elif 'add\|sub\|mul\|div' in u:
                    key = f'arith: {u[:40]}'
                elif 'icmp' in u:
                    key = f'icmp: {u[:40]}'
                else:
                    key = f'single use: {u[:60]}'
            else:
                use_types = set()
                for u in uses:
                    if 'getelementptr' in u:
                        use_types.add('gep')
                    elif 'call' in u:
                        use_types.add('call')
                    elif 'icmp' in u:
                        use_types.add('icmp')
                    elif 'add' in u or 'sub' in u or 'mul' in u:
                        use_types.add('arith')
                    elif 'store' in u:
                        use_types.add('store')
                    else:
                        use_types.add('other')
                key = f'multi: {sorted(use_types)}'
            
            pattern_counts[key] = pattern_counts.get(key, 0) + 1
            if key not in examples:
                examples[key] = (base, line.strip(), uses[:3])

print("zext i32 to i64 use patterns (1-remaining files):")
for k, v in sorted(pattern_counts.items(), key=lambda x: -x[1])[:20]:
    print(f"  {v:5d}  {k}")
    if k in examples:
        base, instr, uses = examples[k]
        print(f"          e.g. {base}: {instr}")
        for u in uses[:2]:
            print(f"               uses: {u[:80]}")
