"""A small state-vector PPO teacher, then distil it into the LM.

The rule-based expert is a weak teacher: it hand-codes wall margins and jump
timing and plateaus around 3.1k frames.  A tiny PPO policy on the structured
game state learns the same job much better and is cheap to train (CPU, no LM).
Its greedy actions are then recorded as supervised data for the language
model, and PPO fine-tunes the LM afterwards.

Usage:
    python ppo_teacher.py train --steps 2000000 --out teacher.pt
    python ppo_teacher.py eval --ckpt teacher.pt --seeds 1000-1015
    python ppo_teacher.py data --ckpt teacher.pt --episodes 60 --out teacher_data.pt
"""

import argparse
import random

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

from canyon_env import CanyonEnv, ACTION_NAMES, obs_text

ACTIONS = [0, 1, 2, 5]  # wait, left, right, jump
N_ACTIONS = 4
N_TYPES = 5
FEAT = 3 + 3 + 1 + 4 + 6 + 3 * 3 + 3 * 11 + 5


def features(o):
    """Fixed-size, roughly normalised game state."""
    f = np.zeros(FEAT, dtype=np.float32)
    i = 0
    f[i] = o.ship_x / 80.0
    f[i + 1] = o.ship_y / 72.0
    f[i + 2] = o.ship_z / 20.0
    i += 3
    if 0 <= o.state < 3:
        f[i + o.state] = 1.0
    i += 3
    f[i] = o.invincible
    i += 1
    gl = o.ship_x + 8 - o.canyon_left
    gr = o.canyon_right - (o.ship_x + 16)
    f[i] = o.canyon_left / 80.0
    f[i + 1] = o.canyon_right / 80.0
    f[i + 2] = o.dx_center / 80.0
    f[i + 3] = gl / 40.0
    i += 4
    f[i] = gr / 40.0
    f[i + 1] = o.wall_left / 80.0
    f[i + 2] = gl / 20.0
    f[i + 3] = gr / 20.0
    f[i + 4] = o.obstacle_speed / 1000.0
    f[i + 5] = o.level / 10.0
    i += 6
    for k in range(3):
        present = k < o.n_canyon
        if present:
            f[i] = 1.0
            f[i + 1] = (o.canyon_l[k] - o.ship_x) / 80.0
            f[i + 2] = (o.canyon_r[k] - o.ship_x) / 80.0
        i += 3
    for k in range(3):
        if k < o.n_ahead:
            t = int(o.ahead_type[k])
            f[i] = 1.0
            f[i + 1] = o.ahead_dy[k] / 72.0
            f[i + 2 + t] = 1.0
            f[i + 7] = (o.ahead_l[k] - o.ship_x) / 80.0
            f[i + 8] = (o.ahead_r[k] - o.ship_x) / 80.0
            f[i + 9] = o.ahead_dx_l[k] / 80.0
            f[i + 10] = o.ahead_dx_r[k] / 80.0
        i += 11
    f[i] = o.score / 1000.0
    f[i + 1] = o.frames / 5000.0
    f[i + 2] = o.n_ahead / 3.0
    f[i + 3] = o.n_canyon / 3.0
    f[i + 4] = 1.0
    return f


class Net(nn.Module):
    def __init__(self):
        super().__init__()
        self.body = nn.Sequential(
            nn.Linear(FEAT, 128), nn.Tanh(),
            nn.Linear(128, 128), nn.Tanh(),
        )
        self.pi = nn.Linear(128, N_ACTIONS)
        self.v = nn.Linear(128, 1)

    def forward(self, x):
        h = self.body(x)
        return self.pi(h), self.v(h).squeeze(-1)


class Vec:
    def __init__(self, n, seed=0, action_repeat=4):
        self.n = n
        self.envs = [CanyonEnv(lives=1, fast_crash=True, action_repeat=action_repeat)
                     for _ in range(n)]
        self.rng = random.Random(seed + 1)
        self.frames_prev = np.zeros(n, dtype=np.int64)
        self.ep_cleared = np.zeros(n, dtype=np.int64)
        self.ep_max_score = np.zeros(n, dtype=np.int64)
        for i, e in enumerate(self.envs):
            e.reset(self._seed())
            self.frames_prev[i] = e.obs.frames

    def _seed(self):
        return self.rng.randrange(1, 65535)

    def obs(self):
        return np.stack([features(e.obs) for e in self.envs])

    def step(self, actions):
        rewards = np.zeros(self.n, dtype=np.float32)
        dones = np.zeros(self.n, dtype=np.float32)
        infos = [None] * self.n
        for i, (e, a) in enumerate(zip(self.envs, actions)):
            o, done, ev = e.step(int(a))
            rewards[i] = (
                2.0 * ev["cleared"] + 5.0 * ev["items"] - 5.0 * ev["crashed"]
                + 0.001 * (o.frames - self.frames_prev[i])
            )
            self.frames_prev[i] = o.frames
            self.ep_cleared[i] += ev["cleared"]
            self.ep_max_score[i] = max(self.ep_max_score[i], o.score)
            if done:
                infos[i] = {
                    "frames": o.frames, "score": o.score,
                    "max_score": self.ep_max_score[i],
                    "level": o.level, "cleared": self.ep_cleared[i],
                }
                dones[i] = 1.0
                e.reset(self._seed())
                self.frames_prev[i] = 0
                self.ep_cleared[i] = 0
                self.ep_max_score[i] = 0
        return rewards, dones, infos


def train(args):
    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    random.seed(args.seed)
    vec = Vec(args.envs, args.seed, args.action_repeat)
    net = Net()
    opt = torch.optim.Adam(net.parameters(), lr=args.lr, eps=1e-5)
    obs = torch.from_numpy(vec.obs())
    T, n = args.horizon, args.envs
    gamma, lam = 0.99, 0.95
    total = 0
    for it in range(1, args.iters + 1):
        obuf, abuf, lbuf, rbuf, dbuf, vbuf = [], [], [], [], [], []
        for _ in range(T):
            with torch.no_grad():
                logits, v = net(obs)
                dist = torch.distributions.Categorical(logits=logits)
                a = dist.sample()
                lp = dist.log_prob(a)
            r, d, infos = vec.step([ACTIONS[int(x)] for x in a])
            obuf.append(obs)
            abuf.append(a)
            lbuf.append(lp)
            rbuf.append(torch.from_numpy(r))
            dbuf.append(torch.from_numpy(d))
            vbuf.append(v)
            obs = torch.from_numpy(vec.obs())
            total += n
        with torch.no_grad():
            _, last_v = net(obs)
        adv = torch.zeros(T, n)
        ret = torch.zeros(T, n)
        gae = torch.zeros(n)
        for t in reversed(range(T)):
            nonterm = 1.0 - dbuf[t]
            next_v = last_v if t == T - 1 else vbuf[t + 1]
            delta = rbuf[t] + gamma * next_v * nonterm - vbuf[t]
            gae = delta + gamma * lam * nonterm * gae
            adv[t] = gae
            ret[t] = gae + vbuf[t]
        O = torch.cat(obuf)
        A = torch.cat(abuf)
        LP = torch.cat(lbuf)
        V = torch.cat(vbuf)
        ADV = adv.reshape(-1)
        RET = ret.reshape(-1)
        ADV = (ADV - ADV.mean()) / (ADV.std() + 1e-8)
        idx = torch.randperm(T * n)
        for _ in range(args.epochs):
            for start in range(0, T * n, args.minibatch):
                b = idx[start:start + args.minibatch]
                logits, v = net(O[b])
                dist = torch.distributions.Categorical(logits=logits)
                lp = dist.log_prob(A[b])
                ratio = torch.exp(lp - LP[b])
                s1 = ratio * ADV[b]
                s2 = torch.clamp(ratio, 1 - 0.2, 1 + 0.2) * ADV[b]
                pi_loss = -torch.min(s1, s2).mean()
                v_loss = F.mse_loss(v, RET[b])
                ent = dist.entropy().mean()
                loss = pi_loss + 0.5 * v_loss - args.ent_coef * ent
                opt.zero_grad()
                loss.backward()
                nn.utils.clip_grad_norm_(net.parameters(), 0.5)
                opt.step()
        if it % 20 == 0:
            with torch.no_grad():
                _, v = net(obs)
            print(
                f"it {it:4d} steps {total:8d} R {float(rbuf[-1].mean()):6.2f} "
                f"V {float(v.mean()):6.2f} ent {ent.item():.3f}", flush=True
            )
    torch.save({"model": net.state_dict(), "feat": FEAT}, args.out)
    print("saved", args.out)


def load(ckpt):
    net = Net()
    net.load_state_dict(torch.load(ckpt, map_location="cpu")["model"])
    net.eval()
    return net


def run_episode(net, seed, max_steps=4000, collect=None):
    e = CanyonEnv(lives=1, fast_crash=True, action_repeat=4)
    e.reset(seed)
    last = "wait"
    for step in range(max_steps):
        o = e.obs
        with torch.no_grad():
            logits, _ = net(torch.from_numpy(features(o)).unsqueeze(0))
        a = ACTIONS[int(logits.argmax())]
        if collect is not None:
            collect.append((obs_text(o), last, a))
        last = ACTION_NAMES[a]
        o, done, _ = e.step(a)
        if done:
            return o.frames, o.score, o.level
    return e.obs.frames, e.obs.score, e.obs.level


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("train")
    t.add_argument("--iters", type=int, default=1000)
    t.add_argument("--envs", type=int, default=16)
    t.add_argument("--horizon", type=int, default=128)
    t.add_argument("--epochs", type=int, default=4)
    t.add_argument("--minibatch", type=int, default=256)
    t.add_argument("--lr", type=float, default=3e-4)
    t.add_argument("--ent-coef", type=float, default=0.005)
    t.add_argument("--action-repeat", type=int, default=4)
    t.add_argument("--seed", type=int, default=0)
    t.add_argument("--out", default="teacher.pt")
    e = sub.add_parser("eval")
    e.add_argument("--ckpt", default="teacher.pt")
    e.add_argument("--seeds", default="1000-1015")
    d = sub.add_parser("data")
    d.add_argument("--ckpt", default="teacher.pt")
    d.add_argument("--episodes", type=int, default=60)
    d.add_argument("--seed0", type=int, default=10000)
    d.add_argument("--out", default="teacher_data.pt")
    args = ap.parse_args()
    if args.cmd == "train":
        train(args)
    elif args.cmd == "eval":
        net = load(args.ckpt)
        a, b = args.seeds.split("-")
        seeds = range(int(a), int(b) + 1)
        rows = [run_episode(net, s) for s in seeds]
        for s, (f, sc, lv) in zip(seeds, rows):
            print(f"seed {s}: frames {f} score {sc} level {lv}")
        print(f"mean frames {np.mean([r[0] for r in rows]):.0f} "
              f"mean score {np.mean([r[1] for r in rows]):.1f}")
    else:
        net = load(args.ckpt)
        rows = []
        for i in range(args.episodes):
            rows.append([])
            f, sc, lv = run_episode(net, args.seed0 + i, collect=rows[-1])
            print(f"ep {i}: {len(rows[-1])} decisions, {f} frames, score {sc}")
        obs = [r[0] for row in rows for r in row]
        lasts = [r[1] for row in rows for r in row]
        acts = [r[2] for row in rows for r in row]
        torch.save({"obs": obs, "last": lasts, "actions": acts}, args.out)
        print(f"saved {args.out} ({len(acts)} decisions)")


if __name__ == "__main__":
    main()