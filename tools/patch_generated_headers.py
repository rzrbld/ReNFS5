#!/usr/bin/env python3
"""Post-process generated translation headers (idempotent).

The generated code must see the project's virtual-CPUID override instead of the
lifter's default no-op. This also keeps older generated trees working.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for header in sorted((ROOT / "build/translated").glob("*/functions.h")):
    text = header.read_text()
    text = text.replace('#include "cpu_features.h"', '#include "nfs5_cpu_features.h"')
    text = text.replace('#include "nfs5_cpu_features.h"\n#include "nfs5_cpu_features.h"',
                        '#include "nfs5_cpu_features.h"')
    header.write_text(text)
    print(f"patched {header.relative_to(ROOT)}")

# The renderer-heavy guest uses x87 operations in its geometry path. Rewrite
# the lifter's top-first array accesses to the circular stack's logical slots.
# Comments and literals are skipped; any unexpected raw use stops the build.
import re
X87 = re.compile(
    r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|'
    r'(?P<access>\b(?P<name>_st|g_st_i64)\s*\[\s*(?P<index>\d+)\s*\])|'
    r'(?P<bare>\b(?:_st|g_st_i64)\b)', re.S)

def logical_slot(match):
    if match['bare']:
        raise ValueError(f"unsupported raw x87 reference: {match[0]}")
    if not match['access']:
        return match[0]
    index = int(match['index'])
    if index >= 8:
        raise ValueError(f"invalid x87 slot: {index}")
    name = 'FP_ST' if match['name'] == '_st' else 'FP_SI'
    return f'{name}({index})'

for source in sorted((ROOT / "build/translated").glob("*/functions_*.c")):
    before = source.read_text()
    after = X87.sub(logical_slot, before)
    if after != before:
        source.write_text(after)
        print(f"x87 logical slots: {source.relative_to(ROOT)}")
