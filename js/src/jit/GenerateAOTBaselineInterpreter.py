import sys
import os
import struct

# Must match BASELINE_MANIFEST_FIELDS in BaselineAOT.h (order matters).
MANIFEST_FIELD_NAMES = [
    "InterpretOp",
    "InterpretOpNoDebugTrap",
    "BailoutPrologue",
    "ProfilerEnterToggle",
    "ProfilerExitToggle",
    "DebugTrapHandler",
    "DispatchTableOffset",
    "CallVMDebugPrologue",
    "CallVMDebugEpilogue",
    "CallVMDebugAfterYield",
    "HeaderSize",
    "PrologueEndOffset",
    "DebugInstrumentationCount",
    "DebugTrapCount",
    "CodeCoverageCount",
    "ICReturnCount",
    "RuntimePatchCount",
]

# Must match the write order in serializeAOTManifest.
# (symbol_name, count_field_name, element_size_bytes)
VECTORS = [
    ("DebugInstrumentationOffsets", "DebugInstrumentationCount", 4),
    ("DebugTrapOffsets",            "DebugTrapCount",            4),
    ("CodeCoverageOffsets",         "CodeCoverageCount",         4),
    ("ICReturnOffsets",             "ICReturnCount",             8),
    ("RuntimePatches",              "RuntimePatchCount",        12),
]

FOOTER_SIZE = 12


def emit_bytes(c_out, data):
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        c_out.write(f"    .byte {', '.join(f'0x{b:02x}' for b in chunk)}\n")

def emit_global(c_out, name):
    c_out.write(f".global {name}\n")

def parse_footer(data):
    footer_off = len(data) - FOOTER_SIZE
    magic, version, manifest_offset = struct.unpack_from('<III', data, footer_off)
    return footer_off, magic, version, manifest_offset


def parse_manifest(data, manifest_offset):
    manifest = {}
    for i, name in enumerate(MANIFEST_FIELD_NAMES):
        val, = struct.unpack_from('<I', data, manifest_offset + i * 4)
        manifest[name] = val
    return manifest


def parse_vectors(data, manifest, manifest_offset, footer_off):
    manifest_size = len(MANIFEST_FIELD_NAMES) * 4
    cursor = manifest_offset + manifest_size
    vectors = {}
    for vec_name, count_field, elem_size in VECTORS:
        nbytes = manifest[count_field] * elem_size
        vectors[vec_name] = data[cursor:cursor + nbytes]
        cursor += nbytes
    return vectors


def emit_code_section(c_out, code_bytes):
    c_out.write(".balign 16\n")
    emit_global(c_out, "bl_aot_code_start")
    emit_global(c_out, "bl_aot_code_end")
    c_out.write("bl_aot_code_start:\n")
    emit_bytes(c_out, code_bytes)
    c_out.write("bl_aot_code_end:\n\n")


def emit_manifest_scalars(c_out, manifest):
    c_out.write(".balign 4\n")
    for name in MANIFEST_FIELD_NAMES:
        sym = f"bl_aot_{name}"
        emit_global(c_out, sym)
        c_out.write(f"{sym}:    .long 0x{manifest[name]:08x}\n")
    c_out.write("\n")


def emit_vector_payloads(c_out, vectors):
    for vec_name, _, _ in VECTORS:
        raw = vectors[vec_name]
        start_sym = f"bl_aot_{vec_name}_start"
        end_sym = f"bl_aot_{vec_name}_end"
        c_out.write(".balign 4\n")
        emit_global(c_out, start_sym)
        emit_global(c_out, end_sym)
        c_out.write(f"{start_sym}:\n")
        if raw:
            emit_bytes(c_out, raw)
        c_out.write(f"{end_sym}:\n\n")


def emit_stub(c_out):
    c_out.write("// Stub: no valid baseline_interpreter.bin (bootstrap build)\n")
    c_out.write(".section .rodata\n")
    emit_code_section(c_out, b"")
    emit_manifest_scalars(c_out, {name: 0 for name in MANIFEST_FIELD_NAMES})
    emit_vector_payloads(c_out, {name: b"" for name, _, _ in VECTORS})
    c_out.write(".section .note.GNU-stack,\"\",@progbits\n")


def main(c_out, bin_path):
    if not os.path.exists(bin_path):
        print(f"WARNING: {bin_path} not found, emitting stub "
              f"(run --dump-bl to bootstrap)", file=sys.stderr)
        emit_stub(c_out)
        return

    with open(bin_path, 'rb') as f_in:
        data = f_in.read()

    if os.name == 'nt':
        raise RuntimeError("TODO: handle MSVC")

    if len(data) <= FOOTER_SIZE:
        print(f"WARNING: {bin_path} too small ({len(data)} bytes), emitting stub",
              file=sys.stderr)
        emit_stub(c_out)
        return

    footer_off, magic, version, manifest_offset = parse_footer(data)
    if magic != 0x424C494E or version != 1:
        print(f"WARNING: {bin_path} has bad magic/version, emitting stub",
              file=sys.stderr)
        emit_stub(c_out)
        return

    manifest = parse_manifest(data, manifest_offset)
    vectors = parse_vectors(data, manifest, manifest_offset, footer_off)
    code_bytes = data[:manifest_offset]

    c_out.write(".section .rodata\n")
    emit_code_section(c_out, code_bytes)
    emit_manifest_scalars(c_out, manifest)
    emit_vector_payloads(c_out, vectors)
    c_out.write(".section .note.GNU-stack,\"\",@progbits\n")

