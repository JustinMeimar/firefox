"""Dataflow over block instances: symbolic stack simulation and CacheIR
operand decoding.

Values on the expression stack and in locals/args get symbolic ids. An IC op
reads its inputs from the values it pops (bottom to top) and defines fresh
ids for what it pushes. Inputs beyond the popped values (e.g. the
environment of GetGName) are treated as opaque per-op values.
"""

import os
import re
import struct

POINTER_IMMS = {"JSNativeImm", "StaticStringImm"}
WORD_IMMS = {"Int32Imm", "UInt32Imm"}
OPERAND_TYPES = {
    "ValId",
    "ObjId",
    "StringId",
    "SymbolId",
    "BooleanId",
    "Int32Id",
    "NumberId",
    "BigIntId",
    "ValueTagId",
    "IntPtrId",
    "RawId",
}


def arg_size(ty):
    if ty in POINTER_IMMS:
        return 8
    if ty in WORD_IMMS:
        return 4
    return 1


def load_op_args():
    path = os.path.join(
        os.path.dirname(__file__), "..", "js", "src", "jit", "CacheIROps.yaml"
    )
    ops = {}
    name = None
    in_args = False
    for line in open(path):
        m = re.match(r"- name: (\w+)", line)
        if m:
            name = m.group(1)
            ops[name] = []
            in_args = False
            continue
        if re.match(r"\s+args:\s*$", line):
            in_args = True
            continue
        m = re.match(r"\s{4}(\w+): (\w+)", line)
        if in_args and m and name:
            ops[name].append((m.group(1), m.group(2)))
            continue
        if not line.startswith("    "):
            in_args = False
    return ops


class RecipeDecoder:
    def __init__(self, trace):
        self.trace = trace
        self.op_args = load_op_args()
        self.cache = {}

    def decode(self, rid):
        """Returns [(op name, [(arg name, type, raw value)])] or None."""
        if rid in self.cache:
            return self.cache[rid]
        _, code = self.trace.recipe_info[rid]
        out = []
        i = 0
        ok = True
        while i < len(code):
            op = struct.unpack_from("<H", code, i)[0]
            i += 2
            arglen, name = self.trace.cacheops[op]
            spec = self.op_args.get(name, [])
            if sum(arg_size(t) for _, t in spec) != arglen:
                ok = False
                spec = []
            args = []
            j = i
            for aname, ty in spec:
                n = arg_size(ty)
                args.append((aname, ty, int.from_bytes(code[j : j + n], "little")))
                j += n
            out.append((name, args))
            i += arglen
        result = out if ok else None
        self.cache[rid] = result
        return result


class StackSim:
    """Symbolic interpretation of one basic block's straight-line ops."""

    def __init__(self, script, jsop_names):
        self.script = script
        self.jsop_names = jsop_names
        self.next_id = 0
        self.stack = []
        self.slots = {}

    def fresh(self):
        self.next_id += 1
        return self.next_id

    def get_slot(self, key):
        v = self.slots.get(key)
        if v is None:
            v = self.slots[key] = self.fresh()
        return v

    def pop_n(self, n):
        while len(self.stack) < n:
            self.stack.insert(0, self.fresh())
        if n == 0:
            return []
        popped = self.stack[-n:]
        del self.stack[-n:]
        return popped

    def peek(self, depth):
        while len(self.stack) <= depth:
            self.stack.insert(0, self.fresh())
        return self.stack[-1 - depth]

    def step(self, offset, op, uses, defs):
        """Returns (popped, pushed) value ids for the op at offset."""
        name = self.jsop_names[op]
        code = self.script.code
        imm = code[offset + 1 : offset + 4]
        if name == "Dup":
            v = self.peek(0)
            self.stack.append(v)
            return [], [v]
        if name == "Dup2":
            a, b = self.peek(1), self.peek(0)
            self.stack += [a, b]
            return [], [a, b]
        if name == "DupAt":
            v = self.peek(int.from_bytes(imm[:3], "little"))
            self.stack.append(v)
            return [], [v]
        if name == "Swap":
            a, b = self.peek(1), self.peek(0)
            self.stack[-2:] = [b, a]
            return [], []
        if name == "Pick":
            n = imm[0]
            self.peek(n)
            v = self.stack.pop(-1 - n)
            self.stack.append(v)
            return [], []
        if name == "Unpick":
            n = imm[0]
            self.peek(n)
            v = self.stack.pop()
            self.stack.insert(len(self.stack) - n, v)
            return [], []
        if name == "GetLocal":
            v = self.get_slot(("local", int.from_bytes(imm[:3], "little")))
            self.stack.append(v)
            return [], [v]
        if name == "GetArg":
            v = self.get_slot(("arg", int.from_bytes(imm[:2], "little")))
            self.stack.append(v)
            return [], [v]
        if name == "SetLocal":
            self.slots[("local", int.from_bytes(imm[:3], "little"))] = self.peek(0)
            return [], []
        if name == "SetArg":
            self.slots[("arg", int.from_bytes(imm[:2], "little"))] = self.peek(0)
            return [], []
        popped = self.pop_n(uses)
        pushed = [self.fresh() for _ in range(defs)]
        self.stack += pushed
        return popped, pushed


def recipe_inputs(decoded):
    """Operand ids read before being defined, in id order."""
    defined = set()
    inputs = set()
    for _, args in decoded:
        for aname, ty, value in args:
            if ty not in OPERAND_TYPES:
                continue
            if aname == "result":
                defined.add(value)
            elif value not in defined:
                inputs.add(value)
    return sorted(inputs)


def guard_keys(decoded, input_values, env_tag):
    """Canonical keys for the guards of one recipe.

    Each operand gets a canonical expression built from its input value or
    from the op and operands that defined it. Field arguments are wildcards.
    """
    expr = {}
    for i, opid in enumerate(recipe_inputs(decoded)):
        if i < len(input_values):
            expr[opid] = ("v", input_values[i])
        else:
            expr[opid] = ("env", env_tag, i)
    keys = []
    for name, args in decoded:
        operands = tuple(
            expr.get(v, ("?", v))
            for aname, ty, v in args
            if ty in OPERAND_TYPES and aname != "result"
        )
        fields = tuple(ty for aname, ty, v in args if ty not in OPERAND_TYPES)
        key = (name, operands, fields)
        for aname, ty, v in args:
            if aname == "result" and ty in OPERAND_TYPES:
                expr[v] = key
        if name.startswith("Guard"):
            keys.append(key)
    return keys
