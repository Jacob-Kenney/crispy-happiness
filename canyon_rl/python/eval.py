"""Evaluate a saved Canyon Racer policy (greedy over the seven actions)."""

import argparse
import json
from dataclasses import fields

import numpy as np
import torch

from agent import load_policy
from train_ppo import Config, evaluate, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("checkpoint")
    ap.add_argument("--episodes", type=int, default=8)
    ap.add_argument("--seed-start", type=int, default=1000)
    ap.add_argument("--device", default="mps")
    ap.add_argument("--max-decisions", type=int, default=900)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if args.device == "mps" and not torch.backends.mps.is_available():
        args.device = "cpu"

    policy, tokenizer, raw_cfg = load_policy(args.checkpoint, args.device)
    cfg = Config()
    for f in fields(Config):
        if f.name in raw_cfg:
            setattr(cfg, f.name, raw_cfg[f.name])
    cfg.device = args.device

    seeds = list(range(args.seed_start, args.seed_start + args.episodes))
    res = evaluate(
        policy, tokenizer, cfg, seeds,
        max_decisions=args.max_decisions, verbose=args.verbose,
    )
    print(json.dumps(res, indent=2))
    log(
        f"mean max_score {np.mean([r['max_score'] for r in res]):.1f}  "
        f"mean cleared {np.mean([r['cleared'] for r in res]):.1f}  "
        f"mean frames {np.mean([r['frames'] for r in res]):.0f}"
    )


if __name__ == "__main__":
    main()
