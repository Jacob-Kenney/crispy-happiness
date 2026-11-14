#!/usr/bin/env node
import {readFile, writeFile, mkdir} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {createSocket} from 'node:dgram';
import {parseArgs} from 'node:util';
import {fileURLToPath} from 'node:url';
import {CablePeer, CORE_URL, CORE_SHA256, inputMask} from './protocol.mjs';

const {values: args} = parseArgs({options: {
  room: {type: 'string'}, core: {type: 'string', default: fileURLToPath(new URL('.cache/sglink.wasm', import.meta.url))},
  'ticket-file': {type: 'string'}, port: {type: 'string', default: '8765'},
  'fetch-core': {type: 'boolean'}, help: {type: 'boolean'},
}});
if (args.help) {
  console.log('node chromatic/bridge.mjs --fetch-core\nnode chromatic/bridge.mjs --room CODE --ticket-file /tmp/shiny-ticket [--port 8765]');
  process.exit(0);
}
if (args['fetch-core']) {
  const response = await fetch(CORE_URL);
  if (!response.ok) throw new Error(`core download: HTTP ${response.status}`);
  const bytes = Buffer.from(await response.arrayBuffer());
  if (createHash('sha256').update(bytes).digest('hex') !== CORE_SHA256) throw new Error('core checksum mismatch');
  await mkdir(fileURLToPath(new URL('.cache/', import.meta.url)), {recursive: true});
  await writeFile(args.core, bytes);
  console.log('Verified Shiny networking core downloaded.');
  process.exit(0);
}
if (!args.room || !args['ticket-file']) throw new Error('Supply --room and --ticket-file; see chromatic/README.md');
const port = Number(args.port);
if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('invalid UDP port');
const bytes = await readFile(args.core);
if (createHash('sha256').update(bytes).digest('hex') !== CORE_SHA256) throw new Error('core checksum mismatch');
const {instance} = await WebAssembly.instantiate(bytes, {});
const core = instance.exports;
core._initialize();
if (core.sgl_abi_version() !== 2) throw new Error('unsupported CWL core ABI');
const ticket = (await readFile(args['ticket-file'], 'utf8')).trim();
if (!ticket) throw new Error('empty ticket file');
const udp = createSocket('udp4');
const ws = new WebSocket('wss://mp.shinygen.ai/link');
ws.binaryType = 'arraybuffer';
let subscriber, latest, welcome = false, lastInput = 0, lastCommandSequence;
let receivedAt = 0;
const sendState = () => {
  if (subscriber && latest) udp.send(JSON.stringify({...latest, ageMs: Math.round(performance.now() - receivedAt)}), subscriber.port, subscriber.address);
};
const peer = new CablePeer(core, p => { if (welcome && ws.readyState === WebSocket.OPEN) ws.send(p); }, state => {
  latest = state; receivedAt = performance.now(); sendState();
});
// A single local agent owns controls. New endpoints can subscribe after the old
// controller's lease expires. No authentication token ever travels over UDP.
udp.on('message', (message, rinfo) => {
  if (message.length > 512) return;
  const now = performance.now();
  if (subscriber && (rinfo.port !== subscriber.port || rinfo.address !== subscriber.address) && now - lastInput < 500) return;
  try {
    const command = JSON.parse(message);
    const mask = inputMask(command);
    if (!Number.isSafeInteger(command.sequence) || command.sequence < 0) return;
    if (now - lastInput < 500 && lastCommandSequence !== undefined && command.sequence <= lastCommandSequence) return;
    subscriber = rinfo; lastCommandSequence = command.sequence;
    peer.mask = mask; lastInput = now;
    sendState();
  } catch { /* malformed local datagrams have no effect */ }
});
udp.on('error', error => { console.error(`UDP: ${error.message}`); shutdown(1); });
udp.bind(port, '127.0.0.1', () => console.log(`Agent UDP listening on 127.0.0.1:${port}`));
ws.addEventListener('open', () => ws.send(JSON.stringify({t: 'hello', ticket, mode: 'join', code: args.room.toUpperCase(), client_id: createHash('sha256').update(String(process.pid)+Date.now()).digest('hex').slice(0,16), kind: 'cwl2'})));
ws.addEventListener('message', event => {
  if (typeof event.data === 'string') {
    let m; try { m = JSON.parse(event.data); } catch { return; }
    if (m.t === 'welcome') { welcome = true; console.log('Joined Shiny Relay cable room.'); }
    if (m.t === 'peer') console.log(m.present ? 'Chromatic connected.' : 'Waiting for Chromatic.');
    if (m.t === 'err') { console.error(`Shiny Relay refused connection: ${m.code}`); shutdown(1); }
  } else peer.packet(new Uint8Array(event.data));
});
ws.addEventListener('error', () => { console.error('Shiny Relay WebSocket failed.'); shutdown(1); });
ws.addEventListener('close', () => shutdown(0));
const timer = setInterval(() => {
  if (performance.now() - lastInput >= 500) peer.mask = 0;
  peer.pump();
}, 5);
const ping = setInterval(() => { if (welcome && ws.readyState === WebSocket.OPEN) ws.send('{"t":"ping"}'); }, 15000);
let closing = false;
function shutdown(code) {
  if (closing) return;
  closing = true; peer.mask = 0;
  clearInterval(timer); clearInterval(ping);
  ws.close(); udp.close(); peer.close(); process.exitCode = code;
}
process.on('SIGINT', () => shutdown(0));
process.on('SIGTERM', () => shutdown(0));
