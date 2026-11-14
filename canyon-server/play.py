"""Play Canyon Racer against a llama.cpp server serving the fine-tuned LM.

The training policy does not sample the raw next token: it sums each action
word's probability mass over all case/whitespace token variants (logsumexp),
adds a small learned 7-vector action prior, and takes the argmax.  This client
reconstructs that from the server's top-N token logprobs, so the server's
sampler settings (repeat penalty, top-k) are disabled to expose the raw
distribution.

    # terminal 1
    llama-server -m models/canyon-lfm2-230m-f16.gguf --port 8080 -c 512
    # terminal 2
    python canyon-server/play.py --ckpt /path/policy.pt --seed 1011 --verbose
"""

import argparse
import json
import sys
import urllib.request
from pathlib import Path

import torch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "canyon_rl" / "python"))

from actions import action_table  # noqa: E402
from canyon_env import ACTION_NAMES, CanyonEnv, obs_text  # noqa: E402
from transformers import AutoTokenizer  # noqa: E402

SYSTEM_PROMPT = """You pilot a ship up a narrow canyon in Canyon Racer.
Walls, rocks and items scroll toward you.
Reply with exactly one action word:
left, right, up, down, a (jump), b (save), wait (release buttons)
Hold a to jump higher; while airborne you cannot crash.
Other words are invalid and penalized."""


def build_prompt(tok, obs: str, last: str) -> str:
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {
            "role": "user",
            "content": (
                f"State:\n{obs}\nlast={last}\n"
                "Reply with one word: left, right, up, down, a, b, wait."
            ),
        },
    ]
    return tok.apply_chat_template(messages, add_generation_prompt=True, tokenize=False)


def ask(url: str, prompt: str, n_probs: int, timeout: float = 120.0) -> dict:
    body = json.dumps(
        {
            "prompt": prompt,
            "temperature": 1.0,
            "n_predict": 1,
            "n_probs": n_probs,
            "cache_prompt": True,
            "repeat_penalty": 1.0,
            "top_k": 0,
            "top_p": 1.0,
            "min_p": 0.0,
            "dry_multiplier": 0.0,
            "xtc_probability": 0.0,
        }
    ).encode()
    req = urllib.request.Request(
        url.rstrip("/") + "/completion",
        data=body,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def top_logprobs(resp: dict) -> list[dict]:
    cps = resp.get("completion_probabilities") or []
    if not cps:
        raise RuntimeError(f"no probabilities in response: {str(resp)[:200]}")
    entry = cps[0]
    return entry.get("top_logprobs") or [{"id": entry["id"], "logprob": entry["logprob"]}]


def group_logprobs(entries: list[dict], table: torch.Tensor) -> torch.Tensor:
    out = torch.full((len(ACTION_NAMES),), -float("inf"))
    for e in entries:
        g = int(table[e["id"]])
        if g >= 0:
            out[g] = torch.logaddexp(out[g], torch.tensor(float(e["logprob"])))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True, help="training checkpoint with action_prior")
    ap.add_argument("--url", default="http://127.0.0.1:8080")
    ap.add_argument("--tokenizer", default="LiquidAI/LFM2.5-230M")
    ap.add_argument("--seed", type=int, default=1011)
    ap.add_argument("--decisions", type=int, default=4000)
    ap.add_argument("--action-repeat", type=int, default=4)
    ap.add_argument("--n-probs", type=int, default=200)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    prior = torch.zeros(len(ACTION_NAMES))
    if args.ckpt:
        ckpt = torch.load(args.ckpt, map_location="cpu", weights_only=False)
        prior = ckpt["state_dict"]["action_prior"].float().cpu()
        print(f"action prior: {[round(float(v), 3) for v in prior]}")

    tok = AutoTokenizer.from_pretrained(args.tokenizer)
    table = action_table(tok)["token_to_action"]

    env = CanyonEnv(lives=1, fast_crash=True, action_repeat=args.action_repeat)
    env.reset(args.seed)
    last = "wait"
    for step in range(args.decisions):
        prompt = build_prompt(tok, obs_text(env.obs), last)
        entries = top_logprobs(ask(args.url, prompt, args.n_probs))
        gl = group_logprobs(entries, table) + prior
        a = int(torch.argmax(gl).item())
        in_group = [e for e in entries if int(table[e["id"]]) == a]
        word = max(in_group, key=lambda e: e["logprob"])["token"].strip()
        o, done, ev = env.step(a)
        last = ACTION_NAMES[a]
        if args.verbose and (step % 20 == 0 or ev["crashed"] or done):
            print(
                f"{step:4d} frame={o.frames:5d} score={o.score:5d} lvl={o.level} "
                f"lives={o.lives} | token={word!r:10s} action={ACTION_NAMES[a]:5s}"
                + ("  CRASH" if ev["crashed"] else "")
                + ("  REWIND" if ev["restored"] else "")
                + ("  GAME_OVER" if done else "")
            )
        if done:
            break
    print(
        f"final: frames={env.obs.frames} score={env.obs.score} "
        f"level={env.obs.level} lives={env.obs.lives}"
    )
    env.close()


if __name__ == "__main__":
    main()