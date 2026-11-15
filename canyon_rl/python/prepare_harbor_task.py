"""Prepare the upstream Harbor task for v1.5 with a shared optional action cap."""
import argparse
from pathlib import Path
import shutil


def replace_once(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise ValueError(f"Unexpected upstream task format in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1))


def prepare(source, destination, steps=0):
    if steps < 0:
        raise ValueError("steps must be nonnegative (0 = unlimited)")
    shutil.copytree(source, destination)
    here = Path(__file__).resolve().parent
    for name in ("play_mcp.py", "v15_action_policy.json"):
        shutil.copyfile(here / name, destination / "solution" / name)
    server = destination / "environment/mcp-server/server.py"
    replace_once(server, 'MAX_STEPS = int(os.environ.get("MAX_STEPS", "1500"))', f"MAX_STEPS = {steps}")
    replace_once(server, "if self.n >= MAX_STEPS:", "if MAX_STEPS > 0 and self.n >= MAX_STEPS:")
    verifier = destination / "tests/replay.py"
    replace_once(verifier, "MAX_STEPS = 1500", f"MAX_STEPS = {steps}")
    replace_once(verifier, "if steps >= MAX_STEPS:", "if MAX_STEPS > 0 and steps >= MAX_STEPS:")
    (destination / "solution/solve.sh").write_text(
        "#!/bin/bash\nset -euo pipefail\ncd /solution\n"
        "python3 play_mcp.py --model aldigobbler/canyon-racer-lfm2-230m-v1.5-gguf "
        "--base-url http://host.docker.internal:8001/v1 --mcp-url http://mcp-server:8000/mcp "
        f"--steps {steps} --trace /logs/agent/v15-decisions.jsonl\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--steps", type=int, default=0)
    args = parser.parse_args()
    prepare(args.source, args.destination, args.steps)


if __name__ == "__main__":
    main()
