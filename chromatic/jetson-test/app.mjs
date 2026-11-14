import {readFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {createSocket} from 'node:dgram';
import {createServer} from 'node:http';
import {CablePeer, CORE_SHA256} from './protocol.mjs';

const host = process.env.CHROMATIC_IP || '10.42.0.60';
const bytes = await readFile(new URL('./sglink.wasm', import.meta.url));
if (createHash('sha256').update(bytes).digest('hex') !== CORE_SHA256) throw Error('core checksum mismatch');
const {instance} = await WebAssembly.instantiate(bytes, {});
instance.exports._initialize();
const socket = createSocket('udp4');
const report = {app: 'canyon-input-test', host, receivedPackets: 0, sentPackets: 0, states: 0, phase: 'waiting-for-game', results: []};
let receivedAt = 0, phaseAt = 0, step = 0, started = false, closing = false;
// One finite sequence; stale telemetry stops all input. Each pulse gets a
// release interval longer than the ROM's remote-input timeout.
const steps = [{name:'start', mask:128}, {name:'left', mask:2}, {name:'right', mask:1}];
const log = (event, fields={}) => console.log(JSON.stringify({event, ...fields}));
const peer = new CablePeer(instance.exports, packet => {
  if (!closing) socket.send(packet, 47820, host, error => {
    if (error) report.sendError = error.message;
    else report.sentPackets++;
  });
}, state => {
  receivedAt = performance.now(); report.states++; report.latest = state;
  if (!started) { started = true; phaseAt = receivedAt; report.phase = 'settling'; }
  const result = report.results[step];
  if (result && report.phase === 'press' && state.remoteKeys === steps[step].mask) result.echoed = true;
  if (result && report.phase === 'release' && state.remoteKeys === 0) result.released = true;
  log('game-state', {sequence:state.sequence, remoteKeys:state.remoteKeys, ship:state.ship, playing:state.playing});
});
socket.on('message', (packet, from) => {
  if (from.address !== host || from.port !== 47820 || packet.length !== 6) return;
  report.receivedPackets++; peer.packet(packet);
});
socket.on('error', error => { report.socketError = error.message; log('socket-error', {message:error.message}); });
socket.bind(47820, '0.0.0.0', () => log('udp-ready', {host, port:47820}));
const timer = setInterval(() => {
  const now = performance.now();
  if (started && now - receivedAt > 5000 && !['complete','failed'].includes(report.phase)) {
    peer.mask = 0; report.phase = 'failed'; log('test-failed', {reason:'game-state timeout; controls released'});
  }
  if (report.phase === 'settling' && now-phaseAt >= 4000) begin(now);
  else if (report.phase === 'press' && now-phaseAt >= 3000) {
    peer.mask=0; report.phase='release'; phaseAt=now; log('release');
  } else if (report.phase === 'release' && now-phaseAt >= 6000) {
    step++;
    if (step === steps.length) {
      report.phase='complete'; report.passed=report.results.every(r=>r.echoed && r.released);
      log('test-complete', {passed:report.passed, results:report.results});
    } else begin(now);
  }
  peer.pump();
}, 5);
function begin(now) {
  const command=steps[step];
  report.results.push({button:command.name, echoed:false, released:false});
  peer.mask=command.mask; report.phase='press'; phaseAt=now; log('press', command);
}
const server=createServer((req,res) => {
  res.writeHead(req.url === '/health' || req.url === '/status' ? 200 : 404, {'content-type':'application/json'});
  res.end(JSON.stringify({...report, ageMs:receivedAt ? Math.round(performance.now()-receivedAt) : null}));
}).listen(8787, '0.0.0.0', ()=>log('http-ready', {port:8787}));
function shutdown() {
  if(closing) return;
  peer.mask=0; peer.pump(); closing=true; clearInterval(timer);
  socket.close(); server.close(); peer.close();
}
process.on('SIGINT',shutdown); process.on('SIGTERM',shutdown);
