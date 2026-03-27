#!/usr/bin/env python3
"""
gen_wasm.py — Generate a test WASM module for Limnx
Outputs initrd/test.wasm with three exported functions:
  - add(i32, i32) -> i32   : returns a + b
  - fib(i32) -> i32         : fibonacci via recursion
  - hello() -> i32          : calls imported "print" with 42
"""

import struct
import os

def leb128_u(val):
    """Encode unsigned LEB128."""
    out = bytearray()
    while True:
        byte = val & 0x7f
        val >>= 7
        if val:
            byte |= 0x80
        out.append(byte)
        if not val:
            break
    return bytes(out)

def leb128_s(val):
    """Encode signed LEB128."""
    out = bytearray()
    while True:
        byte = val & 0x7f
        val >>= 7
        if (val == 0 and (byte & 0x40) == 0) or (val == -1 and (byte & 0x40) != 0):
            out.append(byte)
            break
        else:
            out.append(byte | 0x80)
    return bytes(out)

def encode_str(s):
    """Encode a WASM string (length + bytes)."""
    b = s.encode('utf-8')
    return leb128_u(len(b)) + b

def section(sec_id, data):
    """Encode a WASM section."""
    return bytes([sec_id]) + leb128_u(len(data)) + data

# ---- Type section ----
# Type 0: (i32, i32) -> i32   [for add]
# Type 1: (i32) -> i32         [for fib]
# Type 2: () -> i32            [for hello]
# Type 3: (i32) -> i32         [for print import]

type_0 = b'\x60' + leb128_u(2) + b'\x7f\x7f' + leb128_u(1) + b'\x7f'  # (i32,i32)->i32
type_1 = b'\x60' + leb128_u(1) + b'\x7f' + leb128_u(1) + b'\x7f'      # (i32)->i32
type_2 = b'\x60' + leb128_u(0) + leb128_u(1) + b'\x7f'                 # ()->i32
type_3 = b'\x60' + leb128_u(1) + b'\x7f' + leb128_u(1) + b'\x7f'      # (i32)->i32

type_sec = leb128_u(4) + type_0 + type_1 + type_2 + type_3

# ---- Import section ----
# Import "env"."print" as function type 3  (func index 0)
import_entry = encode_str("env") + encode_str("print") + b'\x00' + leb128_u(3)
import_sec = leb128_u(1) + import_entry

# ---- Function section ----
# func index 1 = add   (type 0)
# func index 2 = fib   (type 1)
# func index 3 = hello  (type 2)
func_sec = leb128_u(3) + leb128_u(0) + leb128_u(1) + leb128_u(2)

# ---- Export section ----
export_add   = encode_str("add")   + b'\x00' + leb128_u(1)  # func idx 1
export_fib   = encode_str("fib")   + b'\x00' + leb128_u(2)  # func idx 2
export_hello = encode_str("hello") + b'\x00' + leb128_u(3)  # func idx 3
export_sec = leb128_u(3) + export_add + export_fib + export_hello

# ---- Code section ----

# --- add(a, b) -> a + b ---
# local.get 0
# local.get 1
# i32.add
# end
add_body = b''
add_body += b'\x20' + leb128_u(0)   # local.get 0
add_body += b'\x20' + leb128_u(1)   # local.get 1
add_body += b'\x6a'                  # i32.add
add_body += b'\x0b'                  # end

add_code = leb128_u(0) + add_body   # 0 local declarations
add_entry = leb128_u(len(add_code)) + add_code

# --- fib(n) -> fib ---
# if n <= 1: return n
# else: return fib(n-1) + fib(n-2)
#
# WASM:
#   local.get 0        ;; n
#   i32.const 2
#   i32.lt_s
#   if [i32]
#     local.get 0      ;; return n
#   else
#     local.get 0
#     i32.const 1
#     i32.sub
#     call $fib         ;; fib(n-1)
#     local.get 0
#     i32.const 2
#     i32.sub
#     call $fib         ;; fib(n-2)
#     i32.add
#   end

fib_body = b''
fib_body += b'\x20' + leb128_u(0)   # local.get 0
fib_body += b'\x41' + leb128_s(2)   # i32.const 2
fib_body += b'\x48'                  # i32.lt_s
fib_body += b'\x04\x7f'             # if [i32]
fib_body += b'\x20' + leb128_u(0)   # local.get 0
fib_body += b'\x05'                  # else
fib_body += b'\x20' + leb128_u(0)   # local.get 0
fib_body += b'\x41' + leb128_s(1)   # i32.const 1
fib_body += b'\x6b'                  # i32.sub
fib_body += b'\x10' + leb128_u(2)   # call $fib (func idx 2)
fib_body += b'\x20' + leb128_u(0)   # local.get 0
fib_body += b'\x41' + leb128_s(2)   # i32.const 2
fib_body += b'\x6b'                  # i32.sub
fib_body += b'\x10' + leb128_u(2)   # call $fib (func idx 2)
fib_body += b'\x6a'                  # i32.add
fib_body += b'\x0b'                  # end (if)
fib_body += b'\x0b'                  # end (func)

fib_code = leb128_u(0) + fib_body   # 0 local declarations
fib_entry = leb128_u(len(fib_code)) + fib_code

# --- hello() -> i32 ---
# i32.const 42
# call $print          ;; import func idx 0
# end
hello_body = b''
hello_body += b'\x41' + leb128_s(42)  # i32.const 42
hello_body += b'\x10' + leb128_u(0)   # call $print (import, func idx 0)
hello_body += b'\x0b'                  # end

hello_code = leb128_u(0) + hello_body  # 0 local declarations
hello_entry = leb128_u(len(hello_code)) + hello_code

code_sec = leb128_u(3) + add_entry + fib_entry + hello_entry

# ---- Assemble module ----
wasm = b'\x00\x61\x73\x6d'      # magic
wasm += b'\x01\x00\x00\x00'     # version 1
wasm += section(1, type_sec)     # type section
wasm += section(2, import_sec)   # import section
wasm += section(3, func_sec)     # function section
wasm += section(7, export_sec)   # export section
wasm += section(10, code_sec)    # code section

# Write to initrd/test.wasm
script_dir = os.path.dirname(os.path.abspath(__file__))
project_dir = os.path.dirname(script_dir)
out_path = os.path.join(project_dir, "initrd", "test.wasm")
os.makedirs(os.path.dirname(out_path), exist_ok=True)

with open(out_path, 'wb') as f:
    f.write(wasm)

print(f"Generated {out_path} ({len(wasm)} bytes)")
print(f"  Type section: 4 types")
print(f"  Import section: 1 import (env.print)")
print(f"  Function section: 3 functions (add, fib, hello)")
print(f"  Export section: 3 exports")
print(f"  Code section: 3 bodies")

# Verify by dumping hex
print(f"\nHex dump ({len(wasm)} bytes):")
for i in range(0, len(wasm), 16):
    hex_str = ' '.join(f'{b:02x}' for b in wasm[i:i+16])
    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in wasm[i:i+16])
    print(f"  {i:04x}: {hex_str:<48s} {ascii_str}")
