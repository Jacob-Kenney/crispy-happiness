# canyon-server — llama.cpp server for the trained Canyon Racer LM

The PPO-trained agent is LFM2.5-230M + LoRA. This directory turns it into a
plain llama.cpp deployment: merge the LoRA, export a Q8_0 GGUF, serve it with
`llama-server`, and play the C++ game port through the server's
`/completion` endpoint.

The trained model is exported at
`canyon-server/models/canyon-lfm2-230m-q8_0.gguf` (236 MB, iteration 368).

## 1. Build the GGUF

Prerequisites: a built `llama.cpp` (the server binary at
`$HOME/llama.cpp/build/bin/llama-server`, or set `LLAMA_SERVER`), the
Hugging Face base weights cached (`LiquidAI/LFM2.5-230M`), and the Python
converter deps:

```sh
uv pip install --python /path/to/venv/bin/python gguf sentencepiece
```

Merge the PPO checkpoint (LoRA + heads) into the base and export Q8_0:

```sh
python canyon-server/merge_lora.py \
    --ckpt runs/main11/policy_latest.pt \
    --out  canyon-server/models/merged

cd "$HOME/llama.cpp"
python convert_hf_to_gguf.py \
    /abs/path/canyon-server/models/merged \
    --outfile /abs/path/canyon-server/models/canyon-lfm2-230m-q8_0.gguf \
    --outtype q8_0
```

`merge_lora.py` accepts any `policy_latest.pt` from the training runs; only
the LoRA is needed for inference (the value head stays in the checkpoint).
`models/merged` is an intermediate and can be deleted after conversion.
Swap `--outtype q8_0` for `f16` if you want an unquantized export.

## 2. Serve

```sh
canyon-server/server.sh            # 127.0.0.1:8080, context 512
# or override
PORT=9000 MODEL=/path/model.gguf canyon-server/server.sh
```

## 3. Play

```sh
python canyon-server/play.py --ckpt runs/main11/policy_latest.pt \
    --seed 1011 --decisions 4000 --verbose
```

The client builds the exact training prompt (system prompt + compact game
state + last action) and asks `/completion` for the top-N token logprobs.
The training policy does not sample the raw next token: it sums each action
word's probability mass over all case/whitespace token variants (logsumexp),
adds the checkpoint's learned 7-vector action prior, and takes the argmax.
The client reconstructs exactly that, so it disables the server's sampler
shaping (repeat penalty, top-k/top-p, min-p) to expose the raw distribution.
The action is then held for 4 game frames in the C++ port, matching training.

## Verification

On the iteration-368 checkpoint (`runs/main11/policy_latest.pt`), seed 1011,
the PyTorch policy and the Q8 llama.cpp server + client both reach 480 frames
/ score 50 after 120 decisions, and both reach 1600 frames / score 210 at 400
decisions on the earlier iteration-320 checkpoint. The top-200 logprob cutoff
is the only approximation versus the training's full-vocabulary group sums.

Training-run summary at iteration 368 (753,664 steps, run stopped): mean eval
1,410 frames / max score 162, invalid token mass 0.020, entropy 0.937
(`runs/main11/metrics.csv`).