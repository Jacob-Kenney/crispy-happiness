"""Fast sanity checks for the ctypes env wrapper (no model needed)."""

import sys

from canyon_env import ACTION_NAMES, CanyonEnv, obs_text


def main():
    fails = 0

    e = CanyonEnv(lives=2, action_repeat=2)
    o = e.reset(seed=0)
    assert o.done == 0
    x0, y0 = o.ship_x, o.ship_y
    assert "obs " in obs_text(o) and "canyon" in obs_text(o)

    o, done, ev = e.step(ACTION_NAMES.index("left"))
    if o.ship_x >= x0:
        print("FAIL: left did not move ship left"); fails += 1
    o, done, ev = e.step(ACTION_NAMES.index("right"))
    o, done, ev = e.step(ACTION_NAMES.index("right"))
    if o.ship_x <= x0:
        print("FAIL: right did not move ship right"); fails += 1

    # Jump: a press -> airborne with positive z.
    e.reset(seed=0)
    o, _, _ = e.step(ACTION_NAMES.index("a"))
    o, _, _ = e.step(ACTION_NAMES.index("a"))
    if o.state != 1 or o.ship_z <= 0:
        print(f"FAIL: jump state={o.state} z={o.ship_z}"); fails += 1
    o, _, _ = e.step(ACTION_NAMES.index("wait"))
    if o.state != 1:
        print("FAIL: released jump did not stay airborne"); fails += 1

    # Wall crash consumes a life and rewinds.
    e = CanyonEnv(lives=2, action_repeat=1)
    e.reset(seed=0)
    crashed = False
    for _ in range(400):
        o, done, ev = e.step(ACTION_NAMES.index("left"))
        if ev["crashed"]:
            crashed = True
        if ev["restored"]:
            break
    if not crashed or o.lives != 1:
        print(f"FAIL: crash/rewind crashed={crashed} lives={o.lives}"); fails += 1

    # Frame counting: 300 decisions x 1 frame.
    e = CanyonEnv(action_repeat=1)
    e.reset(seed=11)
    for _ in range(300):
        e.step(0)
    if e.obs.frames != 300:
        print(f"FAIL: frame counting frames={e.obs.frames}"); fails += 1

    if fails:
        sys.exit(1)
    print("env tests passed")


if __name__ == "__main__":
    main()
