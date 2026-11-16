#!/usr/bin/env python3
"""Build a newc cpio archive from a gen_init_cpio-style spec. Needs no root.

Spec lines (fields separated by whitespace, '#' starts a comment):
    dir   <path> <mode> <uid> <gid>
    file  <path> <source> <mode> <uid> <gid>
    nod   <path> <mode> <uid> <gid> <c|b> <major> <minor>
    slink <path> <target> <mode> <uid> <gid>

Paths of `file` sources are relative to the spec file's directory unless absolute.
Variables like ${NAME} in a line are expanded from the environment; a `file`
line whose source expands to an empty string or a missing file is skipped when
it is marked optional with a leading '?'. Output is deterministic (mtime 0,
sorted inode numbers in spec order).
"""
import os
import stat
import sys

def hdr(ino, mode, uid, gid, nlink, size, rmaj, rmin, name):
    nb = name.encode() + b"\0"
    h = "070701" + "".join("%08X" % v for v in (
        ino, mode, uid, gid, nlink, 0, size, 0, 0, rmaj, rmin, len(nb), 0))
    out = h.encode() + nb
    return out + b"\0" * (-len(out) % 4)

def pad(data):
    return data + b"\0" * (-len(data) % 4)

def main():
    if len(sys.argv) != 3:
        sys.exit("usage: mkcpio.py <spec> <out.cpio>")
    spec, outp = sys.argv[1], sys.argv[2]
    base = os.path.dirname(os.path.abspath(spec))
    out, ino = bytearray(), 300
    for lineno, raw in enumerate(open(spec), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        optional = line.startswith("?")
        line = os.path.expandvars(line.lstrip("?").strip())
        f = line.split()
        kind, path = f[0], f[1].lstrip("/")
        ino += 1
        try:
            if kind == "dir":
                mode, uid, gid = int(f[2], 8), int(f[3]), int(f[4])
                out += hdr(ino, stat.S_IFDIR | mode, uid, gid, 2, 0, 0, 0, path)
            elif kind == "file":
                src = f[2] if len(f) == 6 else ""
                if len(f) != 6 or "$" in src:
                    if optional:
                        continue
                    raise ValueError("bad or unset source")
                src = src if os.path.isabs(src) else os.path.join(base, src)
                if not os.path.isfile(src):
                    if optional:
                        continue
                    raise FileNotFoundError(src)
                mode, uid, gid = int(f[3], 8), int(f[4]), int(f[5])
                data = open(src, "rb").read()
                out += hdr(ino, stat.S_IFREG | mode, uid, gid, 1, len(data), 0, 0, path)
                out += pad(data)
            elif kind == "tree":
                # tree <dest> <srcdir> <filemode> <uid> <gid>: every file under srcdir (sorted, for reproducible
                # images), directories created 0755; with "?" a missing/unset srcdir is skipped
                src = f[2] if len(f) == 6 else ""
                if not src or "$" in src or not os.path.isdir(src if os.path.isabs(src) else os.path.join(base, src)):
                    if optional:
                        continue
                    raise FileNotFoundError(src or "unset tree source")
                src = src if os.path.isabs(src) else os.path.join(base, src)
                mode, uid, gid = int(f[3], 8), int(f[4]), int(f[5])
                made = set()
                for root, dirs, files in sorted(os.walk(src)):
                    dirs.sort()
                    rel = os.path.relpath(root, src)
                    parts = [] if rel == "." else rel.split(os.sep)
                    for k in range(len(parts) + 1):
                        d = "/".join([path] + parts[:k])
                        if d not in made:
                            made.add(d); ino += 1
                            out += hdr(ino, stat.S_IFDIR | 0o755, uid, gid, 2, 0, 0, 0, d)
                    for name in sorted(files):
                        data = open(os.path.join(root, name), "rb").read()
                        ino += 1
                        out += hdr(ino, stat.S_IFREG | mode, uid, gid, 1, len(data), 0, 0, "/".join([path] + parts + [name]))
                        out += pad(data)
            elif kind == "nod":
                mode, uid, gid = int(f[2], 8), int(f[3]), int(f[4])
                t = stat.S_IFCHR if f[5] == "c" else stat.S_IFBLK
                out += hdr(ino, t | mode, uid, gid, 1, 0, int(f[6]), int(f[7]), path)
            elif kind == "slink":
                target = f[2].encode()
                mode, uid, gid = int(f[3], 8), int(f[4]), int(f[5])
                out += hdr(ino, stat.S_IFLNK | mode, uid, gid, 1, len(target), 0, 0, path)
                out += pad(target)
            else:
                raise ValueError("unknown entry type " + kind)
        except (ValueError, IndexError, OSError) as e:
            sys.exit(f"{spec}:{lineno}: {e}")
    out += hdr(0, 0, 0, 0, 1, 0, 0, 0, "TRAILER!!!")
    out += b"\0" * (-len(out) % 512)
    with open(outp, "wb") as fh:
        fh.write(out)

if __name__ == "__main__":
    main()
