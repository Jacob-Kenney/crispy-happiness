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
