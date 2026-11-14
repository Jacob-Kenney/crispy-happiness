import { createSocket } from 'node:dgram';
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { open, rename, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { BUTTONS, decodeState } from './protocol.mjs';

const page = readFileSync(new URL('./index.html', import.meta.url));
const udp = createSocket('udp4');
const bind = process.env.GAMEBOY_IP || '10.42.0.1';
const handheld = process.env.CHROMATIC_IP || '10.42.0.60';
const panelHost = process.env.PANEL_IP || '192.168.2.2';
const model = process.env.MODEL_URL || 'http://127.0.0.1:8080/action';
const hazards = ['', 'Chicken from left', 'Chicken from right', 'Pedestrian from left',
  'Pedestrian from right', 'Cone in left lane', 'Cone in center lane', 'Cone in right lane'];
const queue = [];
let id = 0;
let latest = null;
let menuPressAt = -Infinity;
let inFlight = false;
let checks = 0;
let hits = 0;
let lastCrash = false;
let decision = 'waiting';
let modelAction = null;
let lastPlaced = '';
let modelError = false;
const activity = [];
let eventId = 0;
const scoreFile = join(process.env.ROAD_DATA_DIR || '/data', 'leaderboard.json');
let leaderboard = [];
let nextPlayer = 1;
try {
  const stored = JSON.parse(readFileSync(scoreFile, 'utf8'));
  if (Array.isArray(stored)) {
    // The original leaderboard stored a score-sorted array without player numbers.
    for (const run of [...stored].sort((a, b) => a.at - b.at)) {
      if (run.driver !== 'Bot') run.player = nextPlayer++;
    }
    leaderboard = stored;
  } else {
    ({ runs: leaderboard, nextPlayer } = stored);
  }
  if (!Array.isArray(leaderboard) || !Number.isSafeInteger(nextPlayer) || nextPlayer < 1) {
    throw new Error('Invalid leaderboard data');
  }
} catch (error) {
  if (error.code !== 'ENOENT') throw error;
}
let saveScores = Promise.resolve();
let race = null;
let racePacket = null;

function changeScores(change) {
  const pending = saveScores.then(async () => {
    const update = change();
    if (!update) return false;
    await writeFile(scoreFile + '.new', JSON.stringify(update) + '\n', { flush: true });
    await rename(scoreFile + '.new', scoreFile);
    const dataDir = await open(dirname(scoreFile), 'r');
    try {
      await dataDir.sync();
    } finally {
      await dataDir.close();
    }
    ({ runs: leaderboard, nextPlayer } = update);
    return true;
  });
  saveScores = pending.catch(error => console.error('Could not save leaderboard:', error.message));
  return pending;
}

function trackRace(state, manual, packet, now) {
  if (racePacket && now - racePacket.at < 2000) {
    const advance = (state.sequence - racePacket.sequence) & 0xffff;
    if (!advance || advance >= 0x8000) return;
  }
  racePacket = { sequence: state.sequence, at: now };
  const phase = packet[62];
  const number = packet.readUInt16LE(84); // RACE counter in the mailbox metrics.
  if (!state.playing && phase !== 5) {
    race = null;
    return;
  }
  if (phase === 5) { // The ROM sends its final score after the last rewind is spent.
    if (!race || race.number !== number) return;
    const { drivers } = race;
    race = null;
    if (!drivers) return;
    const driver = drivers === 1 ? 'You' : drivers === 2 ? 'Bot' : 'Mixed';
    changeScores(() => {
      const result = { driver, score: state.score, at: now };
      if (driver !== 'Bot') result.player = nextPlayer;
      let botScores = 0;
      return {
        runs: [...leaderboard, result]
          .sort((a, b) => b.score - a.score || a.at - b.at)
          .filter(run => run.driver !== 'Bot' || ++botScores <= 10),
        nextPlayer: nextPlayer + Number(driver !== 'Bot'),
      };
    });
    return;
  }
  if (!race || race.number !== number) race = { number, drivers: 0 };
  if (!state.paused) race.drivers |= manual ? 1 : 2;
}

function recordEvent(type, label, at) {
  activity.unshift({ id: ++eventId, type, label, at });
  if (activity.length > 8) activity.pop();
}

function formatObservation(state, packet) {
  const { x, y, z, state: pose, counter } = state.ship;
  const road = packet[58];
  const ahead = [...packet.subarray(59, 62)].map(left => left + '..' + (left + 32)).join(' ');
  const obs = state.obstacles.filter(o => !(o.type & 128) && o.y <= y + 22)
    .sort((a, b) => b.y - a.y).slice(0, 3)
    .map(o => 'dy' + (y - o.y) + ' middle ' + o.left + '..' + (o.left + 8) +
      ' dl' + (o.left - x) + ' dr' + (o.left + 8 - x));
  const last = ['left','right','up','down','a'].find(action => state.remoteKeys === BUTTONS[action]) || 'wait';
  return [
    'x=' + x + ' y=' + y + ' z=' + z + ' ' + (pose & 32 ? 'crash' : 'ground') +
      ' inv=' + Number(counter > 0) + ' lv=' + state.level + ' sc=' + state.score +
      ' spd=' + Math.round(state.obstacleSpeed * 256),
    'canyon ' + road + '..' + (road + 32) + ' dx=' + (x - road - 16) +
      ' gl=' + (x + 8 - road) + ' gr=' + (road + 32 - x - 16),
    'front ' + ahead,
    'obs ' + (obs.join('; ') || 'none'),
    'last=' + last,
  ].join('\n');
}

function steer(state, packet, proposed) {
  const road = packet[58];
  const x = state.ship.x + 12;
  const left = road + 6;
  const right = road + 26;
  const threat = state.obstacles.filter(o => !(o.type & 128) &&
      o.y <= state.ship.y + 22 && o.y >= state.ship.y - 104)
    .sort((a, b) => b.y - a.y)[0];
  if (x < left + 1) return 'right';
  if (x > right - 1) return 'left';
  const turn = target => x < target - 2 ? 'right' : x > target + 2 ? 'left' : 'wait';
  if (!threat) return turn(road + 16);

  const offset = y => {
    if (threat.type >= 5) return 0;
    const speed = threat.type <= 2 ? 2 : 4;
    const progress = Math.min(24, Math.floor(Math.max(0, y - 48) / speed));
    return threat.type % 2 ? progress : 24 - progress;
  };
  if (packet[54] >= 1 && !packet[52] && !packet[53] && !(state.ship.state & 32)) {
    const gap = state.ship.y + 5 - (threat.y + 8);
    const speed = state.obstacleSpeed;
    const contactY = threat.y + speed * Math.max(0, gap / (6 + speed) + 3);
    const aim = threat.left + offset(contactY) - offset(threat.y);
    if (gap >= 8 && x >= aim + 1 && x <= aim + 7 &&
        (proposed === 'a' || proposed === 'wait' || gap < 44)) return 'a';
  }
  const first = Math.max(threat.y, state.ship.y - 3);
  const last = Math.max(first, state.ship.y + 22);
  const ends = threat.type >= 5 ? [threat.left, threat.left] :
    [first, last].map(y => threat.left + offset(y) - offset(threat.y));
  const safeLeft = Math.min(...ends) - 6;
  const safeRight = Math.max(...ends) + 14;
  if (x <= safeLeft || x >= safeRight) return 'wait';
  const canTurnLeft = safeLeft >= left;
  const canTurnRight = safeRight <= right;
  if (canTurnLeft && canTurnRight) {
    if (proposed === 'left' || proposed === 'right') return proposed;
    return x - safeLeft < safeRight - x ? 'left' : 'right';
  }
  if (canTurnLeft) return 'left';
  if (canTurnRight) return 'right';
  return turn(x - left < right - x ? left + 1 : right - 1);
}

udp.on('message', async (packet, peer) => {
  if (peer.address !== handheld || packet.length !== 90 || packet[63] !== 0x52) return;
  let state;
  try { state = decodeState(packet.subarray(0, 64), false); }
  catch { return; }
  const now = Date.now();
  const manual = packet[54] >= 4 && packet[41] === 0;
  trackRace(state, manual, packet, now);
  const crashed = Boolean(state.playing && (state.ship.state & 32));
  const newCrash = crashed && !lastCrash;
  if (newCrash) hits++;
  lastCrash = crashed;
  const destroyed = packet[54] >= 1 ? packet.readUInt16LE(49) : 0;
  const rocket = packet[54] >= 1 && packet[52] ? { x:packet[51], y:packet[52] } : null;
  const splatter = packet[54] >= 2 && packet[39] ? {
    x:packet[37], y:packet[38], frames:packet[39], kind:packet[54] >= 3 ? packet[40] : 1,
  } : null;
  const rocketHit = latest && destroyed > latest.destroyed;
  if (rocketHit) {
    console.log(JSON.stringify({event:'rocket-hit', destroyed, at:now}));
    recordEvent('rocket', 'Rocket hit', now);
  }
  if (newCrash) recordEvent('crash', 'Car crashed', now);
  if (splatter && (!latest?.splatter || splatter.frames > latest.splatter.frames)) {
    const effects = { 1: 'Pedestrian hit', 2: 'Feathers flew', 3: 'Cone sparked' };
    if (effects[splatter.kind]) recordEvent('impact', effects[splatter.kind], now);
  }
  const roadProfile = [61, 60, 59, 58].map((byte, index) => ({
    y: Math.max(0, state.ship.y + [-40, -24, -8, 16][index]), left: packet[byte],
  }));
  latest = { state, manual, road: packet[58], roadProfile, at: now, ack: packet.readUInt16LE(47), destroyed, rocket, splatter };
  const ack = packet.readUInt16LE(47);
  if (queue[0]?.id === ack) {
    lastPlaced = hazards[queue.shift().kind];
    recordEvent('placed', lastPlaced, now);
    console.log(JSON.stringify({event:'placed', hazard:lastPlaced, at:now}));
  }
  let action = 'wait';
  if (state.paused || manual) {
    modelAction = null;
    decision = state.paused ? 'paused' : 'manual';
  } else if (!state.playing) {
    if (now - menuPressAt >= 1500) {
      menuPressAt = now;
      action = 'start';
    }
    decision = 'waiting';
  } else {
    if (inFlight) return;
    inFlight = true;
    let proposed;
    try {
      const response = await fetch(model, {
        method: 'POST', body: formatObservation(state, packet), signal: AbortSignal.timeout(60),
      });
      if (!response.ok) throw new Error('Model returned ' + response.status);
      proposed = (await response.json()).action;
      if (!Object.hasOwn(BUTTONS, proposed) && proposed !== 'wait') throw new Error('Invalid model action');
      if (!checks) console.log(JSON.stringify({event:'model-ready', action:proposed}));
      checks++;
      modelError = false;
    } catch (error) {
      if (!modelError) console.error(error.message);
      modelError = true;
    } finally {
      inFlight = false;
    }
    if (latest.state !== state) return;
    modelAction = proposed || null;
    action = steer(state, packet, proposed);
    decision = action === 'a' ? 'fire' : action;
  }
  const next = state.playing && !state.paused && !crashed ? queue[0] : undefined;
  const reply = Buffer.from([67, 65, packet[4], packet[5], BUTTONS[action] || 0, next?.kind || 0, 0, 0]);
  reply.writeUInt16LE(next?.id || 0, 6);
  udp.send(reply, peer.port, peer.address);
});

async function renamePlayer(req, res, player) {
  const error = (status, message) => {
    res.statusCode = status;
    res.end(JSON.stringify({ error: message }));
  };
  try {
    const chunks = [];
    let size = 0;
    for await (const chunk of req.iterator({ destroyOnReturn: false })) {
      size += chunk.length;
      if (size > 256) {
        req.resume();
        return error(413, 'Name is too long.');
      }
      chunks.push(chunk);
    }
    const name = Buffer.concat(chunks).toString('utf8').trim();
    if ([...name].length > 24 || /[\p{Cc}\p{Bidi_Control}]/u.test(name)) {
      return error(400, 'Use up to 24 characters on one line.');
    }
    const saved = await changeScores(() => {
      if (!leaderboard.some(run => run.player === player)) return null;
      return {
        runs: leaderboard.map(run => run.player === player ? { ...run, name } : run),
        nextPlayer,
      };
    });
    if (!saved) return error(404, 'This player could not be found.');
    res.end(JSON.stringify({ name }));
  } catch {
    if (!res.destroyed) error(500, 'Could not save the name. Try again.');
  }
}

const http = createServer((req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  if (req.method === 'GET' && req.url === '/') {
    res.setHeader('Content-Type', 'text/html; charset=utf-8');
    return res.end(page);
  }
  res.setHeader('Content-Type', 'application/json');
  const playerPath = /^\/leaderboard\/players\/([1-9][0-9]*)$/.exec(req.url);
  if (req.method === 'PUT' && playerPath && req.headers['x-road-racer'] === '1') {
    return void renamePlayer(req, res, Number(playerPath[1]));
  }
  const connected = latest && Date.now() - latest.at < 2000;
  if (req.method === 'GET' && req.url === '/state') {
    return res.end(JSON.stringify({ connected: !!connected, playing: Boolean(connected && latest.state.playing),
      manual: Boolean(connected && latest.manual), paused: Boolean(connected && latest.state.paused),
      decision, modelAction, modelError, checks, hits, lastPlaced, queue:queue.map(event => hazards[event.kind]),
      car: connected ? latest.state.ship : null, road: latest?.road,
      roadProfile: connected && latest.state.playing ? latest.roadProfile : [],
      frame: connected ? latest.state.frame : 0, crashed: connected && lastCrash, activity, leaderboard,
      obstacles: connected ? latest.state.obstacles.filter(o => !(o.type & 128)).map(o => ({...o, kind:o.type})) : [],
      rocket: connected ? latest.rocket : null, destroyed: latest?.destroyed || 0,
      splatter: connected ? latest.splatter : null,
      cleared: connected ? Math.floor(latest.state.score / 10) : 0 }));
  }
  if (req.method === 'POST' && /^\/hazards\/[1-7]$/.test(req.url) &&
      req.headers['x-road-racer'] === '1') {
    if (!connected || !latest.state.playing || latest.state.paused || queue.length >= 6) {
      res.statusCode = 409;
      return res.end(JSON.stringify({ error: queue.length >= 6 ? 'Queue is full' :
        latest?.state.paused && connected ? 'Resume on the Chromatic to place hazards' : 'Waiting for the Chromatic' }));
    }
    const kind = Number(req.url.at(-1));
    id = id % 65535 + 1;
    if (id === latest.ack) id = id % 65535 + 1;
    queue.push({ kind, id });
    return res.end(JSON.stringify({ queued: hazards[kind] }));
  }
  res.statusCode = 404;
  res.end('{}');
});
udp.on('error', error => { console.error(error); process.exit(1); });
http.on('error', error => { console.error(error); process.exit(1); });
udp.bind(Number(process.env.ROAD_PORT || 47823), bind);
http.listen(Number(process.env.PANEL_PORT || 8789), panelHost, () => console.log('Road Racer panel on ' + panelHost));

process.once('SIGTERM', () => {
  udp.close();
  http.close(async () => {
    await saveScores;
    process.exit(0);
  });
});
