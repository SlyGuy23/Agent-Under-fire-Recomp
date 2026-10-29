#!/usr/bin/env python3
"""Post-process generated C: bring-up switches the recompiler has no option for.

    python tools/patch_gen.py <gen_dir> <patch_list.json>

patch_list.json:
  {"skip_if_env": {"AUF_NOSOUND": ["0x000139C0", "0x000148A0", ...]}}

Each listed function gets, as its first statement,

    if (auf_env("AUF_NOSOUND")) { eax = 0; esp += N; return; }

where N is the stack clean-up of the function's own `ret` (read from the
generated body), so callers see a normal return. The check is made at run time
and cached, so the switch can be flipped without rebuilding.
"""
import json
import re
import sys
from pathlib import Path

HELPER = r'''
#include <stdlib.h>
static int auf_env(const char *name)
{
    /* Cached per call site name; getenv is not cheap on a hot path. */
    static const char *names[8]; static int vals[8]; static int n;
    int i;
    for (i = 0; i < n; i++) if (names[i] == name) return vals[i];
    if (n < 8) {
        const char *e = getenv(name);
        names[n] = name; vals[n] = e && *e && *e != '0';
        return vals[n++];
    }
    { const char *e = getenv(name); return e && *e && *e != '0'; }
}
'''


def main():
    gen = Path(sys.argv[1])
    spec = json.load(open(sys.argv[2]))
    wanted = {}
    for env, addrs in spec.get("skip_if_env", {}).items():
        for a in addrs:
            wanted[int(a, 16)] = env

    done = set()
    for f in sorted(gen.glob("recomp_*.c")):
        text = f.read_text(errors="replace")
        changed = False
        for addr, env in wanted.items():
            name = f"sub_{addr:08X}"
            m = re.search(rf"^void {name}\(void\)\n\{{\n", text, re.M)
            if not m:
                continue
            body_end = text.find("\n}\n", m.end())
            body = text[m.end():body_end]
            rets = re.findall(r"esp \+= (\d+); return;", body)
            if not rets:
                print(f"  {name}: no plain return found, left alone")
                continue
            n = max(set(rets), key=rets.count)
            guard = (f'    if (auf_env("{env}")) {{ eax = 0; esp += {n}; return; }}'
                     f' /* bring-up: {env} */\n')
            text = text[:m.end()] + guard + text[m.end():]
            done.add(addr)
            changed = True
        if changed:
            inc = '#include "recomp_funcs.h"\n'
            if "static int auf_env(" not in text:
                text = text.replace(inc, inc + HELPER, 1)
            f.write_text(text)
    missing = set(wanted) - done
    print(f"patch_gen: guarded {len(done)} function(s)"
          + (f"; not found: {', '.join(hex(a) for a in sorted(missing))}" if missing else ""))


if __name__ == "__main__":
    main()
