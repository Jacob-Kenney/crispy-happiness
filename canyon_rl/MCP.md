Play our model through canyon-jumpeRL MCP
=======================================

For our published v1.5 GGUF, run from this repository:

```sh
bash canyon_rl/scripts/play_v15_mcp.sh --trace /tmp/v15-mcp.jsonl

# Run the isolated Harbor task and authoritative replay verifier:
bash canyon_rl/scripts/play_v15_mcp.sh --harbor
```

Playback now has **no action/frame cap by default**: it runs until game over.
Use `--steps 1500` to reproduce a bounded direct run, or
`CANYON_MAX_STEPS=1500 bash canyon_rl/scripts/play_v15_mcp.sh --harbor` for a
bounded Harbor run. `0` means unlimited. Harbor's generated client, sidecar
and replay verifier share that setting, so none silently stops at 6,000 frames.
Harbor still applies its task wall-clock timeouts.

The launcher requires `uv`, Python 3.14 (available to uv), and `llama-server`
at `~/llama.cpp/build/bin/llama-server` (override with `LLAMA_SERVER`). It caches
the pinned Q4_K_M model, game checkout and client dependencies in
`~/.cache/canyon-mcp`, starts both local servers, runs the policy, then stops
its servers. Ports 8000 and 8001 must be free. Server logs remain in that cache.
An existing checkout can be selected with `CANYON_JUMPERL_REPO`.
Harbor mode also requires Docker. It uses Harbor 0.26.0's `oracle` scripted-agent
harness, with its solution replaced by our model client; no heuristic is used.
The model endpoint is served on the host for Docker access via
`host.docker.internal:8001`. Results are saved under `canyon_rl/runs/harbor`.

Model: https://huggingface.co/aldigobbler/canyon-racer-lfm2-230m-v1.5-gguf
at revision `f89127e45700832a2be112da563e7eda21626907`.
The client selects the published system/user prompt and compact pixel-space
observations when this exact model ID is used. Unlike the original checkpoint,
the GGUF does not contain the learned action prior or value head. The client
restores the 48 valid action tokens and seven learned biases from
`python/v15_action_policy.json`, recovered from Modal volume `canyon-racer-rl`,
`runs/agg32fast/policy.pt` (iteration 350, SHA-256 recorded in the JSON).
It obtains llama.cpp's post-bias token probabilities without top-k/top-p/min-p
filtering, sums all token variants for each action and chooses the largest sum.
That implements the checkpoint's `argmax(logsumexp(group logits) + action_prior)`.
The sampled completion token is ignored. The value head is not needed for play.
The native llama.cpp `/apply-template` and `/completion` APIs are required for
this model; other model IDs continue using the chat-completions API.

The original greedy-text client scored 0. Restoring action selection scored 290
on a seed-0 rollout in the C++ training environment and 290 in a 1,500-step direct
MCP run. These are individual diagnostic runs, not the model-card evaluation.
MCP exposes only sampled canyon geometry, so reconstructing walls is approximate.

To regenerate the action-policy metadata in a Torch/Transformers environment:

```sh
python canyon_rl/python/export_action_policy.py /path/to/policy.pt \
  --model aldigobbler/canyon-racer-lfm2-230m-v1.5-gguf \
  --source modal:canyon-racer-rl/runs/agg32fast/policy.pt \
  --out canyon_rl/python/v15_action_policy.json
```

Integration branch: `codex/jumperl-mcp`. Upstream contract inspected at
`ce720cb3bf638839eeacf9526aa29578a50bbd78` in
https://github.com/Jacob-Kenney/canyon-jumpeRL.

Start the game server in a separate terminal, using upstream's Python/dependency
environment (its current pyproject requires Python 3.14+):

```sh
git clone https://github.com/Jacob-Kenney/canyon-jumpeRL.git /tmp/canyon-jumpeRL
cd /tmp/canyon-jumpeRL
git checkout ce720cb3bf638839eeacf9526aa29578a50bbd78
uv sync
uv run canyon-racer-mcp --http --host 127.0.0.1 --port 8000
```

In this repo's model environment install the MCP client (`pip install 'mcp>=1.10,<3'`),
then choose a model source:

```sh
# Existing LoRA/grouped-token checkpoint saved by python/agent.py:
python canyon_rl/python/play_mcp.py --checkpoint /path/to/policy.pt --device cpu

# Our model served through an OpenAI-compatible chat endpoint:
python canyon_rl/python/play_mcp.py --model OUR_MODEL --base-url http://127.0.0.1:8001/v1
```

Set `MODEL_API_KEY` when the endpoint requires authentication. `--mcp-url`,
`--steps`, `--seed` and `--trace` configure the game connection and local log.
Invalid model responses and MCP errors stop the run. The client sends only
`reset` and `step`; buttons follow upstream's RIGHT,LEFT,UP,DOWN,A,B,SELECT order.

This is turn-based evaluation: every MCP step advances four emulator frames.
Model latency does not advance the game. It therefore measures different
behavior from `model_bench_jumperl.py`'s real-time benchmark.

Checkpoint support is for `agent.py` policies, not GRU checkpoints. The MCP
server exposes normalized 33-feature observations, whereas our C++ training
environment exposes a richer state. The adapter describes the available data
without inventing missing speed/invincibility fields. This changes the prompt
distribution; evaluate performance before treating it as comparable to C++ scores.

Upstream also includes `harbor/canyon-racer-task`, with an isolated MCP sidecar
and trace-replay verifier. When running this client inside that task's agent
container, use `--mcp-url http://mcp-server:8000/mcp`. The agent environment must
contain this repo's `canyon_rl/python` files, model weights and dependencies (or
have access to the model endpoint). This client alone does not register a Harbor
agent or run the verifier; its local JSONL log is diagnostic, not a Harbor reward.
Use the `--harbor` launcher mode above to prepare that agent environment and
execute both gameplay and verification automatically.

Harbor run completed on 2026-11-15: `canyon_rl/runs/harbor/canyon-v15-mcp`.
One trial, zero exceptions; the separate replay verifier processed 791 actions
and returned reward **0.0** (`steps=791 best_score=0 reward=0.0`). The agent
reached game over at 3,164 frames with no lives left. Results include the model
decision log, sidecar action trace, verifier replay log, and Harbor result JSON.

After restoring grouped action probabilities and the learned prior, Harbor run
`canyon_rl/runs/harbor/canyon-v15-mcp-fixed` completed with **reward 290.0**,
one trial and zero exceptions. The separate verifier replayed the full
1,500-action budget: `steps=1500 best_score=290 reward=290.0`. The ship was
still alive at the budget limit (6,000 frames, seven lives). Harbor scores the
best raw in-game score, so the final post-rewind score of 50 is not the reward.

Paired-seed comparison
----------------------

`python/compare_mcp_policies.py` compares the fixed GGUF decoder against the
upstream `HeuristicPolicy(th=30, hold=2)` using the canonical MCP
`EmulatorBackend` directly, with the same encoded observations and four-frame
actions. Each policy gets a fresh game and a maximum of 1,500 actions for each
seed. Scores are the best raw in-game score, as in Harbor. Every action trace
is independently replayed in another fresh emulator, checking score, steps,
frames, final lives and final score. This batch bypasses MCP HTTP and Harbor
container startup; v1.5's seed-0 result reproduces its verified Harbor score.

With the model server running and upstream game dependencies installed:

```sh
python canyon_rl/python/compare_mcp_policies.py \
  --repo /path/to/canyon-jumpeRL \
  --seeds 0,1,2,3,4,5,6,7,8,9 --steps 1500 \
  --out canyon_rl/runs/harbor/v15-vs-reference-10-seeds
```

The output includes per-run scores, paired score differences, aggregate
statistics, all replayable traces and the benchmark configuration.

Completed seeds 0–9: v1.5 mean **170**, median **145**, alive at budget **10/10**;
reference mean **146**, median **195**, alive at budget **7/10**. v1.5 won six
paired seeds and lost four. All 20 traces replayed exactly. See
`benchmarks/v1.5-1500-steps/report.md` for the committed full comparison,
or `benchmarks/v1.5-1500-steps/top-runs.md` for the labeled best-run selection.
The published ten-seed results retain their original 1,500-action budget;
they are not uncapped results. New comparison runs default to game-over-only
termination; pass `--steps 1500` to reproduce the published budget.
