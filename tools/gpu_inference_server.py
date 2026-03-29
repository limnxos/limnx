#!/usr/bin/env python3
"""
GPU Inference Server for Limnx

TCP server that receives inference requests from inferd_proxy running
inside Limnx, runs them through a model on the host GPU, and returns
the response.

Protocol:
  Client sends: [uint32_t prompt_len][prompt bytes]
  Server sends: [uint32_t resp_len][response bytes]

Usage:
  # With llama-cpp-python (recommended):
  pip install llama-cpp-python
  python gpu_inference_server.py --port 9200 --model path/to/model.gguf

  # With transformers + PyTorch:
  python gpu_inference_server.py --port 9200 --backend hf --model meta-llama/Llama-3-8B

  # Echo mode (for testing, no model needed):
  python gpu_inference_server.py --port 9200 --backend echo
"""

import argparse
import socket
import struct
import sys
import time


def load_llama_cpp(model_path):
    """Load model with llama-cpp-python."""
    from llama_cpp import Llama
    print(f"[gpu-server] Loading model with llama.cpp: {model_path}")
    llm = Llama(model_path=model_path, n_ctx=2048, n_gpu_layers=-1)
    print(f"[gpu-server] Model loaded on GPU")

    def generate(prompt):
        output = llm(prompt, max_tokens=256, temperature=0.8, top_k=40)
        return output["choices"][0]["text"]

    return generate


def load_hf(model_name):
    """Load model with HuggingFace transformers."""
    import torch
    from transformers import AutoTokenizer, AutoModelForCausalLM

    print(f"[gpu-server] Loading HF model: {model_name}")
    tokenizer = AutoTokenizer.from_pretrained(model_name)
    model = AutoModelForCausalLM.from_pretrained(
        model_name, torch_dtype=torch.float16, device_map="auto"
    )
    print(f"[gpu-server] Model loaded on {model.device}")

    def generate(prompt):
        inputs = tokenizer(prompt, return_tensors="pt").to(model.device)
        with torch.no_grad():
            outputs = model.generate(**inputs, max_new_tokens=256,
                                     temperature=0.8, top_k=40,
                                     do_sample=True)
        return tokenizer.decode(outputs[0][inputs.input_ids.shape[1]:],
                                skip_special_tokens=True)

    return generate


def echo_backend():
    """Echo backend for testing (no GPU needed)."""
    print("[gpu-server] Using echo backend (no model)")

    def generate(prompt):
        return f"[echo] You said: {prompt[:200]}"

    return generate


def handle_client(conn, generate_fn, request_num):
    """Handle one client request."""
    try:
        # Read prompt length (uint32_t, little-endian)
        header = b""
        while len(header) < 4:
            chunk = conn.recv(4 - len(header))
            if not chunk:
                return
            header += chunk

        prompt_len = struct.unpack("<I", header)[0]
        if prompt_len == 0 or prompt_len > 65536:
            return

        # Read prompt
        prompt = b""
        while len(prompt) < prompt_len:
            chunk = conn.recv(prompt_len - len(prompt))
            if not chunk:
                return
            prompt += chunk

        prompt_text = prompt.decode("utf-8", errors="replace")
        print(f"[gpu-server] Request #{request_num}: {len(prompt_text)} chars")

        # Generate response
        t0 = time.time()
        response_text = generate_fn(prompt_text)
        elapsed = time.time() - t0
        print(f"[gpu-server] Response: {len(response_text)} chars ({elapsed:.2f}s)")

        # Send response
        response_bytes = response_text.encode("utf-8")
        resp_len = struct.pack("<I", len(response_bytes))
        conn.sendall(resp_len + response_bytes)

    except Exception as e:
        print(f"[gpu-server] Error: {e}")
        try:
            conn.sendall(struct.pack("<I", 0))
        except:
            pass


def main():
    parser = argparse.ArgumentParser(description="GPU Inference Server for Limnx")
    parser.add_argument("--port", type=int, default=9200,
                        help="TCP port to listen on (default: 9200)")
    parser.add_argument("--host", default="0.0.0.0",
                        help="Host to bind to (default: 0.0.0.0)")
    parser.add_argument("--model", default=None,
                        help="Model path (GGUF for llama.cpp, HF name for transformers)")
    parser.add_argument("--backend", default="llama",
                        choices=["llama", "hf", "echo"],
                        help="Backend: llama (llama-cpp-python), hf (transformers), echo (test)")
    args = parser.parse_args()

    # Load model
    if args.backend == "echo":
        generate_fn = echo_backend()
    elif args.backend == "llama":
        if not args.model:
            print("Error: --model required for llama backend")
            sys.exit(1)
        generate_fn = load_llama_cpp(args.model)
    elif args.backend == "hf":
        if not args.model:
            print("Error: --model required for hf backend")
            sys.exit(1)
        generate_fn = load_hf(args.model)

    # Start TCP server
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen(4)

    print(f"[gpu-server] Listening on {args.host}:{args.port}")
    print(f"[gpu-server] Backend: {args.backend}")
    print(f"[gpu-server] Ready for Limnx inferd_proxy connections")

    request_num = 0
    try:
        while True:
            conn, addr = server.accept()
            request_num += 1
            handle_client(conn, generate_fn, request_num)
            conn.close()
    except KeyboardInterrupt:
        print("\n[gpu-server] Shutting down")
    finally:
        server.close()


if __name__ == "__main__":
    main()
