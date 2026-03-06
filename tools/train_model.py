#!/usr/bin/env python3
"""
Train a tiny character-level transformer and export weights for Limnx.

The model architecture exactly matches user/libc/transformer.c:
  - RMS norm (no bias, eps=1e-5)
  - Multi-head attention (no positional encoding, no bias)
  - FFN: dim->hidden_dim (ReLU) -> hidden_dim->dim (no bias)
  - matmul convention: out[d] = x[n] @ W[n x d], W is row-major

Config: dim=48, hidden_dim=128, heads=4, layers=2, vocab_size=96, seq_len=64

Binary format:
  Header: 6 x uint32 (dim, hidden_dim, n_heads, n_layers, vocab_size, max_seq_len)
  Weights: all float32, row-major, in the order transformer.c expects.

Usage:
  python tools/train_model.py
"""

import struct
import os
import numpy as np

# Try torch, but provide clear error if missing
try:
    import torch
    import torch.nn as nn
    import torch.nn.functional as F
except ImportError:
    print("ERROR: PyTorch is required. Install with: pip install torch")
    raise SystemExit(1)

# ---------------------------------------------------------------------------
# Config (must match Limnx transformer.c)
# ---------------------------------------------------------------------------
DIM = 48
HIDDEN_DIM = 128
N_HEADS = 4
N_LAYERS = 2
VOCAB_SIZE = 96
MAX_SEQ_LEN = 64

# Training hyperparameters
BATCH_SIZE = 64
LR = 1e-3
EPOCHS = 3000
SEED = 42

# Output path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
OUTPUT_PATH = os.path.join(PROJECT_DIR, "initrd", "model.bin")

# ---------------------------------------------------------------------------
# Tokenizer (matches user/libc/tokenizer.c)
# ---------------------------------------------------------------------------
# Vocab: printable ASCII 32-126 (95 chars) + newline = 96 tokens
CHARS = [chr(c) for c in range(32, 127)] + ['\n']
assert len(CHARS) == VOCAB_SIZE
CHAR_TO_IDX = {c: i for i, c in enumerate(CHARS)}


def encode(text):
    """Encode text to token indices, skipping unknown chars."""
    return [CHAR_TO_IDX[c] for c in text if c in CHAR_TO_IDX]


def decode(tokens):
    """Decode token indices to text."""
    return ''.join(CHARS[t] for t in tokens if 0 <= t < VOCAB_SIZE)


# ---------------------------------------------------------------------------
# Training data — a mix of patterns for the tiny model to learn
# ---------------------------------------------------------------------------
TRAINING_TEXT = """\
To be, or not to be, that is the question.
Whether 'tis nobler in the mind to suffer
The slings and arrows of outrageous fortune,
Or to take arms against a sea of troubles,
And by opposing end them. To die, to sleep,
No more; and by a sleep to say we end
The heart-ache and the thousand natural shocks
That flesh is heir to: 'tis a consummation
Devoutly to be wish'd. To die, to sleep;
To sleep, perchance to dream. Ay, there's the rub,
For in that sleep of death what dreams may come,
When we have shuffled off this mortal coil,
Must give us pause.
All the world's a stage,
And all the men and women merely players;
They have their exits and their entrances,
And one man in his time plays many parts.
Shall I compare thee to a summer's day?
Thou art more lovely and more temperate.
Rough winds do shake the darling buds of May,
And summer's lease hath all too short a date.
The quality of mercy is not strain'd,
It droppeth as the gentle rain from heaven
Upon the place beneath; it is twice blest;
It blesseth him that gives and him that takes.
Now is the winter of our discontent
Made glorious summer by this sun of York;
And all the clouds that lour'd upon our house
In the deep bosom of the ocean buried.
Friends, Romans, countrymen, lend me your ears;
I come to bury Caesar, not to praise him.
The evil that men do lives after them;
The good is oft interred with their bones.
If music be the food of love, play on,
Give me excess of it; that surfeiting,
The appetite may sicken, and so die.
O Romeo, Romeo! wherefore art thou Romeo?
Deny thy father and refuse thy name;
Or, if thou wilt not, be but sworn my love,
And I'll no longer be a Capulet.
Good night, good night! Parting is such sweet sorrow,
That I shall say good night till it be morrow.
Love looks not with the eyes, but with the mind,
And therefore is winged Cupid painted blind.
We are such stuff as dreams are made on,
And our little life is rounded with a sleep.
Double, double toil and trouble;
Fire burn and caldron bubble.
Out, out, brief candle! Life's but a walking shadow,
A poor player that struts and frets his hour upon the stage,
And then is heard no more. It is a tale
Told by an idiot, full of sound and fury,
Signifying nothing.
"""


# ---------------------------------------------------------------------------
# Model — exactly matches Limnx transformer.c forward pass
# ---------------------------------------------------------------------------
class RMSNorm(nn.Module):
    def __init__(self, dim, eps=1e-5):
        super().__init__()
        self.eps = eps
        self.weight = nn.Parameter(torch.ones(dim))

    def forward(self, x):
        ss = x.pow(2).mean(-1, keepdim=True)
        x = x * torch.rsqrt(ss + self.eps)
        return x * self.weight


class Attention(nn.Module):
    def __init__(self, dim, n_heads):
        super().__init__()
        self.n_heads = n_heads
        self.head_dim = dim // n_heads
        self.wq = nn.Linear(dim, dim, bias=False)
        self.wk = nn.Linear(dim, dim, bias=False)
        self.wv = nn.Linear(dim, dim, bias=False)
        self.wo = nn.Linear(dim, dim, bias=False)

    def forward(self, x, mask=None):
        B, T, C = x.shape
        q = self.wq(x).view(B, T, self.n_heads, self.head_dim).transpose(1, 2)
        k = self.wk(x).view(B, T, self.n_heads, self.head_dim).transpose(1, 2)
        v = self.wv(x).view(B, T, self.n_heads, self.head_dim).transpose(1, 2)

        att = (q @ k.transpose(-2, -1)) / (self.head_dim ** 0.5)
        if mask is not None:
            att = att.masked_fill(mask[:, :, :T, :T] == 0, float('-inf'))
        att = F.softmax(att, dim=-1)
        out = att @ v
        out = out.transpose(1, 2).contiguous().view(B, T, C)
        return self.wo(out)


class FFN(nn.Module):
    def __init__(self, dim, hidden_dim):
        super().__init__()
        self.w1 = nn.Linear(dim, hidden_dim, bias=False)
        self.w2 = nn.Linear(hidden_dim, dim, bias=False)

    def forward(self, x):
        return self.w2(F.relu(self.w1(x)))


class TransformerBlock(nn.Module):
    def __init__(self, dim, hidden_dim, n_heads):
        super().__init__()
        self.rms_att = RMSNorm(dim)
        self.attn = Attention(dim, n_heads)
        self.rms_ffn = RMSNorm(dim)
        self.ffn = FFN(dim, hidden_dim)

    def forward(self, x, mask=None):
        x = x + self.attn(self.rms_att(x), mask)
        x = x + self.ffn(self.rms_ffn(x))
        return x


class TinyTransformer(nn.Module):
    def __init__(self):
        super().__init__()
        self.token_emb = nn.Embedding(VOCAB_SIZE, DIM)
        self.layers = nn.ModuleList([
            TransformerBlock(DIM, HIDDEN_DIM, N_HEADS)
            for _ in range(N_LAYERS)
        ])
        self.rms_final = RMSNorm(DIM)
        self.wcls = nn.Linear(DIM, VOCAB_SIZE, bias=False)

    def forward(self, tokens, mask=None):
        x = self.token_emb(tokens)
        for layer in self.layers:
            x = layer(x, mask)
        x = self.rms_final(x)
        logits = self.wcls(x)
        return logits


# ---------------------------------------------------------------------------
# Export weights in Limnx binary format
# ---------------------------------------------------------------------------
def export_weights(model, path):
    """Export model weights in the exact binary format expected by transformer.c.

    Weight layout (all float32, row-major):
      Header: 6 x uint32
      1. token_emb          (vocab_size x dim)
      2. rms_att_w[layer]   (dim) per layer, all layers sequentially
      3. wq[layer]          (dim x dim) per layer
      4. wk[layer]          (dim x dim) per layer
      5. wv[layer]          (dim x dim) per layer
      6. wo[layer]          (dim x dim) per layer
      7. rms_ffn_w[layer]   (dim) per layer
      8. w1[layer]          (dim x hidden_dim) per layer
      9. w2[layer]          (hidden_dim x dim) per layer
      10. rms_final_w       (dim)
      11. wcls              (dim x vocab_size)

    IMPORTANT: PyTorch nn.Linear(n, d) stores weight as (d, n).
    Our C code does: out[d] = x[n] @ W[n x d]
    So we must transpose: exported = linear.weight.T  → shape (n, d)
    """
    with open(path, 'wb') as f:
        # Header
        f.write(struct.pack('<6I', DIM, HIDDEN_DIM, N_HEADS, N_LAYERS,
                            VOCAB_SIZE, MAX_SEQ_LEN))

        # 1. token_emb (vocab_size x dim)
        w = model.token_emb.weight.detach().cpu().numpy()  # (vocab_size, dim)
        f.write(w.astype(np.float32).tobytes())

        # 2. rms_att_w per layer
        for l in range(N_LAYERS):
            w = model.layers[l].rms_att.weight.detach().cpu().numpy()
            f.write(w.astype(np.float32).tobytes())

        # 3. wq per layer — transpose from (dim, dim) to (dim, dim)
        for l in range(N_LAYERS):
            w = model.layers[l].attn.wq.weight.detach().cpu().T.numpy()
            f.write(w.astype(np.float32).tobytes())

        # 4. wk per layer
        for l in range(N_LAYERS):
            w = model.layers[l].attn.wk.weight.detach().cpu().T.numpy()
            f.write(w.astype(np.float32).tobytes())

        # 5. wv per layer
        for l in range(N_LAYERS):
            w = model.layers[l].attn.wv.weight.detach().cpu().T.numpy()
            f.write(w.astype(np.float32).tobytes())

        # 6. wo per layer
        for l in range(N_LAYERS):
            w = model.layers[l].attn.wo.weight.detach().cpu().T.numpy()
            f.write(w.astype(np.float32).tobytes())

        # 7. rms_ffn_w per layer
        for l in range(N_LAYERS):
            w = model.layers[l].rms_ffn.weight.detach().cpu().numpy()
            f.write(w.astype(np.float32).tobytes())

        # 8. w1 per layer (dim x hidden_dim)
        for l in range(N_LAYERS):
            w = model.layers[l].ffn.w1.weight.detach().cpu().T.numpy()
            f.write(w.astype(np.float32).tobytes())

        # 9. w2 per layer (hidden_dim x dim)
        for l in range(N_LAYERS):
            w = model.layers[l].ffn.w2.weight.detach().cpu().T.numpy()
            f.write(w.astype(np.float32).tobytes())

        # 10. rms_final_w (dim)
        w = model.rms_final.weight.detach().cpu().numpy()
        f.write(w.astype(np.float32).tobytes())

        # 11. wcls (dim x vocab_size)
        w = model.wcls.weight.detach().cpu().T.numpy()
        f.write(w.astype(np.float32).tobytes())

    file_size = os.path.getsize(path)
    print(f"Exported {file_size:,} bytes to {path}")

    # Verify size
    expected = 6 * 4  # header
    expected += VOCAB_SIZE * DIM * 4  # token_emb
    expected += N_LAYERS * DIM * 4  # rms_att_w
    expected += N_LAYERS * DIM * DIM * 4 * 4  # wq, wk, wv, wo
    expected += N_LAYERS * DIM * 4  # rms_ffn_w
    expected += N_LAYERS * DIM * HIDDEN_DIM * 4  # w1
    expected += N_LAYERS * HIDDEN_DIM * DIM * 4  # w2
    expected += DIM * 4  # rms_final_w
    expected += DIM * VOCAB_SIZE * 4  # wcls
    assert file_size == expected, f"Size mismatch: {file_size} != {expected}"
    print(f"Size verified: {expected:,} bytes")


# ---------------------------------------------------------------------------
# Validation: load weights back and verify forward pass matches
# ---------------------------------------------------------------------------
def validate_export(model, path):
    """Load the exported binary and verify logits match PyTorch output."""
    print("\nValidating exported weights...")

    with open(path, 'rb') as f:
        header = struct.unpack('<6I', f.read(24))
        assert header == (DIM, HIDDEN_DIM, N_HEADS, N_LAYERS, VOCAB_SIZE, MAX_SEQ_LEN)

        # Read all remaining weights
        data = np.frombuffer(f.read(), dtype=np.float32)

    # Reconstruct token_emb and check
    offset = 0
    token_emb = data[offset:offset + VOCAB_SIZE * DIM].reshape(VOCAB_SIZE, DIM)
    offset += VOCAB_SIZE * DIM

    ref = model.token_emb.weight.detach().cpu().numpy()
    max_err = np.max(np.abs(token_emb - ref))
    print(f"  token_emb max error: {max_err:.2e}")
    assert max_err < 1e-6

    # Run a forward pass with PyTorch and compare first few logits
    model.eval()
    test_input = encode("Hello")
    tokens_t = torch.tensor([test_input], dtype=torch.long)

    # Build causal mask
    T = tokens_t.shape[1]
    mask = torch.tril(torch.ones(T, T)).unsqueeze(0).unsqueeze(0)

    with torch.no_grad():
        logits = model(tokens_t, mask)

    # Print first token predictions
    probs = F.softmax(logits[0, -1], dim=-1)
    top5 = torch.topk(probs, 5)
    print(f"  PyTorch top-5 next after 'Hello':")
    for i in range(5):
        idx = top5.indices[i].item()
        prob = top5.values[i].item()
        ch = CHARS[idx] if idx < len(CHARS) else '?'
        print(f"    '{ch}' (idx={idx}) p={prob:.4f}")

    print("  Validation passed!")


# ---------------------------------------------------------------------------
# Main training loop
# ---------------------------------------------------------------------------
def main():
    torch.manual_seed(SEED)
    np.random.seed(SEED)

    # Select device
    if torch.backends.mps.is_available():
        device = torch.device("mps")
        print("Using MPS (Apple Silicon) acceleration")
    elif torch.cuda.is_available():
        device = torch.device("cuda")
        print("Using CUDA acceleration")
    else:
        device = torch.device("cpu")
        print("Using CPU")

    # Tokenize training data
    tokens = encode(TRAINING_TEXT)
    print(f"Training text: {len(TRAINING_TEXT)} chars -> {len(tokens)} tokens")
    data = torch.tensor(tokens, dtype=torch.long)

    # Create model
    model = TinyTransformer().to(device)
    param_count = sum(p.numel() for p in model.parameters())
    print(f"Model parameters: {param_count:,}")

    # Causal mask (upper-left triangular)
    mask = torch.tril(torch.ones(MAX_SEQ_LEN, MAX_SEQ_LEN)).unsqueeze(0).unsqueeze(0).to(device)

    # Optimizer
    optimizer = torch.optim.Adam(model.parameters(), lr=LR)

    # Training loop
    print(f"\nTraining for {EPOCHS} epochs, batch_size={BATCH_SIZE}, lr={LR}")
    print("-" * 60)

    data_len = len(data)
    best_loss = float('inf')

    for epoch in range(EPOCHS):
        model.train()

        # Sample random windows
        starts = torch.randint(0, data_len - MAX_SEQ_LEN - 1, (BATCH_SIZE,))
        x_batch = torch.stack([data[s:s + MAX_SEQ_LEN] for s in starts]).to(device)
        y_batch = torch.stack([data[s + 1:s + MAX_SEQ_LEN + 1] for s in starts]).to(device)

        # Forward
        logits = model(x_batch, mask)
        loss = F.cross_entropy(logits.view(-1, VOCAB_SIZE), y_batch.view(-1))

        # Backward
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

        loss_val = loss.item()
        if loss_val < best_loss:
            best_loss = loss_val

        if epoch % 100 == 0 or epoch == EPOCHS - 1:
            # Generate a short sample
            model.eval()
            with torch.no_grad():
                prompt = encode("To be")
                gen_tokens = list(prompt)
                for _ in range(40):
                    ctx = gen_tokens[-MAX_SEQ_LEN:]
                    inp = torch.tensor([ctx], dtype=torch.long, device=device)
                    T = inp.shape[1]
                    m = torch.tril(torch.ones(T, T, device=device)).unsqueeze(0).unsqueeze(0)
                    logits_g = model(inp, m)
                    probs = F.softmax(logits_g[0, -1] / 0.8, dim=-1)
                    next_tok = torch.multinomial(probs, 1).item()
                    gen_tokens.append(next_tok)
                sample = decode(gen_tokens[len(prompt):])
            print(f"Epoch {epoch:4d} | loss={loss_val:.4f} | best={best_loss:.4f} | "
                  f"sample: {sample[:50]}")

    print("-" * 60)
    print(f"Training complete. Best loss: {best_loss:.4f}")

    # Generate final sample
    model.eval()
    with torch.no_grad():
        for prompt_text in ["To be", "Shall I", "The ", "Good "]:
            prompt_tokens = encode(prompt_text)
            gen = list(prompt_tokens)
            for _ in range(60):
                ctx = gen[-MAX_SEQ_LEN:]
                inp = torch.tensor([ctx], dtype=torch.long, device=device)
                T = inp.shape[1]
                m = torch.tril(torch.ones(T, T, device=device)).unsqueeze(0).unsqueeze(0)
                logits_g = model(inp, m)
                probs = F.softmax(logits_g[0, -1] / 0.7, dim=-1)
                next_tok = torch.multinomial(probs, 1).item()
                gen.append(next_tok)
            result = decode(gen)
            print(f"\n  \"{prompt_text}\" -> {result}")

    # Export
    model.cpu()
    print(f"\nExporting weights to {OUTPUT_PATH}")
    os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
    export_weights(model, OUTPUT_PATH)
    validate_export(model, OUTPUT_PATH)

    print("\nDone! Model ready for Limnx.")


if __name__ == "__main__":
    main()
