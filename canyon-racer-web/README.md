# Canyon Racer for the browser

A Next.js App Router player for the original Game Boy demo. Node.js 20.9+ is required.

```sh
npm ci
npm run dev
```

Open [localhost:3000](http://localhost:3000) and click **Play game**. Press Enter on the title screen. The first time you play, press Enter or Z once the instructions finish animating. For production, run `npm run build` followed by `npm start`.

- Arrow keys or WASD steer.
- Z jumps. Hold Z and Up for a longer jump.
- X sets a checkpoint and spends one life. The game rewinds to the checkpoint on a crash if you have a life left.
- Enter starts or pauses. On the in-game help screen, hold Shift and press Up to reset the high score.
- The on-screen controls accept touch and mouse input. Cartridge saves use browser storage for the current site.

The app serves the ROM, emulator, and fonts locally. The original [Canyon Racer game](https://bbbbbr.itch.io/canyon-racer) was made by bbbbbr, with music and SFX by BeatScribe. The unmodified Game Boy ROM is from the author's [browser release](https://html-classic.itch.zone/html/7751638/rom/game.gb). The upstream source and full credits are in the [Canyon Racer repository](https://github.com/bbbbbr/canyon-racer). See the [game license](public/rom/LICENSE.txt) (CC BY-NC-SA 4.0).

The WebAssembly build of [binjgb](https://github.com/binji/binjgb/tree/f7af6eab39964e54d31f8367a3b9a0b40b1dbeff) is by Ben Smith and is distributed under the [MIT license](public/emulator/LICENSE.txt).

## Leaderboard

Connect a private Vercel Blob store to the project. The SDK uses the connected store’s `BLOB_STORE_ID` and Vercel OIDC credentials. For local development, set `BLOB_READ_WRITE_TOKEN` for that private store in `.env.local`.

The leaderboard serves the top 20 from a single JSON blob. The browser loads it once and can refresh it on demand. Public score responses are cached by Vercel for 30 seconds with stale revalidation. Players submit their cartridge high score under a display name. Each submission reads one blob and writes it only if the ranking changes. Conditional writes prevent simultaneous submissions from dropping scores. The submission response includes the updated leaderboard so the browser skips another read.

The board keeps one best score per browser profile. Scores come from the browser and are suitable for casual competition. The server checks the score format and bounds.
