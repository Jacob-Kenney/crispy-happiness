# RL RACER

Take the wheel on a ModRetro Chromatic or hand it to a reinforcement-learning bot.
A second player places hazards from an 8-bit browser dashboard, which shows the race
and the bot's decisions in real time.

The handheld runs the game and sends its state to a Jetson Orin Nano over Wi-Fi.
The Jetson runs an LFM2.5-230M policy locally and sends controls and queued hazards
back to the handheld.

- [Play, build, and deploy RL RACER](road-racer/README.md)
- [Training code and methods](canyon_rl/README.md)
- [Trained model weights](https://huggingface.co/aldigobbler/canyon-racer-lfm2-230m-gguf)
- [Jetson setup and recovery](jetson/README.md)

## Canyon Racer training and inference

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
uv run canyon-server/play.py --ckpt /path/policy.pt --seed 1011 --verbose
```
