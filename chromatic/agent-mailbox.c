/* Canyon Racer mailbox controller, CC BY-NC-SA 4.0. */
#include <gb/gb.h>
#include <stdint.h>
#include "common.h"
#include "entity_ship.h"
#include "lives.h"
#define CANYON_MAILBOX
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
static uint8_t available;
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
    if (outstanding && version == 1u && status == 0u && len == 5u && hi == 0u) {
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
    const uint8_t playing = game_state == GAME_STATE_RUNNING && !state.paused;
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
            pulse = answer & J_START;
            ready = 0;
        }
        if (++menu_tick == MENU_PERIOD) {
            menu_tick = 0;
            abort_request();
            // Keep controls local during a manual pause.
            if (game_state != GAME_STATE_RUNNING) send_state(0, local_keys);
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
            else {
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
