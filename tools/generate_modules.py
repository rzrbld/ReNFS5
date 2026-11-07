#!/usr/bin/env python3
"""Generate host-side dispatch glue for the NFS5 translated modules.

- build/game/main_dispatch.c: recomp_dispatch_table/count for the main EXE
  (recomp32's recomp_lookup uses these symbols).
- build/game/modules.c: nfs_module_preferred/loaded/unloaded/lookup used by the
  patched win32hle module.c to map a translated DLL at its rebased base and to
  dispatch calls into its lifted functions by VA.
"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
OUT = BUILD / "game"
PREFIX = "nfs"


def main():
    reports = json.loads((ROOT / "docs/evidence/translation-summary.json").read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    header = '#include "win32hle.h"\n#include <strings.h>\n#include <string.h>\n'
    entries = []
    for report in reports:
        module = re.sub(r"[^a-z0-9]", "_", Path(report["module"]).stem.lower())
        header += (f"extern const recomp_dispatch_entry_t {PREFIX}_{module}_dispatch[];\n"
                   f"extern const uint32_t {PREFIX}_{module}_count;\n")
        catalog = json.loads((BUILD / "translated" / module / "catalog.json").read_text())
        if Path(report["module"]).suffix.lower() == ".exe":
            source = '#include "win32hle.h"\n'
            source += "\n".join(f'extern void {f["symbol"]}(void);' for f in catalog)
            source += "\nconst recomp_dispatch_entry_t recomp_dispatch_table[]={\n"
            source += "\n".join(f'{{{f["va"]}u,{f["symbol"]}}},' for f in catalog)
            source += f"\n}};\nconst uint32_t recomp_dispatch_count={len(catalog)};\n"
            (OUT / "main_dispatch.c").write_text(source)
        else:
            entries.append(
                f'{{"{report["module"]}",{report["image_base"]}u,{report["entry"]}u,'
                f'{PREFIX}_{module}_dispatch,&{PREFIX}_{module}_count,0}}')
    source = header + """
typedef struct { const char *name; uint32_t base,entry; const recomp_dispatch_entry_t *table; const uint32_t *count; int active; } NfsModule;
static NfsModule modules[]={
""" + ",\n".join(entries) + """
};
static const char *basename_(const char *name) {
    const char *p = name;
    for (const char *s = name; *s; s++) if (*s == '/' || *s == '\\\\') p = s + 1;
    return p;
}
static int same_module(const char *a, const char *b) {
    /* Windows module names carry ".dll" only when the caller spells it; NFS5
     * asks LoadLibraryA(".\\drivers\\dx7z") for dx7z.dll. Compare without it. */
    size_t la = strlen(a), lb = strlen(b);
    if (la > 4 && !strcasecmp(a + la - 4, ".dll")) la -= 4;
    if (lb > 4 && !strcasecmp(b + lb - 4, ".dll")) lb -= 4;
    return la == lb && !strncasecmp(a, b, la);
}
uint32_t nfs_module_preferred(const char *name) {
    if (!name) return 0;
    name = basename_(name);
    for (unsigned i = 0; i < sizeof modules / sizeof *modules; i++)
        if (same_module(name, modules[i].name)) return modules[i].base;
    return 0;
}
void nfs_module_loaded(const char *name, uint32_t base) {
    for (unsigned i = 0; i < sizeof modules / sizeof *modules; i++)
        if (modules[i].base == base) { modules[i].active = 1; return; }
    hle_fatal("unregistered translated DLL %s @ %#x", name, base);
}
void nfs_module_unloaded(uint32_t base) {
    for (unsigned i = 0; i < sizeof modules / sizeof *modules; i++)
        if (modules[i].base == base) modules[i].active = 0;
}
recomp_func_t nfs_module_lookup(uint32_t va) {
    for (unsigned i = 0; i < sizeof modules / sizeof *modules; i++) {
        NfsModule *m = &modules[i];
        if (!m->active || va < m->base || va >= m->base + 0x1000000u) continue;
        uint32_t lo = 0, hi = *m->count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (m->table[mid].address == va) return m->table[mid].func;
            if (m->table[mid].address < va) lo = mid + 1; else hi = mid;
        }
    }
    return NULL;
}
"""
    (OUT / "modules.c").write_text(source)
    print(f"generated main dispatch ({sum(1 for r in reports if Path(r['module']).suffix.lower()=='.exe')} exe) "
          f"and {len(entries)} DLL dispatch tables")


if __name__ == "__main__":
    main()
