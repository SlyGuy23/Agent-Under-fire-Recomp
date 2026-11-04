# Checks an unpacked release carries no game code before it is published:
# - no game code DLL, no xbe or disc image, no compiler work folder or generated sources or objects;
# - each game executable delay loads its <exe>.dll, where the recompiled code goes on the player's PC;
# - no file holds a run of the game's own x86 code: windows from each xbe's executable sections are looked for in
#   every file of the release. Windows also found in a reference file (the toolchain's own clang.exe and lld-link.exe,
#   from the Visual Studio install) are common tables, such as math constants the XDK's libraries share with any C
#   runtime, and are left out; a release file identical to a reference is that toolchain file.
#   python check_release.py <release folder> <xbe>... [--reference <file>]...
import os
import random
import struct
import sys

IMAGES = ["AgentUnderFire", "AgentUnderFireGame", "AgentUnderFireDriving"]
RUNTIME_DLLS = {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"}
WINDOW = 48
SAMPLES = 400


def xbe_code(path):
    data = open(path, "rb").read()
    if data[:4] != b"XBEH":
        sys.exit(f"{path} is not an xbe")
    base, count, headers = struct.unpack_from("<I", data, 0x104)[0], *struct.unpack_from("<II", data, 0x11C)
    code = []
    for i in range(count):
        flags, _, _, raw, size, name = struct.unpack_from("<IIIIII", data, headers - base + i * 0x38)
        label = data[name - base:data.index(b"\0", name - base)].decode("ascii", "replace")
        if flags & 4:  # executable
            code.append((label, data[raw:raw + size]))
    return code


def samples(code, rng):
    # windows of varied bytes (not padding or tables of one value)
    out = []
    total = sum(len(body) for _, body in code)
    for _, body in code:
        want = max(1, SAMPLES * len(body) // total)
        got = 0
        for _ in range(want * 20):
            if got == want or len(body) <= WINDOW:
                break
            at = rng.randrange(0, len(body) - WINDOW)
            w = body[at:at + WINDOW]
            if len(set(w)) >= 24:
                out.append(w)
                got += 1
    return out


def delay_dlls(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    nsec, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    dirs = opt + (112 if struct.unpack_from("<H", data, opt)[0] == 0x20B else 96)
    secs = [struct.unpack_from("<IIII", data, opt + opt_size + i * 40 + 8) for i in range(nsec)]

    def off(rva):
        for vsize, va, rsize, raw in secs:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError(rva)

    rva, _ = struct.unpack_from("<II", data, dirs + 13 * 8)
    found = {}
    if not rva:
        return found
    at = off(rva)
    while True:
        attrs, name, _, _, names = struct.unpack_from("<IIIII", data, at)[:5]
        if not name:
            return found
        dll = data[off(name):data.index(b"\0", off(name))].decode()
        n, p = 0, off(names)
        while struct.unpack_from("<Q", data, p)[0]:
            n, p = n + 1, p + 8
        found[dll] = n
        at += 32


def main():
    args = sys.argv[2:]
    root, refs, xbes = sys.argv[1], [], []
    while args:
        a = args.pop(0)
        (refs.append(args.pop(0)) if a == "--reference" else xbes.append(a))
    references = [open(r, "rb").read() for r in refs]
    bad = []
    files = []
    for d, dirs, names in os.walk(root):
        rel = os.path.relpath(d, root)
        for x in dirs:
            if x.lower() in ("work", "generated"):
                bad.append(f"folder {os.path.join(rel, x)}")
        for n in names:
            p = os.path.join(d, n)
            files.append(p)
            low = n.lower()
            ext = os.path.splitext(low)[1]
            if ext == ".dll" and low not in RUNTIME_DLLS:
                bad.append(f"game code DLL {os.path.relpath(p, root)}")
            if ext in (".xbe", ".iso", ".obj", ".o", ".def") or low.startswith("x86_recomp") or low == "built.txt":
                bad.append(f"game derived file {os.path.relpath(p, root)}")
    total = 0
    for p in sorted(files):
        size = os.path.getsize(p)
        total += size
        print(f"{size:>12}  {os.path.relpath(p, root)}")
    print(f"{total:>12}  in {len(files)} files")

    for image in IMAGES:
        exe = os.path.join(root, "Game Files", image + ".exe")
        if not os.path.exists(exe):
            bad.append(f"missing {image}.exe")
            continue
        dlls = delay_dlls(exe)
        if not dlls or image + ".dll" not in dlls:
            bad.append(f"{image}.exe does not delay load {image}.dll")
        else:
            print(f"{image}.exe delay loads {image}.dll ({dlls[image + '.dll']} functions its hooks call)")

    rng = random.Random(482)
    windows = []
    for x in xbes:
        code = xbe_code(x)
        windows += [(os.path.basename(x), w) for w in samples(code, rng)]
        print(f"{os.path.basename(x)}: code sections {', '.join(f'{n} {len(b)}' for n, b in code)}")
    common = [(x, w) for x, w in windows if any(w in r for r in references)]
    windows = [(x, w) for x, w in windows if (x, w) not in common]
    for x, w in common:
        print(f"left out a window of {x} also in the toolchain (a common table): {w.hex()}")
    print(f"looking for {len(windows)} windows of {WINDOW} bytes of the game's code in the release")
    for p in files:
        data = open(p, "rb").read()
        if any(data == r for r in references):
            print(f"{os.path.relpath(p, root)} is the toolchain's own file")
            continue
        hits = [x for x, w in windows if w in data]
        if hits:
            bad.append(f"{os.path.relpath(p, root)} holds {len(hits)} runs of the game's code ({hits[0]})")

    if bad:
        print("\nNOT CLEAN:\n  " + "\n  ".join(bad))
        sys.exit(1)
    print("\nclean: no game code in the release")


if __name__ == "__main__":
    main()
