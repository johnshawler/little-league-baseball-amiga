#!/usr/bin/env python3
"""mkhunk.py in.bin out  -- wrap a flat, position independent 68k binary into an AmigaDOS load file (HUNK_HEADER, one HUNK_CODE, HUNK_END).
The code starts at the first byte of the hunk, no relocations."""
import struct, sys
code = open(sys.argv[1], 'rb').read()
code += b'\0' * (-len(code) % 4)
n = len(code) // 4
out = struct.pack('>IIIIII', 0x3F3, 0, 1, 0, 0, n)       # HUNK_HEADER: no resident names, 1 hunk (first=last=0), size in longwords
out += struct.pack('>II', 0x3E9, n) + code + struct.pack('>I', 0x3F2)
open(sys.argv[2], 'wb').write(out)
