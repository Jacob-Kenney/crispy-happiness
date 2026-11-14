// C ABI for the Canyon Racer port, consumed from Python via ctypes.
//
// One env == one game instance.  Actions are mini-actions (0..6); the RL
// layer maps language-model tokens onto them.
#include <cstring>
#include <vector>

#include "canyon/game.hpp"

extern "C" {

enum CanyonAction : int {
  kActionWait = 0,
  kActionLeft = 1,
  kActionRight = 2,
  kActionUp = 3,
  kActionDown = 4,
  kActionJump = 5,
  kActionSave = 6,
  kActionCount = 7,
};

typedef struct CanyonObs {
  int32_t ship_x, ship_y, ship_z;
  int32_t state;        // 0 ground, 1 jump, 2 crashing, 3 crash ended
  int32_t invincible;   // frames of invincibility left
  int32_t canyon_left;  // at ship row
  int32_t canyon_right;
  int32_t canyon_center;
  int32_t dx_center;    // ship x - canyon center
  int32_t wall_left;    // canyon left at the row collisions use

  int32_t level, score, lives, frames;
  int32_t obstacle_speed;
  int32_t n_ahead;
  int32_t ahead_y[3], ahead_type[3], ahead_l[3], ahead_r[3];
  int32_t ahead_dy[3], ahead_dx_l[3], ahead_dx_r[3];

  int32_t n_canyon;  // 3 look-ahead rows
  int32_t canyon_y[3], canyon_l[3], canyon_r[3];

  int32_t done;
  int32_t ev_cleared, ev_items, ev_crashed, ev_restored, ev_level_up;
  int32_t stepped_frames;
} CanyonObs;

typedef struct CanyonEnv {
  canyon::Game game;
  CanyonObs obs;
  int32_t last_cleared, last_items, last_crashed, last_restored, last_level_up;
  bool done;
} CanyonEnv;

static void fill_obs(CanyonEnv* e) {
  CanyonObs& o = e->obs;
  const canyon::Game& g = e->game;
  std::memset(&o, 0, sizeof(o));

  o.ship_x = g.ship_x_px();
  o.ship_y = g.ship_y_px();
  o.ship_z = g.ship_z_px();
  o.state = g.ship_state_ground() ? 0 : (g.raw_state() == canyon::kStateJump ? 1
                                        : g.raw_state() == canyon::kStateCrashing ? 2 : 3);
  o.invincible = g.invincible() ? 1 : 0;

  o.canyon_left = g.wall_left_at_ship();
  o.canyon_right = o.canyon_left + canyon::kCanyonWidth;
  o.canyon_center = o.canyon_left + canyon::kCanyonWidth / 2;
  o.dx_center = o.ship_x - o.canyon_center;
  o.wall_left = o.canyon_left;

  o.level = g.level();
  o.score = g.score();
  o.lives = g.lives();
  o.frames = g.frames();
  o.obstacle_speed = g.obstacle_speed();

  canyon::Game::ObstacleView views[3];
  int n = g.obstacles_ahead(views, 3);
  o.n_ahead = n;
  for (int i = 0; i < n; i++) {
    o.ahead_y[i] = views[i].y;
    o.ahead_type[i] = views[i].type;
    o.ahead_l[i] = views[i].x_left;
    o.ahead_r[i] = views[i].x_right;
    o.ahead_dy[i] = o.ship_y - views[i].y;
    o.ahead_dx_l[i] = views[i].x_left - o.ship_x;
    o.ahead_dx_r[i] = views[i].x_right - o.ship_x;
  }

  o.n_canyon = 3;
  const int rows[3] = {o.ship_y - 8, o.ship_y - 24, o.ship_y - 40};
  for (int i = 0; i < 3; i++) {
    int y = rows[i] < 0 ? 0 : rows[i];
    o.canyon_y[i] = y;
    o.canyon_l[i] = g.canyon_left_at(y);
    o.canyon_r[i] = g.canyon_right_at(y);
  }

  o.done = e->done ? 1 : 0;
  o.ev_cleared = e->last_cleared;
  o.ev_items = e->last_items;
  o.ev_crashed = e->last_crashed;
  o.ev_restored = e->last_restored;
  o.ev_level_up = e->last_level_up;
  o.stepped_frames = g.frames();
}

CanyonEnv* canyon_create(void) { return new CanyonEnv(); }

void canyon_destroy(CanyonEnv* e) { delete e; }

void canyon_reset(CanyonEnv* e, int32_t lives, int32_t fast_crash,
                  int32_t seed) {
  e->game.reset(uint8_t(lives), fast_crash != 0, uint16_t(seed));
  e->done = false;
  e->last_cleared = e->last_items = e->last_crashed = 0;
  e->last_restored = e->last_level_up = 0;
  fill_obs(e);
}

// Apply one mini-action for `repeat` frames (stops early on game over).
void canyon_step(CanyonEnv* e, int32_t action, int32_t repeat) {
  if (repeat < 1) repeat = 1;
  if (repeat > 600) repeat = 600;

  canyon::Input in;
  switch (action) {
    case kActionLeft: in.left = true; break;
    case kActionRight: in.right = true; break;
    case kActionUp: in.up = true; break;
    case kActionDown: in.down = true; break;
    case kActionJump: in.a = true; break;
    case kActionSave: in.b = true; break;
    default: break;
  }

  e->last_cleared = e->last_items = e->last_crashed = 0;
  e->last_restored = e->last_level_up = 0;

  for (int i = 0; i < repeat && !e->done; i++) {
    canyon::StepResult r = e->game.step(in);
    e->last_cleared += r.obstacles_cleared;
    e->last_items += r.items_collected;
    e->last_crashed += r.crashed ? 1 : 0;
    e->last_restored += r.restored ? 1 : 0;
    e->last_level_up += r.level_ups;
    if (r.game_over) e->done = true;
  }
  fill_obs(e);
}

void canyon_obs(CanyonEnv* e, CanyonObs* out) { *out = e->obs; }

}  // extern "C"