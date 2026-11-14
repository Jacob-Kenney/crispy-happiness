Jetson input smoke test for the Canyon Racer agent ROM and Shiny Relay 1.1.12.

The Jetson must have an IP route to the Chromatic (default 10.42.0.60).
Enable Shiny Relay's link cable in AUTO mode and run the agent ROM.
The app uses CWL2 UDP port 47820 and the checksum-verified Shiny cable core.
It transmits input bytes and receives semantic game state, without video.

Deploy from this directory with an authenticated Wendy CLI:

```sh
wendy --json run --device '<discovered-Jetson-selector>' --detach --wait-ready --yes
```

After the first valid game state, it settles for four seconds then issues
START, LEFT, RIGHT, each held for three seconds and followed by six seconds
of release. It runs once, then leaves controls released. Restart the app to
repeat. Five seconds without fresh telemetry aborts the sequence.

GET /status on port 8787 returns packet counts, latest game state, and per-button
echo/release results. A running container or HTTP health response alone does
not prove that the Chromatic received commands. `passed` requires all three
button masks and their releases to appear in returned ROM telemetry; ship
movement and menu transitions still require interpretation of that telemetry.

## Play over the GameBoy hotspot

Start the [Jetson policy server](../../canyon-server/README.md#jetson-docker-policy-endpoint).
Copy `policy-controller.mjs`, `observation.mjs`, `protocol.mjs`, and `sglink.wasm` into
`/var/lib/canyon-rl/chromatic` on the Jetson. Stop the input smoke test if it is
running, since both controllers use UDP port 47820.

```sh
docker run -d --name canyon-chromatic --network host \
  -v /var/lib/canyon-rl/chromatic:/app:ro node:22-alpine \
  node /app/policy-controller.mjs
docker logs -f canyon-chromatic
```

On the Chromatic, join `GameBoy`, enable the wireless cable in AUTO mode, and
launch the agent version of Canyon Racer. On the tested unit this is library
entry 2 (`lib play 2` in the USB console). The policy sends START on menus and
uses only gameplay buttons during a race. Check `game-state` and `decision`
events for live state, action timings, and the ROM's `remoteKeys` and score.
Set `CHROMATIC_IP` if its DHCP address changes. `docker stop canyon-chromatic`
stops the controller.

The rebuilt agent ROM can wait for each policy decision and apply it for four
game frames. `paced: true` in the controller logs identifies these requests.
Expect the game to advance in visible steps on the wireless cable, which sends
the game snapshot one serial byte at a time.

The physical pacing test used the rebuilt ROM in temporary game RAM. On this
unit, the verified game staging address was `0x210000` while the MCU was running
from `ota_0`. After staging and verifying the 32 KiB ROM there, the USB console
commands were `rc load 0x210000 32768`, `rc ram 8192`, `rc on 5 2 1` and `cwl on`.
These commands load a fresh save in RAM. Confirm the staging address with
`cart fw` for the current firmware before any USB staging write.

## Mailbox UDP controller

Build the ROM with `GBDK_HOME=/path/to/gbdk ./chromatic/build-agent-rom.sh mailbox`.
Run it on the Chromatic with the matching mailbox ESP32 firmware prepared by
`chromatic-bridge/prepare-mailbox.sh` in the containing workspace. Copy
`mailbox-controller.mjs`, `observation.mjs`, and `protocol.mjs` to the Jetson,
then start the policy server and run:

```sh
docker run -d --name canyon-mailbox --network host \
  -v /var/lib/canyon-rl/chromatic:/app:ro node:22-alpine \
  node /app/mailbox-controller.mjs
```

The ROM sends one request at a time and checks replies on gameplay frames.
It counts four VBlanks as the decision deadline and discards unanswered requests
after six. Each applied reply supplies up to four frames of input. On a miss,
the last command expires and the game continues. The `age1` through `age6`
counters report the request-to-input time measured by the ROM in VBlanks,
`late` includes usable late replies, and `expired` counts discarded requests.
Compare changes in `frame` and `vblank` during active racing to measure the
game's update rate, excluding menus and the game's rewind animations.

Press **START** to pause, **SELECT** to switch between **BOT** and **YOU**,
then **START** to resume. The pause screen shows who will drive. The ROM
starts in **BOT** mode. While paused or in **YOU** mode, it sends no game
state and applies no bot input. The live graph resumes when bot play resumes.

### Startup and recovery

The Chromatic prefers `Road Racer 3 Lives` at startup, followed by
`Road Racer Road Edition`, `Canyon-Select`, and `Canyon-Mailbox`. To play the original
version for a session, exit the current game and select `Canyon-Mailbox` in
the library, or run `rc stop` followed by `lib play <UID>` in the USB console.
Use `lib ls` to find the original entry's UID. Each entry has its own save.

To build the ESP32 image from a Shiny Relay 1.1.12 source tree in
`.chromatic/shiny-relay-src-1.1.12`, run both preparation scripts from a
workspace that contains that directory:

```sh
workspace="$PWD"
sh crispy-happiness/chromatic/firmware/prepare-bridge.sh "$workspace"
sh crispy-happiness/chromatic/firmware/prepare.sh "$workspace"
docker run --rm --mount "type=bind,source=$workspace/.chromatic/bridge-build/mcu,target=/project" \
  -w /project espressif/idf:v5.3 idf.py \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usbcart;sdkconfig.wifiinst;sdkconfig.bridge' build
```

The verified device uses `ota_0` at `0x10000` for the ESP32 image and the
game staging region at `0x210000`. From the USB console,
`lib imp 0x210000 32768 play Canyon-Select` imports a staged raw ROM and
starts it. `lib ls` verifies the entry. Keep the original application backup
and verify the partition layout before flashing. The game library records its
game saves in flash. A reboot starts a new race.

On the Jetson, install [gameboy-hotspot.service](../jetson/gameboy-hotspot.service)
as `/etc/systemd/system/canyon-gameboy-hotspot.service`. It uses the existing
`GameBoy-Live` NetworkManager profile and holds `10.42.0.60` for the Chromatic.
The box has the model and controller files under `/var/lib/canyon-rl`. Enable
the hotspot service and configure the two existing containers to restart:

```sh
systemctl daemon-reload
systemctl enable canyon-gameboy-hotspot.service docker.service
docker update --restart unless-stopped canyon-rl canyon-mailbox
```

Verify with `iw dev wlan0 info`, `docker ps`, and `docker logs --tail 20
canyon-mailbox`. A rising `applied` counter confirms that decisions are
reaching the physical game.

## Online learning on the Jetson

Set `CANYON_LEARNING_DIR=/learning` on the mailbox controller and mount a
writable host directory there. The deployed learner uses
`/var/lib/canyon-rl/online-policy`. It keeps the GGUF as its baseline and adjusts
move preferences by game context. Score gains and crashes update preferences
for inputs the Chromatic confirms applying. The controller occasionally samples
an alternative move near an obstacle. It saves a checkpoint after each observed
crash and keeps the last 32 archives.

Run the rollback commands from the Jetson shell. To restore a prior checkpoint,
list the archives and substitute the selected filename:

```sh
cd /var/lib/canyon-rl/online-policy
ls -t checkpoint-*.json
docker stop canyon-mailbox
cp checkpoint-SELECTED.json current.json
docker start canyon-mailbox
```

To restore the original controller, including the original action selection:

```sh
docker stop canyon-mailbox
docker update --restart no canyon-mailbox
docker rename canyon-mailbox canyon-mailbox-online
docker rename canyon-mailbox-baseline canyon-mailbox
docker update --restart unless-stopped canyon-mailbox
docker start canyon-mailbox
```
