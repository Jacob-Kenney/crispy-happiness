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

## Jetson Docker policy endpoint

The Jetson deployment uses `jetson-policy-server.cpp` with the ARM64 CUDA 13
libraries from [llama-cpp-builder b11530](https://github.com/hybridgroup/llama-cpp-builder/releases/tag/b11530).
It evaluates the [trained Q8_0 export](https://huggingface.co/aldigobbler/canyon-racer-lfm2-230m-gguf)
on the Orin GPU, sums scores over all matching action tokens, and adds the
published action prior. It accepts the compact game state followed by
`last=<action>` and returns the selected action as JSON.

Build the server on Linux ARM64 with a C++20 compiler. Put `llama.h` from
`include/` and the headers from `ggml/include/` from the upstream b11530
source in the include directory:

```sh
g++ -std=c++20 -O2 -Wall -Wextra -I /path/to/headers \
  canyon-server/jetson-policy-server.cpp -ldl -o jetson-policy-server
```

On the Jetson, place the model, binary, and ARM64 `libgomp.so.1` in
`/var/lib/canyon-rl`. Put the extracted llama.cpp libraries in its
`runtime-parallel/` subdirectory. The Docker command below uses the Jetson's
CUDA and glibc libraries.

```sh
docker run -d --name canyon-rl --network host --device nvidia.com/gpu=all \
  -v /var/lib/canyon-rl:/canyon:ro \
  -v /lib/ld-linux-aarch64.so.1:/lib/ld-linux-aarch64.so.1:ro \
  -v /usr/lib:/wendy-usr-lib:ro -v /usr/local/cuda-13.2:/cuda:ro \
  -e LD_LIBRARY_PATH=/canyon/runtime-parallel:/canyon:/cuda/lib:/wendy-usr-lib \
  -w /canyon/runtime-parallel alpine:3.23 \
  /canyon/jetson-policy-server /canyon/canyon-lfm2-230m-q8_0.gguf
```

Run these checks from the Jetson shell. The server listens on loopback.

```sh
curl -fsS http://127.0.0.1:8080/health
curl -fsS -H 'Content-Type: text/plain' --data-binary @- http://127.0.0.1:8080/action <<'EOF'
x=68 y=112 z=0 ground inv=1 lv=0 sc=0 spd=500
canyon 64..96 dx=-12 gl=12 gr=12
front 64..96 64..96 64..96
obs none
last=wait
EOF
```
