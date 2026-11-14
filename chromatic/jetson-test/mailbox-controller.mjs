import {createSocket} from 'node:dgram';
import {BUTTONS, decodeState} from './protocol.mjs';
import {observation} from './observation.mjs';
import {openOnlinePolicy} from './online-policy.mjs';

const host = process.env.CHROMATIC_IP || '10.42.0.60';
const bind = process.env.GAMEBOY_IP || '10.42.0.1';
const actions = ['wait', 'left', 'right', 'up', 'down', 'a', 'b'];
const socket = createSocket('udp4');
const port = 47821;
let menuPressAt = -Infinity;
let active = false;
let skipped = 0;
let errors = 0;
const log = (event, fields = {}) => console.log(JSON.stringify({event, ...fields, at: Date.now()}));
const learner = process.env.CANYON_LEARNING_DIR
  ? await openOnlinePolicy(process.env.CANYON_LEARNING_DIR, log) : null;
let lastSequence;

function reply(remote, sequence, button) {
  const response = Buffer.from([67, 65, sequence & 255, sequence >> 8, button]);
  socket.send(response, remote.port, remote.address);
}

socket.on('message', async (packet, remote) => {
  if (remote.address !== host || packet.length !== 90) return;
  let state;
  try {
    state = decodeState(packet.subarray(0, 64), false);
  } catch {
    return;
  }
  lastSequence = state.sequence;

  const fields = ['scheduled', 'sent', 'applied', 'late', 'age1', 'age2', 'age3', 'age4', 'age5', 'age6',
    'race', 'localOverride', 'vblank'];
  const metrics = Object.fromEntries(fields.map((key, index) => [key, packet.readUInt16LE(64 + 2 * index)]));
  learner?.observe(state, metrics, packet[62]);
  log('state', {sequence: state.sequence, frame: state.frame, playing: state.playing,
    score: state.score, lives: state.lives, localKeys: state.localKeys, remoteKeys: state.remoteKeys,
    valid: packet.readUInt16LE(37), rejected: packet.readUInt16LE(39), expired: packet.readUInt16LE(41),
    lastAge: packet[43], status: packet[44], version: packet[45], length: packet[46],
    phase: packet[62], ...metrics});

  if (!state.playing) {
    const now = performance.now();
    const start = now - menuPressAt >= 1500;
    if (start) menuPressAt = now;
    reply(remote, state.sequence, start ? BUTTONS.start : 0);
    return;
  }
  if (active) {
    ++skipped;
    log('inference-busy', {sequence: state.sequence, skipped});
    return;
  }
  active = true;
  const started = performance.now();
  try {
    const last = actions.find(action => BUTTONS[action] === state.remoteKeys) || 'wait';
    const response = await fetch('http://127.0.0.1:8080/action', {
      method: 'POST', body: observation(state, packet, last),
      signal: AbortSignal.timeout(90),
    });
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const result = await response.json();
    if (!actions.includes(result.action)) throw new Error('Invalid action');
    if (lastSequence !== state.sequence) return;
    const action = learner?.choose(state, packet, result.action) ?? result.action;
    reply(remote, state.sequence, BUTTONS[action] ?? 0);
    log('action', {sequence: state.sequence, action, baseline: result.action, gpuMs: result.inference_ms,
      serviceMs: performance.now() - started});
  } catch (error) {
    log('inference-error', {sequence: state.sequence, message: error.message, errors: ++errors});
  } finally {
    active = false;
  }
});
socket.on('error', error => {
  log('socket-error', {message: error.message});
  process.exit(1);
});
socket.bind(port, bind, () => log('ready', {bind, host, port}));
async function shutdown() {
  socket.close();
  await learner?.flush();
  process.exit(0);
}
process.once('SIGTERM', shutdown);
process.once('SIGINT', shutdown);
