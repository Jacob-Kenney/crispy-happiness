# Canyon Racer RL

A Game Boy game (`canyon-racer/`) ported to C++ and played by a PPO-trained
language model.

- `canyon_rl/` — Mac-portable C++ game logic, the C-API gym env, the PPO
  trainer (system prompt, action tokens, LoRA on LFM2.5-230M), Modal runner.
  See `canyon_rl/README.md`.
- `canyon-server/` — merge the trained LoRA into the base model, export GGUF,
  serve it with llama.cpp's `llama-server`, and play the game through the
  server. See `canyon-server/README.md`.

Quick check (after building the GGUF and starting the server):

```sh
python canyon-server/play.py --ckpt /path/policy.pt --seed 1011 --verbose
```