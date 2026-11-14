"""Rule-based expert for Canyon Racer, used to warm-start the LM policy.

Pure state machine, no learning. It is deliberately simple:
  * keep away from the canyon walls,
  * jump over obstacles that overlap the ship's hitbox,
  * otherwise hold the centre.
"""

from canyon_env import CanyonEnv, ACTION_NAMES

HITBOX_W = 18
WALL_MARGIN = 10
THREAT_DY = 34
JUMP_HOLD = 3


def expert_action(o, jump_hold):
    """Return (action_index, next_jump_hold)."""
    gl = o.ship_x + 8 - o.canyon_left
    gr = o.canyon_right - (o.ship_x + 16)

    # Airborne: keep the jump short by releasing after a couple of decisions.
    if o.state == 1:
        if jump_hold > 0:
            return ACTION_NAMES.index("a"), jump_hold - 1
        return ACTION_NAMES.index("wait"), 0

    threat = False
    for i in range(o.n_ahead):
        dy = o.ahead_dy[i]
        if dy > THREAT_DY:
            continue
        dl = o.ahead_dx_l[i]
        dr = o.ahead_dx_r[i]
        if dl < HITBOX_W and dr > -HITBOX_W:
            threat = True
            break

    if threat:
        return ACTION_NAMES.index("a"), JUMP_HOLD

    if gl < WALL_MARGIN:
        return ACTION_NAMES.index("right"), 0
    if gr < WALL_MARGIN:
        return ACTION_NAMES.index("left"), 0
    if gl + 4 < gr:
        return ACTION_NAMES.index("right"), 0
    if gr + 4 < gl:
        return ACTION_NAMES.index("left"), 0
    return ACTION_NAMES.index("wait"), 0


def run_episode(seed, verbose=False):
    e = CanyonEnv(lives=1, fast_crash=True, action_repeat=4)
    e.reset(seed)
    jump_hold = 0
    steps = 0
    while True:
        o = e.obs
        a, jump_hold = expert_action(o, jump_hold)
        o, done, ev = e.step(a)
        steps += 1
        if verbose and (ev["crashed"] or ev["level_up"]):
            print(seed, steps, ACTION_NAMES[a], ev, "frames", o.frames, "score", o.score)
        if done:
            return o.frames, o.score, o.ev_cleared + (o.score - o.ev_cleared)
        if steps > 20000:
            return o.frames, o.score, o.ev_cleared


if __name__ == "__main__":
    import sys

    n = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    tot_f = tot_s = 0
    for seed in range(n):
        f, s, c = run_episode(seed)
        tot_f += f
        tot_s += s
        print(f"seed {seed}: frames {f} score {s}")
    print(f"mean frames {tot_f/n:.0f} mean score {tot_s/n:.1f}")