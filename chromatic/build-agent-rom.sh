#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
: "${GBDK_HOME:?Set GBDK_HOME to the GBDK installation directory}"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
ln -s "${GBDK_HOME%/}" "$stage/gbdk"
git -C "$root/canyon-racer" archive HEAD | tar -xf - -C "$stage"
cp "$root/chromatic/agent-state.h" "$stage/src/agent_state.h"
mode=${1:-link}
agent=$mode
if [ "$mode" = road ]; then agent=mailbox; fi
cp "$root/chromatic/agent-$agent.c" "$stage/src/agent_link.c"
sed -i.bak 's/#define UPDATE_KEYS().*/uint8_t agent_link_keys(uint8_t local_keys);\n#define UPDATE_KEYS() previous_keys = keys; keys = agent_link_keys(joypad())/' "$stage/src/input.h"
sed -i.bak '/^void main(void) {/i\
extern void agent_link_init(void);\
' "$stage/src/main.c"
sed -i.bak '/^    init();$/a\
    agent_link_init();\
' "$stage/src/main.c"
name=canyon_racer_0.7.7_agent_mbc5
if [ "$agent" = mailbox ]; then
    name=canyon_racer_0.7.7_mailbox_mbc5
    sed -i.bak '/^void gameplay_pause(/i\
extern void agent_link_pause(void);\
' "$stage/src/gameplay_pause.c"
    sed -i.bak '/^    state.paused = true;$/a\
    agent_link_pause();\
' "$stage/src/gameplay_pause.c"
fi
if [ "$mode" = road ]; then
    name=Road-Racer
    cp "$root/road-racer/road.h" "$stage/src/road.h"
    cp "$root/road-racer/rocket.c" "$stage/src/road_rocket.c"
    cp "$root/road-racer/visuals.c" "$stage/src/road_visuals.c"
    node "$root/road-racer/prepare-rom.mjs" "$stage"
fi
make -C "$stage" GBDK_HOME="$stage/gbdk/" CART_TYPE=mbc5 PROJECTNAME="$name" gb
out="$root/canyon-racer/build/gb"
if [ "$mode" = road ]; then out="$root/road-racer/build"; fi
mkdir -p "$out"
cp "$stage/build/gb/$name.gb" "$out/"
printf '%s\n' "$out/$name.gb"
