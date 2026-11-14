import {mkdir, open, readFile, readdir, rename, unlink} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import {join} from 'node:path';
import {BUTTONS} from './protocol.mjs';

const alternatives = ['wait', 'left', 'right', 'a'];
const allActions = new Set([...alternatives, 'up', 'down', 'b']);
const traceFrames = 24;
const maxEntries = 2000;
const difference = (next, previous) => (next - previous + 65536) % 65536;
const clamp = value => Math.max(-4, Math.min(4, value));

function context(state, wall, remaining, baseline) {
  const offset = state.ship.x + 12 - (wall + 16);
  const side = offset < -3 ? 'left' : offset > 3 ? 'right' : 'center';
  const obstacle = state.obstacles.filter(item => {
    const type = item.type & 7;
    return !(item.type & 128) && type < 4 && item.y <= state.ship.y + 16;
  }).sort((a, b) => b.y - a.y)[0];
  const distance = obstacle ? state.ship.y - obstacle.y : Infinity;
  const range = distance <= 10 ? 'near' : distance <= 32 ? 'approach' : distance <= 55 ? 'ahead' : 'clear';
  const curve = remaining - wall;
  return [baseline, side, curve < -3 ? -1 : curve > 3 ? 1 : 0,
    range, range === 'clear' ? '-' : obstacle.type & 7, state.ship.state & 1].join('/');
}

async function writeCheckpoint(dir, checkpoint) {
  const name = 'checkpoint-' + Date.now() + '-' + randomUUID() + '.json';
  const temporary = join(dir, '.' + name);
  const current = join(dir, 'current.json');
  const file = await open(temporary, 'wx', 0o600);
  try {
    await file.writeFile(JSON.stringify(checkpoint) + '\n');
    await file.sync();
  } finally {
    await file.close();
  }
  await rename(temporary, join(dir, name));
  const pointer = await open(current + '.tmp', 'w', 0o600);
  try {
    await pointer.writeFile(JSON.stringify(checkpoint) + '\n');
    await pointer.sync();
  } finally {
    await pointer.close();
  }
  await rename(current + '.tmp', current);
  const directory = await open(dir, 'r');
  try {
    await directory.sync();
  } finally {
    await directory.close();
  }
  const archives = (await readdir(dir)).filter(file => /^checkpoint-\d{13}-[a-f0-9-]+\.json$/.test(file)).sort();
  await Promise.all(archives.slice(0, -32).filter(file => file !== name).map(file => unlink(join(dir, file))));
}

export async function openOnlinePolicy(dir, log, random = Math.random) {
  await mkdir(dir, {recursive: true});
  let checkpoint = {version: 1, updates: 0, entries: {}};
  try {
    const saved = JSON.parse(await readFile(join(dir, 'current.json'), 'utf8'));
    if (saved.version !== 1 || !Number.isSafeInteger(saved.updates) || saved.updates < 0 ||
        !saved.entries || Array.isArray(saved.entries) || Object.keys(saved.entries).length > maxEntries ||
        Object.values(saved.entries).some(value => typeof value !== 'number' || Math.abs(value) > 4)) {
      throw new Error('Invalid online policy checkpoint');
    }
    checkpoint = saved;
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
  log('learning-ready', {updates: checkpoint.updates, contexts: Object.keys(checkpoint.entries).length});

  let previous;
  let proposed;
  let recent = [];
  let credit = new Map();
  let peak = 0;
  let persistence = Promise.resolve();
  const crashing = state => Boolean(state.ship.state & 32);

  function update(frame, outcome) {
    recent = recent.filter(move => difference(frame, move.frame) <= traceFrames);
    for (const move of recent) {
      const age = difference(frame, move.frame);
      const weight = 0.88 ** (age / 4);
      const previousCredit = credit.get(move.key);
      // Within a single reward event, repeated decisions in the same context
      // share the same credit rather than multiplying it.
      const delta = outcome * weight;
      if (previousCredit === undefined || Math.abs(previousCredit) < Math.abs(delta)) {
        credit.set(move.key, delta);
      }
    }
  }

  function finish(state, metrics) {
    const rewarded = credit;
    credit = new Map();
    update(state.frame, -1.4);
    for (const [key, value] of rewarded) credit.set(key, clamp((credit.get(key) ?? 0) + value));
    const entries = new Map(Object.entries(checkpoint.entries));
    for (const [key, value] of credit) {
      const adjusted = clamp((entries.get(key) ?? 0) * 0.99 + value * 0.4);
      entries.delete(key);
      entries.set(key, adjusted);
    }
    while (entries.size > maxEntries) entries.delete(entries.keys().next().value);
    checkpoint = {version: 1, updates: checkpoint.updates + 1, entries: Object.fromEntries(entries)};
    const saved = checkpoint;
    const evidence = {updates: saved.updates, adjusted: credit.size, peakScore: peak,
      sequence: state.sequence, race: metrics.race};
    persistence = persistence.then(async () => {
      await writeCheckpoint(dir, saved);
      log('learning-update', evidence);
    }).catch(error => log('learning-save-error', {message: error.message, updates: saved.updates}));
    recent = [];
    credit.clear();
    peak = state.score;
  }

  return {
    observe(state, metrics, phase) {
      const last = previous;
      previous = {state, metrics};
      const proposal = proposed;
      proposed = undefined;
      if (!last) return;
      const contiguous = difference(state.sequence, last.state.sequence) === 1 &&
        difference(metrics.vblank, last.metrics.vblank) <= 120 && metrics.race === last.metrics.race;
      if (!contiguous || state.localKeys || last.state.localKeys) {
        recent = [];
        credit.clear();
        peak = state.playing ? state.score : 0;
        return;
      }
      if (state.playing && last.state.playing && !crashing(last.state) &&
          proposal?.sequence === last.state.sequence &&
          difference(metrics.applied, last.metrics.applied) === 1 &&
          state.remoteKeys === (BUTTONS[proposal.action] ?? 0)) {
        recent.push({key: proposal.key, frame: state.frame});
      }
      recent = recent.filter(move => difference(state.frame, move.frame) <= traceFrames);
      if (state.playing && !crashing(state)) {
        peak = Math.max(peak, state.score);
        if (last.state.playing && !crashing(last.state) && state.score > last.state.score) {
          update(state.frame, Math.min((state.score - last.state.score) / 10, 2));
        }
      }
      if (last.state.playing && !crashing(last.state) &&
          (state.playing ? crashing(state) : phase === 5)) {
        finish(state, metrics);
      }
    },

    choose(state, packet, baseline) {
      if (!allActions.has(baseline) || state.localKeys || crashing(state) || packet[57] !== 1) return baseline;
      const key = context(state, packet[58], packet[59], baseline);
      const choices = [...new Set([baseline, ...alternatives])];
      const value = action => (checkpoint.entries[key + '/' + action] ?? 0) + (action === baseline ? 0.65 : 0);
      const best = choices.reduce((winner, action) => value(action) > value(winner) ? action : winner);
      const exploring = !key.includes('/clear/') && random() < 0.06;
      const action = exploring ? alternatives[Math.floor(random() * alternatives.length)] : best;
      proposed = {sequence: state.sequence, action, key: key + '/' + action};
      return action;
    },

    flush() { return persistence; },
  };
}
