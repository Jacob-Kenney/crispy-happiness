/* Canyon Racer agent link, distributed under the game's CC BY-NC-SA 4.0. */
#include <gb/gb.h>
#include <stdint.h>

#include "common.h"
#include "entity_ship.h"
#include "lives.h"
#include "map_fx.h"

#include "agent_state.h"
#define CONTROL_MAGIC 0xA5u
#define DECISION_MAGIC 0xA6u
#define CONTROL_XOR 0x5Au

extern uint8_t game_state;
static volatile uint8_t sent = PACKET_SIZE;
static volatile uint8_t remote_keys;
static volatile uint8_t control_epoch;
static volatile uint8_t decision_epoch;
static volatile uint8_t decision_keys;
static volatile uint8_t waiting_for_decision;
static uint8_t last_epoch;
static uint8_t control_age;
static uint8_t control_phase;
static uint8_t pending_keys;
static uint8_t control_kind;
static uint8_t policy_keys;
static uint8_t policy_frames;
static uint8_t busy_frames;

static void start_byte(void) {
    SB_REG = packet[sent++];
    SC_REG = SIOF_XFER_START | SIOF_CLOCK_INT;
}

static void serial_complete(void) {
    uint8_t byte = SB_REG;
    if (control_phase == 0u) {
        if (byte == CONTROL_MAGIC || byte == DECISION_MAGIC) {
            control_kind = byte;
            control_phase = 1u;
        }
    } else if (control_phase == 1u) {
        pending_keys = byte;
        control_phase = 2u;
    } else {
        if (byte == (pending_keys ^ CONTROL_XOR)) {
            if (control_kind == DECISION_MAGIC && (!waiting_for_decision || sent == PACKET_SIZE)) {
                decision_keys = pending_keys;
                ++decision_epoch;
            } else if (control_kind == CONTROL_MAGIC) {
                remote_keys = pending_keys;
                ++control_epoch;
            }
            control_phase = 0u;
        } else {
            control_kind = byte;
            control_phase = byte == CONTROL_MAGIC || byte == DECISION_MAGIC;
        }
    }
    if (sent < PACKET_SIZE) start_byte();
}

void agent_link_init(void) {
    disable_interrupts();
    add_SIO(serial_complete);
    IF_REG &= ~SIO_IFLAG;
    set_interrupts(IE_REG | SIO_IFLAG);
    enable_interrupts();
}

static void send_state(uint8_t playing, uint8_t local_keys, uint8_t input, uint8_t paced) {
    encode_state(playing, local_keys, input, paced);
    packet[63] = crc8();
    sent = 0;
    start_byte();
}

uint8_t agent_link_keys(uint8_t local_keys) {
    uint8_t epoch = control_epoch;
    if (epoch != last_epoch) {
        last_epoch = epoch;
        control_age = 0;
    } else if (control_age < 45u) {
        ++control_age;
    }

    uint8_t input = control_age < 45u ? remote_keys : 0u;
    uint8_t playing = game_state == GAME_STATE_RUNNING;
    ++frame;
    if (sent == PACKET_SIZE && !(SC_REG & SIOF_XFER_START)) busy_frames = 0u;
    else if (++busy_frames == 180u) {
        __critical {
            SC_REG = 0;
            IF_REG &= ~SIO_IFLAG;
            sent = PACKET_SIZE;
            control_phase = 0u;
        }
        busy_frames = 0u;
    }
    if (!playing || state.paused) policy_frames = 0u;

    // A recent cable reply opts in to frame-stepped play. Keep the console
    // responsive with a deadline and let physical input interrupt the wait.
    if (playing && !state.paused && decision_epoch && control_age < 45u && !local_keys) {
        if (policy_frames) {
            --policy_frames;
            return policy_keys;
        }
        for (uint8_t drain = 0; drain < 12u && (SC_REG & SIOF_XFER_START); ++drain) vsync();
        if (sent == PACKET_SIZE && !(SC_REG & SIOF_XFER_START)) {
            uint8_t decision = decision_epoch;
            mapfx_set_setpause(true);
            waiting_for_decision = 1u;
            send_state(playing, local_keys, policy_keys, 1u);
            for (uint8_t wait = 0; wait < 210u && decision == decision_epoch; ++wait) {
                vsync();
                local_keys = joypad();
                if (local_keys) break;
                if (sent == PACKET_SIZE && !(SC_REG & SIOF_XFER_START)) {
                    SB_REG = 0;
                    SC_REG = SIOF_XFER_START | SIOF_CLOCK_INT;
                }
            }
            waiting_for_decision = 0u;
            mapfx_set_setpause(false);
            busy_frames = 0u;
            policy_keys = local_keys || decision == decision_epoch ? 0u : decision_keys;
            policy_frames = 3u;
            return local_keys | policy_keys;
        }
    }

    if ((frame & 7u) == 0u && sent == PACKET_SIZE && !(SC_REG & SIOF_XFER_START)) {
        send_state(playing, local_keys, input, 2u);
    }
    return local_keys | input;
}
