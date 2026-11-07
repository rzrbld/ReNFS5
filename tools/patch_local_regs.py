#!/usr/bin/env python3
"""Make the generated translation RECOMP_LOCAL_REGS-capable (idempotent).

pcrecomp's RECOMP_LOCAL_REGS keeps the eight guest registers of a lifted
function in host locals, so the optimiser can hold them in host registers
instead of reloading globals after every guest memory store (upstream measured
about 2x on a renderer together with a constant memory base). Every call macro
(RECOMP_CALL/ICALL/ITAIL, RECOMP_BACKEDGE, CPUID) already writes the registers
back before and reloads them after; what pcrecomp's own generator adds and our
driver (tools/translate_game.py lifts with Lifter.lift_function directly) does
not is:

  * `RECOMP_REGS_LOCALS` at the top of each body,
  * `RECOMP_REGS_OUT()` before every `return;`,
  * `RECOMP_REGS_OUT()` before the closing brace (a body that runs off its end).

The macros are empty unless RECOMP_LOCAL_REGS is defined, so the patched code
is identical in behaviour for the default build. Run after translate_game.py
(the Makefile `translate` target does).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MARK = "/* local-regs: patched by tools/patch_local_regs.py */"
# Bodies that stay on the global registers. dx7z's setstate (8.7k lines, 17
# switch dispatches -> every instruction a join point) takes the aarch64 GCC 12
# register allocator >15 minutes with locals; mixing modes is safe because every
# call/return boundary goes through the globals either way.
EXCLUDE = {'recomp_nfs_dx7z_10003AC0'}
REGS = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'esi', 'edi', 'ebp')
MACROS = ('RECOMP_REGS_LOCALS', 'RECOMP_REGS_OUT', 'RECOMP_REGS_IN')
GLOBAL_BEGIN = ['#ifdef RECOMP_LOCAL_REGS'] + \
    [f'#pragma push_macro("{r}")' for r in REGS + MACROS] + \
    [f'#undef {r}' for r in REGS + MACROS] + \
    [f'#define {r} g_{r}' for r in REGS] + \
    ['#define RECOMP_REGS_LOCALS', '#define RECOMP_REGS_OUT() ((void)0)',
     '#define RECOMP_REGS_IN() ((void)0)', '#endif']
GLOBAL_END = ['#ifdef RECOMP_LOCAL_REGS'] + \
    [f'#pragma pop_macro("{r}")' for r in REGS + MACROS] + ['#endif']
START = re.compile(r'^void recomp_nfs_[a-z0-9_]+_[0-9A-F]{8}\(void\) \{$')
# `return;` outside comments; generated comments are /* ... */ on one line
TOKEN = re.compile(r'/\*.*?\*/|\breturn;')


def patch_line(line):
    return TOKEN.sub(lambda m: m[0] if m[0].startswith('/*') else '{ RECOMP_REGS_OUT(); return; }', line)


def patch(path):
    text = path.read_text()
    if MARK in text:
        return False
    out, inside, funcs = [], False, 0
    for line in text.split('\n'):
        if not inside:
            if START.match(line):
                inside, entered = True, False
                excluded = line.split('(')[0].split()[-1] in EXCLUDE
                if excluded:
                    out.extend(GLOBAL_BEGIN)
                funcs += 1
            out.append(line)
            continue
        if line == '}':
            out.append('    RECOMP_REGS_OUT();')
            out.append(line)
            if excluded:
                out.extend(GLOBAL_END)
            inside = False
            continue
        if not entered and line.lstrip().startswith('RECOMP_ENTER('):
            out.append('    RECOMP_REGS_LOCALS')
            entered = True
        elif not entered and line.strip() and not line.lstrip().startswith(('int ', 'uint32_t ', '/*')):
            out.append('    RECOMP_REGS_LOCALS')   # a hand-written override body
            entered = True
        # hand-written override bodies must use the register names, not globals
        if re.search(r'\bg_(eax|ecx|edx|ebx|esp|ebp|esi|edi)\b', line):
            raise SystemExit(f"{path}: direct register-global use in a body: {line.strip()!r}")
        out.append(patch_line(line))
    if inside:
        raise SystemExit(f"{path}: unterminated function body")
    path.write_text(MARK + '\n' + '\n'.join(out))
    return funcs


# x86 `lock inc/dec [m32]` is atomic; the lift emits a plain read-modify-write.
# With guest threads running in parallel (RECOMP_THREADED_CPU) that loses
# updates, so make each one a real atomic. The flag snapshot keeps the old
# value, exactly as before. Idempotent: rewritten sites no longer match.
LOCKED = re.compile(
    r'^(?P<ind>\s*)_flag_a = \(uint32_t\)\(MEM32\((?P<a>[^()]*(?:\([^()]*\))?[^()]*)\)\); _flag_b = \(uint32_t\)\(1\);\n'
    r'(?P=ind)MEM32\((?P=a)\) = MEM32\((?P=a)\) (?P<op>[+-]) 1; (?P<c>/\* 0x[0-9A-F]+: lock (?:inc|dec) [^*]*\*/)$',
    re.M)


def atomic_locked(path):
    text = path.read_text()
    def sub(m):
        fn = '__atomic_fetch_add' if m['op'] == '+' else '__atomic_fetch_sub'
        return (f"{m['ind']}_flag_a = {fn}((uint32_t *)ADDR({m['a']}), 1u, __ATOMIC_SEQ_CST); "
                f"_flag_b = (uint32_t)(1); {m['c']}")
    new, n = LOCKED.subn(sub, text)
    if n:
        path.write_text(new)
    return n


# Condition flags across calls (RECOMP_NO_CALL_FLAGS, recomp_types.h).
# Compiled x86 code rarely keeps flags live across a call, yet the lift copies
# them out before every call, in at every entry, out at every return and in
# after every call. tools/flag_liveness.py finds the exceptions; this pass
# marks every other hand-off with the RECOMP_CALL_/RET_/ENTRY_ variants the
# build can drop, and makes the exceptions explicit:
#   * entry readers (CRT math helpers entered by `call` right after a compare,
#     pushfd at entry): RECOMP_FLAGS_IN() after RECOMP_ENTER, RECOMP_FLAGS_OUT()
#     before each direct call to them (indirect calls/tail jumps always pass
#     flags: RECOMP_ICALL/ITAIL keep RECOMP_FLAGS_OUT)
#   * call sites that read the callee's flags: plain RECOMP_FLAGS_IN() after
#     the call and RECOMP_FLAGS_OUT() at the callee's returns.
# Idempotent: re-running recomputes every marker from the analysis.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_liveness  # noqa: E402

ENTRY_MARK = ' RECOMP_FLAGS_IN(); /* flags live at entry */'
CALL_MARK = 'RECOMP_FLAGS_OUT(); /* callee reads flags */ '


def call_flags(files):
    texts = {}
    for f in files:
        t = f.read_text().replace(ENTRY_MARK, '').replace(CALL_MARK, '')
        t = t.replace('RECOMP_CALL_FLAGS_IN();', 'RECOMP_FLAGS_IN();')
        t = t.replace('RECOMP_RET_FLAGS_OUT(); esp', 'RECOMP_FLAGS_OUT(); esp')
        texts[f] = t
    entry, sites, ret_callees = flag_liveness.analyse(texts.values())
    bad = [c for c in ret_callees if not c.startswith('recomp_')]
    if bad:
        raise SystemExit(f"flags read after an indirect call ({bad}): not supported")
    for f, text in texts.items():
        out, cur = [], None
        lines = text.split('\n')
        for i, l in enumerate(lines):
            m = flag_liveness.START.match(l)
            if m:
                cur = m.group(1)
            if cur in entry and 'RECOMP_ENTER(' in l:
                l = l + ENTRY_MARK
            c = flag_liveness.CALL.search(l)
            if c and c.group(1) == 'CALL' and c['t'].strip() in entry:
                l = l.replace('RECOMP_CALL(', CALL_MARK + 'RECOMP_CALL(', 1)
            if (l.strip() == 'RECOMP_FLAGS_IN();' and out
                    and flag_liveness.CALL.search(out[-1])
                    and (cur, out[-1].replace(CALL_MARK, '').strip()) not in sites):
                l = l.replace('RECOMP_FLAGS_IN();', 'RECOMP_CALL_FLAGS_IN();')
            if 'RECOMP_FLAGS_OUT(); esp' in l and cur not in ret_callees:
                l = l.replace('RECOMP_FLAGS_OUT(); esp', 'RECOMP_RET_FLAGS_OUT(); esp')
            out.append(l)
        new = '\n'.join(out)
        if new != f.read_text():
            f.write_text(new)
    return entry, sites, ret_callees


# Lifted functions replaced by a hand-written wrapper in runtime/: the lifted
# body is renamed <name>_lifted and the wrapper keeps the original name.
WRAPPED = {
    "recomp_nfs_porsche_00565030": "runtime/nfs5_timer_init.c (TIMER_init retry)",
    "recomp_nfs_porsche_00565270": "runtime/nfs5_timer_init.c (timer thread: running count)",
    "recomp_nfs_porsche_00564FA0": "runtime/nfs5_timer_init.c (timer shutdown: wait for the thread)",
    "recomp_nfs_porsche_00539F50": "runtime/nfs5_matrix.c (4x4 batch transform)",
    "recomp_nfs_porsche_0053A6C0": "runtime/nfs5_matrix.c (cofactor determinant)",
}


def wrap_functions(files):
    done = []
    for f in files:
        t = f.read_text()
        n = t
        for name in WRAPPED:
            n = n.replace(f"void {name}(void) {{", f"void {name}_lifted(void) {{")
        if n != t:
            f.write_text(n)
        done += [name for name in WRAPPED if f"void {name}_lifted(void) {{" in n]
    missing = set(WRAPPED) - set(done)
    if missing:
        raise SystemExit(f"wrapped functions not found: {sorted(missing)}")
    return done


def main():
    files = sorted((ROOT / "build/translated").glob("*/functions_*.c"))
    if not files:
        sys.exit("no generated sources under build/translated")
    locked = sum(atomic_locked(f) for f in files)
    print(f"atomic lock inc/dec: {locked} sites")
    print(f"wrapped: {wrap_functions(files)}")
    entry, sites, callees = call_flags(files)
    print(f"call flags: entry readers {sorted(entry)}")
    print(f"call flags: {len(sites)} call site(s) read callee flags; callees keeping them: {sorted(callees)}")
    total = 0
    for f in files:
        n = patch(f)
        if n:
            total += n
            print(f"local-regs: {f.relative_to(ROOT)} ({n} functions)")
    print(f"local-regs: {total} functions patched")


if __name__ == '__main__':
    main()
