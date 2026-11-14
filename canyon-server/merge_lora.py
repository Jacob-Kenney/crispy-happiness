"""Merge the trained LoRA policy into the LFM2.5-230M base, for GGUF export.

The PPO checkpoint stores the frozen-base + LoRA state dict (plus the tiny
value head and action prior).  This script rebuilds the PEFT model, loads the
LoRA tensors, merges them into the base, and writes a plain HF checkpoint that
`llama.cpp/convert_hf_to_gguf.py` can consume.

    python canyon-server/merge_lora.py --ckpt /tmp/main11_now.pt \
        --out canyon-server/models/merged
"""

import argparse
import os

import torch
from peft import LoraConfig, get_peft_model
from transformers import AutoModelForCausalLM, AutoTokenizer

MODEL = "LiquidAI/LFM2.5-230M"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--out", default="canyon-server/models/merged")
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--lora-rank", type=int, default=8)
    ap.add_argument("--lora-alpha", type=int, default=16)
    args = ap.parse_args()

    ckpt = torch.load(args.ckpt, map_location="cpu", weights_only=False)
    print("checkpoint meta:", ckpt.get("meta"))

    tokenizer = AutoTokenizer.from_pretrained(args.model)
    base = AutoModelForCausalLM.from_pretrained(args.model, dtype=torch.float32)
    cfg = LoraConfig(
        r=args.lora_rank,
        lora_alpha=args.lora_alpha,
        lora_dropout=0.0,
        target_modules="all-linear",
        bias="none",
        task_type="CAUSAL_LM",
    )
    model = get_peft_model(base, cfg)

    state = ckpt["state_dict"]
    lora = {
        k[len("lm."):]: v
        for k, v in state.items()
        if k.startswith("lm.") and "lora_" in k
    }
    missing, unexpected = model.load_state_dict(lora, strict=False)
    print(f"loaded {len(lora)} LoRA tensors; missing={len(missing)} unexpected={len(unexpected)}")
    if unexpected:
        raise SystemExit(f"unexpected LoRA keys: {unexpected[:5]}")

    merged = model.merge_and_unload()
    os.makedirs(args.out, exist_ok=True)
    merged.save_pretrained(args.out, safe_serialization=True)
    tokenizer.save_pretrained(args.out)
    print("wrote", args.out)


if __name__ == "__main__":
    main()