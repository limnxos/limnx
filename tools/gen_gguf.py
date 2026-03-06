#!/usr/bin/env python3
"""
Generate test GGUF files for Limnx.

1. initrd/test.gguf — Stage 21 test: F32, llama prefix, no GQA
2. initrd/test_gqa.gguf — Stage 22 test: Q8_0/F16, qwen3 prefix, GQA, QK-norm
"""

import struct
import random
import os

# --- GGUF constants ---
GGUF_MAGIC = 0x46554747  # "GGUF"
GGUF_VERSION = 3

# Metadata value types
TYPE_UINT32  = 4
TYPE_FLOAT32 = 6
TYPE_STRING  = 8
TYPE_ARRAY   = 9

# Tensor types
GGML_TYPE_F32  = 0
GGML_TYPE_F16  = 1
GGML_TYPE_Q8_0 = 8

def write_string(f, s):
    """Write a GGUF string: uint64 length + bytes."""
    b = s.encode('utf-8')
    f.write(struct.pack('<Q', len(b)))
    f.write(b)

def write_kv_uint32(f, key, val):
    """Write a uint32 metadata KV pair."""
    write_string(f, key)
    f.write(struct.pack('<I', TYPE_UINT32))
    f.write(struct.pack('<I', val))

def write_kv_float32(f, key, val):
    """Write a float32 metadata KV pair."""
    write_string(f, key)
    f.write(struct.pack('<I', TYPE_FLOAT32))
    f.write(struct.pack('<f', val))

def write_kv_string_array(f, key, strings):
    """Write a string array metadata KV pair."""
    write_string(f, key)
    f.write(struct.pack('<I', TYPE_ARRAY))
    f.write(struct.pack('<I', TYPE_STRING))  # element type
    f.write(struct.pack('<Q', len(strings)))  # count
    for s in strings:
        write_string(f, s)

def write_tensor_info(f, name, shape, dtype, offset):
    """Write tensor info entry."""
    write_string(f, name)
    f.write(struct.pack('<I', len(shape)))  # n_dims
    for d in shape:
        f.write(struct.pack('<Q', d))
    f.write(struct.pack('<I', dtype))
    f.write(struct.pack('<Q', offset))

def gen_random_weights(n, rng):
    """Generate n random float32 weights scaled by 0.1."""
    return [rng.uniform(-0.1, 0.1) for _ in range(n)]

def float_to_f16(val):
    """Convert a Python float to IEEE 754 half-precision (uint16)."""
    # Use struct to convert to half
    return struct.unpack('<H', struct.pack('<e', val))[0]

def floats_to_q8_0(values):
    """Quantize a list of floats (must be multiple of 32) to Q8_0 format.
    Q8_0 block: f16 scale (2B) + int8 quants[32] (32B) = 34B per block."""
    assert len(values) % 32 == 0
    result = bytearray()
    for i in range(0, len(values), 32):
        block = values[i:i+32]
        # Find max absolute value for scale
        amax = max(abs(v) for v in block)
        scale = amax / 127.0 if amax > 0 else 0.0
        result += struct.pack('<e', scale)  # F16 scale
        for v in block:
            if scale > 0:
                q = max(-128, min(127, round(v / scale)))
            else:
                q = 0
            result += struct.pack('<b', q)
    return bytes(result)

def floats_to_f16(values):
    """Convert a list of floats to F16 format (2 bytes each)."""
    result = bytearray()
    for v in values:
        result += struct.pack('<e', v)
    return bytes(result)

def build_vocab(n_merges):
    """Build vocab: 256 byte tokens + n_merges merged tokens."""
    vocab = []
    for i in range(256):
        vocab.append(chr(i) if 32 <= i < 127 else f"<{i:02x}>")

    merges = []
    for i in range(n_merges):
        left = i * 2 + 65   # start from 'A' (65)
        right = left + 1
        if left >= 256 or right >= 256:
            left = i % 128 + 32
            right = left + 1
        merged_str = vocab[left] + vocab[right]
        vocab.append(merged_str)
        merges.append(f"{vocab[left]} {vocab[right]}")

    return vocab, merges


# ===================================================================
# Generate test.gguf (Stage 21 — F32, llama prefix, no GQA)
# ===================================================================

def gen_test_gguf():
    DIM = 64
    HIDDEN_DIM = 192
    N_LAYERS = 2
    N_HEADS = 4
    VOCAB_SIZE = 320
    MAX_SEQ_LEN = 128
    N_MERGES = 64

    rng = random.Random(42)

    vocab, merges = build_vocab(N_MERGES)
    assert len(vocab) == VOCAB_SIZE

    # --- Build tensor data ---
    tensors = []  # (name, shape, data_bytes, offset)
    offset = 0

    def add_tensor(name, shape, data=None, ones=False):
        nonlocal offset
        n = 1
        for d in shape:
            n *= d
        if ones:
            d = struct.pack(f'<{n}f', *([1.0] * n))
        elif data is not None:
            d = struct.pack(f'<{n}f', *data)
        else:
            d = struct.pack(f'<{n}f', *gen_random_weights(n, rng))
        tensors.append((name, shape, d, offset, GGML_TYPE_F32))
        size = n * 4
        padded = (size + 31) & ~31
        offset += padded

    add_tensor("token_embd.weight", [VOCAB_SIZE, DIM])
    for l in range(N_LAYERS):
        add_tensor(f"blk.{l}.attn_norm.weight", [DIM], ones=True)
        add_tensor(f"blk.{l}.attn_q.weight", [DIM, DIM])
        add_tensor(f"blk.{l}.attn_k.weight", [DIM, DIM])
        add_tensor(f"blk.{l}.attn_v.weight", [DIM, DIM])
        add_tensor(f"blk.{l}.attn_output.weight", [DIM, DIM])
        add_tensor(f"blk.{l}.ffn_norm.weight", [DIM], ones=True)
        add_tensor(f"blk.{l}.ffn_gate.weight", [HIDDEN_DIM, DIM])
        add_tensor(f"blk.{l}.ffn_down.weight", [DIM, HIDDEN_DIM])
        add_tensor(f"blk.{l}.ffn_up.weight", [HIDDEN_DIM, DIM])
    add_tensor("output_norm.weight", [DIM], ones=True)
    add_tensor("output.weight", [VOCAB_SIZE, DIM])

    n_kv = 7
    n_tensors = len(tensors)

    out_path = os.path.join(os.path.dirname(__file__), '..', 'initrd', 'test.gguf')
    out_path = os.path.abspath(out_path)

    with open(out_path, 'wb') as f:
        f.write(struct.pack('<I', GGUF_MAGIC))
        f.write(struct.pack('<I', GGUF_VERSION))
        f.write(struct.pack('<Q', n_tensors))
        f.write(struct.pack('<Q', n_kv))

        write_kv_uint32(f, "llama.embedding_length", DIM)
        write_kv_uint32(f, "llama.feed_forward_length", HIDDEN_DIM)
        write_kv_uint32(f, "llama.attention.head_count", N_HEADS)
        write_kv_uint32(f, "llama.block_count", N_LAYERS)
        write_kv_uint32(f, "llama.context_length", MAX_SEQ_LEN)
        write_kv_string_array(f, "tokenizer.ggml.tokens", vocab)
        write_kv_string_array(f, "tokenizer.ggml.merges", merges)

        for name, shape, data, toffset, dtype in tensors:
            write_tensor_info(f, name, shape, dtype, toffset)

        pos = f.tell()
        aligned = (pos + 31) & ~31
        if aligned > pos:
            f.write(b'\x00' * (aligned - pos))

        for name, shape, data, toffset, dtype in tensors:
            expected_pos = aligned + toffset
            current_pos = f.tell()
            if current_pos < expected_pos:
                f.write(b'\x00' * (expected_pos - current_pos))
            f.write(data)
            n = 1
            for d in shape:
                n *= d
            size = n * 4  # F32
            padded = (size + 31) & ~31
            if padded > size:
                f.write(b'\x00' * (padded - size))

    file_size = os.path.getsize(out_path)
    print(f"Generated {out_path}")
    print(f"  Config: dim={DIM}, hidden_dim={HIDDEN_DIM}, n_layers={N_LAYERS}, "
          f"n_heads={N_HEADS}, vocab_size={VOCAB_SIZE}, max_seq_len={MAX_SEQ_LEN}")
    print(f"  Tensors: {n_tensors} (all F32)")
    print(f"  File size: {file_size} bytes ({file_size / 1024:.1f} KB)")


# ===================================================================
# Generate test_gqa.gguf (Stage 22 — Q8_0/F16, qwen3, GQA, QK-norm)
# ===================================================================

def gen_test_gqa_gguf():
    DIM = 64
    HIDDEN_DIM = 192
    N_LAYERS = 2
    N_HEADS = 4
    N_KV_HEADS = 2
    VOCAB_SIZE = 320
    MAX_SEQ_LEN = 128
    N_MERGES = 64
    ROPE_THETA = 1000000.0
    HEAD_DIM = DIM // N_HEADS  # 16
    KV_DIM = N_KV_HEADS * HEAD_DIM  # 32

    rng = random.Random(42)

    vocab, merges = build_vocab(N_MERGES)
    assert len(vocab) == VOCAB_SIZE

    # --- Build tensor data ---
    tensors = []  # (name, shape, data_bytes, offset, dtype)
    offset = 0

    def add_f32(name, shape, data=None, ones=False):
        nonlocal offset
        n = 1
        for d in shape:
            n *= d
        if ones:
            d = struct.pack(f'<{n}f', *([1.0] * n))
        elif data is not None:
            d = struct.pack(f'<{n}f', *data)
        else:
            d = struct.pack(f'<{n}f', *gen_random_weights(n, rng))
        tensors.append((name, shape, d, offset, GGML_TYPE_F32))
        size = n * 4
        padded = (size + 31) & ~31
        offset += padded

    def add_q8_0(name, shape):
        nonlocal offset
        n = 1
        for d in shape:
            n *= d
        assert n % 32 == 0, f"{name}: element count {n} not multiple of 32"
        floats = gen_random_weights(n, rng)
        d = floats_to_q8_0(floats)
        tensors.append((name, shape, d, offset, GGML_TYPE_Q8_0))
        size = len(d)
        padded = (size + 31) & ~31
        offset += padded

    def add_f16(name, shape, ones=False):
        nonlocal offset
        n = 1
        for d in shape:
            n *= d
        if ones:
            floats = [1.0] * n
        else:
            floats = gen_random_weights(n, rng)
        d = floats_to_f16(floats)
        tensors.append((name, shape, d, offset, GGML_TYPE_F16))
        size = len(d)
        padded = (size + 31) & ~31
        offset += padded

    # token_embd.weight [VOCAB_SIZE, DIM] — F32
    add_f32("token_embd.weight", [VOCAB_SIZE, DIM])

    for l in range(N_LAYERS):
        # Norm weights — F32
        add_f32(f"blk.{l}.attn_norm.weight", [DIM], ones=True)
        # Q weight — Q8_0 [DIM, DIM]
        add_q8_0(f"blk.{l}.attn_q.weight", [DIM, DIM])
        # K weight — Q8_0 [KV_DIM, DIM] (GQA!)
        add_q8_0(f"blk.{l}.attn_k.weight", [KV_DIM, DIM])
        # V weight — Q8_0 [KV_DIM, DIM] (GQA!)
        add_q8_0(f"blk.{l}.attn_v.weight", [KV_DIM, DIM])
        # Output weight — Q8_0 [DIM, DIM]
        add_q8_0(f"blk.{l}.attn_output.weight", [DIM, DIM])
        # QK-norm weights — F16 [HEAD_DIM]
        add_f16(f"blk.{l}.attn_q_norm.weight", [HEAD_DIM], ones=True)
        add_f16(f"blk.{l}.attn_k_norm.weight", [HEAD_DIM], ones=True)
        # FFN norm — F32
        add_f32(f"blk.{l}.ffn_norm.weight", [DIM], ones=True)
        # FFN weights — F16
        add_f16(f"blk.{l}.ffn_gate.weight", [HIDDEN_DIM, DIM])
        add_f16(f"blk.{l}.ffn_down.weight", [DIM, HIDDEN_DIM])
        add_f16(f"blk.{l}.ffn_up.weight", [HIDDEN_DIM, DIM])

    # output_norm.weight [DIM] — F32
    add_f32("output_norm.weight", [DIM], ones=True)
    # output.weight [VOCAB_SIZE, DIM] — F32
    add_f32("output.weight", [VOCAB_SIZE, DIM])

    # Metadata: 7 config KVs + 2 tokenizer arrays = 9
    n_kv = 9
    n_tensors = len(tensors)

    out_path = os.path.join(os.path.dirname(__file__), '..', 'initrd', 'test_gqa.gguf')
    out_path = os.path.abspath(out_path)

    with open(out_path, 'wb') as f:
        f.write(struct.pack('<I', GGUF_MAGIC))
        f.write(struct.pack('<I', GGUF_VERSION))
        f.write(struct.pack('<Q', n_tensors))
        f.write(struct.pack('<Q', n_kv))

        # Architecture-specific prefix: qwen3
        write_kv_uint32(f, "qwen3.embedding_length", DIM)
        write_kv_uint32(f, "qwen3.feed_forward_length", HIDDEN_DIM)
        write_kv_uint32(f, "qwen3.attention.head_count", N_HEADS)
        write_kv_uint32(f, "qwen3.attention.head_count_kv", N_KV_HEADS)
        write_kv_uint32(f, "qwen3.block_count", N_LAYERS)
        write_kv_uint32(f, "qwen3.context_length", MAX_SEQ_LEN)
        write_kv_float32(f, "qwen3.rope.freq_base", ROPE_THETA)
        write_kv_string_array(f, "tokenizer.ggml.tokens", vocab)

        # Write merges
        write_kv_string_array(f, "tokenizer.ggml.merges", merges)

        # Tensor info
        for name, shape, data, toffset, dtype in tensors:
            write_tensor_info(f, name, shape, dtype, toffset)

        # Pad to 32-byte alignment before tensor data
        pos = f.tell()
        aligned = (pos + 31) & ~31
        if aligned > pos:
            f.write(b'\x00' * (aligned - pos))

        # Tensor data
        for name, shape, data, toffset, dtype in tensors:
            expected_pos = aligned + toffset
            current_pos = f.tell()
            if current_pos < expected_pos:
                f.write(b'\x00' * (expected_pos - current_pos))
            f.write(data)
            # Pad to 32-byte alignment
            size = len(data)
            padded = (size + 31) & ~31
            if padded > size:
                f.write(b'\x00' * (padded - size))

    file_size = os.path.getsize(out_path)
    print(f"Generated {out_path}")
    print(f"  Config: dim={DIM}, hidden_dim={HIDDEN_DIM}, n_layers={N_LAYERS}, "
          f"n_heads={N_HEADS}, n_kv_heads={N_KV_HEADS}, vocab_size={VOCAB_SIZE}, "
          f"max_seq_len={MAX_SEQ_LEN}, rope_theta={ROPE_THETA}")
    print(f"  Tensors: {n_tensors} (mixed F32/F16/Q8_0)")
    print(f"  File size: {file_size} bytes ({file_size / 1024:.1f} KB)")


if __name__ == '__main__':
    gen_test_gguf()
    print()
    gen_test_gqa_gguf()
