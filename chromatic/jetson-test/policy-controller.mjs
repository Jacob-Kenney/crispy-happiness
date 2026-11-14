import {readFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {createSocket} from 'node:dgram';
import {CablePeer, BUTTONS, CORE_SHA256} from './protocol.mjs';
import {observation} from './observation.mjs';

const host = process.env.CHROMATIC_IP || '10.42.0.60';
const bind = process.env.GAMEBOY_IP || '10.42.0.1';
const endpoint = 'http://127.0.0.1:8080/action';
const wasm = await readFile(new URL('./sglink.wasm', import.meta.url));
if (createHash('sha256').update(wasm).digest('hex') !== CORE_SHA256) {
  throw new Error('Cable core checksum mismatch');
}
const {instance} = await WebAssembly.instantiate(wasm, {});
instance.exports._initialize();

const socket = createSocket('udp4');
let latest;
let lastAction = 'wait';
let pending = false;
let decisions = 0;
let states = 0;
let received = 0;
let sentAt = 0;
let lastLog = 0;
let stopped = false;
let lastMenuPulse = 0;
let menuPulseUntil = 0;
let resetAfter = 0;
const log = (event, data = {}) => console.log(JSON.stringify({event, ...data}));

const sendPacket = packet => {
  socket.send(packet, 47820, host);
};
let peer = new CablePeer(instance.exports, sendPacket, onState);

function onState(state, packet) {
  ++states;
  const now = performance.now();
  const gapMs = latest ? Math.round(now - latest.at) : null;
  latest = {state, packet, at: now};
  resetAfter = now + 8000;
  if (now - lastLog > 1000 || state.remoteKeys) {
    lastLog = now;
    log('game-state', {states, received, decisions, frame: state.frame, gapMs, playing: state.playing,
      paused: state.paused, paced: packet[62] === 1, score: state.score, lives: state.lives, ship: state.ship,
      localKeys: state.localKeys, remoteKeys: state.remoteKeys, lastAction});
  }
  if (!state.initialized || !state.playing || state.paused) {
    lastAction = 'wait';
    if (!state.playing && packet[62] === 2) peer.decisionMask = 0;
    // A short START pulse advances the title/help screens without holding
    // pause into gameplay. Paused races are left for the physical player.
    if (!state.playing && now - lastMenuPulse >= 2500) {
      lastMenuPulse = now;
      menuPulseUntil = now + 210;
    }
    peer.mask = !state.playing && now < menuPulseUntil ? BUTTONS.start : 0;
    return;
  }

  if (pending) return;
  pending = true;
  void (async () => {
    try {
      const response = await fetch(endpoint, {
        method: 'POST', body: observation(state, packet, lastAction),
        signal: AbortSignal.timeout(300),
      });
      if (!response.ok) throw new Error('HTTP ' + response.status);
      const result = await response.json();
      if (result.action !== 'wait' && !['left', 'right', 'up', 'down', 'a', 'b'].includes(result.action)) {
        throw new Error('Invalid action');
      }
      if (stopped || !latest.state.playing || latest.state.paused || performance.now() - now > 250) return;
      lastAction = result.action;
      const mask = lastAction === 'wait' ? 0 : BUTTONS[lastAction];
      if (packet[62] === 1) peer.decisionMask = mask;
      else peer.mask = mask;
      sentAt = performance.now();
      ++decisions;
      if (decisions <= 10 || decisions % 60 === 0) {
        log('decision', {frame: state.frame, action: lastAction, inferenceMs: result.inference_ms, decisions});
      }
    } catch (error) {
      peer.mask = 0;
      if (performance.now() - lastLog > 800) log('inference-error', {message: error.message});
    } finally {
      pending = false;
    }
  })();
}

socket.on('message', (packet, from) => {
  if (from.address === host && from.port === 47820 && packet.length === 6) {
    ++received;
    peer.packet(packet);
  }
});
socket.on('error', error => {
  log('socket-error', {message: error.message});
  process.exit(1);
});
socket.bind(47820, bind, () => log('udp-ready', {bind, host, port: 47820}));
const timer = setInterval(() => {
  const now = performance.now();
  if (!latest || now - latest.at > 500 ||
      (latest.state.playing ? now - sentAt > 250 : now >= menuPulseUntil)) {
    peer.mask = 0;
  }
  if (resetAfter && now >= resetAfter) {
    log('cable-reset', {states, received, stateAgeMs: Math.round(now - latest.at)});
    resetAfter = 0;
    peer.close();
    peer = new CablePeer(instance.exports, sendPacket, onState);
  }
  peer.pump();
}, 5);

function shutdown() {
  if (stopped) return;
  stopped = true;
  peer.mask = 0;
  peer.pump();
  clearInterval(timer);
  socket.close();
  peer.close();
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
