#ifndef ROAD_RACER_H
#define ROAD_RACER_H
#define ROAD_MARKER 0x52
#define ROAD_PORT 47823
#define ROAD_REPLY_SIZE 8

#define ROAD_CHICKEN_LEFT 1
#define ROAD_CHICKEN_RIGHT 2
#define ROAD_WALKER_LEFT 3
#define ROAD_WALKER_RIGHT 4
#define ROAD_CONE_LEFT 5
#define ROAD_CONE_CENTER 6
#define ROAD_CONE_RIGHT 7
#define ROAD_IMPACT_PEDESTRIAN 1
#define ROAD_IMPACT_CHICKEN 2
#define ROAD_IMPACT_CONE 3

#ifdef ROAD_RACER
#define ROAD_SFX_ROCKET 6u
#define ROAD_PAL_CAR 1u
#define ROAD_PAL_CHICKEN 2u
#define ROAD_PAL_PEDESTRIAN 3u
#define ROAD_PAL_CONE 4u
#define ROAD_PAL_FIRE 5u
#define ROAD_PAL_IMPACT 6u
extern const uint8_t road_rocket_sfx[];
void road_color_fade(uint8_t darkness);
extern uint8_t road_request;
extern uint16_t road_request_id, road_ack;
extern uint8_t road_rocket_x, road_rocket_y, road_rocket_cooldown;
extern uint16_t road_rocket_hits;
extern uint8_t road_splatter_x, road_splatter_y, road_splatter_frames;
extern uint8_t road_impact_kind;
void road_obstacle_hit(uint8_t index, uint8_t left);
void road_rocket_reset(void);
uint8_t road_rocket_update(uint8_t sprite);

static inline uint8_t road_offset(uint8_t kind, uint8_t y)
{
    if (kind >= ROAD_CONE_LEFT) return 2u + 11u * (kind - ROAD_CONE_LEFT);
    uint8_t advance = 0;
    if (y > 48u) advance = (y - 48u) >> (kind <= ROAD_CHICKEN_RIGHT ? 1u : 2u);
    if (advance > 24u) advance = 24u;
    return kind & 1u ? advance : 24u - advance;
}
#endif
#endif
