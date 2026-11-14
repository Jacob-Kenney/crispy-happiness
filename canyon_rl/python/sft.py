"""Supervised warm-start of the LFM policy from the rule-based expert.

The expert is not the final agent: PPO refines the policy afterwards.  The
warm start gives PPO a state-sensitive starting point so that it does not
have to discover "move away from the near wall" from scratch.

Usage:
    python sft.py collect --episodes 30 --out sft_data.pt
    python sft.py train --data sft_data.pt --out sft.pt --epochs 2 --lr 1e-4
"""

import argparse
import random
import time
from typing import List, Tuple

import torch
import torch.nn.functional as F

from agent import (
    ACTION_NAMES,
    ActionSpace,
    Policy,
    PromptBuilder,
    build_lm,
    save_policy,
    SYSTEM_PROMPT,
)
from canyon_env import CanyonEnv, obs_text
from expert import expert_action


def collect(episodes: int, seed0: int = 10000, max_steps: int = 1200) -> Tuple[List[str], List[str], List[int]]:
    obs_list, last_list, act_list = [], [], []
    env = CanyonEnv(lives=1, fast_crash=True, action_repeat=4)
    for ep in range(episodes):
        env.reset(seed0 + ep)
        jump_hold = 0
        last = "wait"
        for _ in range(max_steps):
            o = env.obs
            a, jump_hold = expert_action(o, jump_hold)
            obs_list.append(obs_text(o))
            last_list.append(last)
            act_list.append(a)
            last = ACTION_NAMES[a]
            _, done, _ = env.step(a)
            if done:
                break
        print(f"episode {ep}: {len(act_list)} decisions total", flush=True)
    return obs_list, last_list, act_list


def train(args):
    from transformers import AutoTokenizer

    print(f"loading dataset {args.data}")
    blob = torch.load(args.data, weights_only=False)
    obs, lasts, acts = blob["obs"], blob["last"], blob["actions"]
    idx = list(range(len(acts)))
    random.Random(0).shuffle(idx)
    split = int(len(idx) * 0.95)
    train_idx, val_idx = idx[:split], idx[split:]
    print(f"{len(train_idx)} train / {len(val_idx)} val decisions")

    device = args.device
    tok = AutoTokenizer.from_pretrained(args.model)
    lm = build_lm(args.model, device, use_lora=True, rank=args.lora_rank)
    space = ActionSpace(tok, lm.config.vocab_size, device)
    policy = Policy(lm, lm.config.hidden_size, space).to(device)
    policy.train()
    builder = PromptBuilder(tok, device)

    params = [p for p in policy.parameters() if p.requires_grad]
    opt = torch.optim.AdamW(params, lr=args.lr, weight_decay=0.0, betas=(0.9, 0.95))
    scaler = torch.amp.GradScaler(enabled=device == "cuda")
    bs = args.batch

    def batches(indices, shuffle):
        order = list(indices)
        if shuffle:
            random.shuffle(order)
        for i in range(0, len(order), bs):
            yield order[i : i + bs]

    def evaluate():
        policy.eval()
        correct = n = 0
        with torch.no_grad():
            for chunk in batches(val_idx, False):
                ids, mask = builder.batch(
                    [obs[j] for j in chunk], [lasts[j] for j in chunk]
                )
                _, gl, _ = policy(ids, mask)
                pred = gl.argmax(-1).cpu()
                tgt = torch.tensor([acts[j] for j in chunk])
                correct += int((pred == tgt).sum())
                n += len(chunk)
        policy.train()
        return correct / max(n, 1)

    print(f"val accuracy before: {evaluate():.3f}")
    step = 0
    for epoch in range(args.epochs):
        if args.max_steps and step >= args.max_steps:
            break
        t0 = time.time()
        agg_loss = agg_correct = agg_n = 0
        for chunk in batches(train_idx, True):
            ids, mask = builder.batch(
                [obs[j] for j in chunk], [lasts[j] for j in chunk]
            )
            tgt = torch.tensor([acts[j] for j in chunk], device=device)
            with torch.autocast(
                device_type="cuda", dtype=torch.bfloat16, enabled=device == "cuda"
            ):
                logits, gl, _ = policy(ids, mask)
                if args.label_smooth:
                    logp = F.log_softmax(gl, -1)
                    target = torch.full_like(logp, args.label_smooth / gl.shape[-1])
                    target.scatter_(1, tgt.unsqueeze(1), 1.0 - args.label_smooth)
                    loss = -(target * logp).sum(-1).mean()
                else:
                    loss = F.cross_entropy(gl, tgt)
                if args.invalid_weight:
                    loss = loss + args.invalid_weight * space.invalid_mass(logits).mean()
                if args.ent_reg:
                    p = torch.softmax(gl, -1)
                    ent = -(p * torch.log(p + 1e-9)).sum(-1).mean()
                    loss = loss - args.ent_reg * ent
            opt.zero_grad(set_to_none=True)
            scaler.scale(loss).backward()
            scaler.unscale_(opt)
            torch.nn.utils.clip_grad_norm_(params, 1.0)
            scaler.step(opt)
            scaler.update()
            step += 1
            agg_loss += float(loss.detach())
            agg_correct += int((gl.argmax(-1) == tgt).sum())
            agg_n += len(chunk)
            if args.max_steps and step >= args.max_steps:
                break
            if step % 50 == 0:
                print(
                    f"step {step} loss {agg_loss/50:.3f} acc {agg_correct/agg_n:.3f} "
                    f"({time.time()-t0:.0f}s)"
                )
                agg_loss = agg_correct = agg_n = 0
        print(f"epoch {epoch}: val accuracy {evaluate():.3f}")

    cfg = {
        "use_lora": True,
        "lora_rank": args.lora_rank,
        "action_prior": tuple(policy.action_prior.detach().cpu().tolist()),
        "sft": True,
    }
    save_policy(policy, args.out, cfg, args.model)
    print(f"saved {args.out}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect")
    c.add_argument("--episodes", type=int, default=30)
    c.add_argument("--out", default="sft_data.pt")
    c.add_argument("--max-steps", type=int, default=1200)
    t = sub.add_parser("train")
    t.add_argument("--data", default="sft_data.pt")
    t.add_argument("--out", default="sft.pt")
    t.add_argument("--model", default="LiquidAI/LFM2.5-230M")
    t.add_argument("--device", default="mps")
    t.add_argument("--epochs", type=int, default=2)
    t.add_argument("--batch", type=int, default=16)
    t.add_argument("--lr", type=float, default=1e-4)
    t.add_argument("--lora-rank", type=int, default=8)
    t.add_argument("--max-steps", type=int, default=0)
    t.add_argument("--invalid-weight", type=float, default=0.0)
    t.add_argument("--ent-reg", type=float, default=0.0)
    t.add_argument("--label-smooth", type=float, default=0.0)
    args = ap.parse_args()
    if args.cmd == "collect":
        obs, lasts, acts = collect(args.episodes, max_steps=args.max_steps)
        torch.save({"obs": obs, "last": lasts, "actions": acts}, args.out)
        print(f"saved {args.out} ({len(acts)} decisions)")
    else:
        train(args)


if __name__ == "__main__":
    main()