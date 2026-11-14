# Canyon Racer on Shiny Relay

The **real ROM on the Chromatic owns the simulation and renders the game**.
It sends semantic state to an agent, which sends buttons back. No screenshots,
tiles, video frames, emulator save states, or duplicate simulation are streamed.

```
Chromatic ROM → Shiny Relay wireless cable → WSS /link → bridge → localhost UDP agent
             ←       button replies       ←            ←
```

This implementation uses **Shiny Relay's existing CWL2 wireless cable mode**.
UDP is the local agent interface; the internet leg is WebSocket/TCP, not UDP.
It does not implement Shiny's private native network-window/room SDK. The cable
mode exchanges serial bytes and can have substantial latency over the internet.
The 7.5 snapshots/second setting is an upper bound, not a measured device rate.
Physical Chromatic operation and live account authentication still need validation.

## Build and install

Requires GBDK-2020 (tested with 4.5.0), Node.js 22+ and Python 3 for the example.

```sh
make -C canyon-racer gb AGENT_LINK=1 CART_TYPE=mbc5 GBDK_HOME=/path/to/gbdk/
```

ROM: `canyon-racer/build/gb/canyon_racer_0.7.7_agent_mbc5.gb` (32 KB, MBC5).
The agent build has separate objects and filename. Builds without `AGENT_LINK=1`
retain normal controls and do not activate serial telemetry.

Install the ROM using your existing cartridge workflow, or Shiny Relay's
[cartridge installer](https://install.shinyrelay.com/cart/). Preserve the backup
of the cartridge as described in [Shiny's instructions](https://shinyrelay.com/docs/).
This project does not flash firmware or cartridges automatically.

## Connect

1. On the Chromatic open MENU → NETWORK and **host a link-cable room**.
   Use the cable mode, not a native Shiny multiplayer game room. Note its code.
2. Sign in to [Shiny's official web player](https://shinyrelay.com/play/).
   The bridge needs a short-lived multiplayer ticket from your own account.
   Until Shiny publishes a standalone developer sign-in flow, obtain a ticket
   in that page's developer console using the snippet below. It uses the signed-in
   page's existing session; do not paste account tokens into chat or source files.
3. Save **only the returned ticket** to a private file, e.g. `/tmp/shiny-ticket`.
   Start the bridge immediately; tickets expire.

Run in the console of the signed-in official player:

```js
const session = JSON.parse(sessionStorage.getItem('shiny_webplay_session_v2'));
const response = await fetch('https://api.shinygen.ai/mp_ticket', {
  method: 'POST',
  headers: {Authorization: `Bearer ${session.access}`, 'Content-Type': 'application/json'},
  body: '{}', cache: 'no-store'
});
if (!response.ok) throw new Error(`Ticket HTTP ${response.status}; sign in again`);
const {ticket} = await response.json();
copy(ticket); // Chromium DevTools helper: copy only the short-lived ticket
```

Download the exact public networking core used by Shiny's player, with SHA-256
verification. This third-party binary is not redistributed in the repository.

```sh
node chromatic/bridge.mjs --fetch-core
node chromatic/bridge.mjs --room ABCDEF --ticket-file /tmp/shiny-ticket
python3 chromatic/agent_example.py
```

The example steers toward the canyon center, not a trained obstacle-avoidance
policy. Press A on the Chromatic to start, or send `a` / `start` through UDP.
Send an empty button list between menu presses; avoid holding START into gameplay
because it pauses the game. Local buttons remain usable throughout.

The bridge connects only to Shiny's official `wss://mp.shinygen.ai/link` endpoint,
with `kind: cwl2`. Account/room policies remain enforced by Shiny Relay.
It exits on disconnect; get a fresh ticket and restart to reconnect.

## Agent API

Send JSON UDP datagrams to `127.0.0.1:8765` from one persistent socket:

```json
{"sequence":1,"buttons":["left","a"]}
```

Buttons: `right`, `left`, `up`, `down`, `a`, `b`, `select`, `start`.
An empty array releases all buttons. Increase `sequence` on each command;
duplicate/reordered commands are discarded during an active controller lease.
Refresh at least every 500 ms. Only one local socket can own the controller lease.

Replies go to that socket as decoded JSON snapshots, including `ageMs` (age since
receipt at the bridge, **not** total network latency). Check `initialized`,
`playing`, `paused` and packet `sequence` before acting. Telemetry is available
on menus but gameplay fields are meaningful only after `initialized` is true.
After a round, `playing` becomes false and the last gameplay fields remain.
Frame and telemetry sequence counters wrap at 65536. Snapshot sequence can skip;
each packet is independent so no delta baseline or recovery download is needed.

The bridge clears buttons after 500 ms without an agent command. The ROM also
expires received buttons after 180 VBlanks (about 3 seconds) without a valid cable control message.
Delivery of a changed command depends on the cable transport. Pauses, fades and
rewind animations use the original game's timing; some do not poll input.

## Wire format (version 1)

All fields are unsigned bytes except the explicitly little-endian words.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 3 | ASCII `CR`, version 1 |
| 3 | 1 | flags: initialized=1, playing=2, paused=4 |
| 4 | 2 | snapshot sequence, little-endian |
| 6 | 2 | VBlank counter, little-endian |
| 8 | 7 | ship x/y/z pixels, raw ship state/counter, level, packed BCD lives |
| 15 | 2 | local and effective remote button masks, GBDK bit layout |
| 17 | 4 | packed BCD score, least-significant byte first |
| 21 | 1 | obstacle count, at most 5 |
| 22 | 15 | five `(y, raw type/flags, canyon left at y)` records |
| 37 | 18 | canyon left x at rows 0, 8, …, 136; canyon width is 32 |
| 55 | 2 | obstacle speed, unsigned 8.8 fixed point, little-endian |
| 57 | 6 | reserved, zero |
| 63 | 1 | CRC-8 polynomial 0x07, init=0 over bytes 0–62 |

The reverse serial stream repeats `[0xA5, buttons, buttons XOR 0x5A]`.
Commands may cross state-packet boundaries. A partial command cannot change input.

Snapshots are at most 64 × 7.5 = **480 payload bytes/sec**, plus cable protocol,
WebSocket, TCP/TLS and IP overhead. Cable mode sends several protocol packets per
serial byte, so actual network traffic is higher and latency can reduce the rate.
The ROM never queues old snapshots: it captures again only after transmission
finishes. Canyon samples are an agent observation, not a pixel-perfect remote
renderer or complete restore state. Rendering stays on the Chromatic itself.

## Validation

```sh
node --test tests/chromatic_link.test.mjs
```

The ROM integration test additionally needs paths to Shiny's public `gba` and
`sglink` WASM cores (listed in its web player's `manifest.json`):

```sh
CANYON_TEST_GBA=/path/to/gba.wasm CANYON_TEST_SGLINK=/path/to/sglink.wasm \
  node --test tests/chromatic_link.test.mjs
```

This runs the built ROM with Shiny's mGBA core and connects two CWL2 cores in
memory. It validates the ROM's actual serial packets and remote controls without
claiming a live network or hardware test.

Original Canyon Racer and modifications to it are under its
[CC-BY-NC-SA-4.0 license](../canyon-racer/LICENSE), copyright bbbbbr and contributors.
See Shiny's [source notices](https://shinyrelay.com/play/licences.html) for its
networking core and emulator. This project is not affiliated with Shiny or ModRetro.
