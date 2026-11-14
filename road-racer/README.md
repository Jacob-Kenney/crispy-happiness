# RL RACER

The Chromatic starts **RL RACER** from its library on GameBoy Wi-Fi.
Open
`http://192.168.2.2:8789/` on the Mac connected by Ethernet. Pick a side for
a chicken or pedestrian, or a lane for a cone. Use **1–7** for the seven
placements from a keyboard, or tap the buttons on a phone. The live map shows
the car, hazards, and rockets. The queue shows what's coming next, and Race
Activity shows placements the handheld has acknowledged, impacts, and crashes.

Press **START** to pause, **SELECT** to switch to **YOU**, then **START**
to resume. Drive with the D-pad and fire with **A**. The browser keeps
showing the road and can place hazards during manual play. While the game
is paused, the browser shows who will drive next and waits to place hazards.
Turn on **Auto mode** in the dashboard to let the Jetson add hazards while
the bot drives. Auto pauses in YOU mode, during crashes, and while the
race is paused. The setting stays on when the browser closes and resets
to off when the game server restarts.

**A** fires a rocket straight ahead. The Jetson can fire when a hazard is
lined up. One rocket can be on screen at a time, with a short delay between
shots. The panel shows the rocket and counts confirmed hits.
Rocket hits and car collisions show feathers for chickens, sparks for cones,
and a brief pixel-art splatter for pedestrians. Both screens show the effect.

RL RACER reuses Canyon Racer's engine and the Jetson's Canyon model. The
controller reports each hazard's actual position to the model and can steer
away from a road edge or a nearby hazard. The ROM scrolls and checks
collisions locally. Crossing hazards move at different speeds. A placement
stays queued until the ROM acknowledges that it spawned.

Build with GBDK and Node from the repository root:

```sh
GBDK_HOME=/path/to/gbdk sh chromatic/build-agent-rom.sh road
```

The 32 KiB ROM is in `road-racer/build/Road-Racer.gb`. Build the Chromatic
firmware using [the existing preparation and staging steps](../chromatic/jetson-test/README.md#startup-and-recovery).
Import the staged ROM with `lib imp 0x210000 32768 play "RL RACER"`.
The firmware falls back to Road Racer 3 Lives, Road Racer Road Edition,
Canyon-Select, then Canyon-Mailbox if the preferred entry is missing.

On the Jetson, copy `server.mjs`, `index.html`, and
`chromatic/jetson-test/protocol.mjs` to `/var/lib/road-racer`:

```sh
docker run -d --name road-racer --restart unless-stopped --network host \
  -v /var/lib/road-racer:/app:ro -v /var/lib/road-racer/data:/data \
  node:22-alpine node /app/server.mjs
```

The ROM uses UDP 47823 on GameBoy. The panel binds to the Jetson's Ethernet
address. Set `PANEL_IP`, `GAMEBOY_IP`, and `CHROMATIC_IP` if addresses change.
The model endpoint defaults to `http://127.0.0.1:8080/action`.
For boot recovery, enable the GameBoy hotspot, NetworkManager wait-online,
and Docker services. Install [the Docker ordering override](docker-network-online.conf)
as `/etc/systemd/system/docker.service.d/30-road-racer-network.conf` and run
`systemctl daemon-reload`. The active RL RACER, Canyon inference, and Canyon
mailbox containers use `--restart unless-stopped`. Keep rollback containers stopped.

After reboot, check `systemctl is-active canyon-gameboy-hotspot docker`, check
the three containers with `docker ps`, and confirm the panel's `/state` endpoint
shows an advancing frame and working bot inference. If the handheld itself
reboots, it starts a new race. Completed leaderboard entries, player names,
and the Canyon learner's saved checkpoint are stored on the Jetson.

The leaderboard keeps human runs and the top 10 bot-only scores in the data
mount. The panel assigns a new player number to each finished human or mixed
run. Click a player's name to change it or clear it to restore the default.
RL RACER shares Canyon's inference service and adds its own steering and
firing rules. The online learner runs in the Canyon mailbox controller.

RL RACER modifies Canyon Racer by bbbbbr under CC BY-NC-SA 4.0.
