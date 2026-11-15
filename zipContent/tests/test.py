#!/usr/bin/env python3
"""Tests of ZipContent against a model of the rules in zipContent.h.

    tests/test.py BUILD_DIR

Creates archives in BUILD_DIR/t, builds the expected tree in Python and
compares the output of zipContentTool (release and sanitizer builds):
the full listing (l), the statistics (s), directory listings (d) and
lookups (f). Then runs the sanitizer build on corrupted archives, where
errors are allowed but crashes and sanitizer reports are not.
"""
import os
import random
import struct
import subprocess
import sys
import zipfile
import zlib

B = sys.argv[1] if len(sys.argv) > 1 else "build"
T = os.path.join(B, "t")
TOOLS = [os.path.join(B, "zipContentTool"),
         os.path.join(B, "san", "zipContentTool")]
os.makedirs(T, exist_ok=True)
rnd = random.Random(7)
passed = failed = 0
env = dict(os.environ, ASAN_OPTIONS="exitcode=99",
           UBSAN_OPTIONS="halt_on_error=1:exitcode=98:print_stacktrace=1")


def raw_zip(path, names):
    """Stored members with exactly these byte names, no Zip64. The EOCD
    count is wrapped modulo 65536 like old writers did."""
    with open(path, "wb") as f:
        central = []
        for i, n in enumerate(names):
            data = b"member %d\n" % i
            crc = zlib.crc32(data)
            central.append(struct.pack(
                "<IHHHHHHIIIHHHHHII", 0x02014b50, 20, 20, 0, 0, 0, 0, crc,
                len(data), len(data), len(n), 0, 0, 0, 0, 0, f.tell()) + n)
            f.write(struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 0, 0, 0, 0,
                                crc, len(data), len(data), len(n), 0))
            f.write(n + data)
        start = f.tell()
        for c in central:
            f.write(c)
        count = len(central) & 0xffff
        f.write(struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, count, count,
                            f.tell() - start, start, 0))


class Dir:
    def __init__(self):
        self.dirs = {}
        self.files = {}		# name -> [index of entry, ...]


def model(names):
    """The tree that zipContentBuild() must produce for these entries."""
    root, skipped = Dir(), 0
    for index, raw in enumerate(names):
        if b"\0" in raw:
            skipped += 1
            continue
        stripped = raw.rstrip(b"/")
        cut = stripped.rfind(b"/") + 1
        parts, last = stripped[:cut].split(b"/"), stripped[cut:]
        if last in (b"", b".", b"..") or b".." in parts:
            skipped += 1
            continue
        d = root
        for p in parts:
            if p not in (b"", b"."):
                d = d.dirs.setdefault(p, Dir())
        if raw.endswith(b"/"):
            d.dirs.setdefault(last, Dir())
        else:
            d.files.setdefault(last, []).append(index)

    lines, files, dirs, dups = [], {}, {b"": root}, 0

    def walk(d, prefix):
        nonlocal dups
        for name, entries in d.files.items():
            dups += len(entries) - (name not in d.dirs)
        for name in sorted(set(d.dirs) | set(d.files)):
            if name in d.dirs:
                lines.append(prefix + name + b"/")
                dirs[prefix + name] = d.dirs[name]
                walk(d.dirs[name], prefix + name + b"/")
            else:
                lines.append(prefix + name)
                files[prefix + name] = names[d.files[name][-1]]
    walk(root, b"")
    stats = {"directories": len(dirs), "files": len(files),
             "skipped": skipped, "duplicates": dups}
    return lines, stats, files, dirs


def run(tool, *args):
    return subprocess.run([tool, *args], capture_output=True, env=env)


def check(desc, ok, detail=b""):
    global passed, failed
    if ok:
        passed += 1
        return
    failed += 1
    print("FAIL", desc)
    if detail:
        print(detail[-3000:].decode(errors="replace"))


def test_archive(name, names):
    path = os.path.join(T, name)
    lines, stats, files, dirs = model(names)
    for tool in TOOLS:
        tag = "%s %s" % (os.path.relpath(tool, B), name)
        r = run(tool, "l", path)
        check(tag + " l", r.returncode == 0 and
              r.stdout == b"".join(x + b"\n" for x in lines),
              r.stdout + r.stderr)
        r = run(tool, "s", path)
        got = dict(line.split()[:2] for line in r.stdout.decode().splitlines())
        check(tag + " s", r.returncode == 0 and
              all(got.get(k) == str(v) for k, v in stats.items()),
              r.stdout + r.stderr + repr(stats).encode())

        # Lookups: a sample of files and directories, plus misses.
        sample = rnd.sample(sorted(files), min(150, len(files)))
        if sample:
            r = run(tool, "f", path, *[os.fsdecode(p) for p in sample])
            out = r.stdout.splitlines()
            ok = r.returncode == 0 and len(out) == len(sample)
            for p, line in zip(sample, out):
                ok = ok and line.startswith(b"file " + p + b": ") and \
                    line.endswith(b", entry " + files[p])
            check(tag + " f files", ok, r.stdout + r.stderr)
        dsample = rnd.sample(sorted(dirs), min(30, len(dirs)))
        for d in dsample:
            r = run(tool, "d", path, os.fsdecode(d))
            want = sorted([n + b"/" for n in dirs[d].dirs] +
                          [n for n in dirs[d].files if n not in dirs[d].dirs],
                          key=lambda x: x.rstrip(b"/"))
            check(tag + " d " + repr(d), r.returncode == 0 and
                  r.stdout == b"".join(x + b"\n" for x in want),
                  r.stdout + r.stderr)
        for miss in ["no/such/path", "zzz-missing"]:
            r = run(tool, "f", path, miss)
            check(tag + " miss", r.returncode == 1 and
                  b"No such file" in r.stderr, r.stdout + r.stderr)
        if files:
            f = os.fsdecode(sorted(files)[0])
            r = run(tool, "f", path, f + "/")
            check(tag + " file/", r.returncode == 1 and
                  b"Not a directory" in r.stderr, r.stdout + r.stderr)
            r = run(tool, "f", path, f + "/x")
            check(tag + " file/x", r.returncode == 1 and
                  b"Not a directory" in r.stderr, r.stdout + r.stderr)


def tricky_names():
    return [b"a/b/c.txt", b"a/b/", b"a/./d.txt", b"/abs/x", b"a//e",
            b"../evil", b"a/../f", b"x/../y/z", b"x/..", b"./", b"..", b".",
            b"dup.txt", b"dup.txt", b"dup.txt",
            b"clash", b"clash/inner", b"clash2/", b"clash2",
            b"a/b", b"nul\0name", b"dir\0/x", b"trail//", b"lonely/",
            b"UPPER", b"upper", "é/ü.txt".encode(), b"\xff\xfe",
            b"z" * 300 + b"/" + b"y" * 3000, b"deep/" * 200 + b"bottom",
            b"a/b/c.txt", b"a/b/c.txt/"]


def random_names(count, depth, alphabet):
    out = []
    for _ in range(count):
        parts = [rnd.choice(alphabet) for _ in range(rnd.randint(1, depth))]
        name = "/".join(parts)
        if rnd.random() < 0.05:
            name += "/"
        out.append(name.encode())
    return out


def mutants(src, prefix, count):
    raw = open(os.path.join(T, src), "rb").read()
    with zipfile.ZipFile(os.path.join(T, src)) as z:
        start = z.start_dir
    for i in range(count):
        m = bytearray(raw)
        for _ in range(rnd.randint(1, 8)):
            pos = rnd.randrange(max(0, start - 16), len(m)) \
                if rnd.random() < 0.8 else rnd.randrange(len(m))
            m[pos] = rnd.getrandbits(8)
        if rnd.random() < 0.1:
            m = m[:rnd.randrange(len(m))]
        path = os.path.join(T, "fuzz-%s%03d.zip" % (prefix, i))
        open(path, "wb").write(bytes(m))
        yield path


archives = {
    "empty.zip": [],
    "one.zip": [b"only.txt"],
    "basic.zip": [b"docs/", b"docs/readme.txt", b"src/main.c", b"src/lib/a.c",
                  b"src/lib/b.c", b"src/lib/", b"top.txt", b"docs/z/"],
    "tricky.zip": tricky_names(),
    "random.zip": random_names(3000, 5, ["a", "b", "c", "dd", "e.txt", "."]),
    # Wrapped EOCD count (70000 & 0xffff): the arrays have to grow.
    "flat.zip": [b"w%05d" % i for i in rnd.sample(range(70000), 70000)],
    # Many directories: the hash table grows several times.
    "dirs.zip": [b"d%d/s%d/f%d" % (i % 5000, i % 7, i) for i in range(20000)],
    # Interleaved directories defeat the cache of the previous directory.
    "interleaved.zip": [b"p%d/q/%d" % (i % 3, i) for i in range(3000)],
}
for name, names in archives.items():
    raw_zip(os.path.join(T, name), names)
for name, names in archives.items():
    test_archive(name, names)

# Python's zipfile output, with directories and deflate members.
with zipfile.ZipFile(os.path.join(T, "py.zip"), "w") as z:
    z.writestr(zipfile.ZipInfo("dir/"), b"")
    for i in range(100):
        z.writestr("dir/sub%d/file%d.txt" % (i % 10, i), b"x" * i,
                   compress_type=zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(os.path.join(T, "py.zip")) as z:
    test_archive("py.zip", [n.encode() for n in z.namelist()])

# Not archives: errors, not crashes.
open(os.path.join(T, "notzip.bin"), "wb").write(os.urandom(1000))
for tool in TOOLS:
    r = run(tool, "s", os.path.join(T, "notzip.bin"))
    check("notzip", r.returncode == 1 and b"Invalid argument" in r.stderr,
          r.stdout + r.stderr)

fuzz = list(mutants("tricky.zip", "t", 250)) + \
    list(mutants("random.zip", "r", 250))
for path in fuzz:
    for args in (["l", path], ["f", path, "a/b/c.txt", "dup.txt", "a"]):
        r = run(TOOLS[1], *args)
        check("fuzz " + " ".join(args), r.returncode in (0, 1),
              r.stdout[-500:] + r.stderr)

print("passed: %d, failed: %d" % (passed, failed))
sys.exit(1 if failed else 0)
