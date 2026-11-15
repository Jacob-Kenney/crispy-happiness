"""Play canyon-jumpeRL through its MCP interface with our checkpoint or an API model."""
from __future__ import annotations

import argparse
import asyncio
import json
import math
import os
from pathlib import Path
from urllib.request import Request, urlopen

BUTTONS = ("right", "left", "up", "down", "a", "b", "select")
ACTIONS = ("wait", "left", "right", "up", "down", "a", "b")
SYSTEM = """Pilot Canyon Racer. Reply with exactly one word: wait, left, right,
up, down, a, b. a jumps; hold a for a higher jump. b saves a checkpoint and
costs a life. wait releases all buttons. Each action lasts four game frames.
Keep inside the canyon and jump over obstacles. Observations contain the ship
position, canyon wall offsets and the two nearest obstacles. Positive obstacle
dy means ahead of the ship. No game time passes while you choose an action."""
V15_MODEL = "aldigobbler/canyon-racer-lfm2-230m-v1.5-gguf"
V15_SYSTEM = """You pilot a ship up a narrow canyon in Canyon Racer.
Walls, rocks and items scroll toward you.
Reply with exactly one action word:
left, right, up, down, a (jump), b (save), wait (release buttons)
Hold a to jump higher; while airborne you cannot crash.
Other words are invalid and penalized."""


def buttons(action):
    if action not in ACTIONS:
        raise ValueError(f"Invalid model action: {action!r}")
    return [float(button == action) for button in BUTTONS]


def unpack(result):
    if getattr(result, "is_error", getattr(result, "isError", False)):
        raise RuntimeError(f"MCP tool failed: {result.content}")
    value = getattr(result, "structured_content", getattr(result, "structuredContent", None))
    if value is None:
        value = json.loads(next(c.text for c in result.content if c.type == "text"))
    if not all(k in value for k in ("observation", "terminated", "info")):
        raise ValueError("Unexpected MCP game response")
    return value


def observation_text(data, compact=False):
    """Decode the pinned upstream 33-feature contract without importing PyBoy."""
    obs = data["observation"]
    if obs.get("shape") != [33] or obs.get("dtype") != "float32":
        raise ValueError("Expected canyon-jumpeRL float32 observation with shape [33]")
    v = obs["data"]
    if len(v) != 33 or not all(isinstance(x, (int, float)) and math.isfinite(x) for x in v):
        raise ValueError("Invalid observation values")
    x, y, z, vz = v[:4]
    if compact:
        # Restore pixel coordinates and training-style text. MCP does not expose
        # invincibility or obstacle speed, so omit those rather than invent values.
        x, y, z = (round(n * 100) for n in (x, y, z))
        lefts = [round(x + n * 100) for n in v[5:15]]
        row = min(9, max(0, (y + 16) // 16))
        left = lefts[row]
        info = data["info"]
        state = "crash" if v[4] else ("jump" if z else "ground")
        lines = [f"x={x} y={y} z={z} {state} lv={info['level']} sc={info['score']}",
                 f"canyon {left}..{left+32} dx={x-(left+16)} gl={x+8-left} gr={left+32-(x+16)}"]
        front = [lefts[min(9, max(0, (y-dy)//16))] for dy in (8, 24, 40)]
        lines.append("front " + " ".join(f"{n}..{n+32}" for n in front))
        obstacles = []
        for i in (15, 24):
            o = v[i:i+9]
            if o[0] > .5:
                typ = ("left", "right", "middle", "full", "item")[max(range(5), key=lambda j: o[1+j])]
                dl, dr, dy = (round(n*100) for n in o[6:9])
                obstacles.append(f"dy{dy} {typ} {x+dl}..{x+dr} dl{dl} dr{dr}")
        lines.append("obs " + ("; ".join(obstacles) or "none"))
        return "\n".join(lines)
    lines = [
        f"ship x={x*100:.1f} y={y*100:.1f} z={z*100:.1f} vz={vz*3:.2f} crashing={v[4]:g}",
        "canyon left offsets from ship x at y=0,16,...,144: "
        + ",".join(f"{n*100:.1f}" for n in v[5:15]),
        "canyon width=32 pixels; ship hitbox x+8..x+16",
    ]
    for i in (15, 24):
        row = v[i:i+9]
        if row[0] > .5:
            typ = ("left", "right", "middle", "full", "item")[max(range(5), key=lambda j: row[1+j])]
            lines.append(f"obstacle {typ} dl={row[6]*100:.1f} dr={row[7]*100:.1f} dy={row[8]*100:.1f}")
    lines.append("info=" + json.dumps(data["info"], sort_keys=True))
    return "\n".join(lines)


class EndpointPolicy:
    def __init__(self, args):
        self.args = args

    def __call__(self, text, last):
        v15 = self.args.model == V15_MODEL
        user = (f"State:\n{text}\nlast={last}\n"
                "Reply with one word: left, right, up, down, a, b, wait.") if v15 else f"{text}\nlast={last}"
        payload = {"model": self.args.model, "temperature": 0, "max_tokens": 16,
                   "messages": [{"role": "system", "content": V15_SYSTEM if v15 else SYSTEM},
                                {"role": "user", "content": user}]}
        headers = {"Content-Type": "application/json"}
        key = os.environ.get("MODEL_API_KEY")
        if key:
            headers["Authorization"] = "Bearer " + key
        request = Request(self.args.base_url.rstrip("/") + "/chat/completions",
                          data=json.dumps(payload).encode(), headers=headers)
        with urlopen(request, timeout=self.args.request_timeout) as response:
            result = json.load(response)
        action = result["choices"][0]["message"]["content"].strip().lower()
        buttons(action)  # fail visibly on invalid output; never silently play wait
        return action


class GGUFPolicy:
    """Restore PPO's sum-over-token-groups and learned action prior in llama.cpp."""
    def __init__(self, args):
        self.args = args
        self.spec = json.loads(Path(__file__).with_name("v15_action_policy.json").read_text())
        self.groups = {int(k): v for k, v in self.spec["token_to_action"].items()}
        self.prior = self.spec["action_prior"]
        self.last_probabilities = None

    def request(self, path, payload):
        root = self.args.base_url.rstrip("/").removesuffix("/v1")
        headers = {"Content-Type": "application/json"}
        if key := os.environ.get("MODEL_API_KEY"):
            headers["Authorization"] = "Bearer " + key
        req = Request(root + path, data=json.dumps(payload).encode(), headers=headers)
        with urlopen(req, timeout=self.args.request_timeout) as response:
            return json.load(response)

    def __call__(self, text, last):
        messages = [{"role": "system", "content": V15_SYSTEM},
                    {"role": "user", "content": f"State:\n{text}\nlast={last}\n"
                     "Reply with one word: left, right, up, down, a, b, wait."}]
        prompt = self.request("/apply-template", {"messages": messages})["prompt"]
        # A common +100 boost isolates the 48 valid tokens without changing their
        # relative probabilities. Bias each token by its group's learned prior;
        # summing those probabilities equals softmax(logsumexp(group)+prior).
        result = self.request("/completion", {
            "prompt": prompt, "n_predict": 1, "cache_prompt": True,
            "temperature": 1, "top_k": 0, "top_p": 1, "min_p": 0,
            "repeat_penalty": 1, "n_probs": len(self.groups),
            "post_sampling_probs": True,
            "logit_bias": [[token, 100 + self.prior[group]] for token, group in self.groups.items()],
        })
        probs = result["completion_probabilities"][0]["top_probs"]
        if {v["id"] for v in probs} != set(self.groups):
            raise ValueError("llama.cpp did not return all valid action tokens")
        sums = [0.0] * len(ACTIONS)
        for item in probs:
            sums[self.groups[item["id"]]] += item["prob"]
        self.last_probabilities = dict(zip(ACTIONS, sums))
        return ACTIONS[max(range(len(sums)), key=sums.__getitem__)]


class CheckpointPolicy:
    def __init__(self, args):
        import torch
        from agent import PromptBuilder, load_policy
        self.torch = torch
        self.policy, tokenizer, self.cfg = load_policy(str(args.checkpoint), args.device)
        self.builder = PromptBuilder(tokenizer, args.device)

    def __call__(self, text, last):
        ids, mask = self.builder.batch([text], [last])
        with self.torch.inference_mode():
            _, logits, _ = self.policy(ids, mask)
        return ACTIONS[int(logits.argmax(dim=-1).item())]


async def play(args, policy):
    from mcp import ClientSession
    try:
        from mcp.client.streamable_http import streamable_http_client as http_client
    except ImportError:
        from mcp.client.streamable_http import streamablehttp_client as http_client

    async with http_client(args.mcp_url) as streams:
        async with ClientSession(streams[0], streams[1]) as session:
            await session.initialize()
            data = unpack(await session.call_tool("reset", {"seed": args.seed}))
            last = "wait"
            with args.trace.open("w") as trace:
                trace.write(json.dumps({"tool": "reset", "result": data}) + "\n")
                step = 0
                while not data["terminated"] and (args.steps == 0 or step < args.steps):
                    text = observation_text(data, compact=getattr(args, "model", None) == V15_MODEL)
                    action = await asyncio.to_thread(policy, text, last)
                    encoded = buttons(action)
                    data = unpack(await session.call_tool("step", {"action": encoded}))
                    trace.write(json.dumps({"step": step, "action": action, "buttons": encoded,
                                            "action_probabilities": getattr(policy, "last_probabilities", None),
                                            "result": data}) + "\n")
                    trace.flush()
                    last = action
                    step += 1
            print(json.dumps({"terminated": data["terminated"], "info": data["info"],
                              "trace": str(args.trace)}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--checkpoint", type=Path)
    source.add_argument("--model", help="Model name served by an OpenAI-compatible endpoint")
    parser.add_argument("--base-url", default=os.environ.get("MODEL_BASE_URL", "http://127.0.0.1:8001/v1"))
    parser.add_argument("--mcp-url", default=os.environ.get("MCP_URL", "http://127.0.0.1:8000/mcp"))
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--steps", type=int, default=0,
                        help="Maximum actions; 0 = no limit, play until game over (default)")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--request-timeout", type=float, default=120)
    parser.add_argument("--trace", type=Path, default=Path("mcp-play.jsonl"))
    args = parser.parse_args()
    if args.steps < 0 or args.request_timeout <= 0:
        parser.error("steps must be nonnegative and request timeout must be positive")
    policy = (CheckpointPolicy(args) if args.checkpoint else
              GGUFPolicy(args) if args.model == V15_MODEL else EndpointPolicy(args))
    asyncio.run(play(args, policy))


if __name__ == "__main__":
    main()
