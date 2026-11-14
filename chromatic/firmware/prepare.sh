#!/bin/sh
set -eu
assets=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "${1:?Pass the workspace containing .chromatic}"
src=.chromatic/shiny-relay-src-1.1.12/mcu/components/nw/nw_esp.c
target=.chromatic/bridge-build/mcu/components/nw
cp "$assets"/canyon_mailbox.inc "$assets"/train_mailbox.inc "$target/"
cp "$assets"/../../last-train/protocol.h "$target/train_protocol.h"
cp "$assets"/../../road-racer/road.h "$target/road_protocol.h"
sed '
/^\/\* ---------------- request path/i\
#include "canyon_mailbox.inc"\
#include "train_mailbox.inc"\

/^    if (op == NW_OP_INFO)/i\
    if (op == CANYON_OP) { canyon_start(e, body, blen); return; }\
    if (op == TRAIN_OP) { train_start(e, body, blen); return; }\

/^void nw_esp_tick(nw_esp_t \*e, uint32_t now)/,+2 {
    /e->now = now;/a\
    canyon_tick(e);\
    train_tick(e);
}
' "$src" > "$target/nw_esp.c"
grep -q 'canyon_tick(e);' "$target/nw_esp.c"
grep -q 'train_tick(e);' "$target/nw_esp.c"

src=.chromatic/shiny-relay-src-1.1.12/mcu/components/nw/cartflash/cf_rc_dev.c
target=.chromatic/bridge-build/mcu/components/nw/cartflash
cp "$assets"/canyon-autostart.inc "$target/"
sed '
/^static void rc_task(void \*arg)$/i\
#include "canyon-autostart.inc"\

/^        lib_ops();/a\
        canyon_play_saved();

/^    game_load();/a\
    canyon_autostart = true;
' "$src" > "$target/cf_rc_dev.c"
grep -Fq 'canyon_play_saved();' "$target/cf_rc_dev.c"
