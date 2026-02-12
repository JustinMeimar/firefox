import sys
import os
import struct

# These must match BASELINE_MANIFEST_FIELDS in BaselineAOT.h (order matters).
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

# (symbol_name, count_field_name, element_size_bytes)
# Must match the write order in serializeAOTManifest.
VECTORS = [
    ("DebugInstrumentationOffsets", "DebugInstrumentationCount", 4),
    ("DebugTrapOffsets",            "DebugTrapCount",            4),
    ("CodeCoverageOffsets",         "CodeCoverageCount",         4),
    ("ICReturnOffsets",             "ICReturnCount",             8),
    ("RuntimePatches",              "RuntimePatchCount",        12),
]


def emit_bytes(c_out, data):
    """Write raw bytes as .byte directives, 16 bytes per line."""
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
        c_out.write(f"    .byte {hex_str}\n")


def main(c_out, bin_path):
    """
    Parses the self-describing AOT baseline interpreter binary and emits
    a `.S` file with named symbols for every piece of data, so the C++
    load path can reference them directly via `extern` declarations.

    Binary layout (written by serializeAOTManifest):
      [machine code]          0 .. manifestOffset
      [manifest: 17×uint32]  manifestOffset .. manifestOffset + 68
      [vector payloads]       after manifest
      [footer: 12 bytes]      last 12 bytes
    """
    with open(bin_path, 'rb') as f_in:
        data = f_in.read()

    if os.name == 'nt':
        raise RuntimeError("TODO: handle MSVC")

    blob_size = len(data)
    assert blob_size > 12, f"Binary too small: {blob_size} bytes"

    # --- Parse footer (last 12 bytes) ---
    footer_off = blob_size - 12
    magic, version, manifest_offset = struct.unpack_from('<III', data, footer_off)
    assert magic == 0x424C494E, f"Bad magic: 0x{magic:08x}"
    assert version == 1, f"Unsupported version: {version}"

    # --- Parse manifest (17 × uint32_t) ---
    num_fields = len(MANIFEST_FIELD_NAMES)
    manifest_size = num_fields * 4
    assert manifest_offset + manifest_size <= footer_off, "Manifest overflows into footer"

    manifest = {}
    for i, name in enumerate(MANIFEST_FIELD_NAMES):
        val, = struct.unpack_from('<I', data, manifest_offset + i * 4)
        manifest[name] = val

    # --- Extract code bytes (everything before the manifest) ---
    code_bytes = data[:manifest_offset]

    # --- Extract vector payloads ---
    vec_cursor = manifest_offset + manifest_size
    vectors = {}
    for vec_name, count_field, elem_size in VECTORS:
        count = manifest[count_field]
        nbytes = count * elem_size
        assert vec_cursor + nbytes <= footer_off, \
            f"Vector {vec_name} overflows (cursor={vec_cursor}, need={nbytes}, footer={footer_off})"
        vectors[vec_name] = data[vec_cursor:vec_cursor + nbytes]
        vec_cursor += nbytes

    # --- Emit .S ---
    c_out.write(".section .rodata\n")

    # Machine code
    c_out.write(".balign 16\n")
    c_out.write(".global bl_aot_code_start\n")
    c_out.write(".global bl_aot_code_end\n")
    c_out.write("bl_aot_code_start:\n")
    emit_bytes(c_out, code_bytes)
    c_out.write("bl_aot_code_end:\n\n")

    # Manifest scalar fields
    c_out.write(".balign 4\n")
    for name in MANIFEST_FIELD_NAMES:
        sym = f"bl_aot_{name}"
        c_out.write(f".global {sym}\n")
        c_out.write(f"{sym}:    .long 0x{manifest[name]:08x}\n")
    c_out.write("\n")

    # Vector payloads
    for vec_name, _, _ in VECTORS:
        raw = vectors[vec_name]
        start_sym = f"bl_aot_{vec_name}_start"
        end_sym = f"bl_aot_{vec_name}_end"
        c_out.write(".balign 4\n")
        c_out.write(f".global {start_sym}\n")
        c_out.write(f".global {end_sym}\n")
        c_out.write(f"{start_sym}:\n")
        if raw:
            emit_bytes(c_out, raw)
        c_out.write(f"{end_sym}:\n\n")

    c_out.write(".section .note.GNU-stack,\"\",@progbits\n")
