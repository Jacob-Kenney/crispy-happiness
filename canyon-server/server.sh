#!/bin/sh
# Start llama.cpp's server on the fine-tuned Canyon Racer model.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
MODEL=${MODEL:-"$HERE/models/canyon-lfm2-230m-q8_0.gguf"}
SERVER=${LLAMA_SERVER:-"$HOME/llama.cpp/build/bin/llama-server"}
exec "$SERVER" -m "$MODEL" --host 127.0.0.1 --port "${PORT:-8080}" \
    -c 512 -np 1 --temp 0 "$@"