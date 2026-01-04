import sys
import os

def main(c_out, bin_path):
    """
    Reads the raw binary and wraps it in "platform-agnostic assembly"
    which can be included as a build SOURCE
    """
    with open(bin_path, 'rb') as f_in:
        data = f_in.read()
        is_windows = os.name == 'nt'

        if is_windows:
            raise RuntimeError("TODO: handle MSVC")
        else:
            # Todo: what other architectures do we need to consider?
            # Is there a better tool (maybe a Rust script using the object
            # crate) that handles portability easier?

            c_out.write(".section .text\n")
            c_out.write(".global baseline_blob_start\n")
            c_out.write(".global baseline_blob_end\n")
            c_out.write(".balign 16\n")
            c_out.write("baseline_blob_start:\n")
            directive = ".byte"

        # Write data as hex arrays
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hex_str = ", ".join([f"0x{b:02x}" for b in chunk])
            c_out.write(f"    {directive} {hex_str}\n")

        if is_windows:
            raise RuntimeError("TODO: handle MSVC")
            # c_out.write("baseline_blob_end LABEL BYTE\n")
            # c_out.write("END\n")
        else:
            c_out.write("baseline_blob_end:\n")
            c_out.write(".section .note.GNU-stack,\"\",@progbits\n")

