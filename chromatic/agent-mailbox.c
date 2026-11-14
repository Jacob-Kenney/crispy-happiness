/* Canyon Racer mailbox controller, CC BY-NC-SA 4.0. */
#include <gb/gb.h>
#include <stdint.h>
#include "common.h"
#include "entity_ship.h"
#include "gfx.h"
#include "lives.h"
#include "../res/sprite_pause.h"
#define CANYON_MAILBOX
#ifdef ROAD_RACER
#include "road.h"
uint8_t road_request;
uint16_t road_request_id, road_ack;
#define MAILBOX_REPLY_SIZE ROAD_REPLY_SIZE
#else
#define MAILBOX_REPLY_SIZE 5u
#endif
#include "agent_state.h"

#define NW_ID (*(volatile uint8_t *)0xff58u)
#define NW_VER (*(volatile uint8_t *)0xff59u)
#define NW_CMD (*(volatile uint8_t *)0xff5au)
#define NW_STAT (*(volatile uint8_t *)0xff5bu)
#define NW_PTRL (*(volatile uint8_t *)0xff5cu)
#define NW_PTRH (*(volatile uint8_t *)0xff5du)
__sfr __at (0xff5e) NW_DATA;

#define NW_BUSY 1u
#define NW_READY 2u
#define NW_SVC 8u
#define REQUEST_FRAMES 4u
#define MAX_INPUT_AGE 6u
#define METRICS_WORDS 13u
#define MENU_PERIOD 16u

extern uint8_t game_state;
extern uint8_t spr_next_free_tile;
static uint8_t available;
static uint8_t bot_enabled = 1u;
static uint8_t previous_local_keys;
static uint8_t show_pause_mode;
static uint8_t outstanding;
static uint8_t ready;
static uint8_t ready_age;
static uint8_t answer;
static uint8_t active_keys;
static uint8_t was_playing;
static uint8_t action_frames;
static uint8_t menu_tick;
static uint16_t sent_at, next_send;
static uint16_t metrics[METRICS_WORDS];
static uint16_t valid_replies, rejected_replies, expired_replies;
static uint8_t last_reply_age, last_status, last_version, last_length;

enum {
    SCHEDULED, SENT, APPLIED, LATE, AGE_1, AGE_2, AGE_3, AGE_4, AGE_5, AGE_6,
    RACE, LOCAL_OVERRIDE, VBLANK,
};

static void rewind_window(void)
{
    NW_PTRL = 0;
    NW_PTRH = 0;
}

static void write_snapshot(const uint8_t *bytes) __sdcccall(1) __naked
{
    bytes;
    __asm
        ld h, d
        ld l, e
        ld c, #0x5e
        .rept 64
            ld a, (hl+)
            ldh (c), a
        .endm
        ret
    __endasm;
}

static void write_metrics(const uint16_t *words) __sdcccall(1) __naked
{
    words;
    __asm
        ld h, d
        ld l, e
        ld c, #0x5e
        .rept 26
            ld a, (hl+)
            ldh (c), a
        .endm
        ret
    __endasm;
}

void agent_link_init(void)
{
    NW_ID = 'S';
    NW_ID = 'G';
    NW_ID = 'N';
    NW_ID = 'W';
    available = NW_ID == 'N' && (NW_VER >> 4) == 1u;
}

static void read_reply(void)
{
    rewind_window();
    uint8_t version = NW_DATA;
    uint8_t status = NW_DATA;
    (void)NW_DATA;
    (void)NW_DATA;
    uint8_t len = NW_DATA;
    uint8_t hi = NW_DATA;
    NW_PTRL = 10;
    last_status = status;
    last_version = version;
    last_length = len;
    if (outstanding && version == 1u && status == 0u && len == MAILBOX_REPLY_SIZE && hi == 0u) {
        uint8_t magic0 = NW_DATA;
        uint8_t magic1 = NW_DATA;
        uint8_t low = NW_DATA;
        uint8_t high = NW_DATA;
        uint8_t keys = NW_DATA;
        uint16_t seq = (uint16_t)low | ((uint16_t)high << 8);
        if (magic0 == 'C' && magic1 == 'A' && seq == (uint16_t)(sequence - 1u)) {
            ready = 1;
            ready_age = (uint8_t)(sys_time - sent_at);
            last_reply_age = ready_age;
            answer = keys;
            ++valid_replies;
#ifdef ROAD_RACER
            uint8_t kind = NW_DATA;
            uint8_t id_low = NW_DATA;
            uint8_t id_high = NW_DATA;
            uint16_t id = (uint16_t)id_low | ((uint16_t)id_high << 8);
            if (was_playing && kind && kind <= ROAD_CONE_RIGHT && id && id != road_ack) {
                road_request = kind;
                road_request_id = id;
            }
#endif
        }
    }
    if (outstanding && !ready) ++rejected_replies;
    NW_CMD = 2u;
    outstanding = 0;
}

static void abort_request(void)
{
    if (outstanding) NW_CMD = 3u;
    outstanding = 0;
    ready = 0;
}

void agent_link_pause(void)
{
    if (!available) return;
    abort_request();
    active_keys = 0;
    action_frames = 0;
    was_playing = 0;
    show_pause_mode = 1u;
}

static void display_pause_mode(void)
{
    // Use black letters with a white outline so they read over the canyon.
#define ROW(ink, above, below) ((ink) | (ink) << 1 | (ink) >> 1 | (above) | (below)), (ink)
#define LETTER(a, b, c, d, e, f, g) \
    ROW(0, 0, a), ROW(a, 0, b), ROW(b, a, c), ROW(c, b, d), \
    ROW(d, c, e), ROW(e, d, f), ROW(f, e, g), ROW(g, f, 0), ROW(0, g, 0), \
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    static const uint8_t mode_tiles[2][96] = {
        { LETTER(0x44, 0x44, 0x28, 0x10, 0x10, 0x10, 0x10),  // Y
          LETTER(0x38, 0x44, 0x44, 0x44, 0x44, 0x44, 0x38),  // O
          LETTER(0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x38) }, // U
        { LETTER(0x78, 0x44, 0x44, 0x78, 0x44, 0x44, 0x78),  // B
          LETTER(0x38, 0x44, 0x44, 0x44, 0x44, 0x44, 0x38),  // O
          LETTER(0x7c, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10) }, // T
    };
#undef LETTER
#undef ROW
    const uint8_t first_tile = spr_next_free_tile + sprite_pause_TILE_COUNT;
    const uint8_t first_sprite = MAX_HARDWARE_SPRITES - sprite_pause_TILE_COUNT / 2u - 3u;
    set_sprite_data(first_tile, 6u, mode_tiles[bot_enabled]);
    for (uint8_t i = 0; i < 3u; ++i) {
        set_sprite_prop(first_sprite + i, 0u);
        set_sprite_tile(first_sprite + i, first_tile + 2u * i);
        move_sprite(first_sprite + i, 76u + 8u * i, GAMEPLAY_NOTICE_SPRITE_Y + 19u);
    }
}

static void send_state(uint8_t playing, uint8_t local_keys)
{
    if ((NW_STAT & (NW_BUSY | NW_SVC)) != NW_SVC) return;
    metrics[VBLANK] = sys_time;
    encode_state(playing, local_keys, active_keys, game_state);
    packet[37] = (uint8_t)valid_replies;
    packet[38] = (uint8_t)(valid_replies >> 8);
    packet[39] = (uint8_t)rejected_replies;
    packet[40] = (uint8_t)(rejected_replies >> 8);
    packet[41] = (uint8_t)expired_replies;
    packet[42] = (uint8_t)(expired_replies >> 8);
    packet[43] = last_reply_age;
    packet[44] = last_status;
    packet[45] = last_version;
    packet[46] = last_length;
    packet[63] = 0;
#ifdef ROAD_RACER
    packet[47] = (uint8_t)road_ack;
    packet[48] = (uint8_t)(road_ack >> 8);
    packet[49] = (uint8_t)road_rocket_hits;
    packet[50] = (uint8_t)(road_rocket_hits >> 8);
    packet[51] = road_rocket_x;
    packet[52] = road_rocket_y;
    packet[53] = road_rocket_cooldown;
    packet[54] = 4u;
    packet[37] = road_splatter_x;
    packet[38] = road_splatter_y;
    packet[39] = road_splatter_frames;
    packet[40] = road_impact_kind;
    packet[41] = bot_enabled;
    packet[63] = ROAD_MARKER;
#endif
    rewind_window();
    NW_DATA = 1;
    NW_DATA = 0x30;
    NW_DATA = 0;
    NW_DATA = 0;
    write_snapshot(packet);
    write_metrics(metrics);
    NW_CMD = 1;
    sent_at = metrics[VBLANK];
    outstanding = 1;
}

uint8_t agent_link_keys(uint8_t local_keys)
{
    if (!available) return local_keys;
    const uint8_t pressed = local_keys & ~previous_local_keys;
    previous_local_keys = local_keys;
    if (game_state == GAME_STATE_RUNNING && state.paused) {
        if (pressed & J_SELECT) {
            bot_enabled ^= 1u;
            show_pause_mode = 1u;
        }
        if (show_pause_mode) {
            display_pause_mode();
            show_pause_mode = 0;
        }
        if (pressed & J_START) {
            hide_sprites_range(MAX_HARDWARE_SPRITES - sprite_pause_TILE_COUNT / 2u - 3u,
                               MAX_HARDWARE_SPRITES - sprite_pause_TILE_COUNT / 2u);
        }
#ifdef ROAD_RACER
        // Keep the browser informed while the simulation is paused.
        if (NW_STAT & NW_READY) read_reply();
        if (++menu_tick >= MENU_PERIOD) {
            menu_tick = 0;
            abort_request();
            send_state(1, local_keys);
        }
#endif
        return local_keys;
    }
#ifndef ROAD_RACER
    if (!bot_enabled) return local_keys;
#endif

    const uint8_t playing = game_state == GAME_STATE_RUNNING;
    if (playing != was_playing) {
        abort_request();
        active_keys = 0;
        action_frames = 0;
        next_send = sys_time;
        menu_tick = MENU_PERIOD - 1u;
        if (playing) ++metrics[RACE];
        was_playing = playing;
    }

    if (NW_STAT & NW_READY) read_reply();
    if (!playing) {
        uint8_t pulse = 0;
        if (ready) {
            if (bot_enabled) pulse = answer & J_START;
            ready = 0;
        }
        if (++menu_tick == MENU_PERIOD) {
            menu_tick = 0;
            abort_request();
            send_state(0, local_keys);
        }
        return local_keys | pulse;
    }

    ++frame;
    if (action_frames && !--action_frames) active_keys = 0;
    if (ready) {
        if (ready_age >= 1u && ready_age <= MAX_INPUT_AGE) {
            ++metrics[AGE_1 + ready_age - 1u];
            if (ready_age > REQUEST_FRAMES) ++metrics[LATE];
            if (local_keys) ++metrics[LOCAL_OVERRIDE];
            else if (bot_enabled) {
                active_keys = answer & (J_LEFT | J_RIGHT | J_UP | J_DOWN | J_A | J_B);
                action_frames = REQUEST_FRAMES;
                ++metrics[APPLIED];
            }
        } else {
            ++metrics[LATE];
            ++expired_replies;
        }
        ready = 0;
    }

    uint16_t now = sys_time;
    if ((int16_t)(now - next_send) >= 0) {
        if (outstanding && (uint16_t)(now - sent_at) < MAX_INPUT_AGE) {
            return local_keys ? local_keys : active_keys;
        }
        if (outstanding) {
            ++metrics[LATE];
            ++expired_replies;
        }
        abort_request();
        ++metrics[SCHEDULED];
        send_state(1, local_keys);
        if (outstanding) ++metrics[SENT];
        next_send = now + REQUEST_FRAMES;
    }
    return local_keys ? local_keys : active_keys;
}
