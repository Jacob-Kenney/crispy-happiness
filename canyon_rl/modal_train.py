"""Run Canyon Racer PPO training on Modal GPUs.

Setup (one time):
    pip install modal
    modal setup                # browser login, writes ~/.modal.toml

Train (defaults: 300 iterations, 32 envs, batch 64, L4):
    modal run modal_train.py

Close the laptop while it runs:
    modal run --detach modal_train.py

Override training flags (anything train_ppo.py accepts):
    modal run modal_train.py --args "--iters 500 --envs 48 --run big"
    # equivalent if your shell/Modal mangles --args:
    CANYON_ARGS="--iters 500 --envs 48 --run big" modal run modal_train.py

Choose a GPU:
    CANYON_GPU=A10G modal run modal_train.py

Evaluate a checkpoint stored on the volume:
    modal run modal_train.py --args "--mode eval --ckpt /outputs/runs/modal/policy.pt"

Follow / fetch results:
    modal app logs canyon-racer-ppo
    modal volume get canyon-racer-rl runs/modal ./runs/modal

The image builds the C++ env library with g++ (Linux .so) and installs PyTorch,
Transformers and PEFT.  Two volumes are mounted: checkpoints + metrics, and the
Hugging Face cache so the model is downloaded only once.
"""

import argparse
import os
import shlex

import modal

GPU = os.environ.get("CANYON_GPU", "L4")
REMOTE_ROOT = "/root/canyon_rl"

app = modal.App("canyon-racer-ppo")
out_vol = modal.Volume.from_name("canyon-racer-rl", create_if_missing=True)
hf_vol = modal.Volume.from_name("canyon-racer-hf", create_if_missing=True)

VOLUMES = {
    "/outputs": out_vol,
    "/root/.cache/huggingface": hf_vol,
}


def _ignore_cpp(path):
    return path.name.startswith("libcanyon_env") or path.name in (
        "build",
        "__pycache__",
    )


def _ignore_python(path):
    return path.name in ("runs", "__pycache__") or path.suffix == ".pt"


image = (
    modal.Image.debian_slim(python_version="3.12")
    .apt_install("g++", "make")
    .pip_install("torch>=2.6", "transformers>=5.2,<6", "peft>=0.21", "numpy")
    .env({"PYTORCH_CUDA_ALLOC_CONF": "expandable_segments:True"})
    .add_local_dir(
        "cpp",
        remote_path=f"{REMOTE_ROOT}/cpp",
        copy=True,
        ignore=_ignore_cpp,
    )
    .add_local_dir(
        "python",
        remote_path=f"{REMOTE_ROOT}/python",
        copy=True,
        ignore=_ignore_python,
    )
    .run_commands(f"make -C {REMOTE_ROOT}/cpp lib")
)


@app.function(image=image, gpu=GPU, timeout=8 * 60 * 60, volumes=VOLUMES)
def train(args: tuple[str, ...], out_dir: str):
    import os
    import subprocess
    import sys
    import threading

    latest = os.path.join(out_dir, "policy_latest.pt")

    # Persist checkpoints and metrics while training runs so a crash is
    # recoverable and progress is visible between commits.
    stop = threading.Event()

    def committer():
        while not stop.wait(300):
            try:
                out_vol.commit()
                hf_vol.commit()
            except Exception as exc:  # keep training even if a commit hiccups
                print(f"volume commit failed: {exc}", flush=True)

    threading.Thread(target=committer, daemon=True).start()
    try:
        for attempt in range(3):
            cmd = [sys.executable, f"{REMOTE_ROOT}/python/train_ppo.py", *args]
            if attempt and os.path.exists(latest):
                cmd += ["--resume", latest]
                print(f"restarting from {latest}", flush=True)
            print(" ".join(cmd), flush=True)
            rc = subprocess.run(cmd, cwd=f"{REMOTE_ROOT}/python").returncode
            if rc == 0:
                return
            print(f"train_ppo.py exited with {rc}", flush=True)
    finally:
        stop.set()
        out_vol.commit()
        hf_vol.commit()


@app.function(image=image, gpu=GPU, timeout=60 * 60, volumes=VOLUMES)
def evaluate(ckpt: str, episodes: int = 8, seed_start: int = 1000):
    import subprocess
    import sys

    cmd = [
        sys.executable,
        f"{REMOTE_ROOT}/python/eval.py",
        ckpt,
        "--episodes", str(episodes),
        "--seed-start", str(seed_start),
        "--device", "cuda",
        "--verbose",
    ]
    print(" ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=f"{REMOTE_ROOT}/python", check=True)


DEFAULT_TRAIN_ARGS = [
    "--iters", "400",
    "--envs", "48",
    "--horizon", "64",
    "--epochs", "1",
    "--minibatch", "32",
    "--lm-lr", "1e-5",
    "--head-lr", "1e-4",
    "--epsilon", "0.25",
    "--lives", "1",
    "--bf16",
    "--eval-every", "20",
    "--eval-episodes", "4",
    "--save-every", "10",
]


@app.local_entrypoint()
def main(args: str = ""):
    """`--args` (or $CANYON_ARGS) is forwarded almost verbatim to train_ppo.py."""
    raw = args or os.environ.get("CANYON_ARGS", "")
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--mode", default="train", choices=["train", "eval"])
    parser.add_argument("--run", default="modal")
    parser.add_argument("--ckpt", default="/outputs/runs/modal/policy.pt")
    parser.add_argument("--episodes", type=int, default=8)
    known, extra = parser.parse_known_args(shlex.split(raw))

    if known.mode == "eval":
        print(f"evaluating {known.ckpt}")
        evaluate.remote(known.ckpt, known.episodes)
        return

    out_dir = f"/outputs/runs/{known.run}"
    train_args = (
        DEFAULT_TRAIN_ARGS
        + extra
        + [
            "--device", "cuda",
            "--out-dir", out_dir,
            "--save",
        ]
    )
    print(
        f"training on {GPU}\n"
        f"  out={out_dir} (volume canyon-racer-rl)\n"
        f"  fetch: modal volume get canyon-racer-rl runs/{known.run} "
        f"./runs/{known.run}"
    )
    train.remote(tuple(train_args), out_dir)