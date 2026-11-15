#!/usr/bin/env bash
# Local GGUF -> chat endpoint -> MCP -> Canyon Racer ROM.
set -euo pipefail
HARBOR=0
if [ "${1:-}" = "--harbor" ]; then HARBOR=1; shift; fi
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/canyon-mcp"
REPO="${CANYON_JUMPERL_REPO:-$CACHE/canyon-jumpeRL}"
MODEL="$CACHE/canyon-racer-lfm2-230m-v1.5-Q4_K_M.gguf"
LLAMA_SERVER="${LLAMA_SERVER:-$HOME/llama.cpp/build/bin/llama-server}"
MODEL_ID=aldigobbler/canyon-racer-lfm2-230m-v1.5-gguf
command -v uv >/dev/null
test -x "$LLAMA_SERVER" || { echo "Set LLAMA_SERVER to your llama-server binary" >&2; exit 1; }
mkdir -p "$CACHE"
if [ ! -f "$MODEL" ]; then
  curl -Lf --retry 3 "https://huggingface.co/$MODEL_ID/resolve/f89127e45700832a2be112da563e7eda21626907/$(basename "$MODEL")" -o "$MODEL.part"
  mv "$MODEL.part" "$MODEL"
fi
if [ ! -d "$REPO" ]; then
  git clone https://github.com/Jacob-Kenney/canyon-jumpeRL.git "$REPO"
  git -C "$REPO" checkout ce720cb3bf638839eeacf9526aa29578a50bbd78
fi
if [ ! -x "$CACHE/venv/bin/python" ]; then
  uv venv --python 3.14 "$CACHE/venv"
fi
uv pip install --python "$CACHE/venv/bin/python" 'mcp==2.3.0' 'numpy==2.5.4' 'gymnasium==1.4.0' 'pyboy==2.9.0' 'pillow==13.0.0'
MODEL_PID=""
GAME_PID=""
cleanup() {
  if [ -n "$MODEL_PID" ]; then kill "$MODEL_PID" 2>/dev/null || true; fi
  if [ -n "$GAME_PID" ]; then kill "$GAME_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT
# Refuse occupied ports: never connect silently to somebody else's model/game.
"$CACHE/venv/bin/python" - <<'PY'
import socket
for port in (8000, 8001):
    with socket.socket() as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(('127.0.0.1', port))
PY
MODEL_HOST=127.0.0.1
if [ "$HARBOR" = 1 ]; then MODEL_HOST=0.0.0.0; fi
"$LLAMA_SERVER" -m "$MODEL" --host "$MODEL_HOST" --port 8001 --alias "$MODEL_ID" -c 2048 --parallel 1 >"$CACHE/model.log" 2>&1 &
MODEL_PID=$!
if [ "$HARBOR" = 1 ]; then
  bash "$REPO/harbor/canyon-racer-task/sync.sh"
  TASK_PARENT="$(mktemp -d "$CACHE/v15-harbor-XXXXXX")"
  TASK="$TASK_PARENT/canyon-racer-v15"
  "$CACHE/venv/bin/python" "$ROOT/canyon_rl/python/prepare_harbor_task.py" \
    "$REPO/harbor/canyon-racer-task" "$TASK" --steps "${CANYON_MAX_STEPS:-0}"
  # Oracle is Harbor's scripted-client harness here; solve.sh runs v1.5 inference.
  uv tool run --from harbor==0.26.0 harbor run -p "$TASK" -a oracle -n 1 --jobs-dir "$ROOT/canyon_rl/runs/harbor" "$@"
  exit
fi
PYTHONPATH="$REPO/src" "$CACHE/venv/bin/python" -m canyon_jumperl.mcp_server --http --host 127.0.0.1 --port 8000 >"$CACHE/game.log" 2>&1 &
GAME_PID=$!
"$CACHE/venv/bin/python" - <<'PY'
import socket, time
from urllib.request import urlopen
deadline = time.monotonic() + 90
while True:
    try:
        with urlopen('http://127.0.0.1:8001/health', timeout=1): pass
        with socket.create_connection(('127.0.0.1', 8000), timeout=1): pass
        break
    except OSError:
        if time.monotonic() >= deadline:
            raise SystemExit('Server startup timed out; inspect ~/.cache/canyon-mcp/{model,game}.log')
        time.sleep(.2)
PY
cd "$ROOT"
"$CACHE/venv/bin/python" canyon_rl/python/play_mcp.py --model "$MODEL_ID" "$@"
