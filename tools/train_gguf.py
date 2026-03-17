#!/usr/bin/env python3
"""
Train a tiny transformer and export as GGUF for Limnx.
Replaces initrd/test.gguf with trained (non-random) weights.

Config matches gen_gguf.py: dim=64, hidden=192, heads=4, layers=2,
vocab=320 (256 byte + 64 BPE), seq=128. All F32, llama prefix.

Training: simple SGD on output + embedding weights to learn
character patterns. Not a real LLM — just enough to produce
recognizable output instead of random gibberish.
"""

import struct, os
import numpy as np

DIM, HIDDEN, HEADS, LAYERS = 64, 192, 4, 2
HD = DIM // HEADS
VOCAB, SEQ = 320, 128
N_MERGES = 64
LR, EPOCHS = 0.002, 800

np.random.seed(42)

def build_vocab():
    v = [chr(i) if 32 <= i < 127 else f"<{i:02x}>" for i in range(256)]
    m = []
    for i in range(N_MERGES):
        l = i * 2 + 65; r = l + 1
        if l >= 256 or r >= 256: l = i % 128 + 32; r = l + 1
        v.append(v[l] + v[r]); m.append(f"{v[l]} {v[r]}")
    return v, m

def encode(text):
    return [ord(c) for c in text if ord(c) < 256]

def softmax(x):
    e = np.exp(x - x.max(axis=-1, keepdims=True))
    return e / e.sum(axis=-1, keepdims=True)

def rms_norm(x, w):
    return x / np.sqrt((x*x).mean(-1, keepdims=True) + 1e-5) * w

class Model:
    def __init__(self):
        s = 0.02
        self.emb = np.random.randn(VOCAB, DIM).astype(np.float32) * s
        self.L = []
        for _ in range(LAYERS):
            self.L.append({
                'an': np.ones(DIM, np.float32),
                'wq': np.random.randn(DIM, DIM).astype(np.float32) * s,
                'wk': np.random.randn(DIM, DIM).astype(np.float32) * s,
                'wv': np.random.randn(DIM, DIM).astype(np.float32) * s,
                'wo': np.random.randn(DIM, DIM).astype(np.float32) * s,
                'fn': np.ones(DIM, np.float32),
                'w1': np.random.randn(HIDDEN, DIM).astype(np.float32) * s,
                'w2': np.random.randn(DIM, HIDDEN).astype(np.float32) * s,
                'w3': np.random.randn(HIDDEN, DIM).astype(np.float32) * s,
            })
        self.on = np.ones(DIM, np.float32)
        self.out = np.random.randn(VOCAB, DIM).astype(np.float32) * s

    def forward(self, toks):
        T = len(toks)
        x = self.emb[toks].copy()
        mask = np.triu(np.full((T,T), -1e9), 1)
        for l in self.L:
            h = rms_norm(x, l['an'])
            q = (h @ l['wq'].T).reshape(T,HEADS,HD)
            k = (h @ l['wk'].T).reshape(T,HEADS,HD)
            v = (h @ l['wv'].T).reshape(T,HEADS,HD)
            sc = np.einsum('snh,tnh->nst', q, k) / np.sqrt(HD) + mask[None]
            a = softmax(sc)
            x = x + np.einsum('nst,tnh->snh', a, v).reshape(T,DIM) @ l['wo'].T
            h = rms_norm(x, l['fn'])
            g = h @ l['w1'].T; u = h @ l['w3'].T
            x = x + ((g / (1 + np.exp(-g))) * u) @ l['w2'].T
        return rms_norm(x, self.on) @ self.out.T

DATA = [
    "hello hello hello hello hello",
    "world world world world world",
    "limnx limnx limnx limnx",
    "the cat sat on the mat the cat sat on the mat",
    "the dog ran in the park the dog ran in the park",
    "hello world hello world hello world",
    "limnx is an ai native operating system",
    "inference pipeline ready for requests",
    "abcabcabcabcabcabc",
    "one two three four five six seven eight",
    "aaa bbb ccc ddd eee fff ggg hhh",
    "kernel booted successfully all tests passed",
    "the quick brown fox jumps over the lazy dog",
    "to be or not to be that is the question",
    "all your base are belong to us all your base",
]

def train():
    m = Model()
    seqs = [np.array(encode(t), np.int32) for t in DATA if len(encode(t)) >= 2]
    print(f"Training {len(seqs)} seqs, {EPOCHS} epochs, lr={LR}")

    for ep in range(EPOCHS):
        loss_sum = 0.0
        np.random.shuffle(seqs)
        for seq in seqs:
            s = seq[:SEQ]
            logits = m.forward(s[:-1])
            probs = softmax(logits)
            tgts = s[1:]
            T = len(tgts)
            loss_sum += -sum(np.log(max(probs[t, tgts[t]], 1e-10)) for t in range(T)) / T

            for t in range(T):
                g = probs[t].copy(); g[tgts[t]] -= 1.0; g /= T
                h = rms_norm(m.emb[s[:-1]][t:t+1], m.on).flatten()
                # Update output weights
                nz = np.abs(g[:256]) > 0.0005
                m.out[:256][nz] -= LR * np.outer(g[:256][nz], h)
                # Update embeddings
                m.emb[s[t]] -= LR * 0.1 * (m.out.T @ g)

        if ep % 100 == 0 or ep == EPOCHS-1:
            print(f"  ep {ep:4d}: loss={loss_sum/len(seqs):.3f}")

    print("\nTest:")
    for p in ["hello", "the cat", "limnx", "abc", "to be"]:
        toks = list(encode(p))
        for _ in range(30):
            logits = m.forward(np.array(toks, np.int32))
            toks.append(int(np.argmax(logits[-1])))
        print(f"  '{p}' -> '{''.join(chr(t) if 32<=t<127 else '?' for t in toks)}'")
    return m

# ---- GGUF export ----

M, V3, U32, STR, ARR, F32T = 0x46554747, 3, 4, 8, 9, 0

def ws(f,s):
    b=s.encode(); f.write(struct.pack('<Q',len(b))); f.write(b)
def wku(f,k,v):
    ws(f,k); f.write(struct.pack('<II',U32,v))
def wka(f,k,ss):
    ws(f,k); f.write(struct.pack('<IIQ',ARR,STR,len(ss)))
    for s in ss: ws(f,s)
def wti(f,n,sh,off):
    ws(f,n); f.write(struct.pack('<I',len(sh)))
    for d in sh: f.write(struct.pack('<Q',d))
    f.write(struct.pack('<IQ',F32T,off))

def export(model, path):
    vocab, merges = build_vocab()
    ts=[]; off=0
    def a(n,arr):
        nonlocal off
        r=arr.flatten().astype(np.float32).tobytes()
        ts.append((n,list(arr.shape),r,off))
        off+=(len(r)+31)&~31

    a("token_embd.weight", model.emb)
    for i,l in enumerate(model.L):
        a(f"blk.{i}.attn_norm.weight", l['an'])
        a(f"blk.{i}.attn_q.weight", l['wq'])
        a(f"blk.{i}.attn_k.weight", l['wk'])
        a(f"blk.{i}.attn_v.weight", l['wv'])
        a(f"blk.{i}.attn_output.weight", l['wo'])
        a(f"blk.{i}.ffn_norm.weight", l['fn'])
        a(f"blk.{i}.ffn_gate.weight", l['w1'])
        a(f"blk.{i}.ffn_down.weight", l['w2'])
        a(f"blk.{i}.ffn_up.weight", l['w3'])
    a("output_norm.weight", model.on)
    a("output.weight", model.out)

    with open(path,'wb') as f:
        f.write(struct.pack('<IIQQ',M,V3,len(ts),7))
        wku(f,"llama.embedding_length",DIM)
        wku(f,"llama.feed_forward_length",HIDDEN)
        wku(f,"llama.attention.head_count",HEADS)
        wku(f,"llama.block_count",LAYERS)
        wku(f,"llama.context_length",SEQ)
        wka(f,"tokenizer.ggml.tokens",vocab)
        wka(f,"tokenizer.ggml.merges",merges)
        for n,sh,r,to in ts: wti(f,n,sh,to)
        p=f.tell(); al=(p+31)&~31; f.write(b'\x00'*(al-p))
        for n,sh,r,to in ts:
            tgt=al+to; cur=f.tell()
            if cur<tgt: f.write(b'\x00'*(tgt-cur))
            f.write(r); pad=((len(r)+31)&~31)-len(r)
            if pad: f.write(b'\x00'*pad)
    print(f"\nExported {path} ({os.path.getsize(path)/1024:.0f} KB)")

if __name__=='__main__':
    m = train()
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','initrd','test.gguf')
    export(m, os.path.abspath(p))
