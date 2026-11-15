"""Paired-seed v1.5/reference evaluation on the canonical MCP emulator backend."""
from __future__ import annotations

import argparse
import collections
import csv
import json
import os
from pathlib import Path
import statistics
import sys
import time
from types import SimpleNamespace

from play_mcp import ACTIONS, GGUFPolicy, buttons, observation_text


def episode(backend_type, encode, policy, seed, steps, trace_path):
    backend = backend_type()
    count, best, last = 0, 0, "wait"
    actions = collections.Counter()
    started = time.monotonic()
    try:
        obs, info = backend.reset(seed)
        initial = dict(info)
        best = info["score"]
        terminated = info["game_over"]
        with trace_path.open("w") as trace:
            trace.write(json.dumps({"op": "reset", "seed": seed}) + "\n")
            while not terminated and (steps == 0 or count < steps):
                data = {"observation": encode(obs), "info": info, "terminated": terminated}
                if isinstance(policy, GGUFPolicy):
                    action = policy(observation_text(data, compact=True), last)
                    vector = buttons(action)
                else:
                    vector = [float(v) for v in policy(obs)]
                    action = "a" if vector[4] else "wait"
                obs, terminated, info = backend.step(vector)
                best = max(best, info["score"])
                count += 1
                actions[action] += 1
                trace.write(json.dumps({"op": "step", "action": vector}) + "\n")
                last = action
        return {"seed": seed, "steps": count, "frames": info["frames"],
                "best_score": best, "final_score": info["score"],
                "initial_lives": initial["lives"], "final_lives": info["lives"],
                "game_over": terminated, "actions": dict(actions),
                "wall_seconds": round(time.monotonic() - started, 3)}
    finally:
        backend.close()


def replay(backend_type, trace_path, expected):
    backend = backend_type()
    best, count, info = 0, 0, None
    try:
        for line in trace_path.open():
            record = json.loads(line)
            if record["op"] == "reset":
                _, info = backend.reset(record["seed"])
                best = info["score"]
            else:
                _, _, info = backend.step(record["action"])
                best = max(best, info["score"])
                count += 1
        actual = {"best_score": best, "steps": count, "final_score": info["score"],
                  "final_lives": info["lives"], "frames": info["frames"]}
        assert actual == {key: expected[key] for key in actual}, (actual, expected)
        return True
    finally:
        backend.close()


def summarize(rows, seeds):
    summary = {}
    for name in ("v1.5", "reference"):
        subset = [r for r in rows if r["policy"] == name]
        scores = [r["best_score"] for r in subset]
        summary[name] = {"runs": len(subset), "mean": statistics.mean(scores),
                         "median": statistics.median(scores), "min": min(scores),
                         "max": max(scores),
                         "alive_at_budget": sum(not r["game_over"] for r in subset),
                         "replay_verified": sum(r["replay_verified"] for r in subset)}
    paired = []
    for seed in seeds:
        pair = {r["policy"]: r["best_score"] for r in rows if r["seed"] == seed}
        if len(pair) == 2:
            paired.append({"seed": seed, **pair, "delta": pair["v1.5"] - pair["reference"]})
    summary["paired"] = paired
    summary["wins_ties_losses"] = [sum(r["delta"] > 0 for r in paired),
                                    sum(r["delta"] == 0 for r in paired),
                                    sum(r["delta"] < 0 for r in paired)]
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(os.environ.get("CANYON_JUMPERL_REPO", "/private/tmp/canyon-jumperl-integration")))
    parser.add_argument("--seeds", default="0,1,2,3,4,5,6,7,8,9")
    parser.add_argument("--steps", type=int, default=0,
                        help="Maximum actions per run; 0 = play until game over (default)")
    parser.add_argument("--base-url", default="http://127.0.0.1:8001/v1")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.repo / "src"))
    from canyon_jumperl.mcp_server import EmulatorBackend, encode
    from canyon_jumperl.heuristic import HeuristicPolicy
    seeds = [int(seed) for seed in args.seeds.split(",")]
    if not seeds or len(seeds) != len(set(seeds)) or args.steps < 0:
        parser.error("Provide unique seeds and a nonnegative step budget (0 = unlimited)")
    args.out.mkdir(parents=True, exist_ok=True)
    config = {"seeds": seeds, "step_budget": args.steps or None, "frame_skip": 4,
              "stop_condition": "game over" if args.steps == 0 else "game over or step budget",
              "score": "best raw in-game score", "backend": "canonical MCP EmulatorBackend, direct calls",
              "reference": "upstream HeuristicPolicy(th=30, hold=2)",
              "replay": "fresh EmulatorBackend per trace; verify score, steps, frames, final lives and score",
              "game_revision": "ce720cb3bf638839eeacf9526aa29578a50bbd78",
              "seed_semantics": "reset(seed) as in MCP: seed initializes Gym RNG, which selects ROM game_seed"}
    (args.out / "config.json").write_text(json.dumps(config, indent=2) + "\n")
    rows = []
    model = GGUFPolicy(SimpleNamespace(base_url=args.base_url, request_timeout=120))
    for seed in seeds:
        for name, policy in (("reference", HeuristicPolicy()), ("v1.5", model)):
            trace = args.out / f"{name}-seed-{seed}.jsonl"
            row = episode(EmulatorBackend, encode, policy, seed, args.steps, trace)
            row["policy"] = name
            row["replay_verified"] = replay(EmulatorBackend, trace, row)
            rows.append(row)
            (args.out / "episodes.json").write_text(json.dumps(rows, indent=2) + "\n")
            print(json.dumps(row), flush=True)
    summary = summarize(rows, seeds)
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    with (args.out / "scores.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=["seed", "v1.5", "reference", "delta"], lineterminator="\n")
        writer.writeheader()
        writer.writerows(summary["paired"])
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
