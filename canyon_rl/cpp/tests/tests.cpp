// Verification tests for the Canyon Racer game-logic port.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "canyon/game.hpp"

using namespace canyon;

static int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static Input keys(bool l = false, bool r = false, bool u = false,
                  bool d = false, bool a = false, bool b = false) {
  Input in;
  in.left = l; in.right = r; in.up = u; in.down = d; in.a = a; in.b = b;
  return in;
}

// Hold an input for n frames, collecting the running result.
static void run(Game& g, const Input& in, int n, StepResult* last = nullptr) {
  for (int i = 0; i < n; i++) {
    StepResult r = g.step(in);
    if (last) *last = r;
  }
}

static void test_start_state() {
  Game g;
  g.reset();

  // Ship starts on the bottom row, centred in the canyon, and inside it.
  CHECK(g.ship_y_px() == kScreenH - kShipHeight);
  CHECK(g.raw_state() == kStateOnGround);
  int left = g.wall_left_at_ship();
  CHECK(g.ship_x_px() + kShipHitXLeft >= left);
  CHECK(g.ship_x_px() + kShipHitXRight <= left + kCanyonWidth);
  CHECK(g.lives() == kLivesReset);
  CHECK(g.score() == 0);
  CHECK(g.level() == 0);
  CHECK(g.active_obstacle_count() == 0);
  CHECK(g.obstacle_countdown() == kObstacleNextCountGameplayStart);
}

static void test_horizontal_speed() {
  Game g;
  g.reset();
  int x0 = g.ship_x_px();
  run(g, keys(true), 10);  // 10 frames * 0.5 px
  CHECK(g.ship_x_px() == x0 - 5);
  run(g, keys(false, true), 10);
  CHECK(g.ship_x_px() == x0);
}

static void test_vertical_speed_and_limits() {
  Game g;
  g.reset();
  int y0 = g.ship_y_px();
  run(g, keys(false, false, true), 8);
  CHECK(g.ship_y_px() == y0 - 4);
  run(g, keys(false, false, true), 200);
  CHECK(g.ship_y_px() == kShipYMin >> kFixedBits);  // clamped at top
  run(g, keys(false, false, false, true), 400);
  CHECK(g.ship_y_px() == kShipYMax >> kFixedBits);  // clamped at bottom
}

static void test_jump_hold_vs_tap() {
  Game g;
  g.reset();
  run(g, keys(), kStartupInvincibleFrames + 1);

  // Tap: one frame of A, then release.
  StepResult r;
  run(g, keys(false, false, false, false, true), 1, &r);
  CHECK(g.raw_state() == kStateJump);
  int tap_apex = 0;
  for (int i = 0; i < 80; i++) {
    r = g.step(keys());
    if (g.ship_z_px() > tap_apex) tap_apex = g.ship_z_px();
    if (g.raw_state() == kStateOnGround) break;
  }
  CHECK(g.raw_state() == kStateOnGround);
  CHECK(tap_apex > 5 && tap_apex < 20);  // short hop

  // Hold: keep A down; jump should be much higher.
  run(g, keys(), 2);
  CHECK(g.raw_state() == kStateOnGround);
  g.step(keys(false, false, false, false, true));
  int hold_apex = 0;
  for (int i = 0; i < 120; i++) {
    r = g.step(keys(false, false, false, false, true));
    if (g.ship_z_px() > hold_apex) hold_apex = g.ship_z_px();
    if (g.raw_state() == kStateOnGround) break;
  }
  CHECK(g.raw_state() == kStateOnGround);
  CHECK(hold_apex > 35);  // ~40 px
}

static void test_release_clamps_velocity() {
  Game g;
  g.reset();
  g.step(keys(false, false, false, false, true));  // takeoff, vel 800
  CHECK(g.jump_velocity() == kJumpVelocityStart);
  g.step(keys());                                  // release -> clamp
  CHECK(g.jump_velocity() <= kJumpVelocitySmall);
}

static void test_obstacle_spawn_timing() {
  Game g;
  g.reset();
  CHECK(g.obstacle_countdown() == 180);

  for (int i = 0; i < 180; i++) g.step(keys());
  CHECK(g.obstacle_countdown() == 0);
  CHECK(g.active_obstacle_count() == 0);

  g.step(keys());
  CHECK(g.active_obstacle_count() == 1);
}

static void test_crash_animation_length() {
  Game g;
  g.reset(3, false);  // faithful crash animation
  g.step(keys(true));
  // Burn invincibility, then crash into the left wall.
  for (int i = 0; i < kStartupInvincibleFrames + 1; i++) g.step(keys(true));
  CHECK(g.raw_state() == kStateCrashing);

  int frames = 0;
  while (g.raw_state() == kStateCrashing && frames < 400) {
    g.step(keys(true));
    frames++;
  }
  CHECK(frames == kCrashAnimationFrames);
}

static void test_wall_crash_and_rewind() {
  Game g;
  g.reset(3, true);  // 3 lives, no crash animation
  CHECK(g.lives() == 3);
  // Invincible at start: steering into the wall must not crash yet.
  run(g, keys(true), kStartupInvincibleFrames);
  CHECK(g.lives() == 3);

  bool crashed = false, restored = false;
  for (int i = 0; i < 200 && !restored; i++) {
    StepResult r = g.step(keys(true));
    crashed |= r.crashed;
    restored |= r.restored;
  }
  CHECK(crashed);
  CHECK(restored);
  CHECK(g.lives() == 2);

  // Ship was re-centred in the canyon by the rewind.
  int left = g.wall_left_at_ship();
  CHECK(g.ship_x_px() + kShipHitXLeft >= left);
  CHECK(g.ship_x_px() + kShipHitXRight <= left + kCanyonWidth);

  // With no lives left, a crash becomes a terminal game over.
  Game over;
  over.reset(1, true);
  bool game_over = false;
  for (int i = 0; i < 400 && !game_over; i++)
    game_over = over.step(keys(true)).game_over;
  CHECK(game_over);
}

// A dumb controller that keeps the ship near the canyon centre.  It is only
// used to exercise long runs, scoring and level ups.
static Input center_bot(const Game& g) {
  int center = g.canyon_left_at(g.ship_y_px()) + kCanyonWidth / 2;
  if (g.ship_x_px() < center - 3) return keys(false, true);
  if (g.ship_x_px() > center + 3) return keys(true);
  return keys();
}

static void test_long_run_scores() {
  Game g;
  g.reset(10, true);
  int max_score = 0;
  int max_level = 0;
  for (int i = 0; i < 6000; i++) {
    StepResult r = g.step(center_bot(g));
    (void)r;
    if (g.score() > max_score) max_score = g.score();
    if (g.level() > max_level) max_level = g.level();
  }
  CHECK(max_score >= 10);   // cleared at least one obstacle
  CHECK(max_level >= 1);    // advancing levels uses the game level table
}

static void test_determinism() {
  Game a, b;
  a.reset(10, true);
  b.reset(10, true);

  uint32_t seed = 12345;
  for (int i = 0; i < 1200; i++) {
    seed = seed * 1664525u + 1013904223u;
    Input in;
    in.left = (seed >> 3) & 1;
    in.right = (seed >> 5) & 1;
    in.up = (seed >> 7) & 1;
    in.down = (seed >> 9) & 1;
    in.a = (seed >> 11) & 1;
    in.b = (seed >> 13) & 1;
    if (in.left && in.right) in.right = false;
    if (in.up && in.down) in.down = false;

    StepResult ra = a.step(in);
    StepResult rb = b.step(in);
    CHECK(ra.crashed == rb.crashed);
    CHECK(a.raw_ship_x() == b.raw_ship_x());
    CHECK(a.raw_ship_y() == b.raw_ship_y());
    CHECK(a.raw_ship_z() == b.raw_ship_z());
    CHECK(a.raw_state() == b.raw_state());
    CHECK(a.score() == b.score());
    CHECK(a.lives() == b.lives());
    CHECK(a.active_obstacle_count() == b.active_obstacle_count());
    CHECK(a.frame_base_index() == b.frame_base_index());
    CHECK(a.rand_seed_hi() == b.rand_seed_hi());
    if (failures) break;
  }
}

static void test_seeded_layouts() {
  Game a, b;
  a.reset(1, true, 1);
  b.reset(1, true, 2);
  bool differs = false;
  for (int i = 0; i < 400; i++) {
    StepResult ra = a.step(keys());
    StepResult rb = b.step(keys());
    if (ra.crashed != rb.crashed) differs = true;
    if (a.obstacle_countdown() != b.obstacle_countdown()) differs = true;
    if (a.frame_base_index() != b.frame_base_index()) differs = true;
  }
  CHECK(differs);

  // Same seed reproduces exactly.
  Game c, d;
  c.reset(1, true, 7);
  d.reset(1, true, 7);
  for (int i = 0; i < 400; i++) {
    StepResult rc = c.step(keys());
    StepResult rd = d.step(keys());
    CHECK(rc.crashed == rd.crashed);
    CHECK(c.obstacle_countdown() == d.obstacle_countdown());
    CHECK(c.frame_base_index() == d.frame_base_index());
  }
}

static void test_observation() {
  Game g;
  g.reset();
  for (int i = 0; i < 400; i++) g.step(keys());

  Game::ObstacleView v[3];
  int n = g.obstacles_ahead(v, 3);
  CHECK(n == g.active_obstacle_count());
  for (int i = 0; i + 1 < n; i++) CHECK(v[i].y <= v[i + 1].y);

  for (int i = 0; i < n; i++) {
    CHECK(v[i].type >= 0 && v[i].type <= 4);
    CHECK(v[i].x_left <= v[i].x_right);
  }

  // Canyon is a fixed-width window that stays on screen.
  for (int y = 0; y < kScreenH; y += 7) {
    int l = g.canyon_left_at(y);
    CHECK(l >= 0);
    CHECK(l + kCanyonWidth <= kScreenW);
    CHECK(g.canyon_right_at(y) == l + kCanyonWidth);
  }
}

int main() {
  test_start_state();
  test_horizontal_speed();
  test_vertical_speed_and_limits();
  test_jump_hold_vs_tap();
  test_release_clamps_velocity();
  test_obstacle_spawn_timing();
  test_crash_animation_length();
  test_wall_crash_and_rewind();
  test_long_run_scores();
  test_determinism();
  test_seeded_layouts();
  test_observation();

  if (failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}