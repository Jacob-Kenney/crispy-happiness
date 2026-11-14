#include <gb/gb.h>
#include <stdint.h>
#include "common.h"
#include "audio/audio.h"
#include "input.h"
#include "entity_ship.h"
#include "road.h"

#define ROCKET_TILE (SPR_TILES_START_OBSTACLES + 6u)
#define ROCKET_SPEED 6u
#define ROCKET_COOLDOWN 24u
#define SPLATTER_TILE (SPR_TILES_START_OBSTACLES + 8u)
#define FEATHERS_TILE (SPR_TILES_START_OBSTACLES + 12u)
#define SPARKS_TILE (SPR_TILES_START_OBSTACLES + 14u)
#define SPLATTER_DURATION 24u

// CBT-FX channel 4. Start with a sharp burst and lower the pitch as it fades.
const uint8_t road_rocket_sfx[] = {
    0x23, 5u,                 // Noise channel, priority 3, five steps.
    0x81, 0x88, 0x0D, 0x21,   // Pan to both speakers, then volume and noise pitch.
    0x02, 0x0A, 0x33,
    0x02, 0x07, 0x45,
    0x01, 0x03, 0x54,
    0x00, 0x00, 0x54,
};

uint8_t road_rocket_x, road_rocket_y, road_rocket_cooldown;
uint16_t road_rocket_hits;
uint8_t road_splatter_x, road_splatter_y, road_splatter_frames;
uint8_t road_impact_kind;

void road_rocket_reset(void)
{
    road_rocket_y = 0;
    road_rocket_cooldown = 0;
    road_splatter_frames = 0;
}

void road_obstacle_hit(uint8_t index, uint8_t left)
{
    obs_ent *obstacle = &state.obstacles[index];
    if (obstacle->type <= ROAD_CHICKEN_RIGHT) road_impact_kind = ROAD_IMPACT_CHICKEN;
    else if (obstacle->type <= ROAD_WALKER_RIGHT) road_impact_kind = ROAD_IMPACT_PEDESTRIAN;
    else road_impact_kind = ROAD_IMPACT_CONE;
    road_splatter_x = left;
    road_splatter_y = obstacle->y.h;
    road_splatter_frames = SPLATTER_DURATION;
    obstacle->type |= OBJECTS_FLAG_HIDDEN_BIT;
}

static uint8_t update_rocket(uint8_t sprite)
{
    if (!(SHIP_STATE_GET() & SHIP_BITS_PLAYER_ACTIVE)) {
        road_rocket_y = 0;
        return sprite;
    }

    if (road_rocket_cooldown) --road_rocket_cooldown;
    if ((KEYS() & J_A) && !road_rocket_y && !road_rocket_cooldown) {
        road_rocket_x = state.ship_x.h + 12u;
        road_rocket_y = state.ship_y.h + 5u;
        road_rocket_cooldown = ROCKET_COOLDOWN;
        audio_sfx_play(ROAD_SFX_ROCKET);
    }
    if (!road_rocket_y) return sprite;

    const uint8_t old_y = road_rocket_y;
    road_rocket_y = old_y > ROCKET_SPEED ? old_y - ROCKET_SPEED : 0;
    uint8_t idx = state.obstacles_active_first;
    for (uint8_t count = state.obstacles_active_count; count; --count) {
        obs_ent *obstacle = &state.obstacles[idx];
        const uint8_t y = obstacle->y.h;
        if (!(obstacle->type & OBJECTS_FLAG_HIDDEN_BIT) && y < 144u &&
            y <= old_y && y + 8u >= road_rocket_y) {
            const uint8_t left = CANYON_LEFT_X_BASE - state.p_scx_table_frame_base[y] +
                road_offset(obstacle->type, y);
            if (road_rocket_x >= left && road_rocket_x < left + 8u) {
                // Keep FIFO ordering so the existing score and stage progression still run.
                road_obstacle_hit(idx, left);
                ++road_rocket_hits;
                road_rocket_y = 0;
                return sprite;
            }
        }
        if (++idx == OBSTACLES_COUNT_WRAP) idx = 0;
    }
    if (!road_rocket_y) return sprite;
    set_sprite_tile(sprite, ROCKET_TILE);
    set_sprite_prop(sprite, ROAD_PAL_FIRE);
    move_sprite(sprite, road_rocket_x + 4u, road_rocket_y + 12u);
    return sprite + 1u;
}

uint8_t road_rocket_update(uint8_t sprite)
{
    sprite = update_rocket(sprite);
    if (!road_splatter_frames) return sprite;
    const uint8_t tile = road_impact_kind == ROAD_IMPACT_CHICKEN ? FEATHERS_TILE :
        road_impact_kind == ROAD_IMPACT_CONE ? SPARKS_TILE :
        SPLATTER_TILE + (road_splatter_frames > 17u ? 0u : 2u);
    set_sprite_tile(sprite, tile);
    set_sprite_prop(sprite, road_impact_kind == ROAD_IMPACT_CHICKEN ? ROAD_PAL_CHICKEN :
        road_impact_kind == ROAD_IMPACT_CONE ? ROAD_PAL_FIRE : ROAD_PAL_IMPACT);
    move_sprite(sprite, road_splatter_x + 8u, road_splatter_y + 16u);
    --road_splatter_frames;
    return sprite + 1u;
}
