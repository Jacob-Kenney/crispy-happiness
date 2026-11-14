// Canyon Racer game-logic port: header.
//
// Derived from canyon-racer (https://github.com/bbbbbr/canyon-racer) by bbbbbr.
// Original license: CC-BY-NC-SA 4.0.  This port keeps only the gameplay
// simulation: ship physics, canyon walls, obstacles, scoring, levels,
// lives/rewind, and the SCX canyon-shape scroll.  No graphics, audio, input
// hardware, menus or save-cart code.
#pragma once

#include <cstdint>

namespace canyon {

// ---------------------------------------------------------------------------
// Fixed constants (GB screen is 160x144, fixed point is 8.8)
// ---------------------------------------------------------------------------
constexpr int kScreenW = 160;
constexpr int kScreenH = 144;
constexpr int kFixedBits = 8;
constexpr uint16_t kFixedOne = 1u << kFixedBits;

constexpr uint8_t kShipWidth = 24;
constexpr uint8_t kShipHeight = 32;
constexpr uint8_t kShipPivotW = 8;   // hitbox width
constexpr uint8_t kShipPivotH = 17;  // hitbox height

constexpr uint8_t kShipHitXLeft = (kShipWidth - kShipPivotW) / 2;    // 8
constexpr uint8_t kShipHitXRight = kShipHitXLeft + kShipPivotW;      // 16
constexpr uint8_t kShipHitYTop = 5;
constexpr uint8_t kShipHitYBottom = kShipHitYTop + kShipPivotH;      // 22

constexpr int16_t kShipMoveAmtX = (1 << (kFixedBits - 1));  // 0.5 px/frame
constexpr int16_t kShipMoveAmtY = (1 << (kFixedBits - 1));

constexpr uint16_t kShipXMin = 0;
constexpr uint16_t kShipXMax = (kScreenW - kShipPivotW) << kFixedBits;
constexpr uint16_t kShipYMin = 40u << kFixedBits;
constexpr uint16_t kShipYMax = (kScreenH - kShipHeight) << kFixedBits;

constexpr int16_t kJumpGravity = 32;
constexpr int16_t kJumpVelocityStart = 800;
constexpr int16_t kJumpVelocitySmall = 350;
constexpr int16_t kJumpVelocityDownLimit = -512;
constexpr uint8_t kStartupInvincibleFrames = 90;
// SHIP_COUNTER_CRASH = 4 crash sprite frames << 5 = 128
constexpr uint8_t kCrashAnimationFrames = 4 << 5;

// The canyon walls are CANYON_WIDTH wide, with their left edge derived from
// the SCX wave table value at the row.
constexpr uint8_t kCanyonLeftBase = 64;
constexpr uint8_t kCanyonWidth = 32;

constexpr uint8_t kObstaclesCountTotal = 5;
constexpr uint8_t kObstacleRemoveY = 144;
constexpr uint8_t kObstacleSpawnState = 0;
constexpr uint8_t kObstacleNextCountGameplayStart = 60 * 3;

constexpr uint8_t kObjectTypeItemPlus1 = 0x04;
constexpr uint8_t kObjectTypeItemMask = 0x07;
constexpr uint8_t kObjectFlagHidden = 0x80;
constexpr uint8_t kObjectRandomDoubleBit = 0x04;
constexpr uint8_t kObjectRandomItemSpawnMask = 0xF8;
constexpr uint8_t kObjectRandomItemSpawnOk = 0x00;
constexpr uint8_t kObstacleTypeMask = 0x03;
constexpr uint8_t kObstacleNextCountBitmask = 0x1F;  // MED (level table default)

constexpr uint8_t kObstacleHitboxYBottom = 8;  // 32x16 sprite, top half
constexpr uint8_t kObstacleHitboxXLeft[5] = {0, 24, 8, 0, 8};
constexpr uint8_t kObstacleHitboxXRight[5] = {8, 32, 24, 32, 24};

constexpr uint8_t kScoreIncrement = 10;
constexpr uint8_t kLevelObstaclesTillNext = 10;
constexpr uint8_t kLivesReset = 10;
constexpr uint8_t kLivesMax = 99;

constexpr uint16_t kGameplayStartRandInit = 0x1234;

// Ship state values keep the original bit scheme: bit 0x10 == player active.
enum ShipState : uint8_t {
  kStateOnGround = 0x10,
  kStateJump = 0x11,
  kStateCrashing = 0x12,
  kStateCrashEnded = 0x13,
};
inline bool player_active(uint8_t s) { return (s & 0x10u) != 0; }

// ---------------------------------------------------------------------------
// Level table (verbatim from level.c)
// ---------------------------------------------------------------------------
// MAPFX_LVL_*
enum : uint8_t {
  kMapfxLvlEasy = 0,
  kMapfxLvlMedLo = 1,
  kMapfxLvlMed = 2,
  kMapfxLvlMedHi = 3,
  kMapfxLvlHard = 4,
};

// Map scroll speed selector for the SCX table (slow/normal vs fast).
constexpr bool kMapfxScxNormal = false;
constexpr bool kMapfxScxFast = true;
constexpr uint8_t kMapfxScxSpeedMed = 2;

struct LevelEntry {
  uint8_t mapfx_scx_table_level;
  bool mapfx_scx_inc_every_frame;
  uint8_t mapfx_scx_table_index_mask;
  uint8_t mapfx_scx_table_index_base;
  uint8_t obst_qty_max;
  uint16_t obst_speed;
  uint8_t obst_dist_min;
  uint16_t obst_dist_min_raw;
  uint8_t obst_dist_double;
};

// ---------------------------------------------------------------------------
// Canyon wave data
// ---------------------------------------------------------------------------
struct ScxTableEntry {
  int32_t start;  // index into kScxMergedTable
  int32_t stop;
};
extern const uint8_t kScxMergedTable[2016];
extern const ScxTableEntry kScxTables[15];

// ---------------------------------------------------------------------------
// Input: one bit per GB button.  `b` is the "save state" button.
// ---------------------------------------------------------------------------
struct Input {
  bool left = false;
  bool right = false;
  bool up = false;
  bool down = false;
  bool a = false;  // jump
  bool b = false;  // save / rewind point

  bool operator==(const Input& o) const {
    return left == o.left && right == o.right && up == o.up &&
           down == o.down && a == o.a && b == o.b;
  }
};

struct StepResult {
  bool crashed = false;        // a crash started this frame
  bool restored = false;       // rewound to the save state this frame
  bool game_over = false;      // terminal: out of lives after a crash
  int obstacles_cleared = 0;   // obstacles that scrolled past this frame
  int items_collected = 0;     // +1 lives picked up this frame
  int level_ups = 0;
};

// ---------------------------------------------------------------------------
// Game
// ---------------------------------------------------------------------------
class Game {
 public:
  // lives: starting lives (original: 10).  fast_crash: skip the purely visual
  //           96-frame crash animation (default false, faithful).
  // seed: 0 keeps the original fixed seed (0x1234); other values vary the
  //       obstacle/canyon sequence (used for RL training and evaluation).
  void reset(uint8_t lives = kLivesReset, bool fast_crash = false,
             uint16_t seed = 0);

  // Advance exactly one frame (the original 60 Hz gameplay loop body).
  StepResult step(const Input& in);

  // ---- observation ----
  // Canyon left/right x at a screen row (rows 0..143).
  uint8_t canyon_left_at(int y) const;
  uint8_t canyon_right_at(int y) const { return canyon_left_at(y) + kCanyonWidth; }
  uint8_t wall_left_at_ship() const;
  int ship_x_px() const { return ship_x_ >> kFixedBits; }
  int ship_y_px() const { return ship_y_ >> kFixedBits; }
  int ship_z_px() const { return ship_z_ >> kFixedBits; }
  bool ship_state_ground() const { return ship_state_ == kStateOnGround; }
  bool invincible() const { return ship_counter_ != 0; }
  int frames() const { return frames_; }
  int32_t score() const { return score_; }
  uint8_t lives() const { return lives_; }
  uint8_t level() const { return game_level_; }
  uint16_t obstacle_speed() const { return cur_level_.obst_speed; }
  uint8_t obstacle_min_gap() const { return cur_level_.obst_dist_min; }
  uint8_t rand_seed_hi() const { return uint8_t(rand_seed_ >> 8); }

  // Obstacles ordered by increasing distance ahead of the ship (closest first).
  struct ObstacleView {
    int y;          // screen row of obstacle top
    int type;       // 0 left, 1 right, 2 middle, 3 full, 4 item
    int x_left;     // screen x of hitbox
    int x_right;
    bool hidden;
  };
  int obstacles_ahead(ObstacleView* out, int max) const;

  // For tests / debugging.
  uint16_t raw_ship_x() const { return ship_x_; }
  uint16_t raw_ship_y() const { return ship_y_; }
  uint16_t raw_ship_z() const { return ship_z_; }
  int16_t jump_velocity() const { return ship_jump_velocity_; }
  uint8_t raw_state() const { return ship_state_; }
  uint8_t active_obstacle_count() const { return obstacles_active_count_; }
  uint8_t obstacle_countdown() const { return obstacles_next_countdown_; }
  int32_t frame_base_index() const { return scx_frame_base_; }

 private:
  // gameplay pieces
  void level_init();
  void level_update_vars();
  void level_increment();
  void mapfx_set_gameplay();
  void mapfx_level_set(uint8_t level);
  void vblank_map_reset();
  void entity_ship_init();
  void entity_ship_center_in_canyon();
  void entity_obstacles_init();
  void queue_next();
  void entity_obstacles_update(StepResult& r);
  void entity_obstacles_check_spawn_new();
  void ship_handle_input(const Input& in);
  void ship_handle_collisions(StepResult& r);
  void entity_ship_update(const Input& in, StepResult& r);
  void gameplay_state_save();
  bool gameplay_state_restore();

  uint8_t rand8();
  void initrand(uint16_t seed) { rand_seed_ = seed; }

  // ---- state ----
  uint16_t ship_x_ = 0, ship_y_ = 0, ship_z_ = 0;
  uint8_t ship_state_ = kStateOnGround;
  uint8_t ship_counter_ = 0;
  int16_t ship_jump_velocity_ = 0;

  uint8_t game_level_ = 0;
  uint8_t level_count_till_next_ = 0;
  LevelEntry cur_level_{};

  uint16_t obstacles_y_[kObstaclesCountTotal] = {0};
  uint8_t obstacles_type_[kObstaclesCountTotal] = {0};
  uint8_t obstacles_active_first_ = 0;
  uint8_t obstacles_active_last_ = 0;
  uint8_t obstacles_active_count_ = 0;
  uint8_t obstacles_next_countdown_ = 0;
  uint8_t obstacles_next_type_ = 0;
  bool obstacles_next_isdouble_ = false;
  uint8_t obstacles_item_queued_ = 0;

  int32_t score_ = 0;
  uint8_t lives_ = kLivesReset;
  bool fast_crash_ = false;

  // Map FX
  int32_t scx_frame_base_ = 0;   // kScxMergedTable index used this frame
  int32_t scx_cur_ = 0;
  int32_t scx_stop_ = 0;
  uint8_t mapfx_level_mask_ = 0;
  uint8_t mapfx_level_base_ = 0;

  uint16_t rand_seed_ = 0;
  uint8_t sys_time_ = 0;
  int frames_ = 0;

  Input prev_input_{};

  // Rewind snapshot (game_state_data state_copy + rand_seed_copy)
  struct Snapshot {
    uint16_t ship_x, ship_y, ship_z;
    uint8_t ship_state, ship_counter;
    int16_t ship_jump_velocity;
    uint8_t game_level, level_count_till_next;
    LevelEntry cur_level;
    uint16_t obstacles_y[kObstaclesCountTotal];
    uint8_t obstacles_type[kObstaclesCountTotal];
    uint8_t obstacles_active_first, active_last, active_count;
    uint8_t obstacles_next_countdown, next_type, next_isdouble, item_queued;
    int32_t score;
    int32_t scx_frame_base, scx_cur, scx_stop;
    uint8_t mapfx_level_mask, mapfx_level_base;
    uint16_t rand_seed;
  };
  Snapshot snapshot_{};
};

}  // namespace canyon