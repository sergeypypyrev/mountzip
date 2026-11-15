#!/usr/bin/env python3
"""Creates test archives and reference files for tests/run.sh.

    mkZips.py OUTDIR

OUTDIR/ref/<name> holds the expected contents of every member.
"""
import os
import random
import struct
import sys
import zipfile
import zlib

out = sys.argv[1]
rnd = random.Random(1)

FILES = {
    "hello.txt": b"Hello, zipr!\n",
    "empty.txt": b"",
    "data/random.bin": bytes(rnd.getrandbits(8) for _ in range(200_000)),
    "data/text.txt": b"".join(b"line %d of a compressible file\n" % i
                              for i in range(20_000)),
}
DIRS = ["data/"]


def path(name):
    return os.path.join(out, name)


def write(name, data):
    os.makedirs(os.path.dirname(path(name)), exist_ok=True)
    with open(path(name), "wb") as f:
        f.write(data)


def make(name, method, comment=b""):
    with zipfile.ZipFile(path(name), "w") as z:
        for d in DIRS:
            z.writestr(zipfile.ZipInfo(d), b"")
        for n, data in FILES.items():
            z.writestr(n, data, compress_type=method)
        z.comment = comment
    return open(path(name), "rb").read()


def data_offset(archive, member):
    """Offset of the member's data, from its local header."""
    with zipfile.ZipFile(path(archive)) as z:
        off = z.getinfo(member).header_offset
    raw = open(path(archive), "rb").read()
    nlen, xlen = struct.unpack_from("<HH", raw, off + 26)
    return off + 30 + nlen + xlen


def corrupt(src, dst, member, delta=0):
    raw = bytearray(open(path(src), "rb").read())
    raw[data_offset(src, member) + delta] ^= 0x55
    write(dst, bytes(raw))


def zip64_extra():
    """One stored file whose central entry keeps its sizes and offset in
    the Zip64 extra field, plus a Zip64 EOCD record and locator."""
    name, data = b"big.txt", b"pretend this is larger than 4 GiB\n"
    crc = zlib.crc32(data)
    local = struct.pack("<IHHHHHIIIHH", 0x04034b50, 45, 0, 0, 0, 0, crc,
                        len(data), len(data), len(name), 0) + name + data
    extra = struct.pack("<HHQQQ", 1, 24, len(data), len(data), 0)
    central = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 45, 45, 0, 0,
                          0, 0, crc, 0xffffffff, 0xffffffff, len(name),
                          len(extra), 0, 0, 0, 0, 0xffffffff) + name + extra
    cd_off, cd_size = len(local), len(central)
    eocd64 = struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 45, 45, 0, 0, 1, 1,
                         cd_size, cd_off)
    loc64 = struct.pack("<IIQI", 0x07064b50, 0, cd_off + cd_size, 1)
    eocd = struct.pack("<IHHHHIIH", 0x06054b50, 0xffff, 0xffff, 0xffff,
                       0xffff, 0xffffffff, 0xffffffff, 0)
    write("zip64extra.zip", local + central + eocd64 + loc64 + eocd)
    write("ref/big.txt", data)
    # Python must agree that the hand-made archive is valid.
    with zipfile.ZipFile(path("zip64extra.zip")) as z:
        assert z.read("big.txt") == data


def mutants(src, prefix, count):
    """Copies of @src with a few random bytes changed, mostly near the
    central directory where the parser works."""
    raw = open(path(src), "rb").read()
    with zipfile.ZipFile(path(src)) as z:
        cdir = z.start_dir
    for i in range(count):
        m = bytearray(raw)
        for _ in range(rnd.randint(1, 8)):
            if rnd.random() < 0.7:
                pos = rnd.randrange(max(0, cdir - 64), len(m))
            else:
                pos = rnd.randrange(len(m))
            m[pos] = rnd.getrandbits(8)
        if rnd.random() < 0.1:
            m = m[:rnd.randrange(len(m))]
        write("fuzz/%s%03d.zip" % (prefix, i), bytes(m))


def raw_zip(name, members, wrap_count=False):
    """Writes a ZIP without Python's zipfile: stored members, no Zip64.
    With @wrap_count, the EOCD records the count modulo 65536, as old
    writers did for archives with more than 65535 entries."""
    with open(path(name), "wb") as f:
        central = []
        for n, data in members:
            n = n.encode()
            crc = zlib.crc32(data)
            central.append(struct.pack(
                "<IHHHHHHIIIHHHHHII", 0x02014b50, 20, 20, 0, 0, 0, 0, crc,
                len(data), len(data), len(n), 0, 0, 0, 0, 0, f.tell()) + n)
            f.write(struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 0, 0, 0,
                                0, crc, len(data), len(data), len(n), 0))
            f.write(n + data)
        cd_off = f.tell()
        for c in central:
            f.write(c)
        cd_size = f.tell() - cd_off
        count = len(central) & 0xffff if wrap_count else len(central)
        f.write(struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, count, count,
                            cd_size, cd_off, 0))


for n, data in FILES.items():
    write("ref/" + n, data)

# More than 65535 entries without Zip64, the count in the EOCD wrapped.
raw_zip("wrapped.zip", [("w%05d" % i, b"%d\n" % i) for i in range(70_000)],
        wrap_count=True)

# Entries far larger than the iterator buffer's refill unit: long names,
# long comments, and an entry that is larger than 256 bytes on the stack
# path of ziprEntryAt().
with zipfile.ZipFile(path("bigentries.zip"), "w") as z:
    for i, nlen in enumerate([1, 255, 300, 5000, 40000, 65535 - 8,
                              30000, 30000, 30000, 30000]):
        n = ("%03d" % i + "n" * nlen)[:nlen] if nlen >= 3 else "%d" % i
        info = zipfile.ZipInfo(n)
        info.comment = b"c" * (65535 if i % 2 else 1000)
        z.writestr(info, b"member %d\n" % i)
with zipfile.ZipFile(path("bigentries.zip")) as z:
    write("bignames.txt", "\n".join(z.namelist()).encode() + b"\n")

stored = make("stored.zip", zipfile.ZIP_STORED)
make("deflated.zip", zipfile.ZIP_DEFLATED)
make("comment.zip", zipfile.ZIP_STORED,
     comment=b"a comment with a fake PK\x05\x06 signature inside")
write("prefixed.zip", b"#!/bin/sh\nexit 0\n" * 50 + stored)
corrupt("stored.zip", "badcrc.zip", "hello.txt")
corrupt("deflated.zip", "baddeflate.zip", "data/text.txt", delta=100)
write("truncated.zip", stored[:len(stored) // 2])
write("notzip.bin", bytes(rnd.getrandbits(8) for _ in range(1000)))
zip64_extra()

# More than 65535 entries: Python writes a Zip64 EOCD for this.
with zipfile.ZipFile(path("many.zip"), "w") as z:
    for i in range(70_000):
        z.writestr("f%05d" % i, b"")

mutants("stored.zip", "s", 300)
mutants("deflated.zip", "d", 300)
