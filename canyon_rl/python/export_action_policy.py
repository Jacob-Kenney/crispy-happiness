"""Export PPO action grouping and learned biases alongside a merged GGUF model."""
import argparse
import hashlib
import json
from pathlib import Path

import torch
from transformers import AutoTokenizer

from canyon_env import ACTION_ALIASES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("--model", required=True, help="ID used to select this model's decoder")
    parser.add_argument("--source", default="", help="Provenance, e.g. Modal volume and path")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    tokenizer = AutoTokenizer.from_pretrained(checkpoint["base_model"])
    groups = {}
    for token in range(len(tokenizer)):
        word = tokenizer.decode([token]).strip().lower()
        if word in ACTION_ALIASES:
            groups[str(token)] = ACTION_ALIASES[word]
    spec = {
        "model": args.model, "base_model": checkpoint["base_model"],
        "checkpoint_meta": checkpoint.get("meta", {}),
        "checkpoint_source": args.source,
        "action_prior": checkpoint["state_dict"]["action_prior"].tolist(),
        "token_to_action": groups,
        "checkpoint_sha256": hashlib.sha256(args.checkpoint.read_bytes()).hexdigest(),
    }
    args.out.write_text(json.dumps(spec, indent=2) + "\n")


if __name__ == "__main__":
    main()
