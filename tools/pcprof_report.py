#!/usr/bin/env python3
"""Symbolise an NFS5_PCPROF raw dump: pcprof_report.py <dump> <unstripped binary>.

Main-executable PCs go through addr2line (static functions included); other
PCs are attributed to their mapping from <dump>.maps. Run addr2line for the
target architecture (e.g. inside the cross image) via $ADDR2LINE.
"""
import collections, os, subprocess, sys

dump, binary = sys.argv[1], sys.argv[2]
pcs = [int(l, 16) for l in open(dump) if l[0] != '#' and l.strip()]
maps = []
for l in open(dump + '.maps'):
    p = l.split()
    lo, hi = (int(x, 16) for x in p[0].split('-'))
    maps.append((lo, hi, p[5] if len(p) > 5 else '[anon]'))
exe = None
for lo, hi, name in maps:
    if name.endswith(os.path.basename(binary).split('.')[0]) or 'nfs5' in os.path.basename(name):
        exe = name
        break
by_obj, main = collections.Counter(), collections.Counter()
for pc in pcs:
    name = next((n for lo, hi, n in maps if lo <= pc < hi), '?')
    base = os.path.basename(name)
    by_obj[base] += 1
    if name == exe:
        main[pc] += 1
uniq = sorted(main)
a2l = os.environ.get('ADDR2LINE', 'addr2line')
out = subprocess.run([a2l, '-f', '-e', binary] + [hex(p) for p in uniq], capture_output=True, text=True).stdout.split('\n')
by_sym = collections.Counter()
for i, pc in enumerate(uniq):
    by_sym[out[2 * i] if 2 * i < len(out) else '?'] += main[pc]
for base, c in by_obj.items():
    if base != os.path.basename(exe or ''):
        by_sym['[' + base + ']'] += c
n = len(pcs)
print(f'# {n} samples')
print('# by object')
for k, c in by_obj.most_common(12):
    print(f'  {100*c/n:6.2f}%  {k}')
print('# by symbol')
for k, c in by_sym.most_common(60):
    print(f'  {100*c/n:6.2f}%  {k}')
