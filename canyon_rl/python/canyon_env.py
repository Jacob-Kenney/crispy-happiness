"""ctypes wrapper for the Canyon Racer C++ game-logic port.

Only game state is exposed; there is no rendering.  The C++ side owns all
gameplay rules, this module just marshals inputs/outputs and renders the
observation as compact text for a language model.
"""

import ctypes
import os
import sys
import pathlib
from dataclasses import dataclass
from typing import List

ROOT = pathlib.Path(__file__).resolve().parent.parent


def _find_lib() -> pathlib.Path:
    override = os.environ.get("CANYON_LIB")
    if override:
        return pathlib.Path(override)
    names = (
        ("libcanyon_env.dylib", "libcanyon_env.so")
        if sys.platform == "darwin"
        else ("libcanyon_env.so",)
    )
    for name in names:
        p = ROOT / "cpp" / name
        if p.exists():
            return p
    raise FileNotFoundError(
        f"no libcanyon_env in {ROOT / 'cpp'}; run `make -C {ROOT / 'cpp'} lib`"
    )


LIB_PATH = None  # resolved lazily on first env creation

ACTION_NAMES = ["wait", "left", "right", "up", "down", "a", "b"]

# Tokens we accept as actions; everything else in the LM vocabulary is an
# invalid action (penalized, executed as "wait").
ACTION_ALIASES = {
    "wait": 0,
    "none": 0,
    "noop": 0,
    "stay": 0,
    "left": 1,
    "right": 2,
    "up": 3,
    "down": 4,
    "a": 5,
    "jump": 5,
    "b": 6,
    "save": 6,
}

TYPE_NAMES = ["left", "right", "middle", "full", "item"]


class CanyonObs(ctypes.Structure):
    _fields_ = [
        ("ship_x", ctypes.c_int32),
        ("ship_y", ctypes.c_int32),
        ("ship_z", ctypes.c_int32),
        ("state", ctypes.c_int32),
        ("invincible", ctypes.c_int32),
        ("canyon_left", ctypes.c_int32),
        ("canyon_right", ctypes.c_int32),
        ("canyon_center", ctypes.c_int32),
        ("dx_center", ctypes.c_int32),
        ("wall_left", ctypes.c_int32),
        ("level", ctypes.c_int32),
        ("score", ctypes.c_int32),
        ("lives", ctypes.c_int32),
        ("frames", ctypes.c_int32),
        ("obstacle_speed", ctypes.c_int32),
        ("n_ahead", ctypes.c_int32),
        ("ahead_y", ctypes.c_int32 * 3),
        ("ahead_type", ctypes.c_int32 * 3),
        ("ahead_l", ctypes.c_int32 * 3),
        ("ahead_r", ctypes.c_int32 * 3),
        ("ahead_dy", ctypes.c_int32 * 3),
        ("ahead_dx_l", ctypes.c_int32 * 3),
        ("ahead_dx_r", ctypes.c_int32 * 3),
        ("n_canyon", ctypes.c_int32),
        ("canyon_y", ctypes.c_int32 * 3),
        ("canyon_l", ctypes.c_int32 * 3),
        ("canyon_r", ctypes.c_int32 * 3),
        ("done", ctypes.c_int32),
        ("ev_cleared", ctypes.c_int32),
        ("ev_items", ctypes.c_int32),
        ("ev_crashed", ctypes.c_int32),
        ("ev_restored", ctypes.c_int32),
        ("ev_level_up", ctypes.c_int32),
        ("stepped_frames", ctypes.c_int32),
    ]


_lib = None


def _load():
    global _lib, LIB_PATH
    if _lib is None:
        if LIB_PATH is None:
            LIB_PATH = _find_lib()
        if not LIB_PATH.exists():
            raise FileNotFoundError(
                f"{LIB_PATH} missing; run `make -C {ROOT / 'cpp'} lib`"
            )
        lib = ctypes.CDLL(str(LIB_PATH))
        lib.canyon_create.restype = ctypes.c_void_p
        lib.canyon_create.argtypes = []
        lib.canyon_destroy.argtypes = [ctypes.c_void_p]
        lib.canyon_reset.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int32,
            ctypes.c_int32,
            ctypes.c_int32,
        ]
        lib.canyon_step.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32]
        lib.canyon_obs.argtypes = [ctypes.c_void_p, ctypes.POINTER(CanyonObs)]
        _lib = lib
    return _lib


class CanyonEnv:
    def __init__(self, lives=1, fast_crash=True, action_repeat=4):
        self._lib = _load()
        self._env = self._lib.canyon_create()
        self.lives = lives
        self.fast_crash = fast_crash
        self.action_repeat = action_repeat
        self.obs = CanyonObs()
        self.reset()

    def close(self):
        env = getattr(self, "_env", None)
        if env:
            self._lib.canyon_destroy(env)
            self._env = None

    def __del__(self):
        self.close()

    def reset(self, seed=0):
        self._lib.canyon_reset(
            self._env,
            int(self.lives),
            1 if self.fast_crash else 0,
            int(seed) & 0xFFFF,
        )
        self._read_obs()
        return self.obs

    def _read_obs(self):
        self._lib.canyon_obs(self._env, ctypes.byref(self.obs))
        return self.obs

    def step(self, action: int):
        """Apply one mini-action for action_repeat frames.

        Returns (obs, done, events) where events is a dict of per-step counts.
        """
        if not 0 <= action < len(ACTION_NAMES):
            raise ValueError(f"bad action {action}")
        self._lib.canyon_step(self._env, int(action), int(self.action_repeat))
        o = self._read_obs()
        events = {
            "cleared": o.ev_cleared,
            "items": o.ev_items,
            "crashed": o.ev_crashed,
            "restored": o.ev_restored,
            "level_up": o.ev_level_up,
        }
        return o, bool(o.done), events

    @property
    def done(self):
        return bool(self.obs.done)


def obs_text(o: CanyonObs) -> str:
    """Compact, LM-friendly game-state observation."""
    state = "ground" if o.state == 0 else ("jump" if o.state == 1 else "crash")
    lines = [
        f"x={o.ship_x} y={o.ship_y} z={o.ship_z} {state} inv={o.invincible} "
        f"lv={o.level} sc={o.score} spd={o.obstacle_speed}",
        f"canyon {o.canyon_left}..{o.canyon_right} dx={o.dx_center} "
        f"gl={o.ship_x + 8 - o.canyon_left} "
        f"gr={o.canyon_right - (o.ship_x + 16)}",
    ]
    rows = [
        f"{o.canyon_l[i]}..{o.canyon_r[i]}"
        for i in range(min(o.n_canyon, 3))
    ]
    if rows:
        lines.append("front " + " ".join(rows))
    obs_parts = []
    for i in range(min(o.n_ahead, 3)):
        obs_parts.append(
            f"dy{o.ahead_dy[i]} {TYPE_NAMES[o.ahead_type[i]]} "
            f"{o.ahead_l[i]}..{o.ahead_r[i]} "
            f"dl{o.ahead_dx_l[i]} dr{o.ahead_dx_r[i]}"
        )
    lines.append("obs " + ("; ".join(obs_parts) if obs_parts else "none"))
    return "\n".join(lines)


@dataclass
class Transition:
    tokens: List[int]
    action: int
    logprob: float
    value: float
    reward: float
    done: bool
    valid: bool