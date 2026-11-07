"""Condition-flag liveness over lifted pcrecomp C (used by patch_local_regs.py).

The lift hands the x86 flags (_flag_k/_flag_a/_flag_b/_cf) between functions
through thread-locals at every call, entry and return. Compiled code almost
never keeps flags live across those edges, so RECOMP_NO_CALL_FLAGS drops the
hand-off, except where this analysis finds a path that does:

  * entry readers: a function that can read flags on some path from its entry
    before writing them (CRT math helpers entered with `call` right after a
    compare, pushfd at entry, mid-function chunks)
  * return readers: a call site that can read flags after the callee returns
    before writing them (CRT 0x5A4F25 returns a result in ZF)

Paths are followed through labels, conditional and unconditional gotos. The
carry and the other flags are tracked separately (inc/dec keep CF). A call,
return or tail jump ends a path. Unparseable shapes count as reads, so
mistakes keep flags (slower) rather than drop them (wrong).
"""
import re

START = re.compile(r'^void (recomp_\w+)\(void\)')
READ_ALL = re.compile(r'\b(recomp_eflags_setcf|recomp_cond|recomp_cond_cf|recomp_flags_sbb|'
                      r'recomp_flags_adc|recomp_eflags)\s*\(')
READ_CF = re.compile(r'\brecomp_carry\s*\(|\b_cf\b')
READ_ZSO = re.compile(r'\b(CMP_[A-Z]+|TEST_[A-Z]+)\s*\(|\b_flag_[kab]\b')
CALL = re.compile(r'RECOMP_(CALL|ICALL)\((?P<t>[^;]*)\);')
ALL = frozenset({'cf', 'zso'})
CALL_MARK_TEXT = 'RECOMP_FLAGS_OUT(); /* callee reads flags */ '


def _split_if(st):
    """'if (cond) rest' -> (cond, rest) with balanced parentheses, else None."""
    if not st.startswith('if ('):
        return None
    depth = 0
    for k in range(3, len(st)):
        depth += {'(': 1, ')': -1}.get(st[k], 0)
        if depth == 0:
            return st[4:k], st[k + 1:].strip()
    return None


def _events(code):
    """Flag reads ('r') and kills ('k') of one line, in order."""
    ev = []
    for st in re.split(r';\s*', code):
        st = st.strip().lstrip('{').strip()
        if not st or st == '}':
            continue
        definite = True
        m = _split_if(st)
        if m and not st.startswith('if (recomp_cond'):
            c = m[0]
            definite = bool(re.fullmatch(r'[1-9]\d*u?', c))
            if READ_ALL.search(c):
                ev.append(('r', ALL))
            if READ_ZSO.search(c):
                ev.append(('r', {'zso'}))
            if re.search(r'\b_cf\b', c):
                ev.append(('r', {'cf'}))
            st = m[1].lstrip('{').strip()
        if READ_ALL.search(st):
            ev.append(('r', ALL))
        if re.match(r'\w+\s*=[^=]', st):
            lhs, _, rhs = st.partition('=')
        else:
            lhs, rhs = '', st
        if READ_CF.search(rhs):
            ev.append(('r', {'cf'}))
        if READ_ZSO.search(rhs):
            ev.append(('r', {'zso'}))
        lhs = lhs.strip()
        if definite and lhs == '_cf':
            ev.append(('k', {'cf'}))
        if definite and lhs in ('_flag_k', '_flag_a'):
            ev.append(('k', {'zso'}))
    return ev


def _labels(lines):
    out = {}
    for i, l in enumerate(lines):
        m = re.match(r'\s*(L_\w+):', l)
        if m:
            out[m.group(1)] = i
    return out


def reads_flags_from(lines, labels, i0, readers=frozenset()):
    """True if flags live at line i0 can be read before being overwritten.
    A direct call to a function in `readers` (reads flags at entry) is a read."""
    stack, seen, steps = [(i0, ALL)], set(), 0
    while stack:
        i, live = stack.pop()
        while i < len(lines) and live:
            steps += 1
            if steps > 50000:
                return True
            if (i, live) in seen:
                break
            seen.add((i, live))
            raw = lines[i]
            code = raw.strip().split('/*')[0].strip()
            # shift by a variable count: `if (n) _cf = ..;` then `if (n) { ..pack(.., _cf, ..) }`
            # under the same guard - the second only runs when the first did
            pair = _split_if(code)
            if pair and i > 0:
                prev = _split_if(lines[i - 1].strip().split('/*')[0].strip())
                if prev and prev[0] == pair[0] and re.match(r'_cf\s*=', prev[1]):
                    code = re.sub(r'\b_cf\b', '0', code)
            i += 1
            if not code or re.match(r'L_\w+:', code):
                continue
            if raw.startswith('}'):
                break
            c = CALL.search(code)
            if c and c.group(1) == 'CALL' and c['t'].strip() in readers:
                return True
            if 'RECOMP_CALL(' in code or 'RECOMP_ICALL(' in code or 'RECOMP_ITAIL' in code:
                break
            if 'return' in code and 'goto' not in code:
                break
            g = re.search(r'goto (L_\w+)', code)
            if g:
                if any(t == 'r' and s & live for t, s in _events(code.split('goto')[0])):
                    return True
                target = labels.get(g.group(1))
                if target is None:
                    return True
                if code.startswith('goto'):
                    i = target
                else:
                    stack.append((target, live))
                continue
            for t, s in _events(code):
                if t == 'r' and s & live:
                    return True
                if t == 'k':
                    live = live - s
    return False


def functions(text):
    lines = text.split('\n')
    starts = [i for i, l in enumerate(lines) if START.match(l)] + [len(lines)]
    for a, b in zip(starts, starts[1:]):
        yield START.match(lines[a]).group(1), lines[a:b]


def analyse(texts):
    """-> (entry_readers: functions that can read flags at entry,
           return_sites: (function, call line) that can read the callee's flags,
           callees whose returned flags are read)

    Iterates to a fixed point: a call to an entry reader reads the caller's
    flags, so a function calling one before writing flags is itself an entry
    reader, and a call followed by one reads the first callee's flags
    (0x5A1060: call 0x5A4F68 (result in ZF); call 0x5A107D (je at entry))."""
    fns = [(name, fl, _labels(fl)) for text in texts for name, fl in functions(text)]
    entry = set()
    while True:
        new = set()
        for name, fl, labels in fns:
            for i, l in enumerate(fl):
                if 'RECOMP_ENTER(' in l:
                    if reads_flags_from(fl, labels, i + 1, entry):
                        new.add(name)
                    break
        if new <= entry:
            break
        entry |= new
    sites, ret_callees = set(), set()
    for name, fl, labels in fns:
        for i, l in enumerate(fl):
            m = CALL.search(l)
            if m and reads_flags_from(fl, labels, i + 1, entry):
                sites.add((name, l.replace(CALL_MARK_TEXT, '').strip()))
                ret_callees.add(m['t'].strip())
    return entry, sites, ret_callees
