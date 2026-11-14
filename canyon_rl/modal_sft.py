"""Run the expert->SFT warm start on a Modal GPU.

    modal run modal_sft.py --args "--data /root/python/sft_data.pt \
        --out /outputs/runs/sft/sft2.pt --epochs 1 --lr 1e-4 --device cuda \
        --max-steps 1200 --invalid-weight 0.5"

No C++ build is needed: training only consumes the collected `sft_data.pt`.
"""

import os
import shlex

import modal

GPU = os.environ.get("CANYON_GPU", "L4")

app = modal.App("canyon-sft")
out_vol = modal.Volume.from_name("canyon-racer-rl", create_if_missing=True)
hf_vol = modal.Volume.from_name("canyon-racer-hf", create_if_missing=True)

image = (
    modal.Image.debian_slim(python_version="3.12")
    .pip_install("torch>=2.6", "transformers>=5.2,<6", "peft>=0.21", "numpy")
    .add_local_dir("python", remote_path="/root/python", copy=True)
)


@app.function(
    image=image,
    gpu=GPU,
    volumes={"/outputs": out_vol, "/root/.cache/huggingface": hf_vol},
    timeout=2 * 60 * 60,
)
def train(args: tuple[str, ...]):
    import subprocess
    import sys

    cmd = [sys.executable, "/root/python/sft.py", "train", *args]
    print(" ".join(cmd), flush=True)
    subprocess.run(cmd, cwd="/root/python", check=True)
    out_vol.commit()
    hf_vol.commit()


@app.local_entrypoint()
def main(args: str = ""):
    raw = args or os.environ.get("CANYON_ARGS", "")
    train.remote(tuple(shlex.split(raw)))