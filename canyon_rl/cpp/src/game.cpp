// Canyon Racer game-logic port: simulation.
//
// Faithful C++17 transcription of the gameplay loop from canyon-racer/src:
// gameplay.c, entity_ship.c, entity_obstacles.c, entity_collisions.c,
// level.c, map_fx.c and common.c.  Rendering, audio, menus, cart saves and
// hardware timing are intentionally omitted.
//
// Original work (c) 2023 bbbbbr, CC-BY-NC-SA 4.0.

#include "canyon/game.hpp"

#include <cassert>
#include <cstring>

namespace canyon {
namespace {

constexpr int kLevelCount = 16;

// Level table (verbatim from level.c, including raw pre-speed-scale gaps).
const LevelEntry kLevelData[kLevelCount] = {
    {kMapfxLvlEasy, kMapfxScxNormal, 0x03, 0, 2, 500, 0, 90 * 256, 0},
    {kMapfxLvlEasy, kMapfxScxNormal, 0x03, 0, 2, 500, 0, 85 * 256, 0},
    {kMapfxLvlMedLo, kMapfxScxNormal, 0x03, 0, 2, 500, 0, 85 * 256, 0},
    {kMapfxLvlMedLo, kMapfxScxNormal, 0x03, 0, 2, 500, 0, 85 * 256, 0},
    {kMapfxLvlEasy, kMapfxScxFast, 0x03, 0, 2, 500, 0, 90 * 256, 0},

    {kMapfxLvlMedLo, kMapfxScxNormal, 0x03, 0, 3, 550, 0, 85 * 256, 0},
    {kMapfxLvlMed, kMapfxScxNormal, 0x03, 0, 3, 550, 0, 85 * 256, 0},
    {kMapfxLvlMedHi, kMapfxScxNormal, 0x03, 0, 3, 550, 0, 85 * 256, 0},
    {kMapfxLvlHard, kMapfxScxNormal, 0x03, 0, 3, 575, 0, 85 * 256, 0},
    {kMapfxLvlHard, kMapfxScxNormal, 0x03, 0, 3, 575, 0, 85 * 256, 0},

    {kMapfxLvlEasy, kMapfxScxFast, 0x03, 0, 3, 597, 0, 90 * 256, 0},
    {kMapfxLvlMedLo, kMapfxScxFast, 0x03, 0, 4, 597, 0, 85 * 256, 0},
    {kMapfxLvlMed, kMapfxScxFast, 0x03, 0, 4, 597, 0, 85 * 256, 0},
    {kMapfxLvlMedHi, kMapfxScxFast, 0x03, 0, 4, 597, 0, 85 * 256, 0},
    {kMapfxLvlHard, kMapfxScxFast, 0x03, 0, 4, 597, 0, 85 * 256, 0},
    {kMapfxLvlHard, kMapfxScxFast, 0x03, 0, 4, 597, 0, 55 * 256, 0},
};

// ---------------------------------------------------------------------------
// Small helpers matching GB uint8_t wrap-around arithmetic
// ---------------------------------------------------------------------------
inline uint8_t u8(int v) { return static_cast<uint8_t>(v & 0xFF); }

// Rows of the canyon have the same shape for at least sprite height, so keep
// the per-frame base index and index the merged table directly.
inline uint8_t canyon_left(const uint8_t* table, int32_t base, int y) {
  return u8(kCanyonLeftBase - table[base + y]);
}

}  // namespace

// ---------------------------------------------------------------------------
// Reset / init
// ---------------------------------------------------------------------------
void Game::reset(uint8_t lives, bool fast_crash, uint16_t seed) {
  lives_ = lives;
  fast_crash_ = fast_crash;

  ship_x_ = ship_y_ = ship_z_ = 0;
  ship_state_ = kStateOnGround;
  ship_counter_ = 0;
  ship_jump_velocity_ = 0;
  score_ = 0;
  frames_ = 0;
  sys_time_ = 0;
  rand_seed_ = 0;
  prev_input_ = {};

  level_init();
  mapfx_set_gameplay();

  initrand(seed ? seed : kGameplayStartRandInit);

  entity_ship_init();
  entity_obstacles_init();

  // Quiet initial save point (original gameplay_prestart()).
  gameplay_state_save();
}

void Game::level_init() {
  level_count_till_next_ = kLevelObstaclesTillNext;
  game_level_ = 0;
  level_update_vars();
}

void Game::level_update_vars() {
  cur_level_ = kLevelData[game_level_];

  // OBST_COUNT_FROM_SCANLINES(): scaled distance / obstacle speed.
  cur_level_.obst_dist_min =
      uint8_t(cur_level_.obst_dist_min_raw / cur_level_.obst_speed);
  cur_level_.obst_dist_double = uint8_t((10u * 256u) / cur_level_.obst_speed);

  mapfx_level_set(cur_level_.mapfx_scx_table_level);
}

void Game::level_increment() {
  level_count_till_next_ = kLevelObstaclesTillNext;
  if (game_level_ < kLevelCount - 1) {
    game_level_++;
    level_update_vars();
  }
}

// ---------------------------------------------------------------------------
// Map FX (canyon X wave scroll).  Pure state: which 144-byte window of the
// merged SCX table is used this frame.
// ---------------------------------------------------------------------------
void Game::mapfx_set_gameplay() {
  // Start on the "straight -> low" transition (SCX_TABLE_STR_LOW).
  const ScxTableEntry& t = kScxTables[13];
  scx_cur_ = t.start;
  scx_stop_ = t.stop;
  scx_frame_base_ = scx_cur_;

  // level_init() already called mapfx_level_set(0); keep the same here.
  mapfx_level_set(kMapfxLvlEasy);
}

void Game::mapfx_level_set(uint8_t level) {
  if (level > 4) level = 4;
  // scx_table_levels[] in lookup_tables.c
  static const uint8_t kMask[5] = {0x03, 0x07, 0x03, 0x07, 0x03};
  static const uint8_t kBase[5] = {0, 0, 4, 4, 8};
  mapfx_level_mask_ = kMask[level];
  mapfx_level_base_ = kBase[level];
}

void Game::vblank_map_reset() {
  // GBDK's VBL handler increments sys_time before user VBL handlers.
  sys_time_++;

  // The table pointer for this frame is latched before scrolling.
  scx_frame_base_ = scx_cur_;

  if (cur_level_.mapfx_scx_inc_every_frame || (sys_time_ & 1)) {
    scx_cur_ -= kMapfxScxSpeedMed;
    if (scx_cur_ == scx_stop_) {
      // Reached the end of this slice: pick the next canyon piece.
      uint8_t next = uint8_t((rand8() & mapfx_level_mask_) + mapfx_level_base_);
      if (next >= 15) next = 0;  // safety; the masks above keep it in range
      scx_cur_ = kScxTables[next].start;
      scx_stop_ = kScxTables[next].stop;
    }
  }
}

// ---------------------------------------------------------------------------
// Ship
// ---------------------------------------------------------------------------
void Game::entity_ship_init() {
  ship_x_ = (uint16_t)((kScreenW - kShipPivotW) / 2u) << kFixedBits;
  ship_y_ = kShipYMax;
  ship_z_ = 0;
  ship_state_ = kStateOnGround;
  ship_counter_ = kStartupInvincibleFrames;
  ship_jump_velocity_ = 0;
  entity_ship_center_in_canyon();
}

void Game::entity_ship_center_in_canyon() {
  uint8_t left = canyon_left(kScxMergedTable, scx_frame_base_, ship_y_ >> kFixedBits);
  ship_x_ = (uint16_t)((left + (kCanyonWidth - kShipWidth) / 2) << kFixedBits);
}

void Game::ship_handle_input(const Input& in) {
  const bool left = in.left, right = in.right;

  // The original switch means both buttons pressed -> no movement.
  if (left && !right) {
    if (ship_x_ > kShipXMin) ship_x_ -= kShipMoveAmtX;
  } else if (right && !left) {
    if (ship_x_ < kShipXMax) ship_x_ += kShipMoveAmtX;
  }

  const bool up = in.up, down = in.down;
  if (up && !down) {
    if (ship_y_ > kShipYMin) ship_y_ -= kShipMoveAmtY;
  } else if (down && !up) {
    if (ship_y_ < kShipYMax) ship_y_ += kShipMoveAmtY;
  }

  const bool tick_a = in.a && !prev_input_.a;
  if (tick_a) {
    if (ship_state_ == kStateOnGround) {
      ship_state_ = kStateJump;
      ship_jump_velocity_ = kJumpVelocityStart;
      ship_z_ = 0;
    }
  } else if (ship_state_ == kStateJump) {
    // Releasing jump early clamps to a short hop.
    if (!in.a && ship_jump_velocity_ > kJumpVelocitySmall) {
      ship_jump_velocity_ = kJumpVelocitySmall;
    }
  }
}

void Game::ship_handle_collisions(StepResult& r) {
  const uint16_t ship_x_px = ship_x_ >> kFixedBits;
  const uint16_t ship_y_px = ship_y_ >> kFixedBits;

  // Ships' canyon row is sampled through the middle of the sprite.
  uint8_t ship_canyon_left =
      canyon_left(kScxMergedTable, scx_frame_base_,
                  int(ship_y_px) + kShipHeight / 2);

  if (u8(ship_x_px + kShipHitXLeft) < ship_canyon_left) {
    ship_state_ = kStateCrashing;
    ship_counter_ = fast_crash_ ? 0 : kCrashAnimationFrames;
    r.crashed = true;
    return;
  } else if (u8(ship_x_px + kShipHitXRight) >
             u8(ship_canyon_left + kCanyonWidth)) {
    ship_state_ = kStateCrashing;
    ship_counter_ = fast_crash_ ? 0 : kCrashAnimationFrames;
    r.crashed = true;
    return;
  }

  // Obstacles are ordered bottom-of-screen first.
  uint8_t idx = obstacles_active_first_;
  for (uint8_t c = obstacles_active_count_; c != 0; c--) {
    uint8_t oy = obstacles_y_[idx] >> kFixedBits;
    uint8_t type = obstacles_type_[idx];

    if (!(type & kObjectFlagHidden)) {
      // Bottom of obstacle above top of ship? Then no further collisions.
      if (u8(oy + kObstacleHitboxYBottom) < u8(ship_y_px + kShipHitYTop)) {
        return;
      }
      if (u8(oy) <= u8(ship_y_px + kShipHitYBottom)) {
        uint8_t ox = canyon_left(kScxMergedTable, scx_frame_base_, oy);
        uint8_t t = type & 0x07;
        if (t > 4) t = 4;
        uint8_t hl = u8(ox + kObstacleHitboxXLeft[t]);
        uint8_t hr = u8(ox + kObstacleHitboxXRight[t]);
        if (hl <= u8(ship_x_px + kShipHitXRight) &&
            hr >= u8(ship_x_px + kShipHitXLeft)) {
          if (t == kObjectTypeItemPlus1) {
            obstacles_type_[idx] |= kObjectFlagHidden;
            if (lives_ < kLivesMax) lives_++;
            r.items_collected++;
          } else {
            ship_state_ = kStateCrashing;
            ship_counter_ = fast_crash_ ? 0 : kCrashAnimationFrames;
            r.crashed = true;
          }
          return;
        }
      }
    }

    idx++;
    if (idx == kObstaclesCountTotal) idx = 0;
  }
}

void Game::entity_ship_update(const Input& in, StepResult& r) {
  switch (ship_state_) {
    case kStateCrashing:
      if (ship_counter_) {
        ship_counter_--;
      } else {
        ship_state_ = kStateCrashEnded;
      }
      break;

    case kStateJump: {
      ship_handle_input(in);

      // Land exactly when the downward velocity would cross the ground.
      if (ship_jump_velocity_ < 0 &&
          ((-ship_jump_velocity_) > int(ship_z_))) {
        ship_z_ = 0;
        ship_state_ = kStateOnGround;
      } else {
        ship_z_ = uint16_t(ship_z_ + int16_t(ship_jump_velocity_));
        ship_jump_velocity_ -= kJumpGravity;
        if (ship_jump_velocity_ < kJumpVelocityDownLimit) {
          ship_jump_velocity_ = kJumpVelocityDownLimit;
        }
      }

      if (ship_counter_) ship_counter_--;
      break;
    }

    case kStateOnGround:
      ship_handle_input(in);
      if (ship_counter_) {
        ship_counter_--;
      } else {
        ship_handle_collisions(r);
      }
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Obstacles
// ---------------------------------------------------------------------------
void Game::queue_next() {
  uint8_t type = rand8();
  obstacles_next_type_ = type & kObstacleTypeMask;

  if ((type & kObjectRandomItemSpawnMask) == kObjectRandomItemSpawnOk) {
    obstacles_item_queued_ = kObjectTypeItemPlus1;
  }

  if ((type & kObjectRandomDoubleBit) && !obstacles_next_isdouble_) {
    obstacles_next_countdown_ = cur_level_.obst_dist_double;
    obstacles_next_isdouble_ = true;
  } else {
    obstacles_next_countdown_ =
        uint8_t((rand8() & kObstacleNextCountBitmask) + cur_level_.obst_dist_min);
    obstacles_next_isdouble_ = false;
  }
}

void Game::entity_obstacles_init() {
  obstacles_active_count_ = 0;
  obstacles_active_last_ = 0;
  obstacles_active_first_ = 0;
  obstacles_next_isdouble_ = false;
  obstacles_item_queued_ = 0;
  for (int i = 0; i < kObstaclesCountTotal; i++) {
    obstacles_y_[i] = 0;
    obstacles_type_[i] = 0;
  }

  queue_next();
  obstacles_next_countdown_ = kObstacleNextCountGameplayStart;
}

void Game::entity_obstacles_update(StepResult& r) {
  uint8_t idx = obstacles_active_last_;

  for (uint8_t c = obstacles_active_count_; c != 0; c--) {
    obstacles_y_[idx] = uint16_t(obstacles_y_[idx] + cur_level_.obst_speed);
    uint8_t y = obstacles_y_[idx] >> kFixedBits;
    uint8_t type = obstacles_type_[idx];

    if (y > kObstacleRemoveY) {
      obstacles_active_count_--;
      obstacles_active_first_++;
      if (obstacles_active_first_ == kObstaclesCountTotal) {
        obstacles_active_first_ = 0;
      }

      if (player_active(ship_state_)) {
        if ((type & kObjectTypeItemMask) <= 3) {
          score_ += kScoreIncrement;
          r.obstacles_cleared++;
          level_count_till_next_--;
          if (level_count_till_next_ == 0) {
            level_increment();
            r.level_ups++;
          }
        }
      }
    }

    if (idx == 0) {
      idx = kObstaclesCountTotal - 1;
    } else {
      idx--;
    }
  }
}

void Game::entity_obstacles_check_spawn_new() {
  if (obstacles_active_count_ != cur_level_.obst_qty_max &&
      player_active(ship_state_)) {
    if (obstacles_next_countdown_) {
      obstacles_next_countdown_--;
    } else {
      obstacles_active_count_++;
      obstacles_active_last_++;
      if (obstacles_active_last_ == kObstaclesCountTotal) {
        obstacles_active_last_ = 0;
      }

      obstacles_y_[obstacles_active_last_] = 0;

      if (obstacles_item_queued_) {
        obstacles_type_[obstacles_active_last_] = obstacles_item_queued_;
        obstacles_item_queued_ = 0;
      } else {
        obstacles_type_[obstacles_active_last_] = obstacles_next_type_;
      }

      if (obstacles_active_count_ == 1) {
        obstacles_active_first_ = obstacles_active_last_;
      }

      queue_next();
    }
  }
}

// ---------------------------------------------------------------------------
// Rewind (game_state_save / gameplay_state_restore)
// ---------------------------------------------------------------------------
void Game::gameplay_state_save() {
  Snapshot& s = snapshot_;
  s.ship_x = ship_x_;
  s.ship_y = ship_y_;
  s.ship_z = ship_z_;
  s.ship_state = ship_state_;
  s.ship_counter = ship_counter_;
  s.ship_jump_velocity = ship_jump_velocity_;
  s.game_level = game_level_;
  s.level_count_till_next = level_count_till_next_;
  s.cur_level = cur_level_;
  std::memcpy(s.obstacles_y, obstacles_y_, sizeof(obstacles_y_));
  std::memcpy(s.obstacles_type, obstacles_type_, sizeof(obstacles_type_));
  s.obstacles_active_first = obstacles_active_first_;
  s.active_last = obstacles_active_last_;
  s.active_count = obstacles_active_count_;
  s.obstacles_next_countdown = obstacles_next_countdown_;
  s.next_type = obstacles_next_type_;
  s.next_isdouble = obstacles_next_isdouble_;
  s.item_queued = obstacles_item_queued_;
  s.score = score_;
  s.scx_frame_base = scx_frame_base_;
  s.scx_cur = scx_cur_;
  s.scx_stop = scx_stop_;
  s.mapfx_level_mask = mapfx_level_mask_;
  s.mapfx_level_base = mapfx_level_base_;
  s.rand_seed = rand_seed_;
}

bool Game::gameplay_state_restore() {
  if (lives_ == 0) return false;

  const Snapshot& s = snapshot_;
  ship_x_ = s.ship_x;
  ship_y_ = s.ship_y;
  ship_z_ = s.ship_z;
  ship_state_ = s.ship_state;
  ship_counter_ = s.ship_counter;
  ship_jump_velocity_ = s.ship_jump_velocity;
  game_level_ = s.game_level;
  level_count_till_next_ = s.level_count_till_next;
  cur_level_ = s.cur_level;
  std::memcpy(obstacles_y_, s.obstacles_y, sizeof(obstacles_y_));
  std::memcpy(obstacles_type_, s.obstacles_type, sizeof(obstacles_type_));
  obstacles_active_first_ = s.obstacles_active_first;
  obstacles_active_last_ = s.active_last;
  obstacles_active_count_ = s.active_count;
  obstacles_next_countdown_ = s.obstacles_next_countdown;
  obstacles_next_type_ = s.next_type;
  obstacles_next_isdouble_ = s.next_isdouble;
  obstacles_item_queued_ = s.item_queued;
  score_ = s.score;
  scx_frame_base_ = s.scx_frame_base;
  scx_cur_ = s.scx_cur;
  scx_stop_ = s.scx_stop;
  mapfx_level_mask_ = s.mapfx_level_mask;
  mapfx_level_base_ = s.mapfx_level_base;
  rand_seed_ = s.rand_seed;

  lives_--;
  ship_counter_ = kStartupInvincibleFrames;
  entity_ship_center_in_canyon();
  return true;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
StepResult Game::step(const Input& in) {
  StepResult r;
  // vsync(): VBL updates the canyon scroll window.
  vblank_map_reset();
  frames_++;

  // UPDATE_KEYS() happened before B is tested in the original loop.
  const bool tick_b = in.b && !prev_input_.b;
  if (tick_b && player_active(ship_state_)) {
    if (lives_ != 0) {
      gameplay_state_save();
      lives_--;
    }
  }

  entity_obstacles_update(r);
  entity_obstacles_check_spawn_new();
  entity_ship_update(in, r);

  if (ship_state_ == kStateCrashEnded) {
    if (gameplay_state_restore()) {
      r.restored = true;
      r.crashed = false;  // crash was already reported on the crash frame
    } else {
      r.game_over = true;
    }
  }

  prev_input_ = in;
  return r;
}

// ---------------------------------------------------------------------------
// Observation helpers
// ---------------------------------------------------------------------------
uint8_t Game::canyon_left_at(int y) const {
  if (y < 0) y = 0;
  if (y > kScreenH - 1) y = kScreenH - 1;
  return canyon_left(kScxMergedTable, scx_frame_base_, y);
}

uint8_t Game::wall_left_at_ship() const {
  return canyon_left(kScxMergedTable, scx_frame_base_,
                     int(ship_y_ >> kFixedBits) + kShipHeight / 2);
}

int Game::obstacles_ahead(ObstacleView* out, int max) const {
  if (max <= 0 || obstacles_active_count_ == 0) return 0;

  const int ship_y = ship_y_ >> kFixedBits;

  // Gather visible obstacles that still matter (not already below the ship),
  // then sort by closeness: the obstacle nearest the ship comes first.
  ObstacleView tmp[kObstaclesCountTotal];
  int n = 0;
  uint8_t idx = obstacles_active_first_;
  for (uint8_t c = obstacles_active_count_; c != 0; c--) {
    uint8_t type = obstacles_type_[idx];
    int oy = obstacles_y_[idx] >> kFixedBits;
    if (!(type & kObjectFlagHidden) && oy <= ship_y + kShipHitYBottom) {
      uint8_t ox = canyon_left(kScxMergedTable, scx_frame_base_, oy);
      uint8_t t = type & 0x07;
      if (t > 4) t = 4;
      tmp[n].y = oy;
      tmp[n].type = t;
      tmp[n].x_left = ox + kObstacleHitboxXLeft[t];
      tmp[n].x_right = ox + kObstacleHitboxXRight[t];
      tmp[n].hidden = false;
      n++;
    }
    idx++;
    if (idx == kObstaclesCountTotal) idx = 0;
  }

  // Insertion sort by decreasing y: nearest-to-ship first.
  for (int i = 1; i < n; i++) {
    ObstacleView key = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j].y < key.y) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = key;
  }

  int m = n < max ? n : max;
  for (int i = 0; i < m; i++) out[i] = tmp[i];
  return m;
}

uint8_t Game::rand8() {
  // GBDK rand(): 16-bit LCG, returns the high byte.
  rand_seed_ = uint16_t(17u * uint32_t(rand_seed_) + 0x5C93u);
  return uint8_t(rand_seed_ >> 8);
}

}  // namespace canyon