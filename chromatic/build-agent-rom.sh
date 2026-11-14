#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
: "${GBDK_HOME:?Set GBDK_HOME to the GBDK installation directory}"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
ln -s "${GBDK_HOME%/}" "$stage/gbdk"
git -C "$root/canyon-racer" archive HEAD | tar -xf - -C "$stage"
cp "$root/chromatic/agent-state.h" "$stage/src/agent_state.h"
cp "$root/chromatic/agent-${1:-link}.c" "$stage/src/agent_link.c"
sed -i.bak 's/#define UPDATE_KEYS().*/uint8_t agent_link_keys(uint8_t local_keys);\n#define UPDATE_KEYS() previous_keys = keys; keys = agent_link_keys(joypad())/' "$stage/src/input.h"
sed -i.bak '/^void main(void) {/i\
extern void agent_link_init(void);\
' "$stage/src/main.c"
sed -i.bak '/^    init();$/a\
    agent_link_init();\
' "$stage/src/main.c"
name=canyon_racer_0.7.7_agent_mbc5
if [ "${1:-link}" = mailbox ]; then name=canyon_racer_0.7.7_mailbox_mbc5; fi
make -C "$stage" GBDK_HOME="$stage/gbdk/" CART_TYPE=mbc5 PROJECTNAME="$name" gb
out="$root/canyon-racer/build/gb"
mkdir -p "$out"
cp "$stage/build/gb/$name.gb" "$out/"
printf '%s\n' "$out/$name.gb"
