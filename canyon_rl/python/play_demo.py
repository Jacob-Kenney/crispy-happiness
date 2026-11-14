"""Watch a checkpoint play the port, printing the emitted token each turn.

This is the token-level view: the policy selects an action group, and the
emitted word is the highest-probability vocabulary token inside that group.
"""

import argparse

import torch

from agent import ACTION_NAMES, PromptBuilder, load_policy
from canyon_env import CanyonEnv, obs_text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("checkpoint")
    ap.add_argument("--seed", type=int, default=123)
    ap.add_argument("--decisions", type=int, default=400)
    ap.add_argument("--action-repeat", type=int, default=4)
    ap.add_argument("--device", default="mps")
    args = ap.parse_args()

    if args.device == "mps" and not torch.backends.mps.is_available():
        args.device = "cpu"

    policy, tokenizer, cfg = load_policy(args.checkpoint, args.device)
    policy.eval()
    space = policy.action_space
    builder = PromptBuilder(tokenizer, args.device)

    env = CanyonEnv(
        lives=cfg.get("lives", 1), fast_crash=True,
        action_repeat=args.action_repeat,
    )
    env.reset(args.seed)
    last = "wait"
    print("state at start:")
    print(obs_text(env.obs))

    for step in range(args.decisions):
        ids, mask = builder.batch([obs_text(env.obs)], [last])
        with torch.no_grad():
            logits, gl, value = policy(ids, mask)
        a = int(torch.argmax(gl[0]).item())
        ids_in_group = space.valid_ids[space.valid_groups == a]
        token = int(ids_in_group[torch.argmax(logits[0][ids_in_group])].item())
        word = tokenizer.decode([token]).strip()
        o, done, ev = env.step(a)
        last = ACTION_NAMES[a]
        print(
            f"{step:4d} frame={o.frames:5d} score={o.score:5d} "
            f"lvl={o.level} lives={o.lives} | token={word!r:10s} "
            f"action={ACTION_NAMES[a]:5s} value={float(value[0]):+.3f}"
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