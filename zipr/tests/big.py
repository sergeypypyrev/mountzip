#!/usr/bin/env python3
"""Large-archive tests: time and peak memory of ziprTool on

  sparse.zip  a 12 GiB member (a sparse file, so it costs no disk space)
              followed by a member whose offset is beyond 4 GiB;
  many.zip    10 million entries (about 1 GB on disk).

    tests/big.py BUILD_DIR

Environment: BIG_ENTRIES (default 10000000), BIG_GIB (default 12).
"""
import os
import struct
import sys
import time
import zipfile
import zlib

B = sys.argv[1] if len(sys.argv) > 1 else "build"
T = os.path.join(B, "big")
TOOL = os.path.join(B, "ziprTool")
ENTRIES = int(os.environ.get("BIG_ENTRIES", 10_000_000))
GIB = int(os.environ.get("BIG_GIB", 12))
os.makedirs(T, exist_ok=True)
failed = 0


def run(desc, *args, expect=None):
    """Runs ziprTool, reports wall time and peak RSS of that process.

    posix_spawn() and wait4() rather than subprocess: a forked child counts
    the pages of this Python process in its peak RSS until it execs."""
    global failed
    out, err = os.path.join(T, "stdout"), os.path.join(T, "stderr")
    actions = [
        (os.POSIX_SPAWN_OPEN, 1, out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644),
        (os.POSIX_SPAWN_OPEN, 2, err, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644),
    ]
    t = time.monotonic()
    pid = os.posix_spawn(TOOL, [TOOL, *args], os.environ, file_actions=actions)
    _, status, ru = os.wait4(pid, 0)
    t = time.monotonic() - t
    stdout = open(out, "rb").read()
    ok = os.waitstatus_to_exitcode(status) == 0 and \
        (expect is None or expect(stdout))
    if not ok:
        failed += 1
    print("%-4s %-46s %7.2f s  %7.1f MB" %
          ("ok" if ok else "FAIL", desc, t, (ru.ru_maxrss - BASE_RSS) / 1024))
    if not ok:
        print(stdout[-2000:].decode(errors="replace"),
              open(err, "rb").read()[-2000:].decode(errors="replace"))
    return stdout


def central(name, crc, csize, usize, lho, extra=b""):
    return struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 45, 45, 0, 0, 0, 0,
                       crc, csize, usize, len(name), len(extra), 0, 0, 0, 0,
                       lho) + name + extra


def local(name, crc, csize, usize, extra=b""):
    return struct.pack("<IHHHHHIIIHH", 0x04034b50, 45, 0, 0, 0, 0, crc,
                       csize, usize, len(name), len(extra)) + name + extra


def end_records(count, cd_off, cd_size):
    """Zip64 EOCD record and locator, then a classic EOCD."""
    eocd64 = struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 45, 45, 0, 0,
                         count, count, cd_size, cd_off)
    loc64 = struct.pack("<IIQI", 0x07064b50, 0, cd_off + cd_size, 1)
    eocd = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, min(count, 0xffff),
                       min(count, 0xffff), min(cd_size, 0xffffffff),
                       min(cd_off, 0xffffffff), 0)
    return eocd64 + loc64 + eocd


def make_sparse(path):
    size = GIB << 30
    after = b"I live beyond 4 GiB\n"
    crc = 0
    zeros = bytes(64 << 20)
    for _ in range(size // len(zeros)):
        crc = zlib.crc32(zeros, crc)
    with open(path, "wb") as f:
        f.write(local(b"zeros.bin", crc, 0xffffffff, 0xffffffff,
                      struct.pack("<HHQQ", 1, 16, size, size)))
        f.seek(size, os.SEEK_CUR)		# a hole: reads as zeros
        after_off = f.tell()
        f.write(local(b"after.txt", zlib.crc32(after), len(after),
                      len(after)) + after)
        cd_off = f.tell()
        f.write(central(b"zeros.bin", crc, 0xffffffff, 0xffffffff, 0,
                        struct.pack("<HHQQ", 1, 16, size, size)))
        f.write(central(b"after.txt", zlib.crc32(after), len(after),
                        len(after), 0xffffffff,
                        struct.pack("<HHQ", 1, 8, after_off)))
        cd_size = f.tell() - cd_off
        f.write(end_records(2, cd_off, cd_size))
    with zipfile.ZipFile(path) as z:	# Python must agree
        assert [i.file_size for i in z.infolist()] == [size, len(after)]
        assert z.read("after.txt") == after
    return after


def name_of(i):
    return b"dir%04d/file%07d.txt" % (i // 10_000, i)


def content_of(i):
    return name_of(i) + b"\n" if i % 100_000 == 0 else b""


def make_many(path):
    cdir = path + ".cdir"
    with open(path, "wb") as f, open(cdir, "wb") as c:
        for i in range(ENTRIES):
            n, data = name_of(i), content_of(i)
            crc = zlib.crc32(data)
            c.write(central(n, crc, len(data), len(data), f.tell()))
            f.write(local(n, crc, len(data), len(data)) + data)
        cd_off = f.tell()
        c.flush()
        with open(cdir, "rb") as c2:
            while chunk := c2.read(64 << 20):
                f.write(chunk)
        cd_size = f.tell() - cd_off
        f.write(end_records(ENTRIES, cd_off, cd_size))
    os.unlink(cdir)
    return cd_size


def base_rss():
    """Peak RSS that posix_spawn() itself reports for a trivial program;
    subtracted from the numbers above."""
    pid = os.posix_spawn("/bin/true", ["/bin/true"], os.environ)
    return os.wait4(pid, 0)[2].ru_maxrss


BASE_RSS = base_rss()
print("%-51s %9s  %10s" % ("", "time", "peak RSS*"))
sparse = os.path.join(T, "sparse.zip")
many = os.path.join(T, "many.zip")

t = time.monotonic()
after = make_sparse(sparse)
print("made %s: %d GiB member, %.0f s" % (sparse, GIB, time.monotonic() - t))
run("l sparse.zip", "l", sparse, expect=lambda o: b"after.txt" in o)
run("x sparse.zip after.txt (offset > 4 GiB)", "x", sparse, "after.txt",
    expect=lambda o: o == after)
run("t sparse.zip (reads %d GiB, checks CRC)" % GIB, "-b", str(1 << 20),
    "t", sparse, expect=lambda o: o.count(b"ok ") == 2)

if not os.path.exists(many) or os.environ.get("BIG_REGEN"):
    t = time.monotonic()
    cd_size = make_many(many)
    print("made %s: %d entries, directory %.0f MB, %.0f s" %
          (many, ENTRIES, cd_size / 1e6, time.monotonic() - t))
last, mid = ENTRIES - 1, ENTRIES // 2
names = [name_of(i).decode() for i in (0, mid, last)]
with_data = (ENTRIES - 1) // 100_000 * 100_000

run("n many.zip (iterate %d entries)" % ENTRIES, "n", many,
    expect=lambda o: o == b"%d entries\n" % ENTRIES)
run("f many.zip last name (linear scan)", "f", many, names[2],
    expect=lambda o: names[2].encode() in o)
run("-i f many.zip 3 names (index + search)", "-i", "f", many, *names,
    expect=lambda o: o.count(b"\n") == 3)
run("-i x many.zip %s" % name_of(with_data).decode(), "-i", "x", many,
    name_of(with_data).decode(), expect=lambda o: o == content_of(with_data))

print("* above the %.1f MB that posix_spawn() reports for /bin/true" %
      (BASE_RSS / 1024))
print("big tests:", "FAILED" if failed else "passed")
sys.exit(1 if failed else 0)
