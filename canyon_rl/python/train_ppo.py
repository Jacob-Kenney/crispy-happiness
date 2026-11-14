"""PPO fine-tuning of LFM2.5-230M (LoRA) to play the Canyon Racer C++ port.

Each decision the model receives the game state as text (system prompt + one
user message) and outputs one action token.  The token distribution is grouped
into the seven actions (left/right/up/down/a/b/wait); probability on any other
token is invalid and is penalized.

Run from this directory:
    python train_ppo.py --smoke
    python train_ppo.py --iters 40 --eval-every 5 --save
"""

import argparse
import csv
import json
import random
import time
from contextlib import nullcontext
from dataclasses import dataclass
from pathlib import Path
from typing import List

import numpy as np
import torch
import torch.nn.functional as F
from transformers import AutoTokenizer

from agent import (
    ACTION_NAMES,
    ActionSpace,
    Policy,
    PromptBuilder,
    SYSTEM_PROMPT,
    build_lm,
    load_policy_state,
    save_policy,
)
from canyon_env import CanyonEnv, obs_text

MODEL_NAME = "LiquidAI/LFM2.5-230M"
ROOT = Path(__file__).resolve().parent


@dataclass
class Config:
    iters: int = 40
    envs: int = 8
    horizon: int = 64
    epochs: int = 1
    minibatch: int = 8
    lm_lr: float = 2e-6
    head_lr: float = 1e-4
    lora_rank: int = 8
    use_lora: bool = True
    gamma: float = 0.995
    lam: float = 0.95
    clip: float = 0.2
    vf_coef: float = 0.5
    ent_coef: float = 0.02
    epsilon: float = 0.2       # uniform mixture for exploration
    target_kl: float = 0.03     # stop an epoch early when policy drift exceeds this
    invalid_coef: float = 0.05      # differentiable penalty on invalid token mass
    invalid_reward: float = 0.2     # reward penalty on invalid token mass
    max_grad_norm: float = 1.0
    action_repeat: int = 4
    lives: int = 1
    eval_every: int = 5
    save_every: int = 0
    eval_episodes: int = 8
    seed: int = 0
    device: str = "mps"
    bf16: bool = False
    action_prior: tuple = (0.0, -4.5, 0.0, 0.0, 0.0, 0.0, 0.0)
    out_dir: str = str(ROOT / "runs" / "ppo")


def log(msg: str):
    print(msg, flush=True)


class VecGame:
    """N independent game instances stepped in lockstep."""

    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.envs = [
            CanyonEnv(lives=cfg.lives, fast_crash=True,
                      action_repeat=cfg.action_repeat)
            for _ in range(cfg.envs)
        ]
        self.rng = random.Random(cfg.seed + 1)
        self.obs_texts = [""] * cfg.envs
        self.last_actions = ["wait"] * cfg.envs
        self.frames_prev = [0] * cfg.envs
        self.ep_max_score = [0] * cfg.envs
        self.ep_cleared = [0] * cfg.envs
        for i, e in enumerate(self.envs):
            e.reset(self._next_seed())
            self.obs_texts[i] = obs_text(e.obs)
            self.frames_prev[i] = e.obs.frames

    def _next_seed(self):
        return self.rng.randrange(1, 65535)

    def step(self, actions: List[int]):
        rewards, dones, infos = [], [], []
        for i, (env, a) in enumerate(zip(self.envs, actions)):
            o, done, ev = env.step(a)
            r = (
                1.0 * ev["cleared"]
                + 5.0 * ev["items"]
                - 1.0 * ev["crashed"]
                + 0.002 * (o.frames - self.frames_prev[i])
            )
            self.frames_prev[i] = o.frames
            self.ep_cleared[i] += ev["cleared"]
            self.ep_max_score[i] = max(self.ep_max_score[i], o.score)

            info = dict(ev)
            if done:
                info["final"] = {
                    "score": o.score,
                    "max_score": self.ep_max_score[i],
                    "frames": o.frames,
                    "level": o.level,
                    "cleared": self.ep_cleared[i],
                }
                env.reset(self._next_seed())
                o = env.obs
                self.frames_prev[i] = 0
                self.ep_max_score[i] = 0
                self.ep_cleared[i] = 0
            self.obs_texts[i] = obs_text(o)
            rewards.append(r)
            dones.append(done)
            infos.append(info)
        return rewards, dones, infos

    def set_last_actions(self, actions: List[int], dones=None):
        self.last_actions = [ACTION_NAMES[a] for a in actions]
        if dones is not None:
            # A reset/rewind starts with no button held.
            for i, d in enumerate(dones):
                if d:
                    self.last_actions[i] = "wait"

    def close(self):
        for e in self.envs:
            e.close()


def gae(rewards, values, dones, last_value, gamma, lam):
    n = len(rewards)
    adv = np.zeros(n, dtype=np.float32)
    last = 0.0
    for t in reversed(range(n)):
        next_v = last_value if t == n - 1 else values[t + 1]
        nonterminal = 0.0 if dones[t] else 1.0
        delta = rewards[t] + gamma * next_v * nonterminal - values[t]
        last = delta + gamma * lam * nonterminal * last
        adv[t] = last
    return adv, adv + np.asarray(values, dtype=np.float32)


def autocast_ctx(device, enabled):
    if enabled and device.type == "cuda":
        return torch.autocast("cuda", dtype=torch.bfloat16)
    return nullcontext()


@torch.no_grad()
def evaluate(policy, tokenizer, cfg, seeds, max_decisions=900, verbose=False):
    """Greedy evaluation of several seeds in parallel (one batched forward)."""
    builder = PromptBuilder(tokenizer, cfg.device)
    envs = [
        CanyonEnv(lives=cfg.lives, fast_crash=True,
                  action_repeat=cfg.action_repeat)
        for _ in seeds
    ]
    for env, seed in zip(envs, seeds):
        env.reset(seed)

    active = list(range(len(envs)))
    lasts = ["wait"] * len(envs)
    decisions = [0] * len(envs)
    max_score = [0] * len(envs)
    cleared = [0] * len(envs)
    counts = [{n: 0 for n in ACTION_NAMES} for _ in seeds]
    results = [None] * len(seeds)

    policy.eval()
    while active:
        texts = [obs_text(envs[i].obs) for i in active]
        ids, mask = builder.batch(texts, [lasts[i] for i in active])
        with autocast_ctx(torch.device(cfg.device), cfg.bf16):
            _, gl, _ = policy(ids, mask)
        acts = torch.argmax(gl.float(), dim=-1).tolist()

        still = []
        for j, i in enumerate(active):
            a = int(acts[j])
            o, done, ev = envs[i].step(a)
            decisions[i] += 1
            max_score[i] = max(max_score[i], o.score)
            cleared[i] += ev["cleared"]
            counts[i][ACTION_NAMES[a]] += 1
            lasts[i] = ACTION_NAMES[a]
            if done or decisions[i] >= max_decisions:
                results[i] = {
                    "seed": seeds[i],
                    "score": o.score,
                    "max_score": max_score[i],
                    "cleared": cleared[i],
                    "frames": o.frames,
                    "level": o.level,
                    "decisions": decisions[i],
                }
                envs[i].close()
            else:
                still.append(i)
        active = still

    for r, c in zip(results, counts):
        if verbose:
            log(
                f"  eval seed={r['seed']} max_score={r['max_score']} "
                f"cleared={r['cleared']} frames={r['frames']} "
                f"level={r['level']} actions={c}"
            )
    policy.train()
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--iters", type=int)
    ap.add_argument("--envs", type=int)
    ap.add_argument("--horizon", type=int)
    ap.add_argument("--epochs", type=int)
    ap.add_argument("--minibatch", type=int)
    ap.add_argument("--lm-lr", type=float)
    ap.add_argument("--head-lr", type=float)
    ap.add_argument("--lora-rank", type=int)
    ap.add_argument("--no-lora", action="store_true")
    ap.add_argument("--action-repeat", type=int)
    ap.add_argument("--lives", type=int)
    ap.add_argument("--epsilon", type=float)
    ap.add_argument("--ent-coef", type=float, dest="ent_coef")
    ap.add_argument("--invalid-reward", type=float, dest="invalid_reward")
    ap.add_argument("--target-kl", type=float)
    ap.add_argument("--eval-every", type=int)
    ap.add_argument("--eval-episodes", type=int)
    ap.add_argument("--save-every", type=int)
    ap.add_argument("--device")
    ap.add_argument("--bf16", action="store_true")
    ap.add_argument("--out-dir")
    ap.add_argument("--seed", type=int)
    ap.add_argument("--model", default=MODEL_NAME)
    ap.add_argument("--save", action="store_true")
    ap.add_argument("--resume", default=None)
    args = ap.parse_args()

    cfg = Config()
    if args.smoke:
        cfg.iters = 2
        cfg.envs = 4
        cfg.horizon = 16
        cfg.minibatch = 4
        cfg.eval_every = 1
        cfg.eval_episodes = 2
    for name in ["iters", "envs", "horizon", "epochs", "minibatch",
                 "lora_rank", "lives", "eval_every", "eval_episodes",
                 "save_every", "seed"]:
        v = getattr(args, name)
        if v is not None:
            setattr(cfg, name, v)
    if args.epsilon is not None:
        cfg.epsilon = args.epsilon
    if args.target_kl is not None:
        cfg.target_kl = args.target_kl
    for name in ["ent_coef", "invalid_reward"]:
        v = getattr(args, name)
        if v is not None:
            setattr(cfg, name, v)
    for name in ["lm_lr", "head_lr"]:
        v = getattr(args, name)
        if v is not None:
            setattr(cfg, name, v)
    if args.action_repeat is not None:
        cfg.action_repeat = args.action_repeat
    if args.no_lora:
        cfg.use_lora = False
    if args.device:
        cfg.device = args.device
    if args.bf16:
        cfg.bf16 = True
    if args.out_dir:
        cfg.out_dir = args.out_dir
    if cfg.device == "mps" and not torch.backends.mps.is_available():
        cfg.device = "cpu"

    torch.manual_seed(cfg.seed)
    np.random.seed(cfg.seed)
    random.seed(cfg.seed)
    device = torch.device(cfg.device)

    out_dir = Path(cfg.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    log(f"device={cfg.device} out={out_dir}")
    log(json.dumps(cfg.__dict__, indent=2))

    log(f"loading {args.model}")
    lm = build_lm(args.model, cfg.device, use_lora=cfg.use_lora,
                  rank=cfg.lora_rank)
    tokenizer = AutoTokenizer.from_pretrained(args.model)
    space = ActionSpace(tokenizer, lm.config.vocab_size, device)
    policy = Policy(lm, lm.config.hidden_size, space,
                    prior=cfg.action_prior).to(device)

    if cfg.use_lora:
        lm_params = [p for p in policy.lm.parameters() if p.requires_grad]
    else:
        lm_params = list(policy.lm.parameters())
    head_params = list(policy.value_head.parameters()) + [policy.action_prior]
    opt = torch.optim.AdamW(
        [
            {"params": lm_params, "lr": cfg.lm_lr},
            {"params": head_params, "lr": cfg.head_lr},
        ],
        betas=(0.9, 0.95),
        weight_decay=0.0,
    )
    log(
        f"trainable: LM {sum(p.numel() for p in lm_params)/1e6:.2f}M, "
        f"heads {sum(p.numel() for p in head_params)/1e6:.3f}M, "
        f"action tokens {len(space.valid_ids)}"
    )

    start_iter = 1
    total_steps = 0
    if args.resume:
        ckpt = load_policy_state(policy, args.resume)
        meta = ckpt.get("meta", {})
        start_iter = int(meta.get("iter", 0)) + 1
        total_steps = int(meta.get("steps", 0))
        log(f"resumed {args.resume}: iter {start_iter}, steps {total_steps}")

    ac = lambda: autocast_ctx(device, cfg.bf16)
    builder = PromptBuilder(tokenizer, device)
    games = VecGame(cfg)
    n = cfg.envs

    metrics_path = out_dir / "metrics.csv"
    if not metrics_path.exists():
        with open(metrics_path, "w", newline="") as f:
            csv.writer(f).writerow(
                ["iter", "steps", "mean_reward", "mean_max_score", "max_score",
                 "mean_frames", "invalid_mass", "entropy", "pg_loss", "v_loss",
                 "approx_kl", "clipfrac", "wall_s"]
            )

    for it in range(start_iter, cfg.iters + 1):
        t0 = time.time()

        # ---- rollout ----
        if cfg.device == "cuda":
            torch.cuda.synchronize()
        t_roll_start = time.time()
        all_seqs: List[List[int]] = []
        all_actions, all_logp, all_values = [], [], []
        all_rewards, all_dones = [], []
        finished = []
        ep_return = np.zeros(n, dtype=np.float32)
        invalid_sum = 0.0
        for _ in range(cfg.horizon):
            ids, mask = builder.batch(games.obs_texts, games.last_actions)
            with torch.no_grad(), ac():
                logits, gl, values = policy(ids, mask)
                logits, values = logits.float(), values.float()
                probs = torch.softmax(gl.float(), -1)
                probs = (1 - cfg.epsilon) * probs + cfg.epsilon / space.n_actions
                dist = torch.distributions.Categorical(probs=probs)
                actions = dist.sample()
                logp = dist.log_prob(actions)
                invalid_mass = space.invalid_mass(logits)

            acts = actions.cpu().numpy().astype(np.int64)
            rewards, dones, infos = games.step([int(a) for a in acts])
            rewards = np.asarray(rewards, dtype=np.float32)
            inv = invalid_mass.cpu().numpy()
            rewards -= cfg.invalid_reward * inv
            invalid_sum += float(inv.mean())

            ep_return += rewards
            for i, d in enumerate(dones):
                if d:
                    finished.append((float(ep_return[i]), infos[i]["final"]))
                    ep_return[i] = 0.0
            games.set_last_actions([int(a) for a in acts], dones)

            for row_ids, row_mask in zip(ids.cpu(), mask.cpu()):
                all_seqs.append(row_ids[row_mask.bool()].tolist())
            all_actions.append(actions.cpu())
            all_logp.append(logp.cpu())
            all_values.append(values.cpu())
            all_rewards.append(rewards)
            all_dones.append(np.asarray(dones, dtype=np.float32))
            total_steps += n

        with torch.no_grad(), ac():
            ids, mask = builder.batch(games.obs_texts, games.last_actions)
            _, _, last_values = policy(ids, mask)
            last_values = last_values.float().cpu().numpy()
        if cfg.device == "cuda":
            torch.cuda.synchronize()
        t_roll = time.time() - t_roll_start
        t_upd_start = time.time()

        T = cfg.horizon
        r_t = np.stack(all_rewards)
        v_t = torch.stack(all_values).numpy()
        d_t = np.stack(all_dones)
        adv = np.zeros_like(r_t)
        ret = np.zeros_like(r_t)
        for i in range(n):
            adv[:, i], ret[:, i] = gae(
                r_t[:, i], v_t[:, i], d_t[:, i], last_values[i],
                cfg.gamma, cfg.lam,
            )

        pad_id = builder.pad_id
        maxlen = max(len(s) for s in all_seqs)
        obs_ids = torch.full((len(all_seqs), maxlen), pad_id, dtype=torch.long)
        obs_mask = torch.zeros((len(all_seqs), maxlen), dtype=torch.long)
        for i, seq in enumerate(all_seqs):
            obs_ids[i, -len(seq):] = torch.tensor(seq, dtype=torch.long)
            obs_mask[i, -len(seq):] = 1
        obs_actions = torch.cat(all_actions, 0)
        obs_logp = torch.cat(all_logp, 0)
        obs_adv = torch.tensor(adv.reshape(-1), dtype=torch.float32)
        obs_ret = torch.tensor(ret.reshape(-1), dtype=torch.float32)
        obs_adv = (obs_adv - obs_adv.mean()) / (obs_adv.std() + 1e-8)

        B = obs_ids.shape[0]
        idx = np.arange(B)
        logs = []
        n_updates = 0
        for _epoch in range(cfg.epochs):
            np.random.shuffle(idx)
            for start in range(0, B, cfg.minibatch):
                mb = idx[start:start + cfg.minibatch]
                logits, gl, values = policy(
                    obs_ids[mb].to(device), obs_mask[mb].to(device)
                )
                probs_mix = torch.softmax(gl.float(), -1)
                probs_mix = (1 - cfg.epsilon) * probs_mix + cfg.epsilon / space.n_actions
                logp = torch.log(
                    probs_mix.gather(1, obs_actions[mb].to(device).unsqueeze(1)).squeeze(1)
                    .clamp_min(1e-12)
                )
                old_logp = obs_logp[mb].to(device)
                adv_mb = obs_adv[mb].to(device)
                ret_mb = obs_ret[mb].to(device)

                ratio = torch.exp(logp - old_logp)
                pg_loss = -torch.min(
                    ratio * adv_mb,
                    torch.clamp(ratio, 1 - cfg.clip, 1 + cfg.clip) * adv_mb,
                ).mean()
                v_loss = 0.5 * ((values - ret_mb) ** 2).mean()

                probs_core = torch.softmax(gl.float(), -1).clamp_min(1e-12)
                ent = -(probs_core * probs_core.log()).sum(-1).mean()
                invalid = space.invalid_mass(logits).mean()

                loss = (
                    pg_loss
                    + cfg.vf_coef * v_loss
                    - cfg.ent_coef * ent
                    + cfg.invalid_coef * invalid
                )
                opt.zero_grad(set_to_none=True)
                loss.backward()
                if not torch.isfinite(loss):
                    log("  non-finite loss; skipping minibatch")
                    opt.zero_grad(set_to_none=True)
                    continue
                torch.nn.utils.clip_grad_norm_(
                    [p for p in policy.parameters() if p.requires_grad],
                    cfg.max_grad_norm,
                )
                opt.step()

                with torch.no_grad():
                    approx_kl = ((ratio - 1) - (logp - old_logp)).mean().item()
                    clipfrac = ((ratio - 1).abs() > cfg.clip).float().mean().item()
                logs.append([pg_loss.item(), v_loss.item(), ent.item(),
                             approx_kl, clipfrac, invalid.item()])
                n_updates += 1
                if cfg.target_kl and approx_kl > cfg.target_kl:
                    break
        if cfg.device == "cuda":
            torch.cuda.synchronize()
        t_upd = time.time() - t_upd_start
        logs = np.asarray(logs).mean(0)

        if finished:
            mean_reward = float(np.mean([r for r, _ in finished]))
            mean_max_score = float(np.mean([f["max_score"] for _, f in finished]))
            max_score = int(max(f["max_score"] for _, f in finished))
            mean_frames = float(np.mean([f["frames"] for _, f in finished]))
        else:
            mean_reward = 0.0
            mean_max_score = float(np.mean(games.ep_max_score))
            max_score = int(max(games.ep_max_score))
            mean_frames = float(np.mean([e.obs.frames for e in games.envs]))

        wall = time.time() - t0
        if cfg.device == "cuda":
            torch.cuda.synchronize()
        log(
            f"iter {it:3d} steps {total_steps:6d} | R {mean_reward:7.2f} "
            f"maxscore {mean_max_score:6.1f}/{max_score:5d} "
            f"frames {mean_frames:6.1f} | inv {invalid_sum/T:5.4f} "
            f"ent {logs[2]:.3f} pg {logs[0]:+.3f} v {logs[1]:.2f} "
            f"kl {logs[3]:.4f} cf {logs[4]:.2f} | {wall:5.1f}s "
            f"(roll {t_roll:4.1f}s upd {t_upd:4.1f}s, {n_updates} upd)"
        )
        with open(metrics_path, "a", newline="") as f:
            csv.writer(f).writerow([
                it, total_steps, mean_reward, mean_max_score, max_score,
                mean_frames, invalid_sum / T, logs[2], logs[0], logs[1],
                logs[3], logs[4], wall,
            ])

        if cfg.save_every and it % cfg.save_every == 0:
            path = out_dir / "policy_latest.pt"
            save_policy(
                policy, str(path), cfg.__dict__, args.model,
                meta={"iter": it, "steps": total_steps},
            )
            log(f"  saved {path}")

        if cfg.eval_every and it % cfg.eval_every == 0:
            log("evaluating (greedy over the seven actions)")
            seeds = [1000 + 137 * k for k in range(cfg.eval_episodes)]
            res = evaluate(policy, tokenizer, cfg, seeds, verbose=True)
            log(
                f"  eval mean max_score "
                f"{np.mean([r['max_score'] for r in res]):.1f} "
                f"max {max(r['max_score'] for r in res)} "
                f"mean cleared {np.mean([r['cleared'] for r in res]):.1f} "
                f"mean frames {np.mean([r['frames'] for r in res]):.0f}"
            )

    if args.save or args.smoke:
        path = out_dir / "policy.pt"
        save_policy(
            policy, str(path), cfg.__dict__, args.model,
            meta={"iter": cfg.iters, "steps": total_steps},
        )
        log(f"saved {path}")

    games.close()
    log("done")


if __name__ == "__main__":
    main()