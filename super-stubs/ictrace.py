#!/usr/bin/env python3
"""Reader for the binary trace written by the shell's --ic-trace=PATH.

Streams the trace once, resolves which stub hit at each IC execution, groups
consecutive IC executions of one frame into basic-block instances, and writes
an aggregate (pickle) of block instances keyed by their outcome tuple.

  ictrace.py aggregate TRACE OUT.pkl
  ictrace.py report OUT.pkl
  ictrace.py mir OUT.pkl TRACE...
"""

import hashlib
import pickle
import struct
import sys
from collections import Counter, defaultdict

JOF_JUMP, JOF_TARGET, JOF_NOFALL, JOF_IC = 1, 2, 4, 8
MISS = -1


class Reader:
    def __init__(self, f):
        self.f = f

    def read(self, n):
        b = self.f.read(n)
        if len(b) != n:
            raise EOFError
        return b

    def u8(self):
        return self.read(1)[0]

    def u16(self):
        return struct.unpack("<H", self.read(2))[0]

    def u32(self):
        return struct.unpack("<I", self.read(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.read(8))[0]

    def str(self):
        return self.read(self.u32()).decode("utf-8", "replace")


class Script:
    __slots__ = ("id", "filename", "line", "column", "ops", "code", "leader_of")

    def __init__(self, id, filename, line, column, ops, code):
        self.id = id
        self.filename = filename
        self.line = line
        self.column = column
        self.ops = ops  # list of (offset, op, uses, defs)
        self.code = code
        self.leader_of = None

    def compute_blocks(self, jsop_flags):
        leader_of = {}
        leader = 0
        prev_ends = True
        for offset, op, _, _ in self.ops:
            flags = jsop_flags[op]
            if prev_ends or flags & JOF_TARGET:
                leader = offset
            leader_of[offset] = leader
            prev_ends = bool(flags & (JOF_JUMP | JOF_NOFALL))
        self.leader_of = leader_of


class Trace:
    def __init__(self):
        self.jsops = []  # (length, flags, name)
        self.cacheops = []  # (argLength, name)
        self.kinds = []
        self.transpile = []
        self.scripts = {}
        self.recipes = {}  # digest -> recipe id
        self.recipe_info = []  # id -> (kind, code bytes)

    def recipe_id(self, kind, code):
        key = hashlib.blake2b(bytes([kind]) + code, digest_size=12).digest()
        rid = self.recipes.get(key)
        if rid is None:
            rid = len(self.recipe_info)
            self.recipes[key] = rid
            self.recipe_info.append((kind, code))
        return rid

    def decode_recipe(self, rid):
        kind, code = self.recipe_info[rid]
        names = []
        i = 0
        while i < len(code):
            op = struct.unpack_from("<H", code, i)[0]
            i += 2
            arglen, name = self.cacheops[op]
            names.append(name)
            i += arglen
        return self.kinds[kind], names


def read_header(r, t):
    if r.read(8) != b"ICTRACE1":
        raise SystemExit("bad magic")
    t.jsops, t.cacheops, t.kinds, t.transpile = [], [], [], []
    for _ in range(r.u16()):
        length, flags = r.u8(), r.u8()
        t.jsops.append((length, flags, r.str()))
    for _ in range(r.u16()):
        arglen, transpile = r.u8(), r.u8()
        t.cacheops.append((arglen, r.str()))
        t.transpile.append(transpile)
    for _ in range(r.u16()):
        t.kinds.append(r.str())


def collect_mir(trace_paths, out_path):
    """Union of 'M' records over many traces, skipping everything else."""
    t = Trace()
    mir = Counter()
    transpiled = Counter()
    for path in trace_paths:
        with open(path, "rb") as f:
            r = Reader(f)
            try:
                read_header(r, t)
                while True:
                    tag = r.read(1)
                    if tag in (b"E", b"X"):
                        r.read(16)
                    elif tag == b"F":
                        r.read(36)
                    elif tag == b"A":
                        r.read(29)
                        r.read(r.u32())
                    elif tag == b"C":
                        r.read(16)
                    elif tag == b"T":
                        transpiled[t.cacheops[r.u16()][1]] += 1
                    elif tag == b"M":
                        op = r.u16()
                        name = r.str()
                        mir[(t.cacheops[op][1], name, r.u32(), r.u8())] += 1
                    elif tag == b"S":
                        r.read(8)
                        r.str()
                        r.read(8)
                        r.read(r.u32() * 9)
                        r.read(r.u32())
                    else:
                        raise SystemExit(f"{path}: bad tag {tag!r}")
            except EOFError:
                pass
    with open(out_path, "wb") as out:
        pickle.dump(
            {
                "cacheops": t.cacheops,
                "transpile": t.transpile,
                "transpiled": transpiled,
                "mir": mir,
            },
            out,
        )


def aggregate(trace_path, out_path):
    t = Trace()
    stub_site = {}  # stub -> (fallback, pcOffset, recipe id)
    fallback_script = {}  # fallback -> script id
    pending = {}  # frame -> [stub, failed]
    current = {}  # frame -> [script id, leader, last pc, [(pc, outcome)]]
    instances = Counter()
    mir = Counter()  # (cacheop, mir opcode, alias bits, bits)
    unknown_stubs = 0
    hits = 0
    misses = 0

    def site_of(stub):
        return stub_site.get(stub)

    def close(frame):
        cur = current.pop(frame, None)
        if cur:
            instances[(cur[0], cur[1], tuple(cur[3]))] += 1

    def record(frame, script_id, pc, outcome):
        script = t.scripts.get(script_id)
        if script is None:
            return
        leader = script.leader_of.get(pc)
        cur = current.get(frame)
        if cur and cur[0] == script_id and cur[1] == leader and pc > cur[2]:
            cur[2] = pc
            cur[3].append((pc, outcome))
            return
        close(frame)
        current[frame] = [script_id, leader, pc, [(pc, outcome)]]

    def flush_hit(frame):
        nonlocal hits, unknown_stubs
        p = pending.pop(frame, None)
        if p is None or p[1]:
            return p
        site = site_of(p[0])
        if site is None:
            unknown_stubs += 1
            return None
        fallback, pc, rid = site
        script_id = fallback_script.get(fallback)
        if script_id is None:
            unknown_stubs += 1
            return None
        hits += 1
        record(frame, script_id, pc, rid)
        return None

    with open(trace_path, "rb") as f:
        r = Reader(f)
        read_header(r, t)
        jsop_flags = [fl for _, fl, _ in t.jsops]

        try:
            while True:
                tag = r.read(1)
                if tag == b"E":
                    stub, frame = r.u64(), r.u64()
                    flush_hit(frame)
                    pending[frame] = [stub, False]
                elif tag == b"X":
                    stub, frame = r.u64(), r.u64()
                    p = pending.get(frame)
                    if p and p[0] == stub:
                        p[1] = True
                elif tag == b"F":
                    frame, _icscript, fallback = r.u64(), r.u64(), r.u64()
                    script_id, pc = r.u64(), r.u32()
                    flush_hit(frame)
                    fallback_script[fallback] = script_id
                    misses += 1
                    record(frame, script_id, pc, MISS)
                elif tag == b"A":
                    stub, _icscript, fallback = r.u64(), r.u64(), r.u64()
                    pc, kind = r.u32(), r.u8()
                    code = r.read(r.u32())
                    stub_site[stub] = (fallback, pc, t.recipe_id(kind, code))
                elif tag == b"C":
                    new, old = r.u64(), r.u64()
                    site = stub_site.get(old)
                    if site is not None:
                        stub_site[new] = site
                elif tag == b"T":
                    r.u16()
                elif tag == b"M":
                    op = r.u16()
                    name = r.str()
                    mir[(op, name, r.u32(), r.u8())] += 1
                elif tag == b"S":
                    sid = r.u64()
                    filename = r.str()
                    line, column = r.u32(), r.u32()
                    ops = []
                    for _ in range(r.u32()):
                        ops.append((r.u32(), r.u8(), r.u16(), r.u16()))
                    code = r.read(r.u32())
                    s = Script(sid, filename, line, column, ops, code)
                    s.compute_blocks(jsop_flags)
                    t.scripts[sid] = s
                else:
                    raise SystemExit(f"bad tag {tag!r} at {f.tell() - 1}")
        except EOFError:
            pass

    for frame in list(pending):
        flush_hit(frame)
    for frame in list(current):
        close(frame)

    with open(out_path, "wb") as out:
        pickle.dump(
            {
                "jsops": t.jsops,
                "cacheops": t.cacheops,
                "transpile": t.transpile,
                "kinds": t.kinds,
                "scripts": {
                    sid: (sc.filename, sc.line, sc.column, sc.ops, sc.code)
                    for sid, sc in t.scripts.items()
                },
                "recipe_info": t.recipe_info,
                "instances": instances,
                "mir": mir,
                "hits": hits,
                "misses": misses,
                "unknown_stubs": unknown_stubs,
            },
            out,
        )


def load(path):
    with open(path, "rb") as f:
        d = pickle.load(f)
    t = Trace()
    t.jsops, t.cacheops, t.kinds = d["jsops"], d["cacheops"], d["kinds"]
    t.transpile = d["transpile"]
    t.recipe_info = d["recipe_info"]
    jsop_flags = [fl for _, fl, _ in t.jsops]
    for sid, fields in d["scripts"].items():
        sc = Script(sid, *fields)
        sc.compute_blocks(jsop_flags)
        t.scripts[sid] = sc
    return t, d


def report(path):
    t, d = load(path)
    instances = d["instances"]
    print(f"ic hits {d['hits']}")
    print(f"ic misses {d['misses']}")
    print(f"unresolved stub entries {d['unknown_stubs']}")
    print(f"scripts {len(t.scripts)}")
    print(f"recipes {len(t.recipe_info)}")
    print(f"distinct block instance tuples {len(instances)}")

    by_k = Counter()
    ops_by_k = Counter()
    for (_, _, outcomes), n in instances.items():
        k = len(outcomes)
        by_k[k] += n
        ops_by_k[k] += n * k
    total_ops = sum(ops_by_k.values())
    print("\nIC executions per block instance (k: instances, IC executions, share)")
    for k in sorted(by_k):
        print(f"  {k:3d}: {by_k[k]:12d} {ops_by_k[k]:14d} {ops_by_k[k] / total_ops:7.3f}")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd = sys.argv[1]
    if cmd == "aggregate":
        aggregate(sys.argv[2], sys.argv[3])
    elif cmd == "mir":
        collect_mir(sys.argv[3:], sys.argv[2])
    elif cmd == "report":
        report(sys.argv[2])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
