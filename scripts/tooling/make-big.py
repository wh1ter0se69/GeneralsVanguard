#!/usr/bin/env python3
# GeneralsX:
#
#   make-big.py <out.big> <virtual\path>=<source-file> [...]
#   make-big.py --list <archive.big>
#
# Write a BIGF archive. There was no packer in this tree, and the reason one is needed is
# narrow and worth stating, because it decides what a friend downloads.
#
# WHY. A peer is refused unless its dataID matches, and dataID's loose half hashes the RESOLVED
# BYTES of Data\INI\CommandButton.ini (SimulationId.cpp's allow-list - those bytes reach the
# simulation through WeaponSlot and MaxShotsToFire, so they cannot simply be excluded). The
# fleet's copy of that file comes from !HotkeysLeikezeIndicatorsZH.big, a 38 MB UI mod whose
# other 16 entries are user-interface .tga art the fingerprint never looks at. Measured: of that
# archive's 17 entries exactly ONE is fingerprinted, and data-ini does not move at all.
#
# So making a friend download 28.2 MB compressed buys one 352 KB file. This packs that file
# alone, at 26 KB compressed, and the mount order does the rest: '!' sorts before '5', so the
# archive still wins Data\INI\CommandButton.ini over the Community Patch's deliberate zero-byte
# copy exactly as the full mod did.
#
# FORMAT (big-endian except the size, which is not - that asymmetry is in the format, not a bug):
#   'BIGF' | uint32 LE total size | uint32 BE entry count | uint32 BE first data offset
#   per entry: uint32 BE offset, uint32 BE size, NUL-terminated name
#   then the file data, in entry order
#
# The engine is the only validator that matters here: a plausible archive the game cannot mount
# looks exactly like a correct one until dataID comes out wrong. Verify with
# scripts/test/probe-dataid.sh, never by eye.
import struct
import sys


def read_entries(path):
    with open(path, "rb") as f:
        head = f.read(1 << 20)
    if head[:4] != b"BIGF":
        raise SystemExit(f"{path}: not a BIGF archive")
    count, _first = struct.unpack_from(">II", head, 8)
    pos, out = 16, []
    for _ in range(count):
        off, size = struct.unpack_from(">II", head, pos)
        pos += 8
        end = head.index(b"\0", pos)
        out.append((head[pos:end].decode("latin-1"), off, size))
        pos = end + 1
    return out


def write_big(out_path, members):
    # Header size has to be known before any offset can be written, and every name contributes
    # to it - so the table is measured first and only then filled in.
    header = 16 + sum(8 + len(name.encode("latin-1")) + 1 for name, _ in members)
    blobs, offset, table = [], header, []
    for name, src in members:
        with open(src, "rb") as f:
            data = f.read()
        if not data:
            raise SystemExit(f"{src}: empty - refusing to pack a zero-byte entry")
        table.append((name, offset, len(data)))
        blobs.append(data)
        offset += len(data)

    body = bytearray()
    body += b"BIGF"
    body += struct.pack("<I", offset)
    body += struct.pack(">II", len(table), header)
    for name, off, size in table:
        body += struct.pack(">II", off, size)
        body += name.encode("latin-1") + b"\0"
    if len(body) != header:
        raise SystemExit(f"internal: header {len(body)} != computed {header}")
    for blob in blobs:
        body += blob

    with open(out_path, "wb") as f:
        f.write(body)
    return table, len(body)


def main(argv):
    if len(argv) >= 2 and argv[0] == "--list":
        for name, off, size in read_entries(argv[1]):
            print(f"  {size:>10,}  @{off:<10} {name}")
        return 0

    if len(argv) < 2:
        raise SystemExit(__doc__ or "usage: make-big.py <out.big> <virtual\\path>=<file> ...")

    out_path, members = argv[0], []
    for spec in argv[1:]:
        if "=" not in spec:
            raise SystemExit(f"bad member '{spec}' - expected <virtual\\path>=<source-file>")
        name, src = spec.split("=", 1)
        members.append((name, src))

    table, total = write_big(out_path, members)
    print(f"wrote {out_path}: {len(table)} entrie(s), {total:,} bytes")
    for name, off, size in table:
        print(f"  {size:>10,}  @{off:<10} {name}")

    # Read it back with the independent parser above rather than trusting the writer.
    back = read_entries(out_path)
    if [(n, o, s) for n, o, s in back] != table:
        raise SystemExit("readback does not match what was written")
    print("readback OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
