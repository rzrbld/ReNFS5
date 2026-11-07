#!/usr/bin/env python3
"""NFS5 static recompilation driver: PE32 x86 -> C via pcrecomp lift32.

Modules are translated from the user's own original game files. Output goes to
build/translated/<module>/ and is private (generated proprietary game code):
never distribute it. Every module is rebased into a private range so the host's
preferred-base PE loader maps it exactly where the translated code expects.
"""
import argparse, collections, hashlib, json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for part in ("pe", "disasm", "lift"):
    sys.path.insert(0, str(ROOT / "third_party/pcrecomp/tools" / part))
import pefile
from pe_analyze import analyze_pe, build_iat_map
from disasm32 import Disassembler
from lift32 import Lifter

PREFIX = "nfs"
# Private module bases. The main EXE keeps its original base (0x400000). DLLs are
# rebased above it; the guest heap starts above the whole range (see runtime).
MODULE_BASES = {
    "porsche.exe": 0x00400000,
    "dx7z.dll": 0x10000000,
    "dplayerx.dll": 0x11000000,
}


class MappedDisassembler(Disassembler):
    """Disassembler over the memory-mapped image (raw sections are shortened)."""

    def __init__(self, data, base, sections, mapped):
        super().__init__(data, base, sections)
        self.mapped = mapped

    def read_bytes(self, va, size):
        offset = va - self.image_base
        if offset < 0 or offset >= len(self.mapped):
            return None
        return self.mapped[offset:offset + size]


BACKEDGE_RE = re.compile(r"goto (L_([0-9A-Fa-f]+)); /\* 0x([0-9A-Fa-f]+):")
BACKEDGE_MARK = "L_"


def add_backedges(source):
    """Insert RECOMP_BACKEDGE() before every backward branch in a lifted body.

    The reference pipeline (pcrecomp tools/lift/generate.py) does this while it
    emits; our driver uses Lifter.lift_function() directly, so the generated
    code never called the yield hook. Each backward goto is matched against the
    instruction address in its own source comment.
    """
    out = []
    added = 0
    for line in source.split("\n"):
        m = BACKEDGE_RE.search(line)
        if m and int(m.group(2), 16) <= int(m.group(3), 16):
            out.append("    RECOMP_BACKEDGE();")
            added += 1
        out.append(line)
    return "\n".join(out), added


def add_ret_checks(source):
    """Insert RECOMP_RET_CHECK() before every lifted `ret`.

    The macro is empty unless the build defines RECOMP_STACKCHECK; when it is
    defined, runtime/nfs5_shadow.c verifies the callee returned with esp at the
    caller's dummy-return-address slot, which pinpoints stdcall-purge bugs.
    """
    out = []
    for line in source.split("\n"):
        if "RECOMP_FLAGS_OUT(); esp +=" in line and "return;" in line:
            out.append("    RECOMP_RET_CHECK();")
        out.append(line)
    return "\n".join(out)


# Guest functions intentionally replaced in this port, keyed by (module, VA).
# Each entry must say why; the body is C compiled with the same symbol name.
#   dx7z 0x1000C390: THRASH texture mip-chain builder. Our renderer uploads and
#   samples only the base mip level (the GLES path has no mip chain for the
#   hardware formats the game uses), so the mip fill is dead work - and it is
#   also the source of every residual race-session crash (guest faults at
#   guest_base+0x0 / guest_base+0xC4 when it walks and writes mip data through
#   pointers whose chain/shape differs from real DirectDraw). Returning success
#   without doing the work removes that failure mode without changing what is
#   rendered. Re-enable by deleting this entry and rebuilding.
FUNCTION_OVERRIDES = {
    ("dx7z", 0x1000C390): dict(
        why="THRASH mip-chain build is unused by the port renderer and crashes; "
            "skip it (documented in docs/KNOWN_ISSUES.md)",
        body="eax = 1; esp += 4; return;   /* eax=TRUE, pop return address (register names: RECOMP_LOCAL_REGS-safe) */",
    ),
}


def translate(path, target_base, outroot):
    print(f"MODULE {path.name}", flush=True)
    data = path.read_bytes()
    pe = pefile.PE(data=data)
    original_data = data
    analysis_path = path
    if target_base is not None and target_base != pe.OPTIONAL_HEADER.ImageBase:
        pe.relocate_image(target_base)
        data = pe.write()
        analysis_path = outroot / "rebased" / path.name
        analysis_path.parent.mkdir(parents=True, exist_ok=True)
        analysis_path.write_bytes(data)
        pe = pefile.PE(data=data)
    info = analyze_pe(str(analysis_path))
    if pe.FILE_HEADER.Machine != 0x14C:
        raise ValueError("Expected x86 PE32")
    overrides = []
    for section in info.sections:
        if section.name == ".text" or section.name.startswith("stxt"):
            if not section.is_code:
                overrides.append(section.name)
            section.characteristics |= 0x20000020
    code = [s for s in info.sections if s.is_code]
    low = info.image_base + min(s.virtual_address for s in code)
    high = info.image_base + max(s.virtual_address + s.raw_size for s in code)
    dis = MappedDisassembler(data, info.image_base, info.sections, pe.get_memory_mapped_image())
    iat = build_iat_map(info)
    seeds = [info.image_base + info.entry_point_rva]
    seeds += [info.image_base + e.rva for e in info.exports if e.rva]
    funcs = dis.find_functions(low, high, iat, seeds=seeds)
    if not funcs or seeds[0] not in funcs:
        raise ValueError("Entry point not recovered")
    module = re.sub(r"[^a-z0-9]", "_", path.stem.lower())
    names = {addr: f"{PREFIX}_{module}_{addr:08X}" for addr in funcs}
    for addr, f in funcs.items():
        f.name = "recomp_" + names[addr]
    lifter = Lifter(iat_map=iat, func_names=names, lifted=set(funcs),
                    precise_sbb=True, precise_carry=True)
    out = outroot / "translated" / module
    out.mkdir(parents=True, exist_ok=True)
    for pattern in ("functions_*.c", "*.o", "*.compile.log", "*.a", "dispatch.c",
                    "dispatch.o", "functions.h", "catalog.json"):
        for old in out.glob(pattern):
            old.unlink()
    header = "#pragma once\n#include \"recomp_types.h\"\n#include \"nfs5_cpu_features.h\"\n"
    header += f"void {PREFIX}_unsupported_instruction(uint32_t address, const char *instruction);\n"
    header += "\n".join(f"void {f.name}(void);" for _, f in sorted(funcs.items())) + "\n"
    (out / "functions.h").write_text(header)
    unsupported, exceptions, catalog = [], [], []
    overrides_applied = []
    backedge_count = 0
    chunk, chunk_no = [], 0

    def flush():
        nonlocal chunk_no, chunk
        if not chunk:
            return
        (out / f"functions_{chunk_no:04d}.c").write_text(
            "/* Generated translated game code from the user's original files. Keep local. */\n"
            "#define RECOMP_GENERATED_CODE\n#include \"functions.h\"\n" + "\n".join(chunk))
        chunk_no += 1
        chunk = []

    for addr, f in sorted(funcs.items()):
        catalog.append(dict(va=hex(addr), rva=hex(addr - info.image_base), symbol=f.name,
                            instructions=f.num_instructions, kind=f.entry_kind))
        try:
            override = FUNCTION_OVERRIDES.get((module, addr))
            if override is not None:
                # Deliberate, documented replacement of a guest function. The
                # override body is C source; the wrapper keeps the same name and
                # calling convention (caller pushes a dummy return address, the
                # body pops it like a `ret`).
                source = (f"void {f.name}(void) {{\n"
                          f"    /* port override: {override['why']} */\n"
                          f"    {override['body']}\n"
                          f"}}\n")
                overrides_applied.append(dict(function=hex(addr), why=override["why"]))
                chunk.append(source)
                if len(chunk) >= 100:
                    flush()
                continue
            source = lifter.lift_function(f)

            def trap(match):
                instruction, va = match.group(1), int(match.group(2), 16)
                unsupported.append(dict(function=hex(addr), address=hex(va), instruction=instruction))
                return (f"{PREFIX}_unsupported_instruction(0x{va:08X}u, "
                        f"{json.dumps(instruction)});")

            source = re.sub(r"; /\* UNIMPLEMENTED: (.*?) \*/ /\* 0x([0-9A-Fa-f]+):.*?\*/", trap, source)
            if "UNIMPLEMENTED:" in source:
                raise ValueError("Unrecognized unsupported-instruction marker")
            # A backward goto is a loop. Lifted spin-waits make no native call,
            # so without this the thread holding the machine lock never hands it
            # over and any guest worker thread it is waiting for starves. NFS5's
            # front-end movie player spins on a stream state filled by a per-slot
            # async file worker and hung exactly that way.
            source, added = add_backedges(source)
            backedge_count += added
            source = add_ret_checks(source)
        except Exception as exc:
            exceptions.append(dict(function=hex(addr), error=str(exc)))
            source = (f"void {f.name}(void) {{ {PREFIX}_unsupported_instruction("
                      f"0x{addr:08X}u, \"lift exception\"); }}\n")
        chunk.append(source)
        if len(chunk) >= 100:
            flush()
    flush()
    dispatch = '#include "functions.h"\n'
    dispatch += f"const recomp_dispatch_entry_t {PREFIX}_{module}_dispatch[] = {{\n"
    dispatch += "\n".join(f"{{0x{addr:08X}u, {f.name}}}," for addr, f in sorted(funcs.items()))
    dispatch += f"\n}};\nconst uint32_t {PREFIX}_{module}_count = {len(funcs)};\n"
    (out / "dispatch.c").write_text(dispatch)
    report = dict(module=path.name,
                  sha256=hashlib.sha256(original_data).hexdigest(),
                  image_base=hex(info.image_base),
                  original_image_base=hex(pefile.PE(data=original_data).OPTIONAL_HEADER.ImageBase),
                  image_size=info.sections[-1].virtual_address + info.sections[-1].effective_size,
                  entry=hex(seeds[0]), metadata_overrides=overrides,
                  functions=len(funcs),
                  instructions=sum(f.num_instructions for f in funcs.values()),
                  backedge_yields=backedge_count,
                  function_overrides=overrides_applied,
                  unsupported=unsupported, lift_exceptions=exceptions,
                  unsupported_mnemonics=dict(collections.Counter(
                      e["instruction"].split()[0] for e in unsupported)),
                  exports=[dict(name=e.name, va=hex(info.image_base + e.rva))
                           for e in info.exports],
                  limitations=["Function discovery is heuristic; indirect target closure is unverified.",
                               "DLL dispatch selects a loaded module, not just a virtual address.",
                               "No semantic differential testing or game execution established."])
    (out / "catalog.json").write_text(json.dumps(catalog, indent=2) + "\n")
    ev = ROOT / "docs/evidence"
    ev.mkdir(parents=True, exist_ok=True)
    (ev / f"translation-{module}.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"LIFTED {len(funcs)} functions, {report['instructions']} instructions; "
          f"backedge-yield sites={backedge_count}; "
          f"unsupported={len(unsupported)} exceptions={len(exceptions)}", flush=True)
    if unsupported:
        print("  unsupported mnemonics:", report["unsupported_mnemonics"], flush=True)
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", default=str(ROOT / "build/game-input"))
    ap.add_argument("--out", default=str(ROOT / "build"))
    ap.add_argument("--modules", nargs="*", default=["Porsche.exe", "Drivers/dx7z.dll"])
    ap.add_argument("--report", default=str(ROOT / "docs/evidence/translation-summary.json"))
    args = ap.parse_args()
    game = Path(args.game_dir)
    outroot = Path(args.out)
    reports = []
    for rel in args.modules:
        path = game / rel
        if not path.is_file():
            print(f"missing module {path}", file=sys.stderr)
            continue
        reports.append(translate(path, MODULE_BASES.get(path.name.lower()), outroot))
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(json.dumps(reports, indent=2) + "\n")
    print(f"translated {len(reports)} modules -> {outroot/'translated'}")


if __name__ == "__main__":
    main()
