# A small Xbox disc image (XDVDFS game partition, as an xiso) holding the given files in its root, for the release
# check: the first start reads every xbe from the disc image in ROM, as a player's would. Only made on the CI runner,
# from the game's own xbe files, and never published.
#   python make_test_iso.py out.iso default.xbe bond_eng.xbe driving.xbe
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"


def main():
    out, files = sys.argv[1], sys.argv[2:]
    # a folder table is a binary tree ordered by upper case name: here each entry's right child is the next one
    files = sorted(files, key=lambda f: os.path.basename(f).upper())
    entries = []
    for f in files:
        name = os.path.basename(f).encode("ascii")
        entries.append((name, os.path.getsize(f)))
    table = b""
    offsets = []
    for name, _ in entries:
        offsets.append(len(table))
        table += b"\0" * ((14 + len(name) + 3) // 4 * 4)
    root_sector = 33  # the sector after the volume descriptor
    table_sectors = (len(table) + SECTOR - 1) // SECTOR
    sector = root_sector + table_sectors
    body = bytearray(table)
    starts = []
    for i, (name, size) in enumerate(entries):
        right = offsets[i + 1] // 4 if i + 1 < len(entries) else 0
        entry = struct.pack("<HHIIBB", 0, right, sector, size, 0x20, len(name)) + name
        body[offsets[i]:offsets[i] + len(entry)] = entry
        starts.append(sector)
        sector += (size + SECTOR - 1) // SECTOR
    body += b"\xff" * (table_sectors * SECTOR - len(body))  # the rest of a table is 0xFF
    volume = bytearray(SECTOR)
    volume[0:20] = MAGIC
    volume[0x14:0x1C] = struct.pack("<II", root_sector, len(table))
    volume[0x7EC:0x7EC + 20] = MAGIC
    with open(out, "wb") as o:
        o.write(b"\0" * (32 * SECTOR))
        o.write(volume)
        o.write(body)
        for f, start in zip(files, starts):
            o.seek(start * SECTOR)
            with open(f, "rb") as i:
                data = i.read()
            o.write(data)
            o.write(b"\0" * (-len(data) % SECTOR))
    print(f"{out}: {', '.join(os.path.basename(f) for f in files)}, {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()
