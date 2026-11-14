import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import './build-art.mjs';

const root = process.argv[2];
function edit(path, edits) {
  const file = join(root, path);
  let source = readFileSync(file, 'utf8');
  for (const [from, to] of edits) {
    if (typeof from === 'string' ? !source.includes(from) : !from.test(source)) {
      throw new Error(path + ': missing ' + from);
    }
    source = source.replace(from, to);
  }
  writeFileSync(file, source);
}
edit('Makefile', [
  ['CFLAGS += -DCART_', 'CFLAGS += -DROAD_RACER -DCART_'],
  ['yn"CANYONRACER"', 'yn"RLRACER" -Wm-yc'],
]);
edit('src/fade.c', [
  ['#include "fade.h"', '#include "fade.h"\n#include "road.h"'],
  ['BGP_REG  = fade_steps_dmg_BGP[c];', 'road_color_fade(c);\n        BGP_REG  = fade_steps_dmg_BGP[c];'],
  ['BGP_REG  = fade_steps_dmg_BGP[ARRAY_LEN', 'road_color_fade(DMG_FADE_STEP_COUNT - 1u - c);\n        BGP_REG  = fade_steps_dmg_BGP[ARRAY_LEN'],
]);
edit('src/gfx.c', [
  ['#include "../res/map_canyon.h"', '#include "../res/map_canyon.h"\nextern const uint8_t road_map_attributes[80];'],
  ['    set_bkg_tiles(0,0,', '    fill_bkg_rect(0, 0, 32, 32, 0);\n    set_bkg_tiles(0,0,'],
  ['    bg_next_free_tile += map_canyon_TILE_COUNT;',
    '    if (_cpu == CGB_TYPE) {\n        VBK_REG = VBK_ATTRIBUTES;\n        fill_bkg_rect(0, 0, 32, 32, 1);\n        VBK_REG = VBK_TILES;\n        for (uint8_t y = 0; y < 32; y += 4)\n            set_bkg_attributes(0, y, 20, 4, road_map_attributes);\n    }\n    bg_next_free_tile += map_canyon_TILE_COUNT;'],
]);
edit('src/audio/audio.c', [
  ['#include "audio.h"', '#include "audio.h"\n#include "../road.h"'],
  ['    &SFX_12[0], // speedup   5', '    &SFX_12[0], // speedup   5\n    [ROAD_SFX_ROCKET] = road_rocket_sfx,'],
]);
edit('src/entity_obstacles.c', [
  ['#include "input.h"', '#include "input.h"\n#include "road.h"'],
  [/static inline void queue_next\(void\) \{[\s\S]*?\n\}/, ''],
  ['    queue_next();', '    // Obstacles arrive from the browser.'],
  [/                oam_high_water \+= move_metasprite\([\s\S]*?y_pos\);/,
   '                const uint8_t kind = object_type & OBJECT_SPRITE_MASK;\n' +
   '                set_sprite_tile(oam_high_water, SPR_TILES_START_OBSTACLES + (kind <= 2 ? 0 : kind <= 4 ? 2 : 4));\n' +
   '                set_sprite_prop(oam_high_water, kind <= 2 ? ROAD_PAL_CHICKEN : kind <= 4 ? ROAD_PAL_PEDESTRIAN : ROAD_PAL_CONE);\n' +
   '                move_sprite(oam_high_water++, CANYON_LEFT_X_BASE - state.p_scx_table_frame_base[y_pos] + road_offset(kind, y_pos) + 8, y_pos + 16);'],
  ['(object_type & OBJECT_TYPE_ITEM_MASK) <= OBSTACLE_TYPE_FULL', '(object_type & OBJECT_TYPE_ITEM_MASK) != 0'],
  ['            state.obstacles_next_countdown--;\n        } else {', '            state.obstacles_next_countdown--;\n        } else if (road_request) {'],
  [/            \/\/ Override the obstacle[\s\S]*?(?=            \/\/ If this is first entry)/,
   '            state.obstacles[state.obstacles_active_last].type = road_request;\n' +
   '            road_ack = road_request_id;\n            road_request = 0;\n\n'],
  ['            queue_next();\n', '            state.obstacles_next_countdown = 22u;\n'],
]);
edit('src/entity_collisions.c', [
  ['#include "entity_obstacles.h"', '#include "entity_obstacles.h"\n#include "road.h"'],
  ['uint8_t obstacle_x_hitbox_left  = obstacle_x_pos + obstacles_x_hitbox_left[collision_last_type];',
   'uint8_t obstacle_x_hitbox_left = obstacle_x_pos + road_offset(collision_last_type, obstacle_y_pos);'],
  ['obstacle_x_pos + obstacles_x_hitbox_right[collision_last_type]', 'obstacle_x_hitbox_left + 8u'],
  ['                    collision_last_index = idx;',
   '                    road_obstacle_hit(idx, obstacle_x_hitbox_left);\n                    collision_last_index = idx;'],
]);
edit('src/entity_ship.c', [
  [/        if \(COLLISION_GET_LAST_TYPE\(\) == OBJECT_TYPE_ITEM_PLUS_1\) \{[\s\S]*?        else \{/, '        {'],
  [/    \/\/ Jumping\n[\s\S]*?(?=\n\}\n\n\nstatic void ship_handle_collisions)/, '    // This car stays on the road.\n'],
]);
edit('src/main.c', [
  [/                if \(return_button == J_A\) \{[\s\S]*?(?=                break;)/,
   '                if (return_button & (J_START | J_A)) game_state = GAME_STATE_START_GAME;\n'],
  ['                    intro_credits_show();', '                    // Go straight to the road title.'],
]);
edit('src/lives.h', [
  ['MAKE_BCD(10) // Initial number of lives / save states', 'MAKE_BCD(3) // Includes the current attempt'],
]);
edit('src/lives.c', [
  [/    if \(IS_MAGIC_CODE_ACTIVE\)\n        lives_count = LIVES_COUNT_CHEAT;\n    else\n        lives_count = LIVES_COUNT_RESET;/,
   '    lives_count = LIVES_COUNT_RESET;'],
]);
edit('src/gameplay.c', [
  ['#include "input.h"', '#include "input.h"\n#include "road.h"'],
  ['    lives_count_reset();', '    road_rocket_reset();\n    lives_count_reset();'],
  ['        // Deduct Life / Restore Point (not part of save state)\n        LIVES_COUNT_SUBTRACT_ONE();\n',
   '        // Saving a checkpoint does not cost a life in RL RACER.\n'],
  ['        // Deduct Life / Restore Point and update display counters\n        LIVES_COUNT_SUBTRACT_ONE();',
   '        // Refresh the HUD after restoring the checkpoint.'],
  ['            // Try to restore if the user has rewinds left',
   '            // Spend the current life before deciding whether to rewind.\n' +
   '            LIVES_COUNT_SUBTRACT_ONE();\n' +
   '            lives_display_update();'],
  ['        oam_high_water = entity_obstacles_update(oam_high_water);',
   '        oam_high_water = road_rocket_update(oam_high_water);\n        oam_high_water = entity_obstacles_update(oam_high_water);'],
]);
edit('src/splash_screen.c', [
  ['#include "splash_screen.h"', '#include "splash_screen.h"\n#include "road_title.h"'],
  [/    \/\/ This doesn't accept arbitrary tile count[\s\S]*?(?=    \/\/ Window start)/,
   '    set_bkg_data(bg_next_free_tile, sizeof road_font / 16u, road_font);\n' +
   '    fill_win_rect(0, 0, 22, 13, bg_next_free_tile);\n' +
   '    set_win_based_tiles(7, 4, sizeof road_title, 1, road_title, bg_next_free_tile);\n' +
   '    set_win_based_tiles(6, 8, sizeof road_start, 1, road_start, bg_next_free_tile);\n\n'],
]);
